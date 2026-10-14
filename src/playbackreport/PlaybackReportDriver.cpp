#include "pch.h"

#include "PlaybackReportDriver.h"

#include "ReporterSwitchCatalog.h"
#include "mpvbridge/MpvController.h"
#include "streams/StreamTransit.h"

namespace {
// 采样节拍（规格 N74 §3：驱动器常量；将来 SMTC 一类要 5s 只改这个数）。1s 是下界
// 不是精确间隔——一轮实长 = 每报告者现读三项 + Σ 各报告者的阻塞，被拖长的轮次不补发
constexpr int kSampleIntervalMs = 1000;

// B17 EventThread 同族：run() 内 exec() 跑事件循环（差别是本线程跑 Qt 循环而非收
// mpv 事件）。对象随进程存活、不 delete（与 EventThread 同口径）——事件循环一退
// run() 即自然结束，不留活动线程跑在本 DLL 代码里
class PlaybackReportThread final : public QThread {
  public:
    using QThread::QThread;
    void run() override { exec(); }
};
} // namespace

PlaybackReportDriver::PlaybackReportDriver(MpvController *controller,
                                           std::shared_ptr<PlaybackReporterRegistry> reporters)
    : QObject(nullptr), m_controller(controller), m_reporters(std::move(reporters)) {
    // 连接恒在，起的只是线程：对象随 MainWindow 构造期出生（此刻在 GUI 线程），
    // moveToThread 之后信号即按 queued 投递堆进线程队列，exec() 一跑即按序处理
    m_thread = new PlaybackReportThread();
    moveToThread(m_thread);
}

PlaybackSnapshot PlaybackReportDriver::playbackSnapshot() const { return m_state.copy(); }

void PlaybackReportDriver::boot() {
    if (gate() != Gate::Unbooted)
        return; // 启动判据只看闸门、不看指针（N74 §8），持有闸门的一侧判
    m_thread->start();
    QMetaObject::invokeMethod(this, [this] { beginCollecting(); }, Qt::QueuedConnection);
}

void PlaybackReportDriver::flushAndJoin(int ms) {
    if (gate() == Gate::Unbooted)
        return; // 线程从未起过：对未启动的 QThread 调 wait() 属编程错误，整体短路
    QMetaObject::invokeMethod(this, [this] { shutdownFlush(); }, Qt::QueuedConnection);
    if (!m_thread->wait(ms))
        softStopFromGui(); // 超时即软停：置 Dead 并退出线程事件循环（不发 Stop）
}

void PlaybackReportDriver::beginCollecting() {
    // Unbooted → Running 用 CAS（N99：所有非 Dead 的迁移带期望前态，"单向不可回"
    // 从注释变成代码）——重复触发在此一并吸收
    Gate expected = Gate::Unbooted;
    if (!m_gate.compare_exchange_strong(expected, Gate::Running, std::memory_order_release,
                                        std::memory_order_acquire))
        return; // 已启动，重复触发防御
    // 把折叠快照当作刚到的起播事实处理：标签 / 暂停 / 时长已由折叠覆盖就位，身份
    // 非空即开会话。不补一轮采样、不现读控制器、不由 MainWindow 转交载荷——
    // MainWindow 不接 mediaMetadataChanged，为它新加一份标签缓存等于把 §4 的归属键
    // 通路搬回 GUI 侧
    if (m_state.copy().isPlaying())
        startSession();
}

void PlaybackReportDriver::beginSegment(const QString &path, const QUuid &listUuid) {
    const Gate g = gate();
    if (g == Gate::Dead)
        return;
    // 纯赋值覆盖（任何闸门态）：来源 + 身份 + 位置清零——新段无采样来源，Start 取 0，
    // ~1s 后第一条采样事件带上真实位置（报告者投不投由它自己的位置差节流定）。来源由
    // MainWindow 随段交入（正播实体的归属列表，模型侧现算）；身份仍取载荷经 unwrap
    // 折回：N61 唯一出口的产出是"播放形态"——转接条目（Jellyfin 恒 Required）是
    // oxbow:// 包装，不是报告者认领判据吃的条目身份（变更记录）；unwrap 幂等，非转接
    // 形态原样通过
    const QString identity = StreamTransit::unwrap(path);
    // 残余会话收口**排在写穿折叠之前**：晚一步那条防御性 Stop 就只能从刚清零的 position、
    // 以及已被新段覆盖的 duration / metadata 取值——采样条禁止的"抹回片头"。正常链路
    // endFile 恒先于 fileLoaded，此分支不触发
    if (g == Gate::Running && m_sessionActive)
        stopSession();
    m_state.update([&](PlaybackSnapshot &s) {
        s.identity = identity;
        s.listUuid = listUuid;
        s.positionSeconds = 0;
    });
    // Unbooted / Gated 只折叠；空身份（无正播）不开会话——无从归属，报告者一律
    // 自过滤，开了会话只是把采样空转起来
    if (g != Gate::Running || identity.isEmpty())
        return;
    startSession();
}

void PlaybackReportDriver::onEndFile() {
    const Gate g = gate();
    if (g == Gate::Dead)
        return;
    // 先闭会话、后清身份（N99 次序）：那条 Stop 的归属从快照出生，此刻身份未清；
    // 反序即"已清身份但会话未闭"的中间态——缺口 2 嫌的双源病灶
    if (g == Gate::Running)
        stopSession();
    m_state.update([](PlaybackSnapshot &s) { s.identity.clear(); });
}

void PlaybackReportDriver::onIdleActive(bool active) {
    const Gate g = gate();
    if (g == Gate::Dead)
        return;
    if (!active)
        return; // 离开空闲无事可做——起播事实由 fileLoaded 携带
    // 先闭会话、后清身份（同 onEndFile）；无活动会话时 stopSession 幂等忽略
    // （启动期 idle、EOF 之后跟随的 idle）
    if (g == Gate::Running)
        stopSession();
    m_state.update([](PlaybackSnapshot &s) { s.identity.clear(); });
}

void PlaybackReportDriver::onPauseChanged(bool paused) {
    const Gate g = gate();
    if (g == Gate::Dead)
        return;
    m_state.update([&](PlaybackSnapshot &s) { s.paused = paused; }); // 纯赋值覆盖（任何闸门态）
    if (g != Gate::Running || !m_sessionActive)
        return;
    // 即时报改走共用取值规则（N99 缺口 1）：位置与暂停态恒带调用前刚现读的新鲜值，
    // "Gated 补发窗口带 Gated 前旧位置"从根部消失。载荷与现读不等只可能期间又翻了
    // 一次，报最新态才是"服务器据此停止计时"的语义（偏离 N74 §3"IsPaused 取载荷"）
    const std::optional<PlaybackSnapshot> snap = refreshForProgress();
    if (!snap)
        return; // 换段在途 / 闸门翻转：本条作废（与采样轮同一条纪律）
    deliverAll(enabledReporters(snap->listUuid),
               snap->toEvent(PlaybackEvent::Phase::Progress, m_sessionToken));
}

void PlaybackReportDriver::onMediaMetadataChanged(const QString &path,
                                                  const QVariantMap &metadata) {
    if (gate() == Gate::Dead)
        return;
    Q_UNUSED(path);
    // 无条件覆盖（不校验 path，规格折叠细节①）
    m_state.update([&](PlaybackSnapshot &s) { s.metadata = metadata; });
}

void PlaybackReportDriver::onDurationChanged(const QString &path, double seconds) {
    if (gate() == Gate::Dead)
        return;
    Q_UNUSED(path);
    // 无条件覆盖含 0（0 即未知，规格折叠细节②）
    m_state.update([&](PlaybackSnapshot &s) { s.durationSeconds = seconds; });
}

void PlaybackReportDriver::setKeyEnabled(const QString &key, bool enabled) {
    const Gate g = gate();
    if (g == Gate::Dead)
        return;                         // 单向不可回：表改动随 Dead 吸收，闸门绝不再动
    m_enableTable.insert(key, enabled); // 仅工作线程读写，无需加锁
    // 聚合与主窗启动判据同调（N99：anyEnabled 随账目单点，"同口径"从验收约束变成
    // 编译期事实）——表内任一为真，或注册表里存在未登记的键（未登记即算启用）。
    // 只看表的话，第二家报告者（不进设置目录、因而不在表里的那种）会被别家键的
    // 关闭一起判成"全关"而误置 Gated。registeredKeys 取**未经任何过滤**的取表结果
    // ——启用表过滤吃的正是这条规则，用它过滤出的子集反推会自指
    std::vector<QString> registeredKeys;
    for (const auto &entry : m_reporters->snapshot())
        registeredKeys.push_back(entry.key);
    const bool anyEnabledNow = anyEnabled(m_enableTable, registeredKeys);
    if (g == Gate::Running && !anyEnabledNow) {
        // 由开转关：先发 Stop 再置 Gated（顺序不可颠倒，反了被挡）；迁移走 CAS——
        // 并发的 softStopFromGui 若已置 Dead，这里覆不回去（N74 §8"单向不可回"
        // 从注释变成代码，不再靠"事件循环已 quit、后续无人执行"的推理链兜底）
        stopSession();
        Gate expected = Gate::Running;
        m_gate.compare_exchange_strong(expected, Gate::Gated, std::memory_order_release,
                                       std::memory_order_acquire);
    } else if (g == Gate::Gated && anyEnabledNow) {
        // 由关转开：先清回 Running 再发 Start（顺序不可颠倒，反了 Start 被首行挡掉）；
        // CAS 失败即并发的退出序已置 Dead，Start 不得再发
        Gate expected = Gate::Gated;
        if (m_gate.compare_exchange_strong(expected, Gate::Running, std::memory_order_release,
                                           std::memory_order_acquire)) {
            if (m_state.copy().isPlaying())
                // 本段补发 Start（快照构造；位置恒 0，首个 Progress 纠偏）
                startSession();
        }
    }
}

void PlaybackReportDriver::shutdownFlush() {
    const Gate g = gate();
    if (g == Gate::Dead)
        return; // 幂等
    if (g == Gate::Unbooted)
        return;    // 线程从未起过：GUI 侧判据（gate()）不该走到这里，防御
    stopSession(); // 最后一条 Stop：从快照构造，不再读属性
    // 迁移走 CAS（N99）：并发的 softStopFromGui 已置 Dead 即落空，Dead 不被覆写
    Gate expected = g;
    m_gate.compare_exchange_strong(expected, Gate::Dead, std::memory_order_release,
                                   std::memory_order_acquire);
    if (m_timer)
        m_timer->stop(); // stopSession 已停，防御双停
    m_thread->quit();    // 正对本线程循环栈顶生效（见头文件注；quit 非本线程调用不兑现的场景是
                         // QCoreApplication::quit()）
}

void PlaybackReportDriver::softStopFromGui() {
    Gate expected = m_gate.load(std::memory_order_acquire);
    if (expected == Gate::Dead || expected == Gate::Unbooted)
        return; // 从未起过不碰 wait 语义；已死幂等
    // CAS 失败即并发的退出序已置 Dead：幂等同归，不再覆写
    if (!m_gate.compare_exchange_strong(expected, Gate::Dead, std::memory_order_release,
                                        std::memory_order_acquire))
        return;
    m_thread->quit(); // 跨线程投递退出；定时器不跨线程停——下轮 sampleRound 首行自吸收
}

void PlaybackReportDriver::sampleRound() {
    if (gate() != Gate::Running)
        return; // 首行同查
    // 会话身份与来源在本轮内恒定（单线程：轮中途无事件处理，换段必有 endFile /
    // beginSegment 先收口并停采样），取一次即可
    const PlaybackSnapshot session = m_state.copy();
    const auto reporters = enabledReporters(session.listUuid);
    for (const auto &entry : reporters) {
        if (gate() != Gate::Running)
            return; // 软停中途：当轮剩余报告者一并放弃——不读属性、不发请求、不重排
                    // 下一次（上一轮扇出可能已阻塞若干秒，此处必须现查，见头注释）
        // 每报告者调用前刷新一次（"每报告者调用前刷新"的语义经 refreshForProgress
        // 原样保留）：某个报告者阻塞了若干秒，下一个拿到的仍是调用前刚刷的新值；
        // 调用期间快照静止（线程阻塞在报告者里，事件循环不跑、queued 推送进不来），
        // 但那本就不是取值时刻。身份已变（换段 / 已卸载）本轮作废：不补发、不在扇出
        // 中途改会话，交给随后的 endFile / beginSegment 收口
        const std::optional<PlaybackSnapshot> snap = refreshForProgress();
        if (!snap)
            break;
        deliver(entry, snap->toEvent(PlaybackEvent::Phase::Progress, m_sessionToken));
    }
    if (gate() == Gate::Running && m_timer)
        m_timer->start(kSampleIntervalMs); // 每轮结束重排下一次（本轮作废同样重排）
}

void PlaybackReportDriver::startSession() {
    // 防御：新段起播而旧会话未闭。beginSegment 已把这道收口挪到写穿折叠之前（那条
    // Stop 的位置不能从刚清零的快照取），此处只剩幂等——其余调用点（beginCollecting /
    // setKeyEnabled 转开）进来时恒无活动会话
    if (m_sessionActive)
        stopSession();
    const PlaybackSnapshot snap = m_state.copy();
    const auto reporters = enabledReporters(snap.listUuid);
    if (reporters.empty())
        return; // 该来源名下无启用的报告者：不开会话、不启定时器（本段零开销）
    m_sessionActive = true;
    m_sessionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PlaybackEvent ev = snap.toEvent(PlaybackEvent::Phase::Start, m_sessionToken);
    // Start 恒取 0，不经快照：转开路径此刻快照的 position 是 Gated 前的旧值（Gated
    // 期不采样），按快照实现会把报告者的节流基线抬到旧位置、首个纠偏 Progress 延后
    ev.positionSeconds = 0;
    deliverAll(reporters, ev);
    // 采样定时器首次在此构造（工作线程内——moveToThread 之前创建会归 GUI 线程；
    // 线程没起就没有定时器），单次定时器每轮结束重排
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &PlaybackReportDriver::sampleRound);
    }
    m_timer->start(kSampleIntervalMs);
}

void PlaybackReportDriver::stopSession() {
    if (!m_sessionActive)
        return; // Stop 幂等：无活动会话即忽略
    m_sessionActive = false;
    const QString token = m_sessionToken;
    m_sessionToken.clear(); // 会话闭：此后重复的 Stop / 无会话的 Progress 全部忽略
    if (m_timer)
        m_timer->stop(); // 会话闭即停采样（换段由随后的 fileLoaded 重开）
    // 归属与全部取值从快照出生：清身份的入口都排在闭会话之后（N99 次序），此刻
    // 快照身份仍是本会话的；路由取快照的来源列表身份（会话期恒与快照同值）。
    // EOF 不再抢 duration（N99 偏离 3）：恒报快照最后已知位置
    const PlaybackSnapshot snap = m_state.copy();
    deliverAll(enabledReporters(snap.listUuid), snap.toEvent(PlaybackEvent::Phase::Stop, token));
}

std::optional<PlaybackSnapshot> PlaybackReportDriver::refreshForProgress() {
    if (gate() != Gate::Running)
        return std::nullopt; // 闸门翻转（软停 / 置 Gated）：取值作废
    // 现读四项先取齐再进锁（mpv 属性读是 client API 直读、µs 级，不该压在临界区内）：
    // 比对身份、写穿三项、交出写后的快照，一次加锁一气完成
    const QString identity = StreamTransit::unwrap(m_controller->currentPlayingPath());
    const double position = m_controller->propertyDouble("time-pos", 0.0);
    const double duration = m_controller->propertyDouble("duration", 0.0);
    const bool paused = m_controller->propertyFlag("pause", false);
    const PlaybackSnapshot snap = m_state.update([&](PlaybackSnapshot &s) {
        // 现读身份与快照不符（换段在途 / 已卸载）一律**不写穿**：卸载态现读 time-pos /
        // duration 取到 fallback 0，写穿会把随后待发的那条 Stop 的位置抹回片头——与
        // "残余会话收口排在写穿折叠之前"是同一条纪律
        if (s.identity != identity)
            return;
        // 身份不写（会话期内折叠身份恒等，比对已保证——写它是死值）；metadata 只在
        // FILE_LOADED 主动读一次、无 observe 源，不经本函数刷新
        s.positionSeconds = position;
        s.durationSeconds = duration;
        s.paused = paused;
    });
    if (snap.identity != identity)
        return std::nullopt; // 调用方作废本轮 / 本条（采样不参与会话切换的纪律）
    return snap;
}

void PlaybackReportDriver::deliver(const PlaybackReporterRegistry::Entry &entry,
                                   const PlaybackEvent &ev) {
    try {
        entry.reporter->playbackEvent(ev);
    } catch (...) {
        // 契约：允许抛，单个报告者失败不影响后续报告者与播放
    }
}

void PlaybackReportDriver::deliverAll(const std::vector<PlaybackReporterRegistry::Entry> &reporters,
                                      const PlaybackEvent &ev) {
    for (const auto &entry : reporters)
        deliver(entry, ev);
}

std::vector<PlaybackReporterRegistry::Entry>
PlaybackReportDriver::enabledReporters(const QUuid &sourceUuid) {
    auto reporters = m_reporters->snapshot();
    std::erase_if(reporters, [this, sourceUuid](const PlaybackReporterRegistry::Entry &entry) {
        return !keyEnabled(entry.key) ||
               !PlaybackReporterRegistry::claimsSource(entry.listUuid, sourceUuid);
    });
    return reporters;
}

bool PlaybackReportDriver::keyEnabled(const QString &key) const {
    const auto it = m_enableTable.constFind(key);
    return it == m_enableTable.cend() || it.value(); // 未登记的键按启用处理
}

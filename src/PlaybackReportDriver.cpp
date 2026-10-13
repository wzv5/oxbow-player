#include "pch.h"

#include "PlaybackReportDriver.h"

#include "MpvController.h"
#include "StreamTransit.h"

#include <mpv/client.h>

namespace {
// 采样节拍（规格 N74 §3：驱动器常量；将来 SMTC 一类要 5s 只改这个数）。1s 是下界
// 不是精确间隔——一轮实长 = 每报告者现读四项 + Σ 各报告者的阻塞，被拖长的轮次不补发
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

PlaybackReportDriver::StateSnapshot PlaybackReportDriver::stateSnapshot() const {
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    StateSnapshot s;
    s.playing = m_statePlaying;
    s.listUuid = m_stateListUuid;
    s.identity = m_stateIdentity;
    s.positionSeconds = m_statePosition;
    s.durationSeconds = m_stateDuration;
    s.paused = m_statePaused;
    s.metadata = m_stateMetadata;
    return s;
}

void PlaybackReportDriver::beginCollecting() {
    if (m_gate.load(std::memory_order_acquire) != Gate::Unbooted)
        return; // 已启动，重复触发防御
    m_gate.store(Gate::Running, std::memory_order_release);
    // 把折叠快照当作刚到的起播事实处理：标签 / 暂停 / 时长已由折叠覆盖就位，身份
    // 非空即开会话。不补一轮采样、不现读控制器、不由 MainWindow 转交载荷——
    // MainWindow 不接 mediaMetadataChanged，为它新加一份标签缓存等于把 §4 的归属键
    // 通路搬回 GUI 侧
    QString identity;
    QUuid listUuid;
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        identity = m_stateIdentity;
        listUuid = m_stateListUuid;
    }
    if (!identity.isEmpty())
        startSession(identity, listUuid);
}

void PlaybackReportDriver::beginSegment(const QString &path, const QUuid &listUuid) {
    if (m_gate.load(std::memory_order_acquire) == Gate::Dead)
        return;
    // 纯赋值覆盖（任何闸门态）：来源 + 身份 + 位置清零——新段无采样来源，Start 取 0，
    // ~1s 后第一条采样事件带上真实位置（报告者投不投由它自己的位置差节流定）。来源由
    // MainWindow 随段交入（正播实体的归属列表，模型侧现算）；身份仍取载荷经 unwrap
    // 折回：N61 唯一出口的产出是"播放形态"——转接条目（Jellyfin 恒 Required）是
    // oxbow:// 包装，不是报告者认领判据吃的条目身份（变更记录）；unwrap 幂等，非转接
    // 形态原样通过
    const QString identity = StreamTransit::unwrap(path);
    // 残余会话收口**排在写穿折叠之前**：晚一步就只能从刚清零的 position、以及已被新段
    // 覆盖的 duration / metadata 取值——那条防御性 Stop 会把旧段在服务器侧的续播点抹回
    // 片头，与"采样先比身份、未变才写穿"是同一条纪律。正常链路 endFile 恒先于
    // fileLoaded，此分支不触发
    if (m_gate.load(std::memory_order_acquire) == Gate::Running && m_sessionActive)
        stopSession(false);
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_stateIdentity = identity;
        m_stateListUuid = listUuid;
        m_statePosition = 0;
    }
    Gate g = m_gate.load(std::memory_order_acquire);
    // Unbooted / Gated 只折叠；空身份（无正播）不开会话——无从归属，报告者一律
    // 自过滤，开了会话只是把采样空转起来
    if (g != Gate::Running || identity.isEmpty())
        return;
    startSession(identity, listUuid);
}

void PlaybackReportDriver::onEndFile(int reason) {
    if (m_gate.load(std::memory_order_acquire) == Gate::Dead)
        return;
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_stateIdentity.clear(); // 折叠清身份（任何闸门态，纯赋值）
    }
    if (m_gate.load(std::memory_order_acquire) != Gate::Running)
        return;
    stopSession(reason == MPV_END_FILE_REASON_EOF);
}

void PlaybackReportDriver::onIdleActive(bool active) {
    if (m_gate.load(std::memory_order_acquire) == Gate::Dead)
        return;
    if (!active)
        return; // 离开空闲无事可做——起播事实由 fileLoaded 携带
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_stateIdentity.clear();
    }
    if (m_gate.load(std::memory_order_acquire) != Gate::Running)
        return;
    stopSession(false); // 无活动会话时幂等忽略（启动期 idle、EOF 后跟随的 idle）
}

void PlaybackReportDriver::onPauseChanged(bool paused) {
    if (m_gate.load(std::memory_order_acquire) == Gate::Dead)
        return;
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_statePaused = paused; // 纯赋值覆盖（任何闸门态）
    }
    if (m_gate.load(std::memory_order_acquire) != Gate::Running || !m_sessionActive)
        return;
    // 即时 Progress：从快照构造（position ≤1 拍旧），IsPaused 取载荷——服务器据此
    // 停止计时，报告者侧由暂停切换判据放行（位置差恒不命中）
    PlaybackEvent ev;
    ev.phase = PlaybackEvent::Phase::Progress;
    ev.sessionToken = m_sessionToken;
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        ev.identity = m_stateIdentity;
        ev.positionSeconds = m_statePosition;
        ev.durationSeconds = m_stateDuration;
        ev.metadata = m_stateMetadata;
    }
    ev.paused = paused;
    sendToReporters(ev, enabledReporters(m_sessionListUuid));
}

void PlaybackReportDriver::onMediaMetadataChanged(const QString &path,
                                                  const QVariantMap &metadata) {
    if (m_gate.load(std::memory_order_acquire) == Gate::Dead)
        return;
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    (void)path;
    m_stateMetadata = metadata; // 无条件覆盖（不校验 path，规格折叠细节①）
}

void PlaybackReportDriver::onDurationChanged(const QString &path, double seconds) {
    if (m_gate.load(std::memory_order_acquire) == Gate::Dead)
        return;
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    (void)path;
    m_stateDuration = seconds; // 无条件覆盖含 0（0 即未知，规格折叠细节②）
}

void PlaybackReportDriver::setKeyEnabled(const QString &key, bool enabled) {
    if (m_gate.load(std::memory_order_acquire) == Gate::Dead)
        return;                         // 单向不可回：表改动随 Dead 吸收，闸门绝不再动
    m_enableTable.insert(key, enabled); // 仅工作线程读写，无需加锁
    // "有任一启用键"与 keyEnabled 同口径：表内任一为真，或注册表里存在未登记的
    // 键（未登记即算启用）。只看表的话，第二家报告者（不进设置目录、因而不在表里
    // 的那种）会被别家键的关闭一起判成"全关"而误置 Gated
    bool anyEnabled =
        std::any_of(m_enableTable.cbegin(), m_enableTable.cend(), [](bool v) { return v; });
    if (!anyEnabled) {
        for (const auto &entry : m_reporters->snapshot()) {
            if (!m_enableTable.contains(entry.key)) {
                anyEnabled = true;
                break;
            }
        }
    }
    Gate g = m_gate.load(std::memory_order_acquire);
    if (g == Gate::Running && !anyEnabled) {
        stopSession(false); // 由开转关：先发 Stop 再置 Gated（顺序不可颠倒，反了被挡）
        m_gate.store(Gate::Gated, std::memory_order_release);
    } else if (g == Gate::Gated && anyEnabled) {
        m_gate.store(Gate::Running, std::memory_order_release); // 先清回 Running 再发 Start
        QString identity;
        QUuid listUuid;
        {
            const std::lock_guard<std::mutex> lock(m_stateMutex);
            identity = m_stateIdentity;
            listUuid = m_stateListUuid;
        }
        if (!identity.isEmpty())
            // 本段补发 Start（快照构造；位置恒 0，首个 Progress 纠偏）
            startSession(identity, listUuid);
    }
}

void PlaybackReportDriver::shutdownFlush() {
    const Gate g = m_gate.load(std::memory_order_acquire);
    if (g == Gate::Dead)
        return; // 幂等
    if (g == Gate::Unbooted)
        return;         // 线程从未起过：GUI 侧判据（gate()）不该走到这里，防御
    stopSession(false); // 最后一条 Stop：从快照构造，不再读属性
    m_gate.store(Gate::Dead, std::memory_order_release); // 单向不可回
    if (m_timer)
        m_timer->stop(); // stopSession 已停，防御双停
    m_thread->quit(); // 正对本线程循环栈顶生效（见头文件注；quit 非本线程调用不兑现的场景是
                      // QCoreApplication::quit()）
}

void PlaybackReportDriver::softStopFromGui() {
    const Gate g = m_gate.load(std::memory_order_acquire);
    if (g == Gate::Dead || g == Gate::Unbooted)
        return;                                          // 从未起过不碰 wait 语义；已死幂等
    m_gate.store(Gate::Dead, std::memory_order_release); // 单向不可回
    m_thread->quit(); // 跨线程投递退出；定时器不跨线程停——下轮 sampleRound 首行自吸收
}

void PlaybackReportDriver::sampleRound() {
    if (m_gate.load(std::memory_order_acquire) != Gate::Running)
        return; // 首行同查
    const auto reporters = enabledReporters(m_sessionListUuid);
    for (const auto &entry : reporters) {
        if (m_gate.load(std::memory_order_acquire) != Gate::Running)
            return; // 软停中途：当轮剩余报告者一并放弃——置死后不再读任何属性、
                    // 不再发任何请求，也不重排下一次
        // 现读串经 unwrap 折回条目身份（同 beginSegment），与会话身份的比对才同源
        const QString identity = StreamTransit::unwrap(m_controller->currentPlayingPath());
        // 身份已变（换段 / 已卸载）：本轮作废，跳过余下报告者——不补发、不在扇出
        // 中途改会话，交给随后的 endFile / beginSegment 收口（换段恒有这两路信号，
        // 采样不参与会话切换，免与 queued 信号抢序）。比对排在写穿之前：卸载态的
        // time-pos / duration 现读不可用（取 fallback 0），写穿会把随后待发的那条
        // Stop 的位置抹回片头，而新段的值本就由 beginSegment 折叠供给。来源不参与
        // 比对——它是会话属性，换列表必伴随换段，新来源由 beginSegment 随段交入
        if (identity != m_sessionIdentity)
            break;
        // 每报告者调用前现读四项并写穿快照（µs 级）——某个报告者阻塞了若干秒，
        // 下一个报告者拿到的是调用前刚刷新的值；调用期间快照静止（线程阻塞在
        // 报告者里，事件循环不跑、queued 推送进不来），但那本就不是取值时刻
        const double position = m_controller->propertyDouble("time-pos", 0.0);
        const double duration = m_controller->propertyDouble("duration", 0.0);
        const bool paused = m_controller->propertyFlag("pause", false);
        {
            const std::lock_guard<std::mutex> lock(m_stateMutex);
            m_stateIdentity = identity;
            m_statePosition = position;
            m_stateDuration = duration;
            m_statePaused = paused;
        }
        PlaybackEvent ev;
        ev.phase = PlaybackEvent::Phase::Progress;
        ev.sessionToken = m_sessionToken;
        ev.identity = identity;
        ev.positionSeconds = position;
        ev.durationSeconds = duration;
        ev.paused = paused;
        {
            const std::lock_guard<std::mutex> lock(m_stateMutex);
            ev.metadata = m_stateMetadata;
        }
        try {
            entry.reporter->playbackEvent(ev);
        } catch (...) {
            // 契约：允许抛，单个报告者失败不影响后续报告者与播放
        }
    }
    if (m_gate.load(std::memory_order_acquire) == Gate::Running && m_timer)
        m_timer->start(kSampleIntervalMs); // 每轮结束重排下一次（本轮作废同样重排）
}

void PlaybackReportDriver::startSession(const QString &identity, const QUuid &listUuid) {
    // 防御：新段起播而旧会话未闭。beginSegment 已把这道收口挪到写穿折叠之前（那条
    // Stop 的位置不能从刚清零的快照取），此处只剩幂等——其余调用点（beginCollecting /
    // setKeyEnabled 转开）进来时恒无活动会话
    if (m_sessionActive)
        stopSession(false);
    const auto reporters = enabledReporters(listUuid);
    if (reporters.empty())
        return; // 该来源名下无启用的报告者：不开会话、不启定时器（本段零开销）
    m_sessionActive = true;
    m_sessionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_sessionIdentity = identity;
    m_sessionListUuid = listUuid;
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_statePlaying = true;
    }
    PlaybackEvent ev;
    ev.phase = PlaybackEvent::Phase::Start;
    ev.sessionToken = m_sessionToken;
    ev.identity = identity;
    ev.positionSeconds = 0; // 快照位置已在 beginSegment 清零（新段无采样来源）
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        ev.durationSeconds = m_stateDuration;
        ev.paused = m_statePaused;
        ev.metadata = m_stateMetadata;
    }
    sendToReporters(ev, reporters);
    // 采样定时器首次在此构造（工作线程内——moveToThread 之前创建会归 GUI 线程；
    // 线程没起就没有定时器），单次定时器每轮结束重排
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &PlaybackReportDriver::sampleRound);
    }
    m_timer->start(kSampleIntervalMs);
}

void PlaybackReportDriver::stopSession(bool eof) {
    if (!m_sessionActive)
        return; // Stop 幂等：无活动会话即忽略
    m_sessionActive = false;
    // 事件归属取会话自己的身份，不从快照取——endFile / idle 的折叠在 stopSession
    // 之前已把快照身份清空（"此刻在播"判据），快照此刻交不出归属
    const QString token = m_sessionToken;
    const QString identity = m_sessionIdentity;
    const QUuid listUuid = m_sessionListUuid; // Stop 仍要投给开会话那一条列表的报告者
    m_sessionToken.clear(); // 会话闭：此后重复的 Stop / 无会话的 Progress 全部忽略
    m_sessionIdentity.clear();
    m_sessionListUuid = QUuid();
    if (m_timer)
        m_timer->stop(); // 会话闭即停采样（换段由随后的 fileLoaded 重开）
    QVariantMap metadata;
    double position = 0;
    double duration = 0;
    bool paused = false;
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        metadata = m_stateMetadata;
        position = m_statePosition;
        duration = m_stateDuration;
        paused = m_statePaused;
        m_statePlaying = false;
        // 快照身份不动：由开转关的挂起路径靠它存活（Gated 期折叠照跑、切回开
        // 补发 Start 的判据），清空是折叠槽（endFile / idle）的专属职责
    }
    PlaybackEvent ev;
    ev.phase = PlaybackEvent::Phase::Stop;
    ev.sessionToken = token;
    ev.identity = identity;
    // EOF 时位置取快照 duration（endFile 时刻现读 duration 可能已不可用，快照是
    // 唯一可靠出处），其余取最后已知位置——服务器据此判"已看"；duration 未知（0）
    // 回落最后已知位置：转接流取不到 duration 是现实分支，报 0 会把服务器续播点
    // 抹回片头，比误差更坏
    ev.positionSeconds = (eof && duration > 0) ? duration : position;
    ev.durationSeconds = duration;
    ev.paused = paused;
    ev.metadata = metadata;
    sendToReporters(ev, enabledReporters(listUuid));
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

void PlaybackReportDriver::sendToReporters(
    const PlaybackEvent &ev, const std::vector<PlaybackReporterRegistry::Entry> &reporters) {
    for (const auto &entry : reporters) {
        try {
            entry.reporter->playbackEvent(ev);
        } catch (...) {
            // 契约：允许抛，单个报告者失败不影响后续报告者与播放
        }
    }
}

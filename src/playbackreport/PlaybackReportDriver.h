#pragma once

#include "pch.h"

#include "PlaybackReporterRegistry.h"
#include "PlaybackSnapshot.h"

class MpvController;
class QTimer;

// N74：播放进度上报驱动器。QObject 驻独立工作线程（QThread 子类 run() 内 exec()，
// B17 EventThread 同族），构造期由 MainWindow 创建并连好全部信号、不启动线程——
// 线程按需启动（判据 = 账目聚合判"有任一启用" 且 注册表里有在册报告者，触发点只在
// 正播段起播一处），启动一次即不再销毁。
//
// 状态快照是唯一事实源，事件构造一律经 PlaybackSnapshot::toEvent（唯一例外：Start
// 的 position 由 startSession 显式置 0）；更新源只有两个：①推送（起播段 beginSegment /
// mediaMetadataChanged / pauseChanged / durationChanged）任何闸门态到达即纯覆盖
// （Unbooted / Gated 的"折叠"与 Running 期的更新是同一条规则）；②采样轮内每个
// 报告者调用前，现读身份与快照一致才把 position / duration / paused 三项写穿（身份
// 写穿是死值——会话期内折叠身份恒等，已由比对保证；换段那次的现读不写，新段的值由
// beginSegment 折叠供给）。endFile / idleActive(true) **先闭会话、后清快照身份**——
// Stop 的归属从快照出生，"已清身份但会话未闭"的中间态不复存在（N99 缺口 2 根因）；
// "此刻在播"由折叠身份判定（isPlaying 派生），开关收口与 beginCollecting 据此补发。
//
// 投递路由按**来源列表**选桶（N74）：条目身份只到服务器级（URN 里没有列表与账户），
// 同一台服务器上的多条列表各注册一个报告者，故每条事件只投给"加工这条播放形态的
// 那一册"。来源身份由 MainWindow 随起播段交入（正播实体的 ownerUuid，模型侧知识）——
// **不从播放串反推**：直发形态的串里没有列表身份，而"在播哪一册"本就归模型不归协议层。
// 认领判据（这条 URN 是不是我那台服务器、我需不需要它）仍归报告者，驱动器不代做。
//
// 闸门纪律：首行同查；Dead 一律早退（单向不可回）；非 Running 不现读属性、
// 不发请求、不开新会话——纯赋值覆盖在任何态都执行。一个入口读一枚、本入口内各判据
// 同用这一枚（Dead 只由 GUI 侧的软停置入，跨判据复用至多在收摊进程里多投一条会被传输
// 闸门吞掉的事件，不构成回归）。两个例外是**两次决策之间可能插入阻塞**的地方：采样轮
// 在每个报告者调用前后各现查一次，取值规则入口再现查一次（那是现读属性与发请求前最近
// 的一道守卫）。
class PlaybackReportDriver : public QObject {
    Q_OBJECT
  public:
    // 四态闸门。Unbooted：线程未起；Running：采集中；Gated：全键关闭的暂停态
    // （折叠照跑、零读取零请求）；Dead：退出序置位、单向不可回。原子承载：
    // GUI 侧（退出序的判据与 wait 决策）需跨线程现查
    enum class Gate { Unbooted, Running, Gated, Dead };

    // controller 只借不持（寿命归 plugin.cpp）；reporters 是 MainWindow 构造期
    // 建好并 provide 进 ServiceRegistry 的同一实例（本对象在 provide 之后才出生，
    // 句柄直接注入，不再折回 resolve）
    PlaybackReportDriver(MpvController *controller,
                         std::shared_ptr<PlaybackReporterRegistry> reporters);

    Gate gate() const { return m_gate.load(std::memory_order_acquire); }

    // 拉取出口：最近状态只读副本，任意线程随时可读——为 MPRIS2 一类"属性在被问
    // 时才答"的拉取型控制面预留（推型报告者走事件，不读它）。刷新时机 = 四条推送
    // 到达即覆盖 + 采样轮每次报告者调用前覆盖；字段语义与 playing 派生判据见
    // PlaybackSnapshot（identity 为空 ⇒ 其余字段是"仅供续播的陈旧值"）
    PlaybackSnapshot playbackSnapshot() const;

    // 线程启动协议收进驱动器（N99：m_thread 不出本类）——start() 与 queued 投本类
    // beginCollecting 两步必须同在，如今同一个方法保证。判据（要不要起）仍归主窗
    // （gate() 与账目聚合）；Unbooted 短路在此判：N74 §8 明写判据只看闸门、不看
    // 指针，本就该由持有闸门的一侧判。重复 start() 落在 Qt 文档担保的幂等上
    // （N74 变更记录"走查修正五"判良性，维持不加旗）
    void boot();
    // 退出收口协议收进驱动器：queued 投 shutdownFlush（最后一条 Stop，从快照构造）
    // → 有界 join → 超时软停（置 Dead 并退出线程事件循环，不发 Stop——当轮扇出
    // 阻塞正说明服务器已不应答）。Unbooted 整体短路在此判（对未启动的 QThread 调
    // wait() 属编程错误）。整段必须排在 QCoreApplication::exit(0) 之前（N74 §8 硬
    // 约束：aboutToQuit 的传输闸门会让最后一条 Stop 静默蒸发）——这个次序由调用方
    // 保证，本方法阻塞至收线
    void flushAndJoin(int ms);
    // 折叠来源 + 身份（任何闸门态）+ 位置清零（新段无采样来源，Start 取 0，首个
    // Progress 纠偏）。listUuid 是正播条目的**归属列表身份**，由 MainWindow 在本段
    // 起播时随载荷交入（模型现算，收编项交原列表）——不从播放串反推，直发形态的
    // 串里没有列表身份。身份仍取载荷经 unwrap 折回（N61 唯一出口的产出是播放形态）。
    // Running 期且身份非空才开会话（空身份无从归属，报告者一律自过滤，开了只是把
    // 采样空转起来）。Running 期尚有残余会话时，收口**排在写穿折叠之前**——晚一步
    // 那条防御性 Stop 就只能从刚清零的 position、以及已被新段覆盖的 duration /
    // metadata 取值（采样条禁止的"抹回片头"）
    void beginSegment(const QString &path, const QUuid &listUuid);
    // 设置通路（queued，仅工作线程读写表故无需加锁）：改启用表并切闸门。由开转关
    // 先发 Stop 再置 Gated（顺序不可颠倒）；由关转开先清回 Running 再从快照补发
    // Start（判据是快照身份非空——由开转关那条路已把会话闭掉，"有活动会话"照写
    // 即死分支）。"全键关闭"与调用前的过滤同口径：聚合规则随账目单点
    // （ReporterSwitchCatalog::anyEnabled，未登记的键按启用处理）。Dead 不动闸门
    // （单向不可回）
    void setKeyEnabled(const QString &key, bool enabled);
    // ——信号接收端（MainWindow 构造期经函数指针 connect，成员指针的取用受可见性
    // 约束故公开；它们已不是槽——functor 投递与函数指针 connect 都不要求槽身份，
    // `slots` 关键字只在字符串查找与自动连接时才必要）——
    // 先闭会话（Stop 从快照取归属）、后折叠清身份（任何闸门态，纯赋值）。不认识
    // mpv 的 end-file 枚举（N99：reason 形参退役，"客户端不判完播"在代码里显形）
    void onEndFile();
    void onIdleActive(bool active);
    // 折叠 paused（任何闸门态）；Running 且有活动会话即经共用取值规则现读构造
    // Progress（paused 取现读，服务器据此停止计时）
    void onPauseChanged(bool paused);
    // ——只喂快照的连接：到达即无条件覆盖对应字段，不触发任何动作。不校验 path
    // （发射序在 fileLoaded 之前，覆盖到达时归属键还是上一条）；duration 含 0
    // 无条件覆盖（0 即未知，禁写"沿用旧值"守卫，否则上一条的时长会串进无时长
    // 条目的停止上报）——
    void onMediaMetadataChanged(const QString &path, const QVariantMap &metadata);
    void onDurationChanged(const QString &path, double seconds);

  private:
    // ——只由本类入口驱动（boot / flushAndJoin），不出公共面：读的人不再从
    // "它是槽"推出"有人在连它的信号"——
    // 线程启动后的第一条 queued 调用：闸门清回 Running，把折叠快照当作刚到的
    // 起播事实处理（身份非空即铸 token、发 Start、起采样定时器）。不补一轮采样、
    // 不现读控制器——标签 / 时长 / 暂停 / 来源都由折叠就位（beginSegment 与三条
    // 只喂快照的连接先于本条入队，时序由 MainWindow 的投递次序保证）
    void beginCollecting();
    // 退出收口（queued 调用，必须排在 exit(0) 之前——aboutToQuit 的传输闸门会让
    // 最后一条 Stop 静默蒸发）：发最后一条 Stop（从快照构造，不再读属性）、置 Dead、
    // 退出本线程事件循环。退出形态必须是运行在本线程上的槽里调线程对象自己的
    // quit()——QCoreApplication::quit() 在非主线程的 Qt 循环里永不兑现（本仓实测
    // 教训，见 MainWindow::onMpvShutdown 注释）
    void shutdownFlush();
    // GUI 侧软停（退出序 wait(200) 超时路径）：置 Dead 并从外部退出线程事件循环，
    // 不发 Stop——收口槽尚未开跑正是当轮扇出阻塞所致，即服务器已不应答，这条 Stop
    // 即便挤出去也没人接，服务器侧续播点仍由此前 ≤10s 那次 Progress 定住（规格 §8
    // 排队账：这是设计不是漏洞）。已在队列里的收口槽此后首行同查 Dead 吸收。
    // 出口在此**不走"该线程内的槽"那条**（规格 §8 第 1 步的同一出口）：走到本函数
    // 的前提正是当轮扇出未排空，queued 的收口槽排在它后面、可能永不落地，同线程槽在
    // 超时分支上根本不成立；`QThread::quit` / `exit` 为 Qt 明写的线程安全入口，本仓
    // 实测失效的是 `QCoreApplication::quit()`（非本线程循环的兑现问题，与此无关）
    void softStopFromGui();
    // 采样轮（定时器唤醒）：首行同查闸门；逐报告者先现读身份比对快照——未变才现读
    // 三项写穿快照、从快照构造 Progress；已变则本轮作废且**不写穿**（卸载态现读
    // 取到 fallback 0，写穿会把随后那条 Stop 的位置抹回片头），跳过余下报告者交给
    // 随后的 endFile / fileLoaded 收口；每轮结束重排下一次（单次定时器，阻塞期间
    // 不堆积定时事件）
    void sampleRound();
    // 铸 token、发 Start（快照构造，位置显式置 0）、起采样定时器；该来源名下无启用
    // 的报告者即不开会话、不启定时器（本段全程无上报，连采样都不起）。身份与来源
    // 不再经形参（N99：会话态不镜像播放态）——调用点都刚把快照写好或读自快照，
    // 会话期恒与快照同值（不变量：m_sessionActive ⇒ 快照 identity 非空）。进来的
    // 残余会话收口只剩幂等（beginSegment 已在写穿折叠之前收过一道）
    void startSession();
    // 从快照构造 Stop 并闭会话、停采样定时器；无活动会话即忽略（幂等——启动期的
    // idleActive、EOF 之后跟随的 idle 推送、退出序都会撞上）。EOF 不再抢 duration：
    // 恒报快照最后已知位置（服务器按"位置接近末尾"自行标已看，不依赖精确落到
    // duration；快照位置的陈旧度上界是一轮采样，≈1s 加在途扇出）
    void stopSession();
    // 取值规则（即时报与采样轮共用的唯一一份，N99 缺口 1）：①闸门非 Running 返回空；
    // ②现读播放串经 unwrap 折回身份与快照比对——已变（换段在途）返回空，调用方作废
    // 本轮 / 本条（采样不参与会话切换、免与 queued 信号抢序的纪律）；③现读 time-pos /
    // duration / pause 三项在一次加锁内写穿快照，并交回写后的那一份（metadata 无
    // observe 源，不经此函数刷新）。产出物是快照而不是半成品事件：事件仍一律经 toEvent
    // 出生，本函数不另开第二份字段映射——PlaybackEvent 加字段时只有 toEvent 一处要改
    std::optional<PlaybackSnapshot> refreshForProgress();
    // 扇出单点（N99 缺口 4）：对单个表项的 try/catch 调用（契约 4：允许抛，单个
    // 失败不影响后续报告者与播放）。将来单轮预算 / 在途计数 / 每报告者成败计数落
    // 在 deliver 一处，即同时管住采样与事件两条通路；deliverAll = 逐个 deliver
    void deliver(const PlaybackReporterRegistry::Entry &entry, const PlaybackEvent &ev);
    void deliverAll(const std::vector<PlaybackReporterRegistry::Entry> &reporters,
                    const PlaybackEvent &ev);
    // 报告者快照（lock 失败者即过期并顺手清理）+ 两道筛：启用表过滤（未登记的键
    // 按启用处理）与来源选桶（判据在 `PlaybackReporterRegistry::claimsSource`，
    // 与表项形状同族、可独立测）
    std::vector<PlaybackReporterRegistry::Entry> enabledReporters(const QUuid &sourceUuid);
    bool keyEnabled(const QString &key) const;

    MpvController *m_controller; // 现读三项（仅 Running 期采样），只借不持
    std::shared_ptr<PlaybackReporterRegistry> m_reporters;
    QThread *m_thread = nullptr; // 构造期建、按需 start、启动后不再销毁、不 delete
    QTimer *m_timer = nullptr;   // 工作线程内构造（首次 startSession），单次重排
    std::atomic<Gate> m_gate{Gate::Unbooted};
    // 键 → 启用；仅工作线程读写（GUI 设置变更经 queued 调用），无需加锁
    QHash<QString, bool> m_enableTable;
    // 会话态（仅工作线程）：只记"开没开 + token"，不镜像播放态（N99 缺口 2 的
    // 根因修法——身份与来源的权威在快照一处）。不变量：m_sessionActive ⇒ 快照
    // identity 非空（开会话的入口都要求身份非空才开，清身份的入口都在闭会话之后）
    bool m_sessionActive = false;
    QString m_sessionToken;
    // 播放态快照（"上报视图"，非主窗 UI 态镜像；值与锁收在一枚内，锁不外露）
    GuardedPlaybackSnapshot m_state;
};

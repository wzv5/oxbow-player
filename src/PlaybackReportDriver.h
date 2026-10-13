#pragma once

#include "pch.h"

#include "PlaybackReporterRegistry.h"

class MpvController;
class QTimer;

// N74：播放进度上报驱动器。QObject 驻独立工作线程（QThread 子类 run() 内 exec()，
// B17 EventThread 同族），构造期由 MainWindow 创建并连好全部信号、不启动线程——
// 线程按需启动（判据 = 有任一启用键 且 注册表里有在册报告者，触发点只在正播段起播
// 一处），启动一次即不再销毁。
//
// 状态快照是唯一事实源，事件构造一律读快照；更新源只有两个：①推送（起播段
// beginSegment / mediaMetadataChanged / pauseChanged / durationChanged）任何闸门态
// 到达即纯覆盖（Unbooted / Gated 的"折叠"与 Running 期的更新是同一条规则）；
// ②采样轮内每个报告者调用前，现读身份与会话一致才把四项写穿快照（换段那次的现读
// 不写，新段的值由 beginSegment 折叠供给）。endFile / idleActive(true) 清快照
// 身份（纯赋值）——"此刻在播"由折叠判定，开关收口与 beginCollecting 据此补发。
//
// 投递路由按**来源列表**选桶（N74）：条目身份只到服务器级（URN 里没有列表与账户），
// 同一台服务器上的多条列表各注册一个报告者，故每条事件只投给"加工这条播放形态的
// 那一册"。来源身份由 MainWindow 随起播段交入（正播实体的 ownerUuid，模型侧知识）——
// **不从播放串反推**：直发形态的串里没有列表身份，而"在播哪一册"本就归模型不归协议层。
// 认领判据（这条 URN 是不是我那台服务器、我需不需要它）仍归报告者，驱动器不代做。
//
// 槽的闸门纪律：首行同查；Dead 一律早退（单向不可回）；非 Running 不现读属性、
// 不发请求、不开新会话——纯赋值覆盖在任何态都执行。
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

    // GUI 侧软停（退出序 wait(200) 超时路径）：置 Dead 并从外部退出线程事件循环，
    // 不发 Stop——收口槽尚未开跑正是当轮扇出阻塞所致，即服务器已不应答，这条 Stop
    // 即便挤出去也没人接，服务器侧续播点仍由此前 ≤10s 那次 Progress 定住（规格 §8
    // 排队账：这是设计不是漏洞）。已在队列里的收口槽此后首行同查 Dead 吸收。
    // 出口在此**不走"该线程内的槽"那条**（规格 §8 第 1 步的同一出口）：走到本函数
    // 的前提正是当轮扇出未排空，queued 的收口槽排在它后面、可能永不落地，同线程槽在
    // 超时分支上根本不成立；`QThread::quit` / `exit` 为 Qt 明写的线程安全入口，本仓
    // 实测失效的是 `QCoreApplication::quit()`（非本线程循环的兑现问题，与此无关）
    void softStopFromGui();

    // 拉取出口：最近状态只读副本，任意线程随时可读——为 MPRIS2 一类"属性在被问
    // 时才答"的拉取型控制面预留（推型报告者走事件，不读它）。刷新时机 = 四条推送
    // 到达即覆盖 + 采样轮每次报告者调用前覆盖
    struct StateSnapshot {
        bool playing = false; // 有活动会话
        QUuid listUuid;       // 来源列表身份（正播实体解析不到 / 判等不符即 nil）
        QString identity;
        double positionSeconds = 0;
        double durationSeconds = 0;
        bool paused = false;
        QVariantMap metadata;
    };
    StateSnapshot stateSnapshot() const;

  public slots:
    // 线程启动后的第一条 queued 调用：闸门清回 Running，把折叠快照当作刚到的
    // 起播事实处理（身份非空即铸 token、发 Start、起采样定时器）。不补一轮采样、
    // 不现读控制器——标签 / 时长 / 暂停 / 来源都由折叠就位（beginSegment 与三条
    // 只喂快照的连接先于本条入队，时序由 MainWindow 的投递次序保证）
    void beginCollecting();
    // 折叠来源 + 身份（任何闸门态）+ 位置清零（新段无采样来源，Start 取 0，首个
    // Progress 纠偏）。listUuid 是正播条目的**归属列表身份**，由 MainWindow 在本段
    // 起播时随载荷交入（模型现算：正播实体的 ownerUuid，即加工这条播放形态那一册，
    // 收编项交原列表）——不从播放串反推，直发形态的串里没有列表身份。身份仍取载荷
    // 经 unwrap 折回（N61 唯一出口的产出是播放形态）。Running 期且身份非空才开会话
    // （空身份无从归属，报告者一律自过滤，开了只是把采样空转起来）。Running 期尚有
    // 残余会话时，收口**排在写穿折叠之前**——晚一步那条防御性 Stop 就只能从刚清零的
    // position、以及已被新段覆盖的 duration / metadata 取值（采样条禁止的"抹回片头"）
    void beginSegment(const QString &path, const QUuid &listUuid);
    // 折叠清身份（任何闸门态）；Running 期从快照构造 Stop（EOF 取快照 duration，
    // 其余取最后已知位置；duration 未知回落最后已知位置）
    void onEndFile(int reason);
    void onIdleActive(bool active);
    // 折叠 paused（任何闸门态）；Running 且有活动会话即从快照构造 Progress
    // （IsPaused 取载荷，服务器据此停止计时）
    void onPauseChanged(bool paused);
    // ——只喂快照的连接：到达即无条件覆盖对应字段，不触发任何动作。不校验 path
    // （发射序在 fileLoaded 之前，覆盖到达时归属键还是上一条）；duration 含 0
    // 无条件覆盖（0 即未知，禁写"沿用旧值"守卫，否则上一条的时长会串进无时长
    // 条目的停止上报）——
    void onMediaMetadataChanged(const QString &path, const QVariantMap &metadata);
    void onDurationChanged(const QString &path, double seconds);
    // 设置通路（queued，仅工作线程读写表故无需加锁）：改启用表并切闸门。由开转关
    // 先发 Stop 再置 Gated（顺序不可颠倒）；由关转开先清回 Running 再从快照补发
    // Start（判据是快照身份非空——由开转关那条路已把会话闭掉，"有活动会话"照写
    // 即死分支）。"全键关闭"与调用前的过滤同口径：表内任一为真即算开，注册表里
    // 存在未登记的键也算开（未登记即启用）。Dead 不动闸门（单向不可回）
    void setKeyEnabled(const QString &key, bool enabled);
    // 退出收口（GUI 侧收到 mpvShutdown 后 queued 调用，必须排在 exit(0) 之前——
    // aboutToQuit 的传输闸门会让最后一条 Stop 静默蒸发）：发最后一条 Stop（从快照
    // 构造，不再读属性）、置 Dead、退出本线程事件循环。退出形态必须是运行在本线程
    // 上的槽里调线程对象自己的 quit()——QCoreApplication::quit() 在非主线程的 Qt
    // 循环里永不兑现（本仓实测教训，见 MainWindow::onMpvShutdown 注释）
    void shutdownFlush();

  private:
    // 采样轮（定时器唤醒）：首行同查闸门；逐报告者先现读身份比对会话——未变才现读
    // 余三项写穿快照、从快照构造 Progress；已变则本轮作废且**不写穿**（卸载态现读
    // 取到 fallback 0，写穿会把随后那条 Stop 的位置抹回片头），跳过余下报告者交给
    // 随后的 endFile / fileLoaded 收口；每轮结束重排下一次（单次定时器，阻塞期间
    // 不堆积定时事件）
    void sampleRound();
    // 先闭残留会话（此处只作幂等：beginSegment 已在写穿折叠之前收过一道，其余调用点
    // 进来时恒无活动会话）——不闭就覆掉旧 token，服务器侧悬挂一条"正在播放"。随后铸
    // token、记来源、发 Start（快照构造，位置恒 0）、起采样定时器；该来源名下无启用
    // 的报告者即不开会话、不启定时器（本段全程无上报，连采样都不起）
    void startSession(const QString &identity, const QUuid &listUuid);
    // 从快照构造 Stop 并闭会话、停采样定时器；无活动会话即忽略（幂等——启动期的
    // idleActive、EOF 之后跟随的 idle 推送、退出序都会撞上）
    void stopSession(bool eof);
    // 报告者快照（lock 失败者即过期并顺手清理）+ 两道筛：启用表过滤（未登记的键
    // 按启用处理）与来源选桶（判据在 `PlaybackReporterRegistry::claimsSource`，
    // 与表项形状同族、可独立测）
    std::vector<PlaybackReporterRegistry::Entry> enabledReporters(const QUuid &sourceUuid);
    bool keyEnabled(const QString &key) const;
    // 逐报告者 try/catch 调用（契约：允许抛，单个失败不影响后续报告者与播放）。
    // 收的是**已过滤的表项快照**而非来源 uuid：一次事件（Start 一路）只取一次表——
    // 判空与投递分两次取即同一次扇出两次加锁、两份强引用，与 §3"取报告者快照"一次
    // 的口径也不合
    void sendToReporters(const PlaybackEvent &ev,
                         const std::vector<PlaybackReporterRegistry::Entry> &reporters);

    MpvController *m_controller; // 现读四项（仅 Running 期采样），只借不持
    std::shared_ptr<PlaybackReporterRegistry> m_reporters;
    QThread *m_thread = nullptr; // 构造期建、按需 start、启动后不再销毁、不 delete
    QTimer *m_timer = nullptr;   // 工作线程内构造（首次 startSession），单次重排
    std::atomic<Gate> m_gate{Gate::Unbooted};
    // 键 → 启用；仅工作线程读写（GUI 设置变更经 queued 调用），无需加锁
    QHash<QString, bool> m_enableTable;
    // 会话态（仅工作线程）
    bool m_sessionActive = false;
    QString m_sessionToken;
    QString m_sessionIdentity;
    QUuid m_sessionListUuid; // 本段来源列表，会话期内不变（换列表即换段，由折叠重开）
    // 状态快照（互斥锁保护：四条推送与采样在驱动器线程写，拉取出口任意线程读）
    mutable std::mutex m_stateMutex;
    QString m_stateIdentity;
    QUuid m_stateListUuid; // 与身份同族：折叠与采样两处同点写
    QVariantMap m_stateMetadata;
    double m_statePosition = 0;
    double m_stateDuration = 0;
    bool m_statePaused = false;
    bool m_statePlaying = false;
};

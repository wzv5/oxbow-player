#pragma once

#include "pch.h"

#include <mpv/client.h>

// N42：滤镜列表条目模型（定义见 FilterListWidget.h，随控件收口）
struct FilterEntry;

// 对 mpv client API 的 Qt 化封装：UI 只与本类交互，不直接触碰 mpv_handle。
// mpv 客户端 API 本身线程安全（client.h "Multithreading"），唯一例外是
// mpv_wait_event 仅单线程可调（本类由 EventThread 专职调用，B17）。
// 信号允许跨线程发射（自动排队连接，槽在 GUI 线程执行）。
class MpvController : public QObject {
    Q_OBJECT
  public:
    explicit MpvController(mpv_handle *mpv, QObject *parent = nullptr);

    // 发送命令：单行命令字符串，或参数数组（推荐用于含路径的命令，避免转义问题）
    // N60 §2.1 硬约束：**播放列表内容的喂入侧必须保持同步 API**。本类与
    // setProperty / loadFile 一样走 mpv_command*，命令返回即已在 core 执行完毕
    // ——推送会话的屏障读正是据这一点成立（发读请求时本批已是终态）。若把任一
    // 喂入点改走 *_async，该不变式即失效，须改成"等本批 reply 全到齐再发屏障读"
    void command(const QString &cmd);
    void command(const QStringList &args);

    // N87：异步命令通道，供按键回落 mpv 的 keypress 注入。同步的 command() 会占住调用线程
    // 直到 core 执行完，而注入进去的是用户自己绑的任意命令（截图落盘 / loadfile / Lua 回调
    // 都可能被一个键拉起来），GUI 线程不等它。reply_userdata 恒 0、回执一律不读
    void commandAsync(const QStringList &args);

    // N92：同步命令并取回返回串（expand-path / normalize-path 等仅 client API 可用的
    // 纯计算命令）。经 mpv_command_node 下发（参数一律包成字符串节点），result 出参
    // 只认 MPV_FORMAT_STRING；命令失败或返回值非字符串交空串，节点内存取完即还
    // （失败路径不触碰 result，无须释放）。同步语义是必须的：取值就是为了定位文件，
    // 本就得在下发前拿到。expand-path 一族无 I/O、不触碰视频/音频链路，GUI 线程直调
    // 不会挂住；与 propertyString 同口径（不改本对象状态，纯读，故 const）
    QString commandResultString(const QStringList &args) const;

    // N78：screenshot-raw 取内存原始图像——命令不带参数，flags 省略即默认
    // subtitles（原始视频分辨率、带字幕），format 省略即默认 bgr0（小端下与
    // QImage::Format_RGB32 同布局）。返回深拷贝出的独立 QImage（mpv 侧数据随
    // result node 释放而失效）；失败（含空闲态无视频）返回空 QImage
    QImage screenshotRaw();

    // N52 §2.10：loadfile 唯一出口。N65：逐文件选项由调用方**显式**传入（不给
    // 缺省值——缺省会藏起"这条链路某天忘了带选项"），本函数退成纯管道：不判
    // scheme、不补选项、不校验键名、不改值（判据只在提供者的解析里，下游一律
    // 无脑接受、原样推给 mpv）。合并后的 map 为空走既有三参数字符串通道，本地项
    // 命令与原行为逐字相同；非空走节点通道。转接项的 cache=yes 申报已随串在
    // 铸造点（PlaylistProvider::playTarget）交出，"转接项必带 cache"的显影由
    // Playlist::playTargets 的 Debug 断言守候，本函数不再认识 StreamTransit。
    // appendMode 由调用方原样传入 append / append-play。
    // N60 §2.1 硬约束：同步语义（经 command / mpv_command_node），不得改异步——
    // 它是最主要的列表喂入点
    void loadFile(const QString &url, const QString &appendMode, const QVariantMap &options);

    // N60 §2.2：推送会话的屏障读——异步读 playlist，reply 自带发出时刻的队列
    // 终值，gen 作 reply_userdata 与终点对账配对。返回 false 表示发射失败
    // （不会产生 reply），调用方据此就地摘除世代
    bool pushBarrier(quint64 gen);

    // N42：String 型 set 改判返回值——下发失败（顶层语法错误 / 未知滤镜名等，
    // mpv 保持原值不清理）由调用方决定不写 conf；既有调用点忽略返回值，零波及。
    // 维护者裁决：四个重载返回值统一（bool = 是否写入成功）
    bool setProperty(const QString &name, const QString &value);
    bool setProperty(const QString &name, int value);
    bool setProperty(const QString &name, double value);
    bool setProperty(const QString &name, bool value);
    // 实时读属性一族：纯读、不改本对象状态，故 const（mpv_get_property 线程安全，
    // 除 wait_event 外任意线程可调，与下方 filterEntries 同一口径）
    QString propertyString(const QString &name) const;
    // 实时读属性（N3）：失败返 fallback；pause 等标志属性必须走 FLAG 格式
    double propertyDouble(const QString &name, double fallback) const;
    bool propertyFlag(const QString &name, bool fallback) const;
    // 实时读整型属性（R4 类型化回填）：失败返 fallback
    qint64 propertyInt64(const QString &name, qint64 fallback) const;
    // N61 §2.3：**「在播哪一条」的唯一出口**——值按 playlist-pos → playlist/<pos>/filename
    // 两步现读，再经回显整形（Util::Url::identityFromMpvEcho）；无正播返回空串（取值形状
    // 与今日 propertyString("path") 一致，迁移点不必改判空逻辑）。命名与 path 对齐是
    // 刻意的：它就是 mpv 的 path 属性在本仓的替代品。
    // 硬约束一：**插件内凡想知道在播哪条，一律走本出口，不得再裸读 path 属性**——实测
    // （mpv 0.41）path 的绝对化基准是 mpv 的 working-directory，而该值取环境变量 PWD、
    // **不等于进程真实 CWD**：从 git-bash 启动时它能拼出一个并不存在的路径，而文件其实
    // 是从真实 CWD 打开的。filename 才是"队列里那条"，与实体身份、与 playTargets 的下发
    // 形态同源（path 是 open 阶段 mangle 的展示值，还会改写分隔符）
    // 硬约束二：**本出口内不得整表现读 playlist，也不得以 playlist/<N>/current 为判据**
    // ——本出口的一个读者挂在 time-pos 心跳上且无节流（实测约 99 Hz）：2000 项表整表
    // NODE 读约 2.6ms/次（≈ 26% 单核常驻），两步 O(1) 读约 3µs/次；而 current 对非
    // 当前项报的是 property unavailable 而非 false，拿来判"是不是当前条"必假。当前条
    // 恒由 playlist-pos 指认（实测二者恒等）
    // 不缓存、不加锁：现读已够便宜，缓存反而要处理事件线程与 GUI 线程的裸 QString 竞态
    QString currentPlayingPath() const;
    // N42：滤镜链结构化读（af / vf object settings list）。MPV_FORMAT_NODE 读出
    // 链表逐项映射为条目（enabled 直取，text 由 label + name + params 序列化，
    // 规则见 FilterListWidget::quoteParamValue）；不可读 / 失败返回空列表。GUI 线程直读
    // （与 propertyString 同一线程口径，client API 除 wait_event 外线程安全）
    QList<FilterEntry> filterEntries(const QString &name);

    // N89：input-bindings 条目（字段语义见规格附录 A；owner / comment 可缺）
    struct InputBinding {
        QString key;         // 归一化键名（多键序列以 '-' 连接）
        QString cmd;         // 命令串（与源一致的原文）
        bool isWeak = false; // 真 = 内置（is_builtin）
        QString owner;       // 非空 = 脚本添加（取自区）
        QString section;     // 所属输入区
        QString comment;     // 出厂描述（可缺）
    };
    // N89：input-bindings 的一次性结构化读（MPV_FORMAT_NODE，形态照 filterEntries
    // 先例；手册明写该属性不支持变更通知，故只在对话框打开时读一次，不观察）。
    // 两档筛选在此收口：内置集 = is_weak 且 section == "default" 且无 owner；
    // 脚本集 = owner 非空者。其余条目（脚本自建区 / 用户具名区里的 weak 条目等）
    // 不进两档。读失败交空档。
    struct InputBindingTiers {
        QList<InputBinding> builtins;
        QList<InputBinding> scripts;
    };
    InputBindingTiers inputBindings() const;

    // N92（含变更记录 1）：mpv 配置路径的两个取值出口——一切位置问 mpv 自己，插件不再
    // 从任何目录自行推导。**取值以目录为单位**：expand-path ~~home/ 交配置目录，两个文件名
    // 由该目录词法拼接（两文件同处一目录，只问一次）。校验顺序即安全顺序，任一不过交空串，
    // 调用方据空串报错，绝不猜目录、绝不 mkpath、绝不降级：
    //   目录非空——~~home/ 为空 ⇔ mpv 确实没有配置目录（--no-config，或显式 --config-dir=）；
    //     判空必须落在目录上而非文件路径上：--no-config 下 expand-path ~~/input.conf 交回的是
    //     非空的裸串 "input.conf"，对它判空等于没判；
    //   能绝对化——相对 --config-dir 是合法用法（实测 mpv 按进程 cwd 正常加载），故不拒绝
    //     相对串，改交 normalize-path 按同一基准词法展开（它不解符号链接，与 Qt 的
    //     canonicalFilePath 不是一类东西）；展开后仍非绝对只作断言级兜底；
    //   目录存在——mpv 不校验目录存在性，QSaveFile 又不建目录，留到写盘炸成一坨系统错误
    //     不如在这里说清。只查不创建，"写到别处等于配置无效"（T10）不变
    QString mpvConfPath() const;
    // N89：扩展 --input-conf 判读（判读四步表见规格 §6 与实现处注释）——~~home/
    // 链的取值语义与校验顺序不变；--no-config 下仍交空串
    QString inputConfPath() const;

    // N13：轨道信息（track-list 解析产物）
    struct TrackInfo {
        qint64 id = 0;
        QString type; // video / audio / sub
        QString title;
        QString lang;
        QString codec;
        bool external = false;
        QString externalFilename;
        bool isDefault = false;
        bool selected = false;
        int demuxW = 0;
        int demuxH = 0;
        double demuxFps = 0.0;
        int demuxSamplerate = 0;
        int demuxChannelCount = 0;
        qint64 demuxBitrate = 0; // bps，0 表无值
        qint64 metadataBPS = 0;  // bps，0 表无值
    };

    // N15：章节信息（chapter-list 解析产物）
    struct ChapterInfo {
        double time = 0.0;
        QString title;
    };

  signals:
    void timePosChanged(double seconds);
    // B18：首参为归属键——START_FILE 时缓存、FILE_LOADED/observe 推送当刻
    // 的当前文件路径（快照），接收方不得再向控制器现读
    void durationChanged(const QString &path, double seconds);
    void pauseChanged(bool paused);
    void volumeChanged(double volume);
    // N38：静音开关（mute 属性，FLAG）
    void muteChanged(bool muted);
    // R3：音量上限（mpv volume-max 属性）推送，MainWindow 接到后同步滑条量程
    void volumeMaxChanged(double newMax);
    // items 为 QVariantMap 列表，键：filename / title / current
    void playlistChanged(const QVariantList &items);
    // N60 §2.2：屏障读的终点回显——items 与 playlistChanged 同形（同一条解析
    // 出口），ok 为假（读失败 / 载荷非数组 / 属性不可读）时不对账。gen 是发出
    // 屏障读时登记的世代号，与 playlistChanged 投同一接收者故保序
    void pushBarrierResult(quint64 gen, bool ok, const QVariantList &items);
    void playlistPosChanged(int pos);
    // mpv 进入/离开空闲状态（列表播完或停止后无当前文件）
    void idleActive(bool active);
    // B18：文件卸载事件，reason 为 mpv_event_end_file::reason 原值透传，
    // 语义判定由消费者负责——B9 续播清除仅认 MPV_END_FILE_REASON_EOF；
    // 会话态重置对所有 reason 生效（见 MainWindow）
    void endFile(int reason);
    // 导航器面板（N1）：video-zoom / video-pan-x / video-pan-y / 视频尺寸
    void videoZoomChanged(double zoom);
    void videoPanXChanged(double panX);
    void videoPanYChanged(double panY);
    void videoSizeChanged(int width, int height); // 0,0 表示无视频
    // B18：以下 4 项首参为归属键（同 durationChanged），由 FILE_LOADED
    // 主动读发出，不再 observe（等值不重推 / 推送滞后均随之消除）
    // 媒体元数据字典（N4）：键大小写因文件而异，接收方需做不敏感匹配
    void mediaMetadataChanged(const QString &path, const QVariantMap &metadata);
    // N14：文件格式（file-format，文件属性单设）
    void fileFormatChanged(const QString &path, const QString &format);
    // N13：轨道列表（track-list 全量推送）。N93：双通道——FILE_LOADED 主动读给装载
    // 时刻的首值，观察推送补运行期增删与外部切换；path 是推送当刻的当前文件，
    // 不承诺与载荷同代（observe 与 START_FILE 的先后无保证）
    void tracksChanged(const QString &path, const QList<TrackInfo> &tracks);
    // N15：章节列表
    void chaptersChanged(const QString &path, const QList<ChapterInfo> &chapters);
    // N87：mpv 内建 console 脚本的开关状态（观察 user-data/mpv/console/open）。为真期间
    // 主窗让出全部键盘。该属性在 console 首次写入前不存在，观察注册成功但不推初值，故
    // 初值判据是"未收到推送即 false"，装配期不得读它
    void consoleOpenChanged(bool open);
    // N88：mpv fullscreen 属性的镜像推送（FLAG，注册即推初值）。初值（含 mpv.conf
    // 写 fullscreen=yes 的启动即全屏）与外部写入（终端 / IPC / 默认键 f）同走本信号，
    // MainWindow 按 Qt 侧窗口真相比对分派——相等不动（回灌守卫），不符即驱动切换或回正
    void fullscreenChanged(bool fullscreen);
    void mpvShutdown();

  public:
    // 当前视频尺寸（N40）：经统一取值路径当场读取——先 video-out-params/w、/h
    // （滤镜后输出尺寸，F1），不可读回退 width、height（F3②：可用时机互补），
    // 仍不可读或读得 0 返回 QSize(0,0) 表示无视频。纯读，不写去重状态
    // （本函数会在 GUI 线程被导航器构造期调用）
    QSize videoSize() const;

    // B17：启动事件线程（阻塞 mpv_wait_event(-1)）。必须在 wid 等嵌入相关
    // 属性设置完成后调用：首次 mpv_wait_event 会解除 mpv 核心的启动门控
    // （fuzzy_initialized），核心随即为命令行文件初始化 vo——若此时 wid
    // 尚未生效，mpv 会先自建窗口再重建嵌入，产生窗口闪烁
    void startEventThread();

  private:
    class EventThread; // 事件专用线程（定义见 .cpp）

    void handlePropertyChange(const mpv_event *event);
    // N60 §2.2：屏障读的 reply 分派（事件线程就地解析并释放 node 后发射）
    void handleBarrierReply(const mpv_event *event);
    // B18：生命周期事件分派（均在事件线程执行）
    void handleStartFile();  // START_FILE：刷新当前文件缓存
    void handleFileLoaded(); // FILE_LOADED：以缓存为归属键主动读 5 项属性并发射
    // N40：视频尺寸取值与发布（均在事件线程执行；videoSize() 复用同一取值路径）
    QSize readVideoSize() const; // 取值收口：out 优先、width/height 回退、皆不可读判无视频
    void publishVideoSize();     // 去重收口：与上次已发布值比对，不同才写入并 emit

    // N92：配置目录取值的唯一收口，两个文件路径出口共用（校验与安全顺序见其声明处）
    QString configDir() const;

    mpv_handle *m_mpv; // 由 mpv 拥有，插件不销毁
    // N40：上次已发布的视频尺寸——去重收口的比对基准（不再是从前"width/height
    // 两属性合并缓存"）；仅事件线程读写（去重收口与 idle-active 兜底），
    // videoSize() 纯读不经过它
    int m_videoW = 0, m_videoH = 0;
    // B18：当前文件路径缓存——START_FILE 时写入，FILE_LOADED / duration
    // observe 推送读取作为归属键；仅事件线程读写，无需加锁
    QString m_currentFile;
    EventThread *m_eventThread = nullptr; // 随进程存活，不经 delete（与宿主同生命周期）
};

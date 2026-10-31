#pragma once

#include "pch.h"

#include "PlaylistFile.h"     // N80：导出条目结构 Entry 与清单写出；本类只留菜单与编排
#include "PlaylistListView.h" // N57 §2.2：视图态的家，本类的编排一律经它读写
#include "PlaylistRegistry.h" // N57 §2.5：播放目标初值取保留身份 kDefaultListUuid
#include "entities/PlaylistEntity.h"
#include "providers/PlaylistProvider.h"
#include "ui/LoadingOverlay.h"

class MpvController;
class Playlist;
class ServiceRegistry; // N35：服务注册表（成员以 shared_ptr 持有）
class StreamRelay;     // N52：转接协议适配层（plugin.cpp 创建并 install，经 MainWindow 注入注册表）

// 右侧播放列表：每个 tab 对应一个播放列表，tab 标题为列表名。
// 约定：第一个 tab 名为"默认"，不可删除；其余 tab 持久化保存到 INI 配置文件。
// 只有一个 tab 是"正在播放列表"（与 mpv 播放列表同步），初始为第一个 tab；
// 双击任意 tab 中的条目会把该 tab 整个载入 mpv 并播放对应条目，同时该 tab 成为新的同步目标。
//
// N26 起条目为播放项实体：行 EntityRole 存实体指针（占位行为空）。
// N45 层级模型：实体集合归属层缓存（键 = enumerate 的 parent 形态，即 id 序列；
// 层在缓存中存在即"已加载"，不设并行标志），显示层的实体仓经视图的 entities() /
// levelOf() 取得；浏览路径（含标题列）与队列根分离——双击容器钻取、返回与
// "定位到正在播放"均只切换显示层；队列根随同步目标转移重锚，持久化（id/标题两列
// 数组键），重启后浏览路径直接取队列根；切曲归位把浏览路径拉回队列根层。
// 行序 = [上一级行][容器行…][实体行…]：凡 mpv 侧语义一律在实体序上工作，
// 行序只出现在渲染与鼠标事件里（§2.4.2）。
// N57 §2.2/§2.8：**这些视图态的家是 PlaylistListView**（一个 tab 一份，对象即状态），
// 本类只留编排（行构建与换入、四步式、异步推进、遮罩与工具条）与面板级共享状态；
// tab ⇔ PlaylistListView ⇔ Playlist 一一对应（不变式，断言在视图与列表构造处守候）。
// 文件级 / 媒体元数据全部归属实体（惰性求值 + 实体内缓存）。
// 视图仅持有全局时长仓 m_duration（mpv 推送，键为规范化路径）。
// 排序与清理对播放中 tab 统一走四步式（N30 收敛为 runFourStep 唯一入口）：
// 停止 → 本地变更 → playRequested 整表推送并从续播记忆恢复（暂停态自动保持）。
// N30 刷新语义：静态与动态同路（回推后重枚举），行级换入不重建行，
// 排序后比对旧序列——一致静默、末尾追加、不一致走四步式。
// N32 异步语义：枚举与元数据预读统一到 QtConcurrent（QPromise + QFutureWatcher），
// 全插件同一时刻至多一个在途任务——枚举与预读共用一份任务槽、一个推进函数
// （advanceTab）、一处"腾槽 → 续作 → 条件复位忙时"的唯一出口；任务只捕获入参副本，
// 不持有宿主对象，故析构无需等待。N45：任务目标由 list 一维变为 (list, parent) 二维
// （JobContext.parent），任务体与续作一律按它读写层缓存。
class PlaylistWidget : public QTabWidget {
    Q_OBJECT
  public:
    // 条目数据角色约定
    static constexpr int EntityRole = Qt::UserRole;          // 实体指针（quintptr，占位行为空）
    static constexpr int PlayingRole = Qt::UserRole + 1;     // 正在播放标记（bool）
    static constexpr int PlaceholderRole = Qt::UserRole + 2; // N19 枚举失败占位行标记（bool）
    // N45 §2.4：容器行与"上一级"行标记（两类行由 buildSpecialRows 合成）
    static constexpr int ContainerRole = Qt::UserRole + 3;  // 容器行（存容器 id，QString）
    static constexpr int ParentLinkRole = Qt::UserRole + 4; // "上一级"行（bool）
    static constexpr int ErrorRole = Qt::UserRole + 5;      // N104 播放失败标记（bool）

    // N56 §2.7：registry 由装配顶点（MainWindow）创建并注入，本类不再自造——
    // 造实体的层不止视图一个，装配顶点只留一处；每个造实体点都因此确保 registry
    // 非空（http(s) + 空 registry 会静默降级 UnknownEntity，形态悄悄变错）。
    // relay 的 provide<StreamRelayIf> 同样上移到 MainWindow（早于本构造即可，
    // 硬顺序②由装配顶点保证）。构造内不再做任何 provide——HttpClient /
    // TraversalPolicy 的注册与更新同样在装配顶点完成
    // N57 §2.3：lists 同为装配顶点注入——注册表是列表模型的唯一所有者，本类只持
    // tab 到视图的对应关系（即 tab 栏本身）；默认 tab 取 registry->defaultList() 建
    // N147：配置路径不注入，ini 读写一律取 App::paths().iniFile()
    explicit PlaylistWidget(std::shared_ptr<ServiceRegistry> registry,
                            std::shared_ptr<PlaylistRegistry> lists, QWidget *parent = nullptr);
    ~PlaylistWidget() override; // N32：非阻塞收尾——作废并取消在途任务后立即返回（不再等待）

    // 注入控制器（只借不拥有，寿命须晚于本表——裸指针不随控制器销毁自动归零）：
    // N30 起四步式在停止前读 pause、归位读 path / idle-active
    void attachController(MpvController *mpv) { m_mpv = mpv; }

    // N60：mpv 播放列表回刷。吞域（推送会话在册）→ 空列表忽略 → 与"正在播放"tab
    // 逐行一致即静默（仅翻转播放标记）→ 占位/枚举失败态忽略 → 差异则整体收编到默认
    // tab 并转移同步目标。回刷不再重建正在播放的 tab，也不驱逐任何元数据缓存
    void setPlaylist(const QVariantList &items);
    // 向当前 tab 添加文件（触发 filesAdded 信号，携建好的实体表）
    void addFiles(const QStringList &files);

    // N11：主菜单"打开文件"专用——整体替换默认 tab 内容并作废其续播记忆；
    // 默认 tab 永不持久化，也不触发 filesAdded（播放由 MainWindow 统一入口显式驱动）。
    // N56 §2.7：回传它造好的实体表，调用方据此驱动统一播放入口（不再经字符串表绕行）
    PlaylistEntity::List setDefaultTabFiles(const QStringList &files);

    // ——N45 §2.8：filesOf 拆双出口的实体版。凡 mpv 侧语义（上一/下一、循环判定、
    // 回刷比对、收编索引、正播名等）一律读队列根层出口；唯一当前层语义是双击入队。
    // N56 §2.7：字符串表出口退到"喂 mpv 前最后一刻"——凡 mpv 侧语义一律传实体表，
    // 下发形态（playTargets）在 MainWindow 的统一入口现场映射
    PlaylistEntity::List queueEntitiesOf(int tabIndex) const; // 队列根层（mpv 队列的内容基准）
    PlaylistEntity::List currentLayerEntitiesOf(int tabIndex) const; // 当前显示层（入队单位）
    // N48/R：喂 mpv 的播放形态——逐实体经该 tab 列表的提供者 playTarget 映射
    // （视图中不含具体提供者头文件，凭据访问只走基类接口）。N57 §2.2：整表映射归
    // `Playlist::playTargets`（逐实体传它自身身份），本出口是薄适配；提供者恒非空
    // 后不再有"无提供者按恒等兜底"那一支，越界（无表可寻）返回空集。
    // 只用于下发 mpv 的 loadfile 循环；收编索引与回刷判据和它同源同形（N56 §2.5，
    // N60：在途快照已随推送会话删除，不再有第三处同形者）。N65：交 PlayTarget
    // 整体（串 + 逐文件选项），Playlist::playTargets 是唯一合并点
    QVector<PlayTarget> playTargets(int tabIndex, const PlaylistEntity::List &entities) const;
    // ——N104 播放失败显影：三条定位口（MainWindow 只做 connect，标 / 清收口在本类）——
    // 定位域恒为"正在播放 tab 此刻显示的实体行"：载体是行，行不在视图上就无从显影。
    // 写前判旧值不等——等值 setData 照样发 dataChanged，会白刷一次重绘
    // 按串定位（end-file 失败标红与 fileLoaded 成功清除共用）：串先过
    // StreamTransit::unwrap（幂等，非转接形态原样通过）折返条目身份，再问行实体
    // matches()（键级宽松判等）；同串只落命中首行——同名重复行不联动，宁漏标不错标
    void markPlayFailed(const QString &path);
    void clearPlayFailed(const QString &path);
    // 按指针定位（命令层被拒通道）：推送循环手里握着实体与 tab，比对行 EntityRole
    // 的裸指针即可、不经任何字符串；行随层亡，不存在悬垂比较。null 实体跳过
    void markPlayRejected(int tabIndex, const PlaylistEntity *entity);
    // N56 §2.7：正播实体——某 tab 队列根层第 entityIndex 项（mpv playlist-pos 语义）。
    // MainWindow 的"以当前内容为对象"取串点（书签 / 聚合器 / duration / OSD 取名）
    // 一律经此取实体，不再读 mpv 属性；越界 / 层不在缓存 / 空位返回空
    PlaylistEntity::Ptr playingEntityOf(int tabIndex, int entityIndex) const;

    // ——N54：书签按列表身份回跳（机制侧：切列表 + 换层 + 定位；播放决策与报错归 MainWindow）——
    // 某 tab 的列表对象（N57 §2.6 新增公开出口）：供 MainWindow 取身份与名字，
    // **不得经它直写**（改名换源一律走本类的显式方法）；越界返回空指针
    const Playlist *listOf(int tabIndex) const;
    // 某 tab 的队列根层 id 序列（与私有 queueRootIdsOf(list) 同语义，仅换键类型）
    QStringList queueRootIdsOf(int tabIndex) const;
    // 按列表身份找 tab（视图坐标只在这里出生）：判等全在代码侧（QUuid，N57 §2.4
    // 删去两侧 toLower 归一），不跳过 tab 0——默认列表带恒定保留身份，命中即 0。
    // 无命中 -1
    int tabIndexOfList(const QUuid &uuid) const;
    // N57 §2.6：原 `listNameOfUuid` 退场——名字直查注册表 `PlaylistRegistry::nameOf`
    // （身份→列表一次直查，不经视图）
    // 一次层级定位请求的全部输入。levelIds / itemKey 供机制使用，time 是纯过路值：
    // 挂起态活在本 widget 跨枚举存活，回传它比让调用方另造一份挂起簿记少一处状态。
    // N57 §2.8：目标改持**列表身份**——它是本篇唯一真正跨忙时、跨异步枚举存活的目标
    // （存进 m_pendingJump 事后消费），tab 序号在这期间会随增删漂移，故不可充当目标；
    // 坐标只在需要动作的那一刻由视图换算（`jumpTarget` / 信号发射时现场取）
    struct LevelJump {
        QUuid listUuid;       // 目标列表身份（nil 恒换算不到 ⇒ ListGone）
        QStringList levelIds; // enumerate 的 parent 形态（空序列 = 根层）
        QString itemKey;      // 条目归一键（Util::Url::key 产出，两侧同形态逐字节比）
        double time = -1.0;
    };
    // 把目标层换入前台并在其中定位 itemKey：层已加载即同步回报，未加载走异步枚举后回报。
    // 忙时只登记不动作（不碰浏览路径、不切 tab、不起任务），由 busy 出口消费
    void requestLevelItemJump(const LevelJump &req);
    // 定位失败的三类原因（N54 §2.2 第 4 步按此分流产文案，不分流行为）
    enum class JumpFailure {
        ListGone,        // 目标身份在视图里换算不到坐标（含请求发起后 tab 被删）
        LevelLoadFailed, // 目标层枚举失败（本轮已按 §2.4 显式重试过一次）
        ItemNotFound,    // 目标层已加载，但其中没有这个条目
    };
    Q_ENUM(JumpFailure)
    // 续播记忆在队列根层的实体序（-1 无）：序号校验优先、失败按路径取首命中。
    // mpv 侧语义（四步式重推、停止态步进起点）一律取本值，不得用行序充数（§2.4.2）
    int resumeEntityIndexOf(int tabIndex) const;
    // 当前 tab 选中行的实体序（非实体行返回 -1）——停止态起播的选中行回退用
    int currentSelectedEntityIndex() const;
    // N45 §2.9：归位——把显示层切回队列根层并滚动到正播项（不动 mpv 队列）。
    // 手动"定位到正在播放"与切曲归位（MainWindow 于 playlist-pos 实际换条目时
    // 调用）共用，恒作用播放 tab；忙时延后（置 m_pendingReveal，busy 结束补执行
    // ——在途期浏览路径不得被搬移，与 requestTab 同属忙时延后家族）
    void revealPlayingItem();
    // N57 §2.8：播放目标改持身份（m_playingUuid），本出口是**现场换算**——扫描个位数
    // 条目。它恒可解析的前提正是 §2.5 给了默认列表一个恒定且不可摘除的身份：身份↔坐标
    // 永不失配，故无需再有任何"索引漂移修正"。无命中 -1 理论上不可达，调用方按 >= 0 用
    int playingTabIndex() const;
    bool currentIsPlayingTab() const { return currentIndex() == playingTabIndex(); }

    // 当前 tab 中选中的行号，无选中项返回 -1
    int currentSelectedRow() const;
    // 转移“正在播放”同步目标到指定 tab（双击条目或会话恢复载入时调用）。
    // B10：异 tab 转移时清空原 tab 的播放标记，防残留；同 tab 早退防闪烁
    void markPlayingTab(int tabIndex);
    // N59 §3.1：最后一次**主动设定**的播放目标身份——会话键的唯一落点。与 m_playingUuid
    // 的分工：一个是"此刻的同步目标"，一个是"最后一次主动设定的目标"。只在 markPlayingTab
    // 写，被动归位（删除当前列表 / 换源 / N53 的失败处理）不擦除它，故刷新失败后重启仍回到
    // 那张表；返回默认列表的保留身份即"本进程没在非默认列表播过（或之后又回默认播过）"
    QUuid lastPlayingTargetUuid() const { return m_lastPlayingTargetUuid; }

    // 记录某 tab 的最后播放项与进度（仅非默认 tab 生效；纯内存，落盘发生在保存时）。
    // N9：记忆条目的路径随写入点自动采集（路径+序号双存，恢复时序号校验优先）
    void recordPlayback(int tabIndex, int pos, double seconds);
    // B9：清除某 tab 的续播进度记忆（保留最后播放项）；整表自然播完后调用
    void clearResumeProgress(int tabIndex);
    // N9：续播行统一解析——序号处路径与记忆一致则用之；否则按路径取首命中；无命中 -1
    int resolveResumeRow(int tabIndex) const;
    // 某 tab 记忆的播放进度，无记录返回 -1.0
    double lastTimePosOf(int tabIndex) const;
    // N57 §2.6：原 `indexOfName` 退场——按名反查全仓唯一调用点（会话恢复）已改按
    // 身份直查（`PlaylistRegistry::find` + `tabIndexOfList` 换算坐标）
    // N19：程序化切 tab 统一入口——MainWindow 侧全部经此调用。忙时（加载在途/
    // 预读中）且目标非当前 tab 则排队（last-wins），空闲后补执行；其余直调
    void requestTab(int index);
    bool isBusy() const { return m_busy; }

    // N60 §2.2 / §2.3 第 3 条：推送会话——本地一批喂 mpv 列表内容的命令的起止范围。
    // 会话期内（开批到屏障 reply 落地）的一切 playlist 回显无条件忽略（吞域），
    // 终点由屏障自带的队列终值对账。**唯一的开闭入口是下面这个 RAII 守卫**
    class PushSession {
      public:
        explicit PushSession(PlaylistWidget *widget)
            : m_widget(widget), m_gen(widget->beginLocalPush()) {}
        ~PushSession() { m_widget->endLocalPush(m_gen); }
        Q_DISABLE_COPY_MOVE(PushSession) // 拷贝会二次摘除世代并重复发屏障读

      private:
        PlaylistWidget *m_widget;
        quint64 m_gen = 0;
    };

    // N60 §2.2：屏障 reply 的终点对账（与 setPlaylist 同一条比对管线，槽内同步完成）。
    // 接收方须与 playlistChanged 同一对象——保序前提是两者投进同一 GUI 队列
    void onPushBarrier(quint64 gen, bool ok, const QVariantList &items);

    // ——N9 提供者——
    // 表头菜单"新建播放列表"：弹列表设置对话框后建 tab；内容一律经首次激活的
    // 异步枚举填充（N28：所有源同路，静态源不再同步填充）
    void createNewList();
    // 表头菜单"列表设置…"：存量列表改名/换提供者。非法参数报错留在对话框；
    // 合法后静默清空重建；仅改名则直接生效不清空。播放中 tab 换提供者时停止，
    // 同步目标归位默认 tab（N25：空中列表推送由回刷忽略，不再需要悬空态）
    void openListSettings(int tabIndex);
    // 删除指定 tab（表头菜单，作用于右键命中的 tab）
    void removeTabAt(int tabIndex);

    // 持久化：从配置文件恢复非默认播放列表 / 保存除默认外的所有列表。
    // N57 §2.7：两者都只剩"逐节调对偶函数"——一组的读写各收在一个函数里（见私有段）。
    // 节序权威源是注册表的有序集，tab 序是它的镜像；默认列表结构上不在其中
    void loadPlaylists();
    void savePlaylists();

    // ——元数据回填（N18：仅保留 duration 写入）——
    // N56 §2.7：键由调用方取正播实体的 key() 传入（不再吃 mpv 路径现归一）
    void applyDuration(const QString &key, double seconds);

  protected:
    // N5：ShortcutOverride 认领导航键——焦点在列表上时 ↑↓←→/PgUp/PgDn/Home/End
    // 走列表原生导航，全局音量动作（↑/↓）本次不触发。
    // 该事件定向投递焦点控件、不沿父链冒泡，故过滤器必须装在每个列表自身
    bool eventFilter(QObject *watched, QEvent *event) override;

  signals:
    // 请求整表载入并播放 entities 中第 index 个；seekTo > 0 时就绪后恢复到该进度
    // （N4 四步式的进度恢复与此同一通道；发出前已将 tab 标记为正在播放列表）。
    // N56 §2.7：播放链路改传实体表——MainWindow 据此经 playTargets(tab, entities)
    // 在喂 mpv 前最后一刻映射播放形态（凭据注入在提供者侧）。
    // N30：keepPaused 为真时载入后保持暂停（四步式在 stop 前读到的 pause 状态）。
    // N62 §2.7 C 档：入参 tab 是**发射时刻由身份现场换算的视图坐标**（与 levelItemReady
    // 同一句钉法），接收方不得跨事件持有
    void playRequested(const PlaylistEntity::List &entities, int tab, int index, double seekTo,
                       bool keepPaused = false);
    // 当前 tab 收到新文件（菜单添加或拖放），携建好的实体表
    void filesAdded(const PlaylistEntity::List &entities);
    // "正在播放"tab 请求移除指定行 / 清空（需同步到 mpv 播放列表，由 MainWindow 执行；
    // 本地 tab 的删除直接在界面完成，不发信号）
    void removeRequested(const QList<int> &rows);
    void clearRequested();
    // 四步式第一步（N4）：排序/清理播放中 tab 前，由 MainWindow 下发 stop
    void stopRequested();
    // N19+A17：内容条目数已变（异步加载收尾/本地增删清/静态重建），MainWindow 刷新导航启停；
    // 播放中 tab 的变更另有 mpv 事件兜底，此信号幂等重刷无害
    void contentsChanged();
    // N25：回刷差异收编（外部变更覆盖默认 tab 并转移同步目标）后发射。
    // 收编不引发 playlist-pos 推送（值不变不推），MainWindow 须主动刷新标题/OSD
    // 的正播名（pos 推送时刻按旧镜像取名已错）
    void playingListAdopted();
    // N54：层级定位的两种结局（恒在 GUI 线程直连，不跨线程投递）。entityIndex 是目标层
    // 的**实体序**（即 mpv playlist-pos 语义，非行序，N45 §2.4.2）；成功回报蕴含一条
    // 不变式：目标列表的当前显示层 == req.levelIds（换入已完成），故调用方可直接取
    // currentLayerEntitiesOf(tabIndex) 当新队列。tabIndex 是**发射时刻由身份现场换算的
    // 视图坐标**（N57 §1.5：坐标只出现在边界与信号里，不出现在待办状态里）。失败发
    // levelItemFailed——三类原因的消费方只用 req 与 why/detail，故坐标不带上（偏离见
    // 变更记录 3）。被作废（取消遮罩 / abandonJob）时两个信号都不发——取消即用户意图，
    // 静默收尾（N54 §2.3）
    void levelItemReady(const PlaylistWidget::LevelJump &req, int tabIndex, int entityIndex);
    void levelItemFailed(const PlaylistWidget::LevelJump &req, PlaylistWidget::JumpFailure why,
                         const QString &detail); // detail 仅 LevelLoadFailed 有值（提供者文案）

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override; // N19：遮罩几何跟随
    // N49：让位的三个补算点——基类在内部重排之后才回调虚函数，正是补算时机
    bool event(QEvent *event) override;   // StyleChange：样式改字体与边框，h 与 y 都变
    void tabInserted(int index) override; // 增删列表会改 tab 栏高度，进而改 stack 的 y
    void tabRemoved(int index) override;

  private:
    // N60 §2.3 第 3 条：会话的开与闭只由 PushSession 调用（故二者一律 private）——
    // 开批入集合、关批发屏障读，配对靠 RAII 机制化，世代泄漏在结构上不可能
    quint64 beginLocalPush();
    void endLocalPush(quint64 gen);

  private slots:
    void onItemDoubleClicked(QListWidgetItem *item);
    void onTabChanged(int index);

  private:
    // 行分派的唯一入口（双击与回车共用）：上一级行返回、容器行进入该节点、实体行整层
    // 入队。useResume 为真（回车一支）时，若该行正是本 tab 的续播记忆项，把记忆进度
    // 一并递上去；双击不带进度（N45 §2.4.3 的整层入队语义照原样）
    void activateItem(QListWidgetItem *item, bool useResume);
    // 回车一支的入口：取当前 tab 视图的选中行（焦点在列表或在 tab 条都走这里）
    void activateCurrentRow();
    // N57 §2.9：排序键 / 规则 / 元数据类别（`SortKey` / `SortRule` / `MetaCategory`
    // 与 `metaCategoryOf`）已搬到 `PlaylistEntity.h` 全局作用域——它们描述"按实体的
    // 哪个字段排"，属实体属性层面的概念；提供者持规则值，本类只执行排序。

    // N30：换入前的视图快照（换入后行号语义失效，按记忆行 key 复原）
    struct ViewSnapshot {
        QString memoryPath;
        bool hadMemory = false;
        bool selectFirst = false;
    };

    // ——N32 单任务槽：意图 / 判定 / 上下文 / 结果 / 槽位——
    // 意图枚举取代原 pend.refresh / pend.selectFirst 两个布尔：在发起处定死并按值随任务走
    enum class Intent { InitialLoad, SwitchSource, Refresh, Reactivate, Sort };
    // 唯一出口的判定。Discarded = 批次已作废（watcher 已非当前槽位项，什么都不碰）；
    // Cancelled = 用户取消；Ok = 正常。原 RescanResult 的 Ok/Cancelled/Stale 三态
    // 由"身份校验 + 二态"承载
    enum class JobVerdict { Discarded, Cancelled, Ok };
    // N45：一次任务的全部输入。oldKeys / ViewSnapshot 一律不在发起时采集——在途期 mpv 事件
    // 仍经 recordPlayback 推进本表记忆（无忙时守卫），发起时快照会丢掉这段进度；
    // 由续作在"用到它的那一步之前一刻"填入（换入类在换入前，不换入类在排序前）。
    // N45 §2.10：任务目标由 list 一维变为 (list, parent) 二维——同一张表在任务在途
    // 期间可以被切到别层，任务体与续作一律按 parent 读写层缓存；缺失即根层，
    // 与 enumerate 的参数形态一致
    struct JobContext {
        Intent intent = Intent::InitialLoad;
        QPointer<PlaylistListView> list;
        // N57 §2.8：原 `int tab` 字段删除——QPointer<PlaylistListView> 本身就是抗索引
        // 漂移的视图句柄，坐标一律 indexOf(view) 现场换算（旧代码四处赋值也已在这么
        // 干且无人读取，留字段只是把同一件事固化成会漂移的副本）
        QStringList parent; // 任务目标层（浏览路径的 id 序列形态）
        // N64：整树形态随批次声明（发起处 startEnumAsync 现查提供者置位，不存模式位）。
        // 收尾据此放宽目标层判定（目标层 = 收尾将落地的层，成功与失败同口径）并做
        // 整树权威重建；逐层批次恒为 false，行为一字不变
        bool wholeTree = false;
        SortRule rule;       // N57 §2.9：仅 Sort 用——判类别与写规则都读它，不读提供者
        QStringList oldKeys; // 换入类由枚举续作在换入前采集
        ViewSnapshot snap;   // 同上；Reactivate 由预读续作在排序前采集
    };
    // 预读任务的结果对象：槽位只存 QFuture<void>，异构结果由它携带（类型擦除的落点）。
    // N35 修订 2：取消改"立即收尾 + 结果静默丢弃"——mutex/abandoned 供遮罩取消
    // 的 GUI 续作与任务体同步：锁内置旗 + 搬走已算批次注入缓存（N32 缓存保留
    // 语义），任务体此后到界即弃。锁不跨求值调用，临界区仅容器 push / move
    struct MetaResult {
        MetaCategory cat = MetaCategory::None;
        PlaylistEntity::List entities;                    // 已算出的快照实体（下标对齐）
        std::vector<FileMeta> fileMeta;                   // cat == File 时有效
        std::vector<PlaylistEntity::MediaMeta> mediaMeta; // cat == Media 时有效
        std::mutex mutex;
        bool abandoned = false;
    };
    // 任务槽（全插件仅一份）：全插件同一时刻至多一个在途任务，故只需一份状态与一处出口
    struct JobSlot {
        quint64 token = 0;                 // 单调代际（并入原 m_loadGen）
        QPointer<PlaylistListView> target; // 目标表（仅 GUI 线程读写）
        bool cancelable = false;           // 枚举批次不可取消（遮罩在 Loading 模式亦无取消按钮）
        QFuture<void> future;              // 在途任务；正常出口与 abandonJob 均复位为默认构造
        QFutureWatcher<void> *watcher = nullptr;
        std::function<void()> onAbandon; // 取消时执行（仅可取消批次）：搬走已算批次注入缓存
        bool active() const { return watcher != nullptr; }
    };

    // N49 §2.5：搜索状态——上次执行的查询串、上次命中所在的列表与行号。
    // 三者合起来即"下一次回车能否接着往下走"的全部判据：任一不符即按新查询从头起搜
    struct SearchState {
        QString query;
        QPointer<PlaylistListView> anchorList; // 删 tab 后自动置空，不悬空
        int anchorRow = -1;                    // -1 = 无锚点（新查询从首行起搜）
    };

    // ——N45 §2.5 层级模型的类型（N57 §2.2：三个定义搬到 PlaylistListView 公开
    // 嵌套，此处按别名引用——状态住视图，本类的行构建与推进照旧读写它们）——
    using Level = PlaylistListView::Level;
    using PathSeg = PlaylistListView::PathSeg;
    using Memory = PlaylistListView::Memory;

    // 全仓唯一的列表页诞生点（N57 §2.2）：视图与所属列表在构造期一次绑死
    PlaylistListView *createView(Playlist *list);
    PlaylistListView *viewAt(int index) const;
    // 由页面控件指针取其所属列表（§2.2 不变式：tab ⇔ 视图 ⇔ 列表一一对应）
    Playlist *listOfView(const PlaylistListView *view) const;

    // ——N57 §2.7：一个 INI 组的读写对偶，编排点留在视图——
    // 同一个组要三段合写（名字与身份属列表、类型与参数属提供者、记忆与队列根属
    // 视图），而读侧必然要建视图与挂 tab ⇒ 顶点只能在视图，注册表不知道组内有什么。
    // 节名 `Playlist_%1` 的构造点在 writeTabGroup / readTabGroup / savePlaylists 的
    // stale 节清理三处，全在视图
    void writeTabGroup(QSettings &settings, Playlist *list, int index);
    void readTabGroup(QSettings &settings, int index);
    // N62 §2.3：显示中条目回传提供者（写侧唯一入口）。守卫表达式原为 writeTabGroup
    // 内联，收成单点供落盘与复制共用，判据一字不变：可修改提供者 + 当前显示层已加载；
    // 未加载（含首载在途）不回传（N28）
    void backfillItems(Playlist *list);
    // N105 §2：交给提供者前的共用取表出口（N58 §2.2 的拆包 + 身份过滤收成一处）——
    // 跳过身份非定形的行（空路径行即其中一例），不留占位、不补东西、其余项序不变；
    // 回推（refreshCurrentTab ①）与回传（backfillItems）共用，判据全仓仅此一份
    PlaylistEntity::List bareEntitiesOf(PlaylistListView *view) const;

    // N57 §2.8：原五张"以控件指针为键的平行表"（层缓存 / 浏览路径 / 队列根 /
    // 显示层 / 记忆）及其定位访问器全部退场——状态是 PlaylistListView 的成员，
    // 取用一律 `list->browsePathIds()` / `list->findLevel(ids)` 形态。
    // 下表留本类的只剩"行 ⇄ 状态"的桥与编排（§2.2：只搬状态，不搬编排）
    // 某层的实体序列（filesOf 双出口的实体版共用体，按实体序）
    PlaylistEntity::List layerEntitiesOf(int tabIndex, const QStringList &ids) const;

    // ——N45 §2.4/§2.5 导航与换入——
    // 切层统一入口：显示层 != 浏览层时按层换入——命中缓存同步重建（不起任务），
    // 失败层只剩骨架与占位行（切入的常规渲染路径，不自动重试；回到上一级即删
    // 条目，再钻进来按未加载重举），未加载层走异步枚举（遮罩，完成后换入）
    void showLayer(PlaylistListView *list);
    // 钻取 / 返回：先保存当前层选中项（lastSelectedKey），再改浏览路径并换入
    void navigateDown(PlaylistListView *list, QListWidgetItem *containerRow);
    void navigateUp(PlaylistListView *list);
    // 缓存命中换入后的预读判定——与切 tab（onTabChanged）同判同路：该层元数据
    // 未预读且排序规则需要时，预读后重排（否则缓存命中会永久呈现旧序，§2.6）
    void reactivateIfStale(PlaylistListView *list);
    // 浏览路径归位队列根层；队列根层在缓存即同步换入并滚动到正播项。
    // 不在缓存属防御兜底（mpv 在播 ⇒ 队列根层在缓存；逐出必与归位同步，
    // N53 §2.2，见各锚点分析），仅归位状态
    void revealQueueRoot(PlaylistListView *list);
    // 整层行重建（视图侧：实体所有权留在 Level，行只引用）：上一级行 + 容器行 +
    // 实体行，行序 = [上一级行][容器行…][实体行…]
    void rebuildRowsForLevel(PlaylistListView *list, const Level &lvl, const QStringList &ids);
    // 前导特殊行构建：上一级行（非根层，占第 0 行）+ 容器行，插在现有行之前
    void buildSpecialRows(PlaylistListView *list, const QStringList &ids,
                          const QList<PlaylistProvider::Container> &containers);
    // 实体行构造（不入仓——所有权已在 Level）：makeItem 的视图侧孪生
    QListWidgetItem *makeEntityRow(PlaylistListView *list, const PlaylistEntity::Ptr &entity);
    // 层选中项记忆：离开该层前保存（实体 key 或容器 id），换入时按层恢复
    void saveLayerSelection(PlaylistListView *list, const QStringList &ids);
    void restoreLayerSelection(PlaylistListView *list, const Level &lvl);
    // 非显示层的数据重排（§2.6 规则变更时队列根层一并重排）：容器组内排 +
    // 实体组内排，不动任何行
    static void sortLevelData(Level &lvl, SortRule rule);
    // 枚举发起内核（意图与目标层显式给定）；startLoadAsync 为其薄壳
    void startEnumAsync(PlaylistListView *list, Intent intent, const QStringList &parent);
    // ——N64 整树形态——
    // 缺层守候（showLayer 与层级定位共用）：目标层不在缓存且提供者为整树形态时，
    // Debug 断言显影、Release 按空层处置（ensureLevel 插入空层，返回 true——调用方
    // 按已加载空层继续，不得再对该层发起枚举）；非整树形态返回 false，走既有枚举
    // 通路。根层缺失不在此列（整树未初载，根层调用合法），由调用方以 ids 非空 gate
    bool wholeTreeMissingLevel(PlaylistListView *list, const QStringList &ids);
    // 整树权威重建（onLoadFinished 换入后调用，§2.3）：校验交付（键前缀一致性 +
    // 同层容器 id 唯一；Debug 断言显影，Release 逐项忽略）→ 先删缺席层 → 后写子孙层
    // （入列盖章 + 簿记重置 + 当场 sortLevelData）。rootLvl 是根层换入落点——
    // enumeration 的根层 containers/entities 已被 move 走，键校验的根层 id 集取它。
    // 全在 GUI 续作内
    void rebuildWholeTree(PlaylistListView *list, Level &rootLvl,
                          PlaylistProvider::Enumeration &enumeration);

    // ——N45 §2.4 双坐标系（实体序 / 行序）换算与三类行谓词——
    // 行序 = [上一级行][容器行…][实体行…]；凡 mpv 侧语义一律在实体序上工作，
    // 行序只出现在渲染与鼠标事件里，换算收成本对辅助、不得就地 ±1
    static bool isContainerRow(const QListWidgetItem *item);  // ContainerRole 命中
    static bool isParentLinkRow(const QListWidgetItem *item); // ParentLinkRole 命中
    // 实体行 = 非占位、非容器、非上一级（即 EntityRole 非空；三类特殊行均无实体）
    static bool isEntityRow(const QListWidgetItem *item);
    int entityIndexToRow(const PlaylistListView *list, int entityIndex) const;
    // 非实体行（占位 / 容器 / 上一级）返回 -1
    int rowToEntityIndex(const PlaylistListView *list, int row) const;

    // ——N9 内部辅助——
    // 提供者取用点（N57 §2.6：以控件指针为键的私有表与 `providerAt` 均已删除）：
    // 经视图取其所属列表再取提供者，视图 / 列表缺失即空指针（越界防御，与现状同形）
    PlaylistProvider *providerOfView(const PlaylistListView *view) const;
    // N27：可修改 tab 判定——supportsModify() 的薄封装。不变式 I1（tab ⇔ 视图 ⇔
    // 列表一一对应、列表恒带提供者）已由 Playlist 构造断言与 PlaylistListView::list()
    // 守候，故此处不再重复断言
    bool isModifiableTab(int tabIndex) const;
    // N30：行级换入——前 min(旧行数, 新实体数) 行逐行换绑实体（行对象复用，
    // 保住 PlayingRole 与选中 / 滚动连续），多出的经 makeItem 追加，多余的删尾部行。
    // 不 clear、不销毁行对象；旧实体连同全部缓存一并丢弃（换绑即换新）
    void applyEntitiesToRows(PlaylistListView *list, PlaylistEntity::List &&entities);
    // 统一条目构造：显示名 / 图标 / tooltip 取自实体；EntityRole 存裸指针，
    // 实体所有权随行同步 push 进所有权容器
    QListWidgetItem *makeItem(PlaylistListView *list, PlaylistEntity::Ptr entity);
    // 行与实体的角色绑定（建行与换绑共用）：显示名 / 图标 / tooltip / EntityRole
    static void setItemEntity(QListWidgetItem *item, const PlaylistEntity::Ptr &entity);
    // 元数据回填后触发所有列表行重绘
    void repaintAllLists();

    // ——N30 统一收尾三单元（首载 / 刷新 / 切入重读共用）——
    // 记忆修正 + 选中与滚动，幂等。selectFirst 时只选中首项（换源场景无记忆可复）；
    // 播放表不在此滚动——滚动与选中由归位一次性完成
    void restoreViewState(PlaylistListView *list, int tab, const ViewSnapshot &snap);
    // 播放态归位：按 mpv 现状重建本表 PlayingRole 与选中，是标记的唯一权威出口
    void restorePlayingState(PlaylistListView *list, int tab);
    // N30：清除本表全部 PlayingRole（四步式第 3 步与祖先层刷新前置处置共用——
    // 都已下发 stop，属性查询读到的是发起前的旧值，不可按归位判断。
    // N53：枚举失败路径不再调用——失败即整行重建，重建行天然无标记）
    static void clearPlayingMarks(PlaylistListView *list);
    // ——四步式唯一入口（刷新 / 切入的"其它"分支、applySort、executeRemoval 共用）——
    // mutate = 停止之后、重推之前的结构变更（重排 / 删行）；空则表示调用前已排好
    void runFourStep(PlaylistListView *list, int tab, const std::function<void()> &mutate);
    // N30：刷新收尾的播放同步——排序后逐行比对旧 key 序列，静默 / 末尾追加 / 四步式三选一；
    // 返回是否走了四步式。非播放表与守卫不通过时一律只做视图更新
    bool syncAfterRefresh(PlaylistListView *list, int tab, const QStringList &oldKeys);

    // ——N19 异步加载与忙时（N32：统一到 QtConcurrent + 单任务槽）——
    void activateTab(int index); // 原 onTabChanged 体：选中记忆行/播放行滚动
    // 唯一出口（六步、顺序不可交换）：① 取判定；② 身份校验——本 watcher 已非当前槽位项
    // 即什么都不碰；③ 腾出槽位并销 watcher；④ 调续作（可能起下一轮任务并占用槽位）；
    // ⑤ 槽位仍空才隐藏遮罩 + setBusy(false)；⑥ N37 枚举警告弹框（零延时投递，
    // 槽位为空才弹）。忙时与遮罩的复位只此一处（另一处是 abandonJob 的显式作废），
    // 且必须晚于续作
    void runJob(const JobContext &ctx, LoadingOverlay::Mode mode, int total, bool cancelable,
                std::function<void(QPromise<void> &)> body, std::function<void(JobVerdict)> cont,
                std::function<void()> onAbandon = {});
    // 枚举发起（意图由发起处经 m_pendingLoad 定死）：清占位、递增代际、记目标表、置忙、起遮罩
    void startLoadAsync(PlaylistListView *list);
    // 枚举续作：失败分支就地收尾；成功则警告入库、换入 → 推进函数
    // （结果按移动语义换入，故非 const；N37 起结果类型为 Enumeration）
    void onLoadFinished(const JobContext &ctx,
                        std::expected<PlaylistProvider::Enumeration, QString> &r, JobVerdict v);
    // N32 推进函数（业务状态机，唯一入口）：要预读就起预读任务并让自己作为其续作
    // （fromMetaScan 为真时不再入内，自然终止）；否则做收尾。
    // 预读触发点由原三处（枚举收尾 / 切入重读 / 手动排序）收敛为这一处
    void advanceTab(JobContext ctx, bool fromMetaScan);
    // 预读发起：快照装配 + 可取消任务 + "只算不写"任务体 + 注入续作
    void startMetaScan(JobContext ctx, MetaCategory cat);
    // 预读续作：注入批次（唯一的写点；取消也注入，保持 N19 的缓存保留语义）→ 置已读标志
    // → 重绘 → 正常则回推进函数收尾，取消则按意图分支收尾
    void onMetaScanFinished(JobContext ctx, const std::shared_ptr<MetaResult> &r, JobVerdict v);
    // 取消收尾：枚举/刷新/重读侧有快照 → 视图恢复 + 播放态归位；Sort 侧无快照 → 一律不动
    void finishCanceled(const JobContext &ctx);
    // N32 外部作废统一入口：先 cancel 在途任务 → 递增代际 → 清槽位 → 复位忙时与遮罩
    void abandonJob();
    void setBusy(bool on);
    void flushPendingTab();
    void setListsEnabled(bool on);
    // ——N54 层级定位内核——
    // 目标身份 → 页面：换算不出坐标（无身份 / 列表已删）即空指针，ListGone 的唯一判据。
    // 坐标只在此处与信号发射点出生，不进待办状态（N57 §1.5 / §2.8）
    PlaylistListView *jumpTarget(const LevelJump &req) const;
    // 唯一推进口：每次重判目标层三态（已加载 / 失败 / 未加载）并据此动作，至多两轮
    // 出结果（一轮起枚举、下一轮定位），故不假设"上一次消费已把该层取回"
    void advanceLevelJump();
    // 整段替换某表的浏览路径（标题列按 loadPlaylists 的降级形态取 id 自兼）；
    // 替换前按导航纪律保存离开层的选中，与钻取/返回同一口径
    void setBrowsePath(PlaylistListView *list, const QStringList &ids);
    // 目标层内定位：首个 key() == itemKey 的实体序，无命中 -1（两侧同为归一形态，逐字节比）
    int entityIndexInLevel(const PlaylistListView *list, const QStringList &ids,
                           const QString &itemKey) const;
    // ——N49 底部工具条——
    // 让位：压住内部页面栈的最大高度并把工具条摆到底部（含主动几何修正，§2.3）
    void reserveBottomBarSpace();
    // N116：底栏两枚图标的唯一赋值点（初值与外观事件共用，幂等；构造期判空）
    void retintIcons();
    // 正播中判据（按钮启用态与定位入口的守卫共用）：mpv 已注入、非空闲、path 非空
    bool hasPlayingItem() const;
    // 手动定位唯一入口：切到在播 tab 并把在播项选中滚动可见；忙时延后（§2.4）
    void locatePlayingItem();
    // 按钮启用态刷新（漏刷只影响灰态：定位入口幂等无副作用）
    void updateBottomBarState();
    // 搜索推进：在当前 tab 的当前显示层按显示名包含匹配，末尾回绕（§2.5）
    void searchNextInCurrentList();
    // 可匹配行谓词：容器行参与；"上一级"行与失败占位行不参与（§2.5）
    static bool isSearchableRow(const QListWidgetItem *item);
    void armOverlay(LoadingOverlay::Mode mode, int total);
    // ——N32 上下文采集（到用到处才采）——
    // 行序 key 序列（比对用；占位行跳过）
    QStringList keysOf(const PlaylistListView *list) const;
    // 视图快照：记忆行 key + 有无记忆（换入前一刻 / 不换入类的排序前一刻调用）
    ViewSnapshot captureViewSnapshot(PlaylistListView *list) const;
    void applySort(PlaylistListView *list, SortRule rule, bool writeRule);
    // N29：对应类别"本层已全表读取完成"标志的读写收敛点（两位同形）。
    // N45：按 (list, parent) 定位层——任务续作写的是任务目标层，不是显示层
    bool metaLoadedOf(const PlaylistListView *list, const QStringList &ids, MetaCategory cat) const;
    void setMetaLoadedOf(PlaylistListView *list, const QStringList &ids, MetaCategory cat, bool on);
    // N20：按表精确驱逐——遍历指定层的实体，按实体 key 逐个删 m_duration；flag 重置
    // 所有表（N45：全部层的两类标志一并复位）。N64：层显式给定（整树批次目标层为
    // 根层，不再按显示行取；行与层内容同构，逐层即逐行的等价改写）
    void evictListKeys(PlaylistListView *list, const QStringList &ids);
    // 已加载 tab 切入重读判定：内容已载、无占位、标志失效、排序依赖元数据。
    // N45：判定对象是本 tab 的当前显示层
    bool needsMetaRescan(int tabIndex) const;
    // 已加载 tab 切入即重读：置意图后交给推进函数（预读 → 排序 → 选中跟随）；
    // N32 起不再是同步函数——预读已在任务槽内异步跑
    void reactivateRescan(PlaylistListView *list, int tabIndex);
    // ——N19 失败占位——
    static bool isPlaceholderRow(const QListWidgetItem *item);
    void showPlaceholder(PlaylistListView *list, const QString &error);
    bool clearPlaceholder(PlaylistListView *list); // 返回是否清掉过占位行
    // 条目重排核心（无四步式副作用）：排序/加载填充共用
    void sortListItems(PlaylistListView *list, SortRule rule);

    // ——N26 实体所有权——
    // 行实体读取：占位行 / 无实体行返回 nullptr
    static const PlaylistEntity *entityFromItem(const QListWidgetItem *item);
    // 行实体的共享计数副本：跨嵌套事件循环（右键菜单 exec）持有，
    // 防菜单期间回刷收编重建表致悬空；行无实体返回 nullptr
    PlaylistEntity::Ptr entityPtrAt(PlaylistListView *list, int row) const;
    // N47：行容器的按值副本：跨嵌套事件循环（右键菜单 exec）持有，防菜单期间
    // 刷新重建层致悬空；行无容器返回 nullopt（Container 为小值类型，按值拷贝）
    std::optional<PlaylistProvider::Container> containerAt(PlaylistListView *list, int row) const;
    // 按行序批量删除实体（rows 必须降序；须在删行前调用）。
    // 单遍前缀计数定位各行容器下标后降序批量 erase，避免逐行 O(n·k) 重扫
    void eraseEntitiesAt(PlaylistListView *list, const QList<int> &rows);
    // ——N26 元数据能力谓词（一对，同形）——遍历该层实体，任一支持即真；
    // 空表 / 未加载层为假
    bool anyEntitySupportsMediaMeta(const PlaylistListView *list, const QStringList &ids) const;
    bool anyEntitySupportsFileMeta(const PlaylistListView *list, const QStringList &ids) const;

    // N29：预读判定内核（各判定点共用）——该层该排序规则仍需预读的类别，
    // 无需预读返回 None（类别 None、支持者缺失、对应标志已置均视为无需）。
    // 只含"类别 / 支持者 / 标志位"三要素，busy / 已加载 / 占位等
    // 外围条件由调用方自判；共用即保证"填充跳过直排"与"收尾补排"等各点一致。
    // N57 §2.9：入参改值对象（判据仍只看键）；键→类别的映射 `metaCategoryOf`
    // 已随键搬到实体侧，菜单灰态判据仍调它
    // N45：按 (list, parent) 定位层
    MetaCategory pendingMetaCategory(const PlaylistListView *list, const QStringList &ids,
                                     SortRule rule) const;
    // N29：对应类别支持者谓词（收敛按类别选支持者谓词的三元散写）。
    // N45：按 (list, parent) 定位层
    bool anyEntitySupports(const PlaylistListView *list, const QStringList &ids,
                           MetaCategory cat) const;

    // ——结构变更公共骨架——
    // 三步法之第 1 步：抄录记忆行对应的身份串；第 3 步：在新序列中找回并写回
    QString captureMemoryPath(PlaylistListView *list) const;
    void restoreMemoryPath(PlaylistListView *list, const QString &captured, bool hadMemory);
    // N30：回刷收编后的记忆重建。内容被整体替换，旧记忆的行号与路径一并失效，
    // 但 mpv 仍在播新表里的某一项——按正播行重建记忆（定位不到才作废），
    // 使随后的四步式（排序 / 移除）始终有续播目标，无需任何兜底
    void rebuildMemoryAfterAdopt(PlaylistListView *list, int playingRow);

    // 排序执行：非播放 tab 直接重排；播放 tab 走四步式
    void sortCurrent(SortRule rule);
    // 未知值恒归末尾组

    // 移除无效项 / 重复项（含四步式与本地直删两条路由）
    void removeInvalidItems();
    void removeDuplicateItems();
    // 收集待删行号后统一执行的收尾：播放 tab 四步式后半段或本地直删
    void executeRemoval(const QList<int> &rows);
    // N25：播放 tab 的本地先行删行（走逐行 playlist-remove 同步通道时用）：
    // 逆序删行 + 记忆复位 + 刷新导航，不含四步式的停止/重推
    void removeRowsLocal(PlaylistListView *list, const QList<int> &rows);

    // N55 §2.2：盖章落点共用——取列表的归属两件套（身份 + 当前提供者弱引用）对实体
    // 显式赋盖。盖与不盖由每个入列落点各自决定（收编 clone() 携带原归属的落点不调
    // 本函数）；实体入列即盖章是归属不变式的入口
    void stampOwnership(PlaylistEntity &entity, const Playlist *owner);

    // N55 §2.6：来源失效清扫——删列表（removeTabAt）与换源（openListSettings）两
    // 触发点各接一处，与触发动作同点同步做完，只把告知弹窗延后。stopAlreadyDone：
    // 触发点的既有停止语义是否已因"正播就是 U"而执行
    void applySourceRetired(const QUuid &retiredUuid, const QString &retiredName,
                            bool stopAlreadyDone);
    // N55 §3.5：三类告知（收编 / 清扫 / 异常数据）共用的记账入口——只入待发队列并
    // 调度一次投递，判断处一律不 exec()。投递步是 pumpDeferredNotices
    void queueDeferredNotice(const QString &text);
    // 延后出口的投递步：队列非空且无弹窗在幕才合成一扇；弹窗在幕时记账者照记账，
    // 本扇关掉的收尾处再自排下一轮（不叠第二扇）
    void pumpDeferredNotices();

    // 右键菜单动作实现（第七节）
    void refreshCurrentTab();
    // N62 §2.2：改序——先动权威（m_lists->moveList，toTab - 1 换算持久域）再动镜像
    // （tabBar()->moveTab，页面栈由 QTabWidget 的 tabMoved 同步槽自动跟随），末尾落盘。
    // 前移后移共用；越界与相等属调用方违约，Debug 断言显影，Release 直接早退
    void reorderList(int fromTab, int toTab);
    // N62 §2.3：复制——点一下立即产出同配置同条目的新表，不弹对话框；落末尾并激活。
    // 过去三样：提供者类型与自身参数（经唯一序列化器往返）、条目内容（回传先行）、
    // 当前排序规则；身份现铸、名字走默认命名算法、视图态不带
    void duplicateList(int tabIndex);
    // N79：复制当前层内容到新建静态列表（主体菜单入口）——内容的拍平快照，
    // 与 duplicateList 的整表配置复制不是一回事；只读动作，不停播不动会话键
    void copyContentsToNewList(int tabIndex);
    // N80 §2：N79 取数段的原样收拢——取串通路仍是 currentLayerEntitiesOf →
    // playTargets（全仓唯一直接形态出口）→ 逐项跳空串、跳转接串并置 *skippedSome；
    // 每项再带实体 title（时长不导出，N80 变更记录 2，不查任何时长源）。
    // 两个动作共用一个采集点：复制侧只取 url 字段，导出侧 url 与标题全用
    QList<PlaylistFile::Entry> collectTrackEntries(int tabIndex, bool *skippedSome = nullptr) const;
    // N80 §4：导出当前层内容为清单文件（主体菜单入口）——另存对话框后缀即格式，
    // PLS/M3U 两档；只读动作，同 N79 不停播、不经 markPlayingTab、不动会话键
    void exportListAs(int tabIndex);
    // N81 §3-6：从 PLS / M3U 清单文件导入条目末尾追加进当前列表（主体菜单入口）
    // ——只取 url（注入显示名持久化不了，同 N79 理由）；相对路径按清单文件所在
    // 目录拼绝对后交既有 addFiles，不停播、不经 markPlayingTab、不动会话键
    void importFromFile();
    // N35：改收共享计数副本——异步属性对话框跨嵌套事件循环持有实体，
    // 求值转发取回的值经注入口写回该实体（关闭即取消）
    void showPropertiesDialog(const PlaylistEntity::Ptr &entity);
    // N47：容器属性对话框（同步即时，无异步求值）：独立实现，只沿用视觉语言；
    // fallbackPath 为 path 缺失时的回退相对路径（§2.2）
    void showContainerPropertiesDialog(const PlaylistProvider::Container &container,
                                       const QString &fallbackPath);
    void copyPathToClipboard(const QString &path);

    // ——N35 服务注册表装配——
    // N56：注册表的维护（HttpClient / TraversalPolicy 的注册与设置变更更新）全部
    // 上移装配顶点 MainWindow，本类只经注入的 registry 取用（提供者构造注入、
    // 实体求值现查），不再负责任何 provide

    // 正在播放条目所在行（PlayingRole 命中处），无则 -1
    int playingRowOf(const PlaylistListView *list) const;

    // N104：按串定位的共用写体（failed 真 = 标红，假 = 清红），两个公共口的薄底座
    void setPlayFailedByKey(const QString &path, bool failed);

    // N57 §2.8：五张"以控件指针为键的平行表"（层缓存 / 浏览路径 / 队列根 / 显示层 /
    // 记忆）已全部搬进 PlaylistListView 成为成员——对象即状态，外层键随之消失。
    // 随之消失的还有那条"返回引用的生命周期不得跨越可能插入新表项的操作"的约束
    // （QHash 重哈希搬移元素）。
    // 面板级共享状态留在本类、不入视图也不入注册表（§2.1）：时长仓（委托按指针
    // 引用）、搜索态、菜单目标、任务槽、延后旗标、遮罩与工具条
    // 全局时长仓（mpv 推送，键为规范化路径）：面板拥有、委托共享只读——页面是子对象，
    // 销毁晚于面板成员，故不给裸指向（见 PlaylistDelegate 构造注释）
    std::shared_ptr<DurationStore> m_duration = std::make_shared<DurationStore>();
    QPointer<PlaylistListView> m_menuTargetList; // 右键命中的列表与行（空白处为空）
    int m_menuTargetRow = -1;

    // N57 §2.8：播放目标 = **列表身份**（不是 tab 序号）。初值取保留身份
    // kDefaultListUuid——它恒可解析的前提正是 §2.5 给了默认列表一个恒定且不可摘除的
    // 身份，于是"同步目标失效归位默认"是一次身份赋值而非索引修补，tab 随增删漂移
    // 与本字段无关（旧 --m_playingTab 那类修正整段消失）
    QUuid m_playingUuid = kDefaultListUuid;
    // N59 §3.2：主动设定的播放目标历史记录，**只在 markPlayingTab 内写**（全仓唯一能把
    // 播放目标定到非默认列表的入口）；四个被动归位点直写 m_playingUuid 绕开它，天然不擦
    QUuid m_lastPlayingTargetUuid = kDefaultListUuid;
    // N62 §2.2：当前停留的表的身份——onTabChanged 的重激活判据。nil 初值保证首次切换
    // 走完整激活；"索引变了、页面没变"的事件（如改序移动当前表）在此判等收编为无操作
    QUuid m_currentListUuid;

    MpvController *m_mpv = nullptr; // 借用的裸指针（不随控制器销毁归零），attachController 注入
    // N35：服务注册表（初始化列表创建，此后恒非空；设置变更时只覆盖其内部实例，
    // 注册表身份恒定——收编 / 拖放 / 打开 URL 经工厂注入实体，实体每次求值现查）
    // N36：TraversalPolicy 同注册于此；创建与装配分离，两个 provide 函数各管一项
    std::shared_ptr<ServiceRegistry> m_registry;
    // N57 §2.3：列表注册表（装配顶点创建并注入，携共享计数，恒非空）。提供者不再
    // 由本类以控件指针为键持有——身份、名字与提供者三件套活在 Playlist 里
    std::shared_ptr<PlaylistRegistry> m_lists;
    // N55 §3.5：待发告知文案队列（同串去重——收编文案恒同串，天然合并）；
    // singleShot(0) 触发时把队列合成一次模态弹窗（分段显示）后清空。
    // m_noticeDialogUp：那扇窗此刻在幕——延后出口的投递步据此不叠第二扇
    QStringList m_pendingNotices;
    bool m_noticeDialogUp = false;

    // N32 忙时与单飞行（N32：枚举与预读共用一个任务槽）——
    JobSlot m_job;       // 当前在途任务（枚举 / 预读同一槽位，全插件同一时刻至多一个）
    quint64 m_token = 0; // 代际计数器；唯一递增点：runJob 起任务、abandonJob 作废
    bool m_busy = false; // 加载在途或预读中：tab 栏 + 列表 + 右键菜单冻结
    // N62 §2.7 A 档：忙时排队的程序化切 tab（last-wins）改持**列表身份**——待办跨整段
    // 在途任务存活，tab 序在这期间会漂移。requestTab 入口把入参索引当场换算成身份存下，
    // flushPendingTab 经 tabIndexOfList 现换算坐标，换算不到即放弃（与旧越界早退等价）。
    // nil 即无待办
    QUuid m_pendingTabUuid;
    bool m_pendingReveal = false; // N45：忙时延后的切曲归位（last-wins，busy 结束补执行）
    // N54：延后的层级定位（last-wins，与 m_pendingTabUuid / m_pendingReveal / m_pendingLocate
    // 同族）。enumerated = 本次跳转已为目标层起过一轮枚举的闸门：既实现"一次跳转至多
    // 一次枚举"（§2.4 的失败层显式重试也只此一次），又让"任务被作废/取消后目标层仍不在
    // 缓存"这一情形落到静默作废而不是无限重起
    struct PendingJump {
        LevelJump req;
        bool enumerated = false;
        bool active = false;
    };
    PendingJump m_pendingJump;
    // N37：枚举警告累积（仅 Ok 批次写入；Discarded 在 onLoadFinished 首行早退不写），
    // 唯一出口第⑤步复位后弹框，弹完即清（§3.5）
    // ——N49 底部工具条（面板级常驻：全 tab 共享一行，不随 tab 切换重建）——
    QWidget *m_bottomBar = nullptr;
    QToolButton *m_locateButton = nullptr;
    QLineEdit *m_searchBox = nullptr;
    QAction *m_searchAction = nullptr; // 搜索框装饰图标动作（图标由 retintIcons 供）
    // 内部页面栈缓存（QTabWidget 唯一的 QStackedWidget 子控件）；取不到时整个让位
    // 静默跳过，不引入崩溃面
    QStackedWidget *m_stack = nullptr;
    SearchState m_search;
    bool m_pendingLocate =
        false; // 忙时延后的手动定位（与 m_pendingTabUuid / m_pendingReveal 同族）
    QStringList m_enumWarnings;
    LoadingOverlay *m_overlay = nullptr;
    LoadingOverlay::Mode m_overlayMode = LoadingOverlay::Mode::Loading;
    int m_overlayTotal = 0;
    QTimer m_overlayTimer; // 遮罩 300ms 延迟显示（可取消）
    // ——N19 收尾挂起（N32：只作发起期意图来源；任务的一切上下文随 JobContext 按值走）——
    struct PendingLoad {
        QPointer<PlaylistListView> list;
        bool selectFirst = false; // 完成后选中首项（列表设置换源）
        bool refresh = false;     // 刷新批次：收尾需比对与播放同步
    };
    PendingLoad m_pendingLoad;
    // ——N60 推送会话（§2.2）——
    // 世代号自 1 起（0 留作"非世代"防御值）；在册的世代即吞域窗口，集合非空 = 有批次在途
    quint64 m_pushGenCounter = 0;
    QSet<quint64> m_openPushGens;
    // 对账暂存槽：按**世代**覆盖（§2.3 第 4 条——async 之间亦无到达顺序保证，
    // 故取"已收到 reply 中的最大世代"而非"最后到达的那条"），集合清空即结算并复位
    struct Barrier {
        quint64 gen = 0;
        bool ok = false;
        QVariantList items;
    };
    Barrier m_lastBarrier;
};

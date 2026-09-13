#pragma once

#include "pch.h"

#include "LoadingOverlay.h"
#include "PlaylistEntity.h"
#include "PlaylistProvider.h"

class MpvController;
class ServiceRegistry; // N35：服务注册表（成员以 shared_ptr 持有）

// 右侧播放列表：每个 tab 对应一个播放列表，tab 标题为列表名。
// 约定：第一个 tab 名为"默认"，不可删除；其余 tab 持久化保存到 INI 配置文件。
// 只有一个 tab 是"正在播放列表"（与 mpv 播放列表同步），初始为第一个 tab；
// 双击任意 tab 中的条目会把该 tab 整个载入 mpv 并播放对应条目，同时该 tab 成为新的同步目标。
//
// N26 起条目为播放项实体：行 EntityRole 存实体指针（占位行为空），
// 实体集合所有权仓 m_entities 以 list 指针为键（容器序恒等于行序）；
// 文件级 / 媒体元数据全部归属实体（惰性求值 + 实体内缓存）。
// 视图仅持有全局时长仓 m_duration（mpv 推送，键为规范化路径）。
// 排序与清理对播放中 tab 统一走四步式（N30 收敛为 runFourStep 唯一入口）：
// 停止 → 本地变更 → playRequested 整表推送并从续播记忆恢复（暂停态自动保持）。
// N30 刷新语义：静态与动态同路（回推后重枚举），行级换入不重建行，
// 排序后比对旧序列——一致静默、末尾追加、不一致走四步式。
// N32 异步语义：枚举与元数据预读统一到 QtConcurrent（QPromise + QFutureWatcher），
// 全插件同一时刻至多一个在途任务——枚举与预读共用一份任务槽、一个推进函数
// （advanceTab）、一处"腾槽 → 续作 → 条件复位忙时"的唯一出口；任务只捕获入参副本，
// 不持有宿主对象，故析构无需等待。
class PlaylistWidget : public QTabWidget {
    Q_OBJECT
  public:
    // 条目数据角色约定
    static constexpr int EntityRole = Qt::UserRole;          // 实体指针（quintptr，占位行为空）
    static constexpr int PlayingRole = Qt::UserRole + 1;     // 正在播放标记（bool）
    static constexpr int PlaceholderRole = Qt::UserRole + 2; // N19 枚举失败占位行标记（bool）

    explicit PlaylistWidget(const QString &configFile = QString(), QWidget *parent = nullptr);
    ~PlaylistWidget() override; // N32：非阻塞收尾——作废并取消在途任务后立即返回（不再等待）

    // 注入控制器（弱引用）：N30 起四步式在停止前读 pause、归位读 path / idle-active
    void attachController(MpvController *mpv) { m_mpv = mpv; }

    // N25：mpv 播放列表回刷。本地在途回显 → 空列表忽略 → 与"正在播放"tab 逐行一致即静默
    // （仅翻转播放标记）→ 占位/枚举失败态忽略 → 差异则整体收编到默认 tab 并转移同步目标。
    // 回刷不再重建正在播放的 tab，也不驱逐任何元数据缓存
    void setPlaylist(const QVariantList &items);
    // 向当前 tab 添加文件（触发 filesAdded 信号）
    void addFiles(const QStringList &files);

    // N11：主菜单"打开文件"专用——整体替换默认 tab 内容并作废其续播记忆；
    // 默认 tab 永不持久化，也不触发 filesAdded（播放由 MainWindow 统一入口显式驱动）
    void setDefaultTabFiles(const QStringList &files);

    QStringList filesOf(int tabIndex) const;
    int playingTabIndex() const { return m_playingTab; }
    bool currentIsPlayingTab() const { return currentIndex() == m_playingTab; }

    // 当前 tab 中选中的行号，无选中项返回 -1
    int currentSelectedRow() const;
    // 转移“正在播放”同步目标到指定 tab（双击条目或会话恢复载入时调用）。
    // B10：异 tab 转移时清空原 tab 的播放标记，防残留；同 tab 早退防闪烁
    void markPlayingTab(int tabIndex);

    // 记录某 tab 的最后播放项与进度（仅非默认 tab 生效；纯内存，落盘发生在保存时）。
    // N9：记忆条目的路径随写入点自动采集（路径+序号双存，恢复时序号校验优先）
    void recordPlayback(int tabIndex, int pos, double seconds);
    // B9：清除某 tab 的续播进度记忆（保留最后播放项）；整表自然播完后调用
    void clearResumeProgress(int tabIndex);
    // N9：续播行统一解析——序号处路径与记忆一致则用之；否则按路径取首命中；无命中 -1
    int resolveResumeRow(int tabIndex) const;
    // 某 tab 记忆的播放进度，无记录返回 -1.0
    double lastTimePosOf(int tabIndex) const;
    // 按 tab 标题查找索引，找不到返回 -1
    int indexOfName(const QString &name) const;
    // N19：程序化切 tab 统一入口——MainWindow 侧全部经此调用。忙时（加载在途/
    // 预读中）且目标非当前 tab 则排队（last-wins），空闲后补执行；其余直调
    void requestTab(int index);
    bool isBusy() const { return m_busy; }

    // N25：本地在途回显登记——本地向 mpv 下发多命令序列（整表载入/批量移除/追加）前，
    // 记录本地变更后的最终序列。回刷据此识别"同一变更的在途中间态"（空列表/部分列表）
    // 并忽略，避免误判为外部变更而把列表搬去默认 tab。空序列不登记（清空/删 tab
    // 由回刷的"空列表忽略"兜底）。整表载入统一在 MainWindow::onPlayRequested 登记；
    // 追加（addFiles）与批量移除（removeRowsLocal）在各自发射点登记
    void noteLocalPush(const QStringList &snapshot);

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

    // 持久化：从配置文件恢复非默认播放列表 / 保存除默认外的所有列表
    void loadPlaylists();
    void savePlaylists();

    // ——元数据回填（N18：仅保留 duration 写入）——
    void applyDuration(const QString &path, double seconds);

  public slots:
    // N35：设置对话框 ini 键变更转接——命中远端请求设置（remoteTimeoutSec /
    // ignoreHttpsErrors）时重读 ini 重建 client 并覆盖注册表，下一次求值即生效
    void onPluginIniKeysChanged(const QStringList &keys);

  protected:
    // N5：ShortcutOverride 认领导航键——焦点在列表上时 ↑↓←→/PgUp/PgDn/Home/End
    // 走列表原生导航，全局音量动作（↑/↓）本次不触发。
    // 该事件定向投递焦点控件、不沿父链冒泡，故过滤器必须装在每个列表自身
    bool eventFilter(QObject *watched, QEvent *event) override;

  signals:
    // 请求整表载入并播放 files 中第 index 个；seekTo > 0 时就绪后恢复到该进度
    // （N4 四步式的进度恢复与此同一通道；发出前已将 tab 标记为正在播放列表）。
    // N30：keepPaused 为真时载入后保持暂停（四步式在 stop 前读到的 pause 状态）
    void playRequested(const QStringList &files, int index, double seekTo, bool keepPaused = false);
    // 当前 tab 收到新文件（菜单添加或拖放）
    void filesAdded(const QStringList &files);
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

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override; // N19：遮罩几何跟随

  private slots:
    void onItemDoubleClicked(QListWidgetItem *item);
    void onTabChanged(int index);

  private:
    // 排序规则枚举（N4/N9/N18）：None=添加顺序。取值随提供者持久化（int 不透明存储）。
    // N18：枚举序与菜单显示顺序解耦——菜单 action 通过 setData(int(SortKey))
    // 显式携带键值，派发时按 data 取键，不再依赖数组下标 +1 的隐式映射
    enum class SortKey {
        None,
        FileName,
        FileNameNumeric,
        Extension,
        Created,
        Modified,
        FileSize,
        Duration,
        ReleaseDate,
    };

    // N29：预读元数据类别（排序键经 metaCategoryOf 映射；None = 该键不依赖元数据）
    enum class MetaCategory { None, File, Media };

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
    // 一次任务的全部输入。oldKeys / ViewSnapshot 一律不在发起时采集——在途期 mpv 事件
    // 仍经 recordPlayback 推进本表记忆（无忙时守卫），发起时快照会丢掉这段进度；
    // 由续作在"用到它的那一步之前一刻"填入（换入类在换入前，不换入类在排序前）
    struct JobContext {
        Intent intent = Intent::InitialLoad;
        QPointer<QListWidget> list;
        int tab = -1;
        SortKey sortKey = SortKey::None; // 仅 Sort 用：判类别与写规则都读它，不读提供者
        bool ascending = true;
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
        quint64 token = 0;            // 单调代际（并入原 m_loadGen）
        QPointer<QListWidget> target; // 目标表（仅 GUI 线程读写）
        bool cancelable = false;      // 枚举批次不可取消（遮罩在 Loading 模式亦无取消按钮）
        QFuture<void> future;         // 在途任务；正常出口与 abandonJob 均复位为默认构造
        QFutureWatcher<void> *watcher = nullptr;
        std::function<void()> onAbandon; // 取消时执行（仅可取消批次）：搬走已算批次注入缓存
        bool active() const { return watcher != nullptr; }
    };

    // 单个 tab 的播放记忆（默认 tab 仅进程内有效，持久化时跳过索引 0）。
    // N9/N26：lastPath 为记忆条目 key（normalizePathForMatch 归一形态，引用条目身份
    // 而非某次字符串形态；旧 INI 读取时归一桥接），恢复时序号校验失败按其寻位
    struct TabMemory {
        int lastPos = -1;
        double lastTimePos = -1.0;
        QString lastPath;
    };

    QListWidget *createList();
    QListWidget *listAt(int index) const;

    // ——N9 内部辅助——
    PlaylistProvider *providerAt(int tabIndex) const;
    // N27：可修改 tab 判定——supportsModify() 的薄封装，唯一用途是收敛判空，
    // 不含任何类型知识。不变式 I1：list 与 provider 成对创建，listAt 非空时
    // providerAt 恒非空（断言守候）
    bool isModifiableTab(int tabIndex) const;
    // N30：行级换入——前 min(旧行数, 新实体数) 行逐行换绑实体（行对象复用，
    // 保住 PlayingRole 与选中 / 滚动连续），多出的经 makeItem 追加，多余的删尾部行。
    // 不 clear、不销毁行对象；旧实体连同全部缓存一并丢弃（换绑即换新）
    void applyEntitiesToRows(QListWidget *list, PlaylistEntity::List &&entities);
    // 统一条目构造：显示名 / 图标 / tooltip 取自实体；EntityRole 存裸指针，
    // 实体所有权随行同步 push 进所有权容器
    QListWidgetItem *makeItem(QListWidget *list, PlaylistEntity::Ptr entity);
    // 行与实体的角色绑定（建行与换绑共用）：显示名 / 图标 / tooltip / EntityRole
    static void setItemEntity(QListWidgetItem *item, const PlaylistEntity::Ptr &entity);
    // 元数据回填后触发所有列表行重绘
    void repaintAllLists();

    // ——N30 统一收尾三单元（首载 / 刷新 / 切入重读共用）——
    // 记忆修正 + 选中与滚动，幂等。selectFirst 时只选中首项（换源场景无记忆可复）；
    // 播放表不在此滚动——滚动与选中由归位一次性完成
    void restoreViewState(QListWidget *list, int tab, const ViewSnapshot &snap);
    // 播放态归位：按 mpv 现状重建本表 PlayingRole 与选中，是标记的唯一权威出口
    void restorePlayingState(QListWidget *list, int tab);
    // N30：清除本表全部 PlayingRole（四步式第 3 步与枚举失败路径共用——
    // 二者都已下发 stop，属性查询读到的是发起前的旧值，不可按归位判断）
    static void clearPlayingMarks(QListWidget *list);
    // ——四步式唯一入口（刷新 / 切入的"其它"分支、applySort、executeRemoval 共用）——
    // mutate = 停止之后、重推之前的结构变更（重排 / 删行）；空则表示调用前已排好
    void runFourStep(QListWidget *list, int tab, const std::function<void()> &mutate);
    // N30：刷新收尾的播放同步——排序后逐行比对旧 key 序列，静默 / 末尾追加 / 四步式三选一；
    // 返回是否走了四步式。非播放表与守卫不通过时一律只做视图更新
    bool syncAfterRefresh(QListWidget *list, int tab, const QStringList &oldKeys);

    // ——N19 异步加载与忙时（N32：统一到 QtConcurrent + 单任务槽）——
    void activateTab(int index); // 原 onTabChanged 体：选中记忆行/播放行滚动
    // 唯一出口（六步、顺序不可交换）：① 取判定；② 身份校验——本 watcher 已非当前槽位项
    // 即什么都不碰；③ 腾出槽位并销 watcher；④ 调续作（可能起下一轮任务并占用槽位）；
    // ⑤ 槽位仍空才隐藏遮罩 + setBusy(false)。忙时与遮罩的复位只此一处（另一处是
    // abandonJob 的显式作废），且必须晚于续作
    void runJob(const JobContext &ctx, LoadingOverlay::Mode mode, int total, bool cancelable,
                std::function<void(QPromise<void> &)> body, std::function<void(JobVerdict)> cont,
                std::function<void()> onAbandon = {});
    // 枚举发起（意图由发起处经 m_pendingLoad 定死）：清占位、递增代际、记目标表、置忙、起遮罩
    void startLoadAsync(QListWidget *list);
    // 枚举续作：失败分支就地收尾；成功则换入 → 推进函数（结果按移动语义换入，故非 const）
    void onLoadFinished(const JobContext &ctx, std::expected<PlaylistEntity::List, QString> &r,
                        JobVerdict v);
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
    void armOverlay(LoadingOverlay::Mode mode, int total);
    // ——N32 上下文采集（到用到处才采）——
    // 行序 key 序列（比对用；占位行跳过）
    QStringList keysOf(const QListWidget *list) const;
    // 视图快照：记忆行 key + 有无记忆（换入前一刻 / 不换入类的排序前一刻调用）
    ViewSnapshot captureViewSnapshot(QListWidget *list) const;
    void applySort(QListWidget *list, SortKey key, bool ascending, bool writeRule);
    // N29：对应类别"本表已全表读取完成"标志的读写收敛点（两位同形）
    bool metaLoadedOf(const QListWidget *list, MetaCategory cat) const;
    void setMetaLoadedOf(QListWidget *list, MetaCategory cat, bool on);
    // N20：按表精确驱逐——遍历该表非占位行，按实体 key 逐个删 m_duration；flag 重置所有表
    void evictListKeys(QListWidget *list);
    // 已加载 tab 切入重读判定：内容已载、无占位、标志失效、排序依赖元数据
    bool needsMetaRescan(int tabIndex) const;
    // 已加载 tab 切入即重读：置意图后交给推进函数（预读 → 排序 → 选中跟随）；
    // N32 起不再是同步函数——预读已在任务槽内异步跑
    void reactivateRescan(QListWidget *list, int tabIndex);
    // ——N19 失败占位——
    static bool isPlaceholderRow(const QListWidgetItem *item);
    void showPlaceholder(QListWidget *list, const QString &error);
    bool clearPlaceholder(QListWidget *list); // 返回是否清掉过占位行
    // 条目重排核心（无四步式副作用）：排序/加载填充共用
    void sortListItems(QListWidget *list, SortKey key, bool ascending);

    // ——N26 实体所有权——
    // 实体集合访问：返回引用（QHash 值整表拷贝 O(n)，禁按值取）；
    // 返回引用的生命周期不得跨越可能插入新表项的操作（QHash 重哈希搬移元素）
    PlaylistEntity::List &entitiesOf(QListWidget *list);
    // 行实体读取：占位行 / 无实体行返回 nullptr
    static const PlaylistEntity *entityFromItem(const QListWidgetItem *item);
    // 行实体的共享计数副本：跨嵌套事件循环（右键菜单 exec）持有，
    // 防菜单期间回刷收编重建表致悬空；行无实体返回 nullptr
    PlaylistEntity::Ptr entityPtrAt(QListWidget *list, int row) const;
    // 按行序批量删除实体（rows 必须降序；须在删行前调用）。
    // 单遍前缀计数定位各行容器下标后降序批量 erase，避免逐行 O(n·k) 重扫
    void eraseEntitiesAt(QListWidget *list, const QList<int> &rows);
    // ——N26 元数据能力谓词（一对，同形）——遍历本表实体，任一支持即真；
    // 空表 / 未加载表为假
    bool anyEntitySupportsMediaMeta(const QListWidget *list) const;
    bool anyEntitySupportsFileMeta(const QListWidget *list) const;

    // N29：排序键 → 预读元数据类别的唯一映射（各判定点共用，防散写漂移）：
    // Created/Modified/FileSize → File；Duration/ReleaseDate → Media；其余 → None
    static MetaCategory metaCategoryOf(SortKey key);
    // N29：预读判定内核（各判定点共用）——该表该排序键仍需预读的类别，
    // 无需预读返回 None（类别 None、支持者缺失、对应标志已置均视为无需）。
    // 只含"类别 / 支持者 / 标志位"三要素，busy / entriesLoaded / 占位等
    // 外围条件由调用方自判；共用即保证"填充跳过直排"与"收尾补排"等各点一致
    MetaCategory pendingMetaCategory(const QListWidget *list, SortKey key) const;
    // N29：对应类别支持者谓词（收敛按类别选支持者谓词的三元散写）
    bool anyEntitySupports(const QListWidget *list, MetaCategory cat) const;

    // ——结构变更公共骨架——
    // 三步法之第 1 步：抄录记忆行对应的 playUrl；第 3 步：在新序列中找回并写回
    QString captureMemoryPath(QListWidget *list) const;
    void restoreMemoryPath(QListWidget *list, const QString &captured, bool hadMemory);
    // N30：回刷收编后的记忆重建。内容被整体替换，旧记忆的行号与路径一并失效，
    // 但 mpv 仍在播新表里的某一项——按正播行重建记忆（定位不到才作废），
    // 使随后的四步式（排序 / 移除）始终有续播目标，无需任何兜底
    void rebuildMemoryAfterAdopt(QListWidget *list, int playingRow);

    // 排序执行：非播放 tab 直接重排；播放 tab 走四步式
    void sortCurrent(SortKey key, bool ascending);
    // 未知值恒归末尾组

    // 移除无效项 / 重复项（含四步式与本地直删两条路由）
    void removeInvalidItems();
    void removeDuplicateItems();
    // 收集待删行号后统一执行的收尾：播放 tab 四步式后半段或本地直删
    void executeRemoval(const QList<int> &rows);
    // N25：播放 tab 的本地先行删行（走逐行 playlist-remove 同步通道时用）：
    // 逆序删行 + 记忆复位 + 刷新导航，不含四步式的停止/重推
    void removeRowsLocal(QListWidget *list, const QList<int> &rows);

    // 右键菜单动作实现（第七节）
    void refreshCurrentTab();
    // N35：改收共享计数副本——异步属性对话框跨嵌套事件循环持有实体，
    // 求值转发取回的值经注入口写回该实体（关闭即取消）
    void showPropertiesDialog(const PlaylistEntity::Ptr &entity);
    void copyPathToClipboard(const QString &path);

    // ——N35 服务注册表装配——
    // 初装配（构造期）与重装配（设置变更）共用：读 ini 两项构建 HttpClient
    // 缺省参数，provide 进注册表（重装配为覆盖，注册表身份恒定——实体持有的
    // registry 指针不变，下一次 resolve 即得新 client）
    void provideHttpServices();

    // ——N36 遍历策略装配——
    // 同走 N35 路子：GUI 线程读 ini 构建 TraversalPolicy 快照 provide 进同一注册表，
    // 替换即覆盖（注册表身份恒定）。取值侧在 DirProvider::enumerate 起始处 resolve
    void provideTraversalPolicy();

    // 正在播放条目所在行（PlayingRole 命中处），无则 -1
    int playingRowOf(const QListWidget *list) const;

    // N26：实体所有权仓（序 == 行序）与全局时长仓（mpv 推送，键为规范化路径）
    QHash<const QListWidget *, PlaylistEntity::List> m_entities;
    QHash<QString, std::optional<double>> m_duration;
    QListWidget *m_menuTargetList = nullptr; // 右键命中的列表与行（空白处为 nullptr）
    int m_menuTargetRow = -1;

    int m_playingTab = 0; // N25：恒 >= 0（默认 0）；同步目标失效时归位 0，不再悬空
    QString m_configFile;
    MpvController *m_mpv = nullptr; // 弱引用，attachController 注入（N4 进度恢复用）
    // N35：服务注册表（初始化列表创建，此后恒非空；设置变更时只覆盖其内部实例，
    // 注册表身份恒定——收编 / 拖放 / 打开 URL 经工厂注入实体，实体每次求值现查）
    // N36：TraversalPolicy 同注册于此；创建与装配分离，两个 provide 函数各管一项
    std::shared_ptr<ServiceRegistry> m_registry;
    // 以 list 指针为键：不受 tab 增删导致的索引漂移影响
    QHash<const QListWidget *, TabMemory> m_memory;
    // N9：每 tab 提供者（list 创建即配静态默认，加载/设置时替换）。
    // N28：shared_ptr 入 QHash——枚举线程 lambda 持有副本，换源/删 tab 后
    // 在途提供者的销毁被自动延后到线程结束，无 use-after-free
    QHash<const QListWidget *, std::shared_ptr<PlaylistProvider>> m_providerOf;

    // ——N19 per-tab 加载状态——
    struct TabLoadState {
        bool entriesLoaded = false;   // 内容已就位（N28：创建/换源/恢复即 false，
                                      // 首次激活后置位；N30：默认 tab 由直写路径
                                      // 维护，恒 true，刷新则显式置 false 后重枚举）
        bool mediaMetaLoaded = false; // 本表 mediaMeta 已全表读取完成
                                      // （N29 更名自 mediaInfoLoaded）
        bool fileMetaLoaded = false;  // 本表 fileMeta 已全表读取完成（N29）
        QString loadError;            // 非空 = 上次枚举失败（占位行显示中）
    };
    QHash<const QListWidget *, TabLoadState> m_loadState;

    // ——N19 忙时与单飞行（N32：枚举与预读共用一个任务槽）——
    JobSlot m_job;         // 当前在途任务（枚举 / 预读同一槽位，全插件同一时刻至多一个）
    quint64 m_token = 0;   // 代际计数器；唯一递增点：runJob 起任务、abandonJob 作废
    bool m_busy = false;   // 加载在途或预读中：tab 栏 + 列表 + 右键菜单冻结
    int m_pendingTab = -1; // 忙时排队的程序化切 tab（last-wins）
    LoadingOverlay *m_overlay = nullptr;
    LoadingOverlay::Mode m_overlayMode = LoadingOverlay::Mode::Loading;
    int m_overlayTotal = 0;
    QTimer m_overlayTimer; // 遮罩 300ms 延迟显示（可取消）
    // ——N19 收尾挂起（N32：只作发起期意图来源；任务的一切上下文随 JobContext 按值走）——
    struct PendingLoad {
        QPointer<QListWidget> list;
        bool selectFirst = false; // 完成后选中首项（列表设置换源）
        bool refresh = false;     // 刷新批次：收尾需比对与播放同步
    };
    PendingLoad m_pendingLoad;
    // ——N25 本地在途回显——
    struct LocalPush {
        QStringList snapshot; // 本地变更后的最终序列（回显比对基准）
        bool pending = false; // 有尚未回显确认的本地序列
    };
    LocalPush m_localPush;
};

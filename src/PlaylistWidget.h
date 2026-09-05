#pragma once

#include "pch.h"

#include "LoadingOverlay.h"
#include "MediaInfoHelper.h"
#include "PlaylistItem.h"
#include "PlaylistProvider.h"

class MpvController;

// 右侧播放列表：每个 tab 对应一个播放列表，tab 标题为列表名。
// 约定：第一个 tab 名为"默认"，不可删除；其余 tab 持久化保存到 INI 配置文件。
// 只有一个 tab 是"正在播放列表"（与 mpv 播放列表同步），初始为第一个 tab；
// 双击任意 tab 中的条目会把该 tab 整个载入 mpv 并播放对应条目，同时该 tab 成为新的同步目标。
//
// N4 起条目只携带路径字符串；元数据存于全局仓 m_meta（路径 → PlaylistItem），
// 由委托按路径查仓绘制。排序与清理对播放中 tab 统一走四步式：
// 停止 → 本地变更 → playRequested 整表推送并从原文件恢复。
//
// N18：原 mpv 元数据回填（media-metadata / file-format / tracks / chapters）已从
// 本类移除，对话框读源改走 MediaInfoHelper 缓存；duration 是唯一保留的 mpv 写入。
class PlaylistWidget : public QTabWidget {
    Q_OBJECT
  public:
    // 条目数据角色约定
    static constexpr int PathRole = Qt::UserRole;            // 文件路径字符串
    static constexpr int PlayingRole = Qt::UserRole + 1;     // 正在播放标记（bool）
    static constexpr int PlaceholderRole = Qt::UserRole + 2; // N19 枚举失败占位行标记（bool）

    explicit PlaylistWidget(const QString &configFile = QString(), QWidget *parent = nullptr);
    ~PlaylistWidget() override; // N19：在途枚举线程 wait(2000) 收尾

    // 注入控制器（弱引用）：四步式停止前读取 time-pos 用于进度恢复
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
    // 动态提供者 tab 判定（内容来自枚举；静态为手工条目）
    bool isDynamicTab(int tabIndex) const;
    // 表头菜单"新建播放列表"：弹列表设置对话框后建 tab（动态则枚举填充）
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

  protected:
    // N5：ShortcutOverride 认领导航键——焦点在列表上时 ↑↓←→/PgUp/PgDn/Home/End
    // 走列表原生导航，全局音量动作（↑/↓）本次不触发。
    // 该事件定向投递焦点控件、不沿父链冒泡，故过滤器必须装在每个列表自身
    bool eventFilter(QObject *watched, QEvent *event) override;

  signals:
    // 请求整表载入并播放 files 中第 index 个；seekTo > 0 时就绪后恢复到该进度
    // （N4 四步式的进度恢复与此同一通道；发出前已将 tab 标记为正在播放列表）
    void playRequested(const QStringList &files, int index, double seekTo);
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

    // 单个 tab 的播放记忆（默认 tab 仅进程内有效，持久化时跳过索引 0）。
    // N9：lastPath 为记忆条目规范化路径，恢复时序号校验失败按其寻位
    struct TabMemory {
        int lastPos = -1;
        double lastTimePos = -1.0;
        QString lastPath;
    };

    QListWidget *createList();
    QListWidget *listAt(int index) const;

    // ——N9 内部辅助——
    PlaylistProvider *providerAt(int tabIndex) const;
    // 同步填充（静态 tab 路径）：enumerate → 解包 → fillListFromEntries
    void fillFromProvider(QListWidget *list);
    // GUI 线程填充：清空 → 逐条 materialize + makeItem → 应用非元数据排序规则
    // （Duration/ReleaseDate 且 MediaInfo 可用时跳过，由预读后的 applySort 执行）
    void fillListFromEntries(QListWidget *list, const QStringList &entries);
    // ——N19 异步加载与忙时——
    void activateTab(int index); // 原 onTabChanged 体：选中记忆行/播放行滚动
    void startLoadAsync(QListWidget *list);
    void finishLoad(QListWidget *list, const std::expected<QStringList, QString> &r);
    void setBusy(bool on);
    void flushPendingTab();
    void setListsEnabled(bool on);
    void armOverlay(LoadingOverlay::Mode mode, int total);
    // ——N19 元数据预读——
    bool runMetaScan(QPointer<QListWidget> list, int total);
    QStringList allPathsOf(QListWidget *list) const;
    void applySort(QListWidget *list, SortKey key, bool ascending, bool writeRule);
    // 预读公共骨架（不碰 m_busy，调用方负责 setBusy 包裹）：遮罩 + 循环 + 收尾重绘；
    // 成功置位 mediaInfoLoaded 并返回 true，取消/销毁返回 false
    bool runMetaPrescan(QListWidget *list);
    // N20：按表精确驱逐——遍历该表非占位行，按仓键逐个删 m_meta/m_mediaInfo；flag 重置所有表
    // （仓 duration 回填耦合共享条目，他表 flag 为 true 会导致时长列空而不重读）
    void evictListKeys(QListWidget *list);
    // 已加载 tab 切入重读判定：内容已载、无占位、标志失效、排序依赖元数据
    bool needsMetaRescan(int tabIndex) const;
    // 已加载 tab 切入即重读：预读 → 排序 → 选中跟随（忙时包裹）
    void reactivateRescan(QListWidget *list, int tabIndex);
    // ——N19 失败占位——
    static bool isPlaceholderRow(const QListWidgetItem *item);
    void showPlaceholder(QListWidget *list, const QString &error);
    bool clearPlaceholder(QListWidget *list); // 返回是否清掉过占位行
    // 条目重排核心（无四步式副作用）：排序/加载填充共用
    void sortListItems(QListWidget *list, SortKey key, bool ascending);

    // 全局元数据仓：缺失键补建并计算文件系统字段（图标/大小/创建/修改时间）；
    // path 约定为入口处已加工的显示路径，原样存入 item.path 供 UI/回传 mpv/持久化
    PlaylistItem &materialize(const QString &path);
    // 为 list 的全部条目补齐元数据（切 tab / 重建后调用）
    void materializeList(QListWidget *list);
    // 统一条目构造：显示文件名、图标取自仓、tooltip 完整路径
    QListWidgetItem *makeItem(QListWidget *list, const QString &path);
    // 元数据回填后触发所有列表行重绘
    void repaintAllLists();

    // ——结构变更公共骨架——
    // 三步法之第 1 步：抄录记忆行对应的路径；第 3 步：在新序列中找回并写回
    QString captureMemoryPath(QListWidget *list) const;
    void restoreMemoryPath(QListWidget *list, const QString &captured, bool hadMemory);

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
    void showPropertiesDialog(const QString &path);
    void copyPathToClipboard(const QString &path);

    // 正在播放条目所在行（PlayingRole 命中处），无则 -1
    int playingRowOf(const QListWidget *list) const;

    QHash<QString, PlaylistItem> m_meta;     // 全局元数据仓：路径 → 条目
    QFileIconProvider m_iconProvider;        // 值语义：具体类无多态需求
    QListWidget *m_menuTargetList = nullptr; // 右键命中的列表与行（空白处为 nullptr）
    int m_menuTargetRow = -1;
    // N18：MediaInfo 缓存（键为 normalizePathForMatch 后的规范化路径），
    // 仅服务按 Duration/ReleaseDate 排序与属性对话框惰性填充
    QHash<QString, MediaInfoHelper::Data> m_mediaInfo;
    // N18：MediaInfo 缓存惰性查询；dll 不可用返回 nullptr；命中即返回，未命中同步解析一次入缓存
    const MediaInfoHelper::Data *mediaInfoOf(const QString &path);

    int m_playingTab = 0; // N25：恒 >= 0（默认 0）；同步目标失效时归位 0，不再悬空
    QString m_configFile;
    MpvController *m_mpv = nullptr; // 弱引用，attachController 注入（N4 进度恢复用）
    // 以 list 指针为键：不受 tab 增删导致的索引漂移影响
    QHash<const QListWidget *, TabMemory> m_memory;
    // N9：每 tab 提供者（list 创建即配静态默认，加载/设置时替换）。
    // unique_ptr 自动回收；unordered_map 容纳不可拷贝值类型
    std::unordered_map<const QListWidget *, std::unique_ptr<PlaylistProvider>> m_providerOf;

    // ——N19 per-tab 加载状态——
    struct TabLoadState {
        bool entriesLoaded = false;   // 提供者枚举已执行（静态 tab 创建即置位）
        bool mediaInfoLoaded = false; // 当前排序键所需 MediaInfo 已全表读取
        QString loadError;            // 非空 = 上次枚举失败（占位行显示中）
    };
    QHash<const QListWidget *, TabLoadState> m_loadState;

    // ——N19 忙时与单飞行——
    bool m_busy = false;             // 加载在途或预读中：tab 栏 + 列表 + 右键菜单冻结
    int m_pendingTab = -1;           // 忙时排队的程序化切 tab（last-wins）
    QThread *m_loadThread = nullptr; // 在途枚举线程（禁切 tab 后同时最多一个）
    QPointer<QListWidget>
        m_loadingList;     // 在途加载的目标 tab（仅 GUI 线程读写；回刷同 tab 才递增 generation）
    quint64 m_loadGen = 0; // 单调 generation：旧批次收尾与同 tab 回刷重建时失配丢弃
    // N19：跨线程存活旗（堆上共享，工作线程只读此旗；QPointer 禁止在工作线程读写）。
    // 析构先置 false 再 wait；收尾调用经 invokeMethod 上下文机制双保险
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    LoadingOverlay *m_overlay = nullptr;
    LoadingOverlay::Mode m_overlayMode = LoadingOverlay::Mode::Loading;
    int m_overlayTotal = 0;
    QTimer m_overlayTimer; // 遮罩 300ms 延迟显示（可取消）
    // ——N19 预读状态——
    bool m_metaCancel = false; // 取消按钮已点，下一个文件边界生效
    int m_scanDone = 0;
    bool m_scanSuppressRepaint = false; // 预读期间抑制 mediaInfoOf 逐条重绘
    // ——N19 收尾挂起（refresh 播放恢复 / 设置页首选项 / 保存）——
    struct PendingLoad {
        QPointer<QListWidget> list;
        bool selectFirst = false; // 完成后选中首项（列表设置换源）
        QString resumePath;       // 非空 = 动态刷新播放中 tab，收尾递延 stop→续播
        bool saveAfter = false;   // 完成后 savePlaylists（动态刷新非播放非默认 tab）
    };
    PendingLoad m_pendingLoad;
    // ——N25 本地在途回显——
    struct LocalPush {
        QStringList snapshot; // 本地变更后的最终序列（回显比对基准）
        bool pending = false; // 有尚未回显确认的本地序列
    };
    LocalPush m_localPush;
};

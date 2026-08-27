#pragma once

#include "pch.h"

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
class PlaylistWidget : public QTabWidget
{
    Q_OBJECT
public:
    // 条目数据角色约定
    static constexpr int PathRole = Qt::UserRole;        // 文件路径字符串
    static constexpr int PlayingRole = Qt::UserRole + 1; // 正在播放标记（bool）

    explicit PlaylistWidget(const QString &configFile = QString(), QWidget *parent = nullptr);

    // 注入控制器（弱引用）：四步式停止前读取 time-pos 用于进度恢复
    void attachController(MpvController *mpv) { m_mpv = mpv; }

    // 路径规范化：统一正斜杠 + cleanPath。全链路（存键/mpv回显比对）必须一致，
    // 否则 mpv 推送按其回显路径写入会落进幻影条目
    static QString normalizePath(const QString &path);

    // 用 mpv 播放列表内容刷新"正在播放"tab：重建行、写入播放标记、跟随滚动
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

    // ——N9 提供者——
    // 动态提供者 tab 判定（内容来自枚举；静态为手工条目）
    bool isDynamicTab(int tabIndex) const;
    // 表头菜单"新建播放列表"：弹列表设置对话框后建 tab（动态则枚举填充）
    void createNewList();
    // 表头菜单"列表设置…"：存量列表改名/换提供者。非法参数报错留在对话框；
    // 合法后静默清空重建；仅改名则直接生效不清空。播放中 tab 换提供者时
    // 停止并选中首项不播（同步目标悬空防回刷竞态）
    void openListSettings(int tabIndex);
    // 删除指定 tab（表头菜单，作用于右键命中的 tab）
    void removeTabAt(int tabIndex);

    // 持久化：从配置文件恢复非默认播放列表 / 保存除默认外的所有列表
    void loadPlaylists();
    void savePlaylists();

    // ——元数据回填（MainWindow 按"当前正播路径"路由至此）——
    void applyDuration(const QString &path, double seconds);
    void applyMediaMetadata(const QString &path, const QVariantMap &tags);
    void applyFileFormat(const QString &path, const QString &format);
    void applyTracks(const QString &path, const QList<MpvController::TrackInfo> &tracks);

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

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private slots:
    void onItemDoubleClicked(QListWidgetItem *item);
    void onTabChanged(int index);

private:
    // 排序规则枚举（N4/N9）：None=添加顺序。取值随提供者持久化（int 不透明存储）
    enum class SortKey {
        None,
        FileName,
        FileNameNumeric,
        Extension,
        Created,
        Modified,
        Duration,
        FileSize,
        ReleaseDate,
    };

    // 单个 tab 的播放记忆（默认 tab 仅进程内有效，持久化时跳过索引 0）。
    // N9：lastPath 为记忆条目规范化路径，恢复时序号校验失败按其寻位
    struct TabMemory
    {
        int lastPos = -1;
        double lastTimePos = -1.0;
        QString lastPath;
    };

    QListWidget *createList();
    QListWidget *listAt(int index) const;

    // ——N9 内部辅助——
    PlaylistProvider *providerAt(int tabIndex) const;
    // 动态 tab：清空现有条目 → enumerate → 按该 tab 排序规则重排 → 物化填充
    void fillFromProvider(QListWidget *list);
    // 条目重排核心（无四步式副作用）：排序/加载填充共用
    void sortListItems(QListWidget *list, SortKey key, bool ascending);

    // 全局元数据仓：缺失键补建并计算文件系统字段（图标/大小/创建/修改时间）
    PlaylistItem &materialize(const QString &path);
    // 变体：先规范化取仓条目，并把原始路径（首次写入者）存入 path 字段供 UI 显示
    PlaylistItem &materializeByRaw(const QString &rawPath);
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

    // 右键菜单动作实现（第七节）
    void refreshCurrentTab();
    void showPropertiesDialog(const QString &path);
    void copyPathToClipboard(const QString &path);

    // 正在播放条目所在行（PlayingRole 命中处），无则 -1
    int playingRowOf(const QListWidget *list) const;

    QHash<QString, PlaylistItem> m_meta;   // 全局元数据仓：路径 → 条目
    QFileIconProvider m_iconProvider;      // 值语义：具体类无多态需求
    QListWidget *m_menuTargetList = nullptr; // 右键命中的列表与行（空白处为 nullptr）
    int m_menuTargetRow = -1;

    int m_playingTab = 0;
    QString m_configFile;
    MpvController *m_mpv = nullptr; // 弱引用，attachController 注入（N4 进度恢复用）
    // 以 list 指针为键：不受 tab 增删导致的索引漂移影响
    QHash<const QListWidget *, TabMemory> m_memory;
    // N9：每 tab 提供者（list 创建即配静态默认，加载/设置时替换）。
    // unique_ptr 自动回收；unordered_map 容纳不可拷贝值类型
    std::unordered_map<const QListWidget *, std::unique_ptr<PlaylistProvider>> m_providerOf;
};

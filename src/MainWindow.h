#pragma once

#include "pch.h"

#include "MpvController.h"

// N54：两处接线要用到嵌套类型（PlaylistWidget::LevelJump / JumpFailure、
// BookmarkManager::Origin），嵌套类型必须有完整类型可见
#include "BookmarkManager.h"
#include "PlaylistWidget.h"

class NavigatorPanel;
class ActionManager;
class BookmarkEditorDialog;
class ChapterSlider;
class MarkAggregator; // N15：章节+书签标记聚合器（定义于 MainWindow.cpp）
class StreamRelay;    // N52：mpv 自定义协议适配层（定义于 StreamRelay.h）

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    // N52：relay 由 plugin.cpp 构造并 install 后传入，构造期收下（不在 show()
    // 之后才装配——列表构造期就要能从注册表取到 relay）；本类仅持有与转交
    // PlaylistWidget（provide 进注册表），不做其他使用
    explicit MainWindow(MpvController *controller, const QString &configFile = QString(),
                        std::shared_ptr<StreamRelay> relay = nullptr, QWidget *parent = nullptr);

    // 视频显示区域（mpv 通过 wid 嵌入的目标窗口）
    QWidget *videoWidget() const { return m_videoWidget; }

  protected:
    void closeEvent(QCloseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // N40 变更：还原最大化时按当前视频重新适配（跟随全屏退出重适配逻辑）
    void changeEvent(QEvent *event) override;
    // A20：启动几何收口的一次性触发点（首次 show 时外框几何与所在屏才可靠）
    void showEvent(QShowEvent *event) override;
    // N3：占位符几何跟随 + 视频区单击/双击分流
    bool eventFilter(QObject *watched, QEvent *event) override;
    // N5：空白区域点击清焦（未被交互控件消费的按下事件冒泡至此）
    void mousePressEvent(QMouseEvent *event) override;

  private slots:
    void openFiles();
    void togglePause();
    void stop();
    void playlistPrev();
    void playlistNext();
    void seekReleased();
    void setPlaylistVisible(bool visible);
    void setNavigatorVisible(bool visible);
    void ensureSettingsDialog(); // R3：按需创建/唤起设置对话框（非模态）
    void showAboutDialog();
    // N6：速度控制（读改写 speed 属性，钳制 0.25~8）
    void changeSpeed(double delta);
    void resetSpeed();
    // N38：静音状态联动单一出口（菜单勾选 + 图标按钮同一处刷新，不播报 OSD）
    void applyMuteState(bool muted);
    // 统一播放入口：整表载入 → 跳转 index → seekTo>0 时经挂起机制恢复进度。
    // N56 §2.7：播放链路传实体表——下发的 loadfile 与在途快照（noteLocalPush）都取
    // playUrls(tab, entities)，即喂 mpv 前最后一刻映射的播放形态（与回显同源同形）。
    // N30：keepPaused 为真时载入后保持暂停（四步式在 stop 前读到的 pause 状态），
    // 使暂停态下的刷新 / 排序 / 移除无效项不误起播
    void onPlayRequested(const PlaylistEntity::List &entities, int tab, int index, double seekTo,
                         bool keepPaused = false);
    void onFilesAdded(const PlaylistEntity::List &entities);
    void onRemoveRequested(QList<int> rows);
    void onClearRequested();
    void onMpvShutdown();
    // N54：书签层级定位的两种结局（PlaylistWidget 只切层与定位，播放与报错在此决定）。
    // entityIndex 是目标层的实体序（即 mpv playlist-pos 语义）
    void onLevelItemReady(const PlaylistWidget::LevelJump &req, int entityIndex);
    void onLevelItemFailed(const PlaylistWidget::LevelJump &req, PlaylistWidget::JumpFailure why,
                           const QString &detail);

  private:
    // R3：音量上限推送回调（由 MpvController::volumeMaxChanged 驱动）
    void applyVolumeMax(int v);
    // N56：注册表维护（自 PlaylistWidget 上移装配顶点）——初装配（构造期）与
    // 重装配（设置变更）共用，读 ini 构建 HttpClient 缺省参数 provide 进注册表；
    // 覆盖式更新（注册表身份恒定——实体持有的 registry 指针不变，下一次求值
    // resolve 即得新 client）
    void provideHttpServices();
    // N36 遍历策略同路：读 ini 构建 TraversalPolicy 快照 provide 进同一注册表；
    // 取值侧在 DirProvider::enumerate 起始处 resolve
    void provideTraversalPolicy();
    // N56 §2.7：正播实体——凡"以当前内容为对象"的书签 / 聚合器 / OSD / duration
    // 读点统一经此取实体，不再读 mpv 属性。零参：playlist-pos 的真相源是本类的
    // m_playlistPos 镜像，取实体的坐标对在此收口；无正播（停止态 / 镜像未就位）
    // 返回空 Ptr。返回 Ptr 而非 optional<Ptr>：shared_ptr 本身就是可空句柄，
    // 再包一层 optional 只会引入语义上不存在区分的第二重空态
    PlaylistEntity::Ptr playingEntity() const;
    // N56：注册表维护的设置变更入口（自 PlaylistWidget 上移）——命中相关键时
    // 重读 ini 覆盖注册表内部实例，下一次求值即生效
    void onPluginIniKeysChanged(const QStringList &keys);
    void createMenus();
    QWidget *createLeftArea();
    // A15：无状态按钮统一接线——事件来源走 action trigger，启停跟随 action enabled
    void bindButton(QAbstractButton *btn, QAction *action);
    void connectController();
    // 导航器浮窗定位到主窗口右下角（N1）
    void repositionNavigator();
    void updateTimeDisplay();
    void playAt(int index);
    // 停止态的上一个/下一个步进（A9/A16）：以当前选中列表为基准并开始播放；
    // 列表循环开启时首尾互绕，否则越界无动作
    void stepStopped(bool next);
    // A16：上一/下一动作三路联动启停（矩阵见 specs/A16.md）
    void updateNavigationActions();
    // 进入停止状态：停止播放（保留列表）、复位播放按钮图标
    void enterStoppedState();
    // N3：覆盖式全屏、视频区几何同步与单击/双击分流
    void toggleFullscreen();
    void syncVideoGeometry();
    // N10：普通态窗口尺寸自适应视频（videoSizeChanged 直连触发，
    // 物理像素 1:1、工作区钳制、最小移动收回；尺寸事实源经 N40 改滤镜后输出尺寸）
    void adaptWindowSizeToVideo(int videoW, int videoH);
    // A20：播放列表显隐几何联动——左 pane 宽度不变、主窗口向右扩展，
    // 空间不足先压视频区触底再压播放列表宽度（实测值写回记忆）
    void applyPlaylistGeometry(bool expanding, int leftAnchor);
    // A20：启动几何收口（定宽 → 定高 → 居中），仅首次 show 执行
    void applyStartupGeometry();
    // A20：主窗口居中到所在屏工作区（外框口径，居中后钳回兜底）
    void centerOnScreen();
    // A20：最小移动钳回——外框矩形按两轴独立、越界多少移多少收进区域，
    // 返回外框左上角（未越界原样返回）；MainWindow 内多处复用
    QPoint minimalMoveTopLeft(const QRect &frame, const QRect &area) const;
    bool isPaused();
    void onVideoClickTimeout();
    // N5：统一音量调节入口——钳制到 [0, 上限] 后下发 mpv，UI 经观察回流刷新
    void changeVolume(int delta);
    // B14：用户音量动作统一收口——钳制、下发并直显音量 OSD；
    // 启动恢复路径刻意绕行（restoreSettings 直写，无 OSD）
    void applyVolume(int value);
    // N6：相对 seek（秒）；停止态（无播放位置）无动作
    void seekRelative(int deltaSeconds);
    // N3：导航器按需创建生命周期收口（关闭即销毁，null=未打开）
    void ensureNavigator();
    void destroyNavigator();
    // 持久化：关闭时保存 / 启动时恢复
    void saveSettings();
    void restoreSettings();
    // 循环模式："none" / "file" / "playlist"
    void applyLoopMode(const QString &mode);
    QString currentLoopMode() const;
    static QString formatTime(double seconds);
    // N13：轨道文案与切换
    QString formatTrackText(const MpvController::TrackInfo &t) const;
    qint64 trackBitrate(const MpvController::TrackInfo &t) const;
    void rebuildTrackMenus();
    // B18：流按钮与流子菜单状态统一由 m_tracks 派生
    void updateStreamControls();
    void showStreamMenu();
    void switchToTrack(const MpvController::TrackInfo &t);
    void disableSubtitles();

    // N43：播放菜单的滤镜子菜单（视频在前）——条目与启用态在播放菜单弹出时
    // 按 af / vf 属性实时重建；点击条目翻转其启用位并整串下发（运行期态，
    // 不写 conf）
    void rebuildFilterMenus();
    void toggleFilterEntry(const QString &property, int index);

    // N15：章节与书签
    void rebuildChapterMenu();
    void updateChapterMenuState();
    void updateChapterMenuChecked();
    void updateChapterSlider();
    // N54：跳转拆成两条语义不共用的路。章节语义是"当前文件的这一时刻"，只能在手上
    // 现有的列表里解决（jumpToChapter 即现 jumpToFileTime 函数体逐字改名沿用）；
    // 书签语义是"回到它所属的列表与所在层"，只认书签自带的身份，不再扫描别的列表
    void jumpToChapter(const QString &path, double time);
    void jumpToBookmark(const QString &path, double time, const BookmarkManager::Origin &origin);
    // N54 §2.2 第 4 步的回退档：把这一个文件作为会话镜像推入默认 tab（仅本地项走到这里）
    void fallbackPlayInDefaultTab(const QString &path, double time);
    // N54：书签跳转的报错出口——正文一句人话，路径与提供者回传原文进 informativeText
    void warnBookmarkJump(const QString &reason, const QString &path, const QString &detail);
    void ensureBookmarkEditor();
    void onAddBookmark();

    MpvController *m_controller;
    QString m_configFile; // 配置文件完整路径（插件 DLL 同目录下的 mpv-qt.ini）
    // N52：转接协议适配层（寿命归 plugin.cpp 的 shared_ptr，覆盖 exec 全程）
    std::shared_ptr<StreamRelay> m_relay;
    ActionManager *m_actions = nullptr; // A15：全部 UI 动作出生与登记处，先于容器构建
    // N56 §2.7：服务注册表由装配顶点（本类）创建并持有，注入 PlaylistWidget；
    // relay 的 provide<StreamRelayIf> 亦在此完成（早于任何提供者出生）
    std::shared_ptr<ServiceRegistry> m_registry;
    QSplitter *m_splitter = nullptr;
    // 视频显示区域（mpv wid 嵌入目标）：主窗直接子控件（N3），
    // 脱离布局管理由 syncVideoGeometry 手动同步，父链构造期定死后全程不动
    QWidget *m_videoWidget = nullptr;
    // N3：布局槽位占位符，普通态视频区几何的锚点；最小尺寸约束在此
    QWidget *m_videoPlaceholder = nullptr;
    // N40：最近一次收到的视频尺寸发布值——去重收口后每次都是一致对、
    // 只在真实变化时到达，不再需要合并缓冲定时器；仅退出全屏的重适配路径取用
    int m_pendingVideoW = 0;
    int m_pendingVideoH = 0;
    // B16：全屏期间若切换了视频（videoSizeChanged 被守卫跳过），退出普通态时
    // 需按当前视频尺寸重新适配；同一视频则维持 N10"全屏退出不重新适配"语义
    bool m_refitAfterFullscreen = false;
    // N40 变更：最大化期间若切换了视频（适配被 isMaximized 守卫拦下），还原
    // 最大化时按当前视频尺寸重新适配——跟随全屏退出重适配的同款语义
    bool m_refitAfterMaximize = false;
    ChapterSlider *m_progressSlider = nullptr; // N15：进度条（带章节/书签标记层）
    QSlider *m_volumeSlider = nullptr;
    // N5：音量调节配置（INI 键 volumeMax / volumeWheelStep / volumeKeyStep）
    int m_volumeMax = 130;     // R3：音量上限，mpv 默认 130；运行时由 applyVolumeMax 同步
    int m_volumeWheelStep = 1; // 滚轮每格步长
    int m_volumeKeyStep = 5;   // 方向键每格步长
    // N38：静音——菜单项与音量行图标按钮共用同一 QAction（勾选态由 mpv 推送驱动）
    QAction *m_muteAction = nullptr;
    QToolButton *m_muteButton = nullptr;
    QPushButton *m_playButton = nullptr;
    QLabel *m_timeLabel = nullptr;
    QPushButton *m_playlistToggle = nullptr;
    QAction *m_playlistAction = nullptr;
    PlaylistWidget *m_playlist = nullptr;
    // ---- A20：播放列表显隐几何联动 ----
    int m_playlistWidth = 0; // 进程内记忆的播放列表宽度（客户区像素）；0 = 未定，用默认值
    bool m_playlistGeometryPending = false; // 构造期展开，几何调整延后到首次 show
    bool m_firstShowDone = false;           // showEvent 一次性守卫（最小化恢复/全屏退出也会触发）
    QAction *m_navigatorAction = nullptr;   // 视图菜单"导航器"勾选项（N1）
    NavigatorPanel *m_navigator = nullptr;  // 导航器浮窗（N3 按需创建）：null=未打开
    QAction *m_fullscreenAction = nullptr;  // 视图菜单"全屏"勾选项（N3）

    double m_duration = 0.0;
    double m_timePos = 0.0;
    bool m_seeking = false;
    int m_playlistPos = -1; // mpv 当前播放项，-1 表示停止状态
    bool m_startupPlaybackCheckDone = false;
    int m_pendingSeekRow = -1;       // >=0：有挂起的会话恢复 seek，值为目标行；-1 无
    double m_pendingSeekTime = -1.0; // 挂起 seek 的目标进度（秒），配合 m_pendingSeekRow 使用
    // 会话数据每次进程只落盘一次（A11）：二次保存在窗口隐藏后触发，
    // isVisible() 等活体状态会被读错（playlistExpanded 曾被覆写为 false）
    bool m_settingsSaved = false;
    // 本次进程是否发生过播放（A13）：未播放就关闭属于会话延续，
    // 不得清除上次保存的 lastTabName
    bool m_playedThisSession = false;
    // ---- N7：VideoOverlay 视频叠加层（A3 顶级 Tool 浮窗，仅 OSD/字幕/弹幕，不含控制栏）----
    QWidget *m_videoOverlay = nullptr; // 顶级浮窗容器（VideoOverlay），几何始终跟随 m_videoWidget
    QLabel *m_osdLabel = nullptr;      // OSD 文本（左上角 12px 内边距）
    QTimer *m_osdTimer = nullptr;      // OSD 2s 自动隐藏
    // 帧合并去重：同事件循环内多信号折叠为一次显示（#2 > #3 > #1）
    bool m_osdCoalescePending = false;
    QString m_pendingNowPlaying;
    QString m_pendingStop;
    QString m_pendingPlayPause;
    void showOsd(const QString &text);
    void repositionVideoOverlay();
    void flushOsd();
    void ensureVideoOverlay();

    // ---- N8：主窗口显示优化 ----
    QString playingFilename(); // 正播真实文件名（含后缀，标题与 OSD 同源）
    // N25：标题与 OSD #2 同源刷新（B11 镜像取名）。playlist-pos 值不变不再推送，
    // 回刷收编后的正播名错位须经此主动刷新
    void updateNowPlayingUi();
    double
    progressPercentAt(const QPoint &pos) const; // 进度条鼠标 x → 百分比（0~100），无效返回 -1

    // ---- N3：单击/双击分流与覆盖式全屏 ----
    QTimer *m_clickTimer = nullptr;     // 单次定时器，间隔=双击间隔
    bool m_pendingPauseRestore = false; // 播放中 press 已暂停，双击需恢复播放
    bool m_clickActionPending = false;  // 暂停/停止态 press 待超时执行
    // B14：一次性吞咽标志——视频区单击引发的暂停/恢复属性回声不入 OSD 队列；
    // 暂停 OSD 由单击超时补显，双击场景全程静默
    bool m_suppressPlayPauseOsd = false;
    bool m_fullscreen = false; // 覆盖式全屏态标志（syncVideoGeometry 分支依据）
    double m_savedZoom = 0.0;  // 进全屏前 zoom/pan 快照（先存后写零）
    double m_savedPanX = 0.0;
    double m_savedPanY = 0.0;
    bool m_preFsMaximized = false; // 进全屏前是否最大化（退出分流依据）
    // A18 无框全屏：进全屏前完整几何快照（含位置，普通态恢复用）+ 窗口样式快照；
    // 落盘守卫与退出恢复共用
    QRect m_preFsRect;
    Qt::WindowFlags m_preFsFlags;

    // ---- N13：轨道选择 ----
    QList<MpvController::TrackInfo> m_tracks;
    QMenu *m_videoStreamMenu = nullptr;
    QMenu *m_audioStreamMenu = nullptr;
    QMenu *m_subStreamMenu = nullptr;
    QPushButton *m_streamButton = nullptr; // ♪ 流选择按钮

    // ---- N43：滤镜子菜单（条目弹出时按属性实时重建，不作缓存）----
    QMenu *m_videoFilterMenu = nullptr;
    QMenu *m_audioFilterMenu = nullptr;

    // ---- N15：章节与书签 ----
    QMenu *m_chapterMenu = nullptr;
    BookmarkManager *m_bookmarkMgr = nullptr;
    BookmarkEditorDialog *m_bookmarkEditor = nullptr;
    // R3：设置对话框（非模态，按需创建）
    class SettingsDialog *m_settingsDialog = nullptr;
    QActionGroup *m_chapterGroup = nullptr;
    // 章节+书签标记聚合器（MainWindow.cpp 内实现）：合并规则 + 缓存
    // 收口于此，主窗口不再持有抽象成员；进程生命周期随主窗口
    MarkAggregator *m_marks = nullptr;
};

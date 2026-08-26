#pragma once

#include "pch.h"

#include "MpvController.h"

class NavigatorPanel;
class PlaylistWidget;
class ActionManager;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(MpvController *controller, const QString &configFile = QString(),
                        QWidget *parent = nullptr);

    // 视频显示区域（mpv 通过 wid 嵌入的目标窗口）
    QWidget *videoWidget() const { return m_videoWidget; }

protected:
    void closeEvent(QCloseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
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
    void showSettingsDialog();
    void showAboutDialog();
    // N6：速度控制（读改写 speed 属性，钳制 0.25~8）
    void changeSpeed(double delta);
    void resetSpeed();
    // 统一播放入口：整表载入 → 跳转 index → seekTo>0 时经挂起机制恢复进度
    void onPlayRequested(const QStringList &files, int index, double seekTo);
    void onFilesAdded(const QStringList &files);
    void onRemoveRequested(QList<int> rows);
    void onClearRequested();
    void onMpvShutdown();

private:
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
    // 物理像素 1:1、工作区钳制、最小移动收回）
    void adaptWindowSizeToVideo(int videoW, int videoH);
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
    void updateStreamButton();
    void showStreamMenu();
    void switchToTrack(const MpvController::TrackInfo &t);
    void disableSubtitles();

    MpvController *m_controller;
    QString m_configFile; // 配置文件完整路径（插件 DLL 同目录下的 mpv-qt.ini）
    ActionManager *m_actions = nullptr; // A15：全部 UI 动作出生与登记处，先于容器构建
    QSplitter *m_splitter = nullptr;
    // 视频显示区域（mpv wid 嵌入目标）：主窗直接子控件（N3），
    // 脱离布局管理由 syncVideoGeometry 手动同步，父链构造期定死后全程不动
    QWidget *m_videoWidget = nullptr;
    // N3：布局槽位占位符，普通态视频区几何的锚点；最小尺寸约束在此
    QWidget *m_videoPlaceholder = nullptr;
    QSlider *m_progressSlider = nullptr;
    QSlider *m_volumeSlider = nullptr;
    // N5：音量调节配置（INI 键 volumeMax / volumeWheelStep / volumeKeyStep）
    int m_volumeMax = 100;      // 音量上限，设置对话框自由填写域 0~400
    int m_volumeWheelStep = 1;  // 滚轮每格步长
    int m_volumeKeyStep = 5;    // 方向键每格步长
    QPushButton *m_playButton = nullptr;
    QLabel *m_timeLabel = nullptr;
    QPushButton *m_playlistToggle = nullptr;
    QAction *m_playlistAction = nullptr;
    PlaylistWidget *m_playlist = nullptr;
    QAction *m_navigatorAction = nullptr; // 视图菜单"导航器"勾选项（N1）
    NavigatorPanel *m_navigator = nullptr; // 导航器浮窗（N3 按需创建）：null=未打开
    QAction *m_fullscreenAction = nullptr; // 视图菜单"全屏"勾选项（N3）

    double m_duration = 0.0;
    double m_timePos = 0.0;
    bool m_seeking = false;
    int m_playlistPos = -1;   // mpv 当前播放项，-1 表示停止状态
    bool m_startupPlaybackCheckDone = false;
    int m_pendingSeekRow = -1;      // >=0：有挂起的会话恢复 seek，值为目标行；-1 无
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
    QString playingFilename();                          // 正播真实文件名（含后缀，标题与 OSD 同源）
    double progressPercentAt(const QPoint &pos) const;  // 进度条鼠标 x → 百分比（0~100），无效返回 -1

    // ---- N3：单击/双击分流与覆盖式全屏 ----
    QTimer *m_clickTimer = nullptr;     // 单次定时器，间隔=双击间隔
    bool m_pendingPauseRestore = false; // 播放中 press 已暂停，双击需恢复播放
    bool m_clickActionPending = false;  // 暂停/停止态 press 待超时执行
    // B14：一次性吞咽标志——视频区单击引发的暂停/恢复属性回声不入 OSD 队列；
    // 暂停 OSD 由单击超时补显，双击场景全程静默
    bool m_suppressPlayPauseOsd = false;
    bool m_fullscreen = false;          // 覆盖式全屏态标志（syncVideoGeometry 分支依据）
    double m_savedZoom = 0.0;           // 进全屏前 zoom/pan 快照（先存后写零）
    double m_savedPanX = 0.0;
    double m_savedPanY = 0.0;
    bool m_preFsMaximized = false;      // 进全屏前是否最大化（退出分流依据）
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
};

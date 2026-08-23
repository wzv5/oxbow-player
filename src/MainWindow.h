#pragma once

#include <QMainWindow>
#include <QString>

class QAction;
class QAbstractButton;
class QLabel;
class QPushButton;
class QTimer;
class QSlider;
class QSplitter;
class MpvController;
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
    void showEvent(QShowEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // N3：占位符几何跟随 + 视频区单击/双击分流
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    void onPlayRequested(const QStringList &files, int index);
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
    bool isPaused();
    void onVideoClickTimeout();
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
    // B7：构造期对未显示窗口设置最大化会被平台层丢弃，记标志待首次显示应用
    bool m_pendingMaximize = false;
    // ---- N3：单击/双击分流与覆盖式全屏 ----
    QTimer *m_clickTimer = nullptr;     // 单次定时器，间隔=双击间隔
    bool m_pendingPauseRestore = false; // 播放中 press 已暂停，双击需恢复播放
    bool m_clickActionPending = false;  // 暂停/停止态 press 待超时执行
    bool m_fullscreen = false;          // 覆盖式全屏态标志（syncVideoGeometry 分支依据）
    double m_savedZoom = 0.0;           // 进全屏前 zoom/pan 快照（先存后写零）
    double m_savedPanX = 0.0;
    double m_savedPanY = 0.0;
    bool m_preFsMaximized = false;      // 进全屏前是否最大化（退出分流依据）
    // A18 无框全屏：进全屏前完整几何快照（含位置，普通态恢复用）+ 窗口样式快照；
    // 落盘守卫与退出恢复共用
    QRect m_preFsRect;
    Qt::WindowFlags m_preFsFlags;
};

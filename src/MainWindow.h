#pragma once

#include <QMainWindow>
#include <QString>

class QAction;
class QLabel;
class QPushButton;
class QSlider;
class QSplitter;
class MpvController;
class NavigatorPanel;
class PlaylistWidget;

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
    void connectController();
    // 导航器浮窗定位到主窗口右下角（N1）
    void repositionNavigator();
    void updateTimeDisplay();
    void playAt(int index);
    // 停止态的上一个/下一个步进（A9）：以当前选中列表为基准并开始播放
    void stepStopped(int delta);
    // 进入停止状态：停止播放（保留列表）、复位播放按钮图标
    void enterStoppedState();
    // 持久化：关闭时保存 / 启动时恢复
    void saveSettings();
    void restoreSettings();
    // 循环模式："none" / "file" / "playlist"
    void applyLoopMode(const QString &mode);
    QString currentLoopMode() const;
    static QString formatTime(double seconds);

    MpvController *m_controller;
    QString m_configFile; // 配置文件完整路径（插件 DLL 同目录下的 mpv-qt.ini）
    QSplitter *m_splitter = nullptr;
    QWidget *m_videoWidget = nullptr;
    QSlider *m_progressSlider = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QPushButton *m_playButton = nullptr;
    QLabel *m_timeLabel = nullptr;
    QPushButton *m_playlistToggle = nullptr;
    QAction *m_playlistAction = nullptr;
    PlaylistWidget *m_playlist = nullptr;
    QAction *m_navigatorAction = nullptr; // 视图菜单"导航器"勾选项（N1）
    NavigatorPanel *m_navigator = nullptr;

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
};

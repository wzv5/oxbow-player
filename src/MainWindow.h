#pragma once

#include <QMainWindow>

class QAction;
class QLabel;
class QPushButton;
class QSlider;
class QSplitter;
class MpvController;
class PlaylistWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(MpvController *controller, QWidget *parent = nullptr);

    // 视频显示区域（mpv 通过 wid 嵌入的目标窗口）
    QWidget *videoWidget() const { return m_videoWidget; }

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openFiles();
    void togglePause();
    void stop();
    void playlistPrev();
    void playlistNext();
    void seekReleased();
    void setPlaylistVisible(bool visible);
    void showSettingsDialog();
    void showAboutDialog();
    void onPlayRequested(const QStringList &files, int index);
    void onFilesAdded(const QStringList &files);
    void onRemoveRequested(QList<int> rows);
    void onClearRequested();

private:
    void createMenus();
    QWidget *createLeftArea();
    void connectController();
    void updateTimeDisplay();
    void playAt(int index);
    // 进入停止状态：停止播放（保留列表）、复位播放按钮图标
    void enterStoppedState();
    // 循环模式："none" / "file" / "playlist"
    void applyLoopMode(const QString &mode);
    QString currentLoopMode() const;
    static QString formatTime(double seconds);

    MpvController *m_controller;
    QSplitter *m_splitter = nullptr;
    QWidget *m_videoWidget = nullptr;
    QSlider *m_progressSlider = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QPushButton *m_playButton = nullptr;
    QLabel *m_timeLabel = nullptr;
    QPushButton *m_playlistToggle = nullptr;
    QAction *m_playlistAction = nullptr;
    PlaylistWidget *m_playlist = nullptr;

    double m_duration = 0.0;
    double m_timePos = 0.0;
    bool m_seeking = false;
    int m_playlistPos = -1;   // mpv 当前播放项，-1 表示停止状态
    int m_lastPos = -1;       // 最近一个有效播放项，用于停止状态下恢复/上下首
    int m_playlistCount = 0;
};

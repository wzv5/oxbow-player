#include "MainWindow.h"

#include "MpvController.h"
#include "PlaylistWidget.h"

#include <algorithm>

#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStyle>
#include <QVBoxLayout>

MainWindow::MainWindow(MpvController *controller, const QString &configFile, QWidget *parent)
    : QMainWindow(parent), m_controller(controller), m_configFile(configFile)
{
    setWindowTitle(QStringLiteral("mpv-qt"));

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(createLeftArea());

    m_playlist = new PlaylistWidget(configFile, m_splitter);
    m_splitter->addWidget(m_playlist);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setCollapsible(0, false);
    m_splitter->setCollapsible(1, true);
    m_playlist->setVisible(false); // 播放列表默认隐藏
    m_playlist->loadPlaylists();

    setCentralWidget(m_splitter);
    createMenus();
    connectController();

    restoreSettings();
}

QWidget *MainWindow::createLeftArea()
{
    auto *left = new QWidget;
    auto *layout = new QVBoxLayout(left);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // 第一行：视频显示区域
    m_videoWidget = new QWidget;
    m_videoWidget->setAttribute(Qt::WA_NativeWindow);
    m_videoWidget->setAttribute(Qt::WA_DontCreateNativeAncestors);
    m_videoWidget->setMinimumSize(480, 270);
    m_videoWidget->setAutoFillBackground(true);
    QPalette pal = m_videoWidget->palette();
    pal.setColor(QPalette::Window, Qt::black);
    m_videoWidget->setPalette(pal);
    layout->addWidget(m_videoWidget, 1);

    // 第二行：进度条 + 音量条（行固定高度，音量条固定宽度）
    auto *seekRow = new QWidget;
    seekRow->setFixedHeight(28);
    auto *seekLayout = new QHBoxLayout(seekRow);
    seekLayout->setContentsMargins(8, 0, 8, 0);
    m_progressSlider = new QSlider(Qt::Horizontal);
    m_progressSlider->setRange(0, 1000);
    m_volumeSlider = new QSlider(Qt::Horizontal);
    m_volumeSlider->setRange(0, 130);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(100);
    seekLayout->addWidget(m_progressSlider, 1);
    seekLayout->addSpacing(8);
    seekLayout->addWidget(new QLabel(tr("音量")));
    seekLayout->addWidget(m_volumeSlider);
    layout->addWidget(seekRow);

    // 第三行：控制按钮行（固定高度）
    auto *controlRow = new QWidget;
    controlRow->setFixedHeight(36);
    auto *controlLayout = new QHBoxLayout(controlRow);
    controlLayout->setContentsMargins(8, 0, 8, 4);
    m_playButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), QString());
    auto *stopButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop), QString());
    auto *prevButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaSkipBackward), QString());
    auto *nextButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaSkipForward), QString());
    m_timeLabel = new QLabel(QStringLiteral("00:00 / 00:00"));
    auto *settingsButton = new QPushButton(tr("设置"));
    m_playlistToggle = new QPushButton(tr("播放列表"));
    m_playlistToggle->setCheckable(true);

    m_playButton->setToolTip(tr("播放/暂停"));
    stopButton->setToolTip(tr("停止"));
    prevButton->setToolTip(tr("上一个文件"));
    nextButton->setToolTip(tr("下一个文件"));
    m_playlistToggle->setToolTip(tr("显示/隐藏播放列表"));

    controlLayout->addWidget(m_playButton);
    controlLayout->addWidget(stopButton);
    controlLayout->addWidget(prevButton);
    controlLayout->addWidget(nextButton);
    controlLayout->addSpacing(8);
    controlLayout->addWidget(m_timeLabel);
    controlLayout->addStretch(1);
    controlLayout->addWidget(settingsButton);
    controlLayout->addWidget(m_playlistToggle);
    layout->addWidget(controlRow);

    connect(m_playButton, &QPushButton::clicked, this, &MainWindow::togglePause);
    connect(stopButton, &QPushButton::clicked, this, &MainWindow::stop);
    connect(prevButton, &QPushButton::clicked, this, &MainWindow::playlistPrev);
    connect(nextButton, &QPushButton::clicked, this, &MainWindow::playlistNext);
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::showSettingsDialog);
    connect(m_playlistToggle, &QPushButton::toggled, this, [this](bool on) {
        if (m_playlistAction)
            m_playlistAction->setChecked(on); // 经 QAction 统一到 setPlaylistVisible
        else
            setPlaylistVisible(on);
    });

    connect(m_progressSlider, &QSlider::sliderPressed, this, [this] { m_seeking = true; });
    connect(m_progressSlider, &QSlider::sliderReleased, this, &MainWindow::seekReleased);
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        m_controller->setProperty(QStringLiteral("volume"), double(value));
    });

    return left;
}

void MainWindow::createMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    QAction *openAction = fileMenu->addAction(tr("打开文件(&O)…"), this, &MainWindow::openFiles);
    openAction->setShortcut(QKeySequence::Open);
    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(tr("退出(&Q)"), this, &QWidget::close);
    quitAction->setShortcut(QKeySequence::Quit);

    QMenu *playMenu = menuBar()->addMenu(tr("播放(&P)"));
    QAction *pauseAction = playMenu->addAction(tr("播放/暂停(&P)"), this, &MainWindow::togglePause);
    pauseAction->setShortcut(Qt::Key_Space);
    playMenu->addAction(tr("停止(&S)"), this, &MainWindow::stop);
    playMenu->addSeparator();
    playMenu->addAction(tr("上一个文件(&R)"), this, &MainWindow::playlistPrev);
    playMenu->addAction(tr("下一个文件(&N)"), this, &MainWindow::playlistNext);

    QMenu *viewMenu = menuBar()->addMenu(tr("视图(&V)"));
    m_playlistAction = viewMenu->addAction(tr("播放列表(&L)"));
    m_playlistAction->setCheckable(true);
    m_playlistAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    connect(m_playlistAction, &QAction::toggled, this, &MainWindow::setPlaylistVisible);

    QMenu *helpMenu = menuBar()->addMenu(tr("帮助(&H)"));
    helpMenu->addAction(tr("关于(&A)"), this, &MainWindow::showAboutDialog);
}

void MainWindow::connectController()
{
    connect(m_controller, &MpvController::timePosChanged, this, [this](double v) {
        m_timePos = v;
        updateTimeDisplay();
    });
    connect(m_controller, &MpvController::durationChanged, this, [this](double v) {
        m_duration = v;
        updateTimeDisplay();
    });
    connect(m_controller, &MpvController::pauseChanged, this, [this](bool paused) {
        m_playButton->setIcon(style()->standardIcon(paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
    });
    connect(m_controller, &MpvController::volumeChanged, this, [this](double v) {
        QSignalBlocker blocker(m_volumeSlider);
        m_volumeSlider->setValue(int(v + 0.5));
    });
    connect(m_controller, &MpvController::playlistChanged, m_playlist, &PlaylistWidget::setPlaylist);
    connect(m_controller, &MpvController::playlistChanged, this, [this](const QVariantList &items) {
        m_playlistCount = items.size();
    });
    connect(m_controller, &MpvController::playlistPosChanged, this, [this](int pos) {
        m_playlistPos = pos;
        if (pos >= 0)
            m_lastPos = pos; // 持续记录最近播放项，停止/列表播完后以此为恢复基准
    });
    // 列表播完或停止后 mpv 进入空闲，复位播放按钮为“可播放”态
    connect(m_controller, &MpvController::idleActive, this, [this](bool active) {
        if (active)
            m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    });

    connect(m_playlist, &PlaylistWidget::playRequested, this, &MainWindow::onPlayRequested);
    connect(m_playlist, &PlaylistWidget::filesAdded, this, &MainWindow::onFilesAdded);
    connect(m_playlist, &PlaylistWidget::removeRequested, this, &MainWindow::onRemoveRequested);
    connect(m_playlist, &PlaylistWidget::clearRequested, this, &MainWindow::onClearRequested);
}

void MainWindow::openFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("打开文件"), QString(),
        tr("媒体文件 (*.mp4 *.mkv *.avi *.mov *.wmv *.flv *.webm *.ts *.mp3 *.flac *.aac *.ogg *.wav *.m4a);;所有文件 (*)"));
    if (!files.isEmpty())
        m_playlist->addFiles(files); // filesAdded 信号中再决定是否需要 append 到 mpv
}

void MainWindow::playAt(int index)
{
    m_restoreTimePos = -1.0; // 用户手动指定，清除会话恢复标记
    m_controller->setProperty(QStringLiteral("playlist-pos"), index);
    m_controller->setProperty(QStringLiteral("pause"), false);
    // 若 pause 原本就是 no，mpv 不会再推送属性变化，图标需手动更新
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
}

void MainWindow::togglePause()
{
    if (m_playlistPos < 0) {
        // 上次会话恢复：加载上次的非默认 tab 到 mpv，从选中项开始播放
        if (m_restoreTimePos >= 0.0) {
            const double seekTarget = m_restoreTimePos;
            const int tab = m_playlist->currentIndex();
            const int row = m_playlist->currentSelectedRow();
            const QStringList files = m_playlist->filesOf(tab);
            m_restoreTimePos = -1.0; // 清除，避免重复触发
            if (!files.isEmpty() && row >= 0 && row < files.size()) {
                onPlayRequested(files, row);
                // 有播放进度：等文件加载完成后跳转
                if (seekTarget > 0.0) {
                    connect(m_controller, &MpvController::durationChanged, this,
                            [this, seekTarget](double d) {
                                if (d > 0.0)
                                    m_controller->command({QStringLiteral("seek"),
                                                           QString::number(seekTarget), QStringLiteral("absolute")});
                            }, Qt::SingleShotConnection);
                }
                return;
            }
        }

        // 停止状态：播放上次停止时的文件，否则播放列表第一个
        if (m_playlistCount > 0) {
            const int target = (m_lastPos >= 0 && m_lastPos < m_playlistCount) ? m_lastPos : 0;
            playAt(target);
        }
        return;
    }
    m_controller->command({QStringLiteral("cycle"), QStringLiteral("pause")});
}

void MainWindow::enterStoppedState()
{
    // stop 默认会清空播放列表，keep-playlist 标志只停止播放
    m_controller->command({QStringLiteral("stop"), QStringLiteral("keep-playlist")});
    // stop 不改变 pause 属性，按钮图标不会自动更新，手动置为“可播放”态
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
}

void MainWindow::stop()
{
    enterStoppedState();
}

void MainWindow::playlistPrev()
{
    if (m_playlistPos < 0) {
        // 停止状态：以最近播放项为基准向前
        const int base = m_lastPos >= 0 ? m_lastPos : 0;
        if (base - 1 >= 0 && m_playlistCount > 0)
            playAt(base - 1);
        return;
    }
    m_controller->command({QStringLiteral("playlist-prev")});
}

void MainWindow::playlistNext()
{
    if (m_playlistPos < 0) {
        // 停止状态：以最近播放项为基准向后
        const int base = m_lastPos >= 0 ? m_lastPos : 0;
        if (base + 1 < m_playlistCount)
            playAt(base + 1);
        return;
    }
    m_controller->command({QStringLiteral("playlist-next")});
}

void MainWindow::seekReleased()
{
    m_seeking = false;
    const double percent = m_progressSlider->value() / 10.0;
    m_controller->command({QStringLiteral("seek"), QString::number(percent), QStringLiteral("absolute-percent")});
}

void MainWindow::setPlaylistVisible(bool visible)
{
    m_playlist->setVisible(visible);
    QSignalBlocker blocker(m_playlistToggle);
    m_playlistToggle->setChecked(visible);
}

void MainWindow::onPlayRequested(const QStringList &files, int index)
{
    m_restoreTimePos = -1.0; // 用户双击播放，清除会话恢复标记
    if (files.isEmpty())
        return;
    // mpv 的 playlist-clear 会保留当前播放的条目，先 stop 才能真正清空
    m_controller->command({QStringLiteral("stop")});
    m_controller->command({QStringLiteral("playlist-clear")});
    for (const QString &file : files)
        m_controller->command({QStringLiteral("loadfile"), file, QStringLiteral("append-play")});
    m_controller->setProperty(QStringLiteral("playlist-pos"), index);
    // pause 状态在文件切换后仍然保持，双击播放时显式恢复播放
    m_controller->setProperty(QStringLiteral("pause"), false);
}

void MainWindow::onRemoveRequested(QList<int> rows)
{
    // 从后往前删，避免索引失效
    std::sort(rows.rbegin(), rows.rend());
    for (int row : rows)
        m_controller->command({QStringLiteral("playlist-remove"), QString::number(row)});
}

void MainWindow::onClearRequested()
{
    // 同样需要先 stop，否则当前播放的条目会被保留
    m_controller->command({QStringLiteral("stop")});
    m_controller->command({QStringLiteral("playlist-clear")});
}

void MainWindow::onFilesAdded(const QStringList &files)
{
    // 只有添加到“正在播放”tab 的文件才同步进 mpv 播放列表；
    // 其他 tab 为本地列表，双击播放时整体载入。
    if (!m_playlist->currentIsPlayingTab())
        return;
    for (const QString &file : files)
        m_controller->command({QStringLiteral("loadfile"), file, QStringLiteral("append-play")});
}

void MainWindow::applyLoopMode(const QString &mode)
{
    if (mode == QLatin1String("file")) {
        m_controller->setProperty(QStringLiteral("loop-file"), QStringLiteral("inf"));
        m_controller->setProperty(QStringLiteral("loop-playlist"), QStringLiteral("no"));
    } else if (mode == QLatin1String("playlist")) {
        m_controller->setProperty(QStringLiteral("loop-file"), QStringLiteral("no"));
        m_controller->setProperty(QStringLiteral("loop-playlist"), QStringLiteral("inf"));
    } else {
        m_controller->setProperty(QStringLiteral("loop-file"), QStringLiteral("no"));
        m_controller->setProperty(QStringLiteral("loop-playlist"), QStringLiteral("no"));
    }
}

QString MainWindow::currentLoopMode() const
{
    if (m_controller->propertyString(QStringLiteral("loop-file")) == QLatin1String("inf"))
        return QStringLiteral("file");
    if (m_controller->propertyString(QStringLiteral("loop-playlist")) == QLatin1String("inf"))
        return QStringLiteral("playlist");
    return QStringLiteral("none");
}

void MainWindow::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("设置"));
    auto *layout = new QFormLayout(&dialog);

    auto *hwdecCombo = new QComboBox(&dialog);
    hwdecCombo->addItems({QStringLiteral("auto"), QStringLiteral("auto-safe"),
                          QStringLiteral("d3d11va"), QStringLiteral("dxva2"), QStringLiteral("no")});
    const int idx = hwdecCombo->findText(m_controller->propertyString(QStringLiteral("hwdec")));
    if (idx >= 0)
        hwdecCombo->setCurrentIndex(idx);
    layout->addRow(tr("硬件解码"), hwdecCombo);

    auto *loopCombo = new QComboBox(&dialog);
    loopCombo->addItem(tr("不循环"), QStringLiteral("none"));
    loopCombo->addItem(tr("单个循环"), QStringLiteral("file"));
    loopCombo->addItem(tr("列表循环"), QStringLiteral("playlist"));
    const int loopIdx = loopCombo->findData(currentLoopMode());
    if (loopIdx >= 0)
        loopCombo->setCurrentIndex(loopIdx);
    layout->addRow(tr("循环播放"), loopCombo);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        m_controller->setProperty(QStringLiteral("hwdec"), hwdecCombo->currentText());

        QSettings settings(m_configFile, QSettings::IniFormat);
        settings.setValue(QStringLiteral("hwdec"), hwdecCombo->currentText());

        const QString loopMode = loopCombo->currentData().toString();
        applyLoopMode(loopMode);
        settings.setValue(QStringLiteral("loopMode"), loopMode);
    }
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("关于 mpv-qt"),
                       tr("mpv-qt：以 mpv C 插件形式加载的 Qt 播放器界面。\n"
                          "视频渲染由 mpv 完成，本插件仅提供图形界面。"));
}

void MainWindow::saveSettings()
{
    QSettings settings(m_configFile, QSettings::IniFormat);

    // 音量
    const double volume = m_controller->propertyString(QStringLiteral("volume")).toDouble();
    settings.setValue(QStringLiteral("volume"), volume);

    // 播放列表面板可见性
    settings.setValue(QStringLiteral("playlistExpanded"), m_playlist->isVisible());

    // 窗口：最大化时只保存状态，不保存具体尺寸
    if (isMaximized()) {
        settings.setValue(QStringLiteral("windowMaximized"), true);
    } else {
        settings.setValue(QStringLiteral("windowMaximized"), false);
        settings.setValue(QStringLiteral("windowWidth"), width());
        settings.setValue(QStringLiteral("windowHeight"), height());
    }

    // 当前播放 tab（跳过默认 tab 0）和播放项
    // 只在有正在播放的内容时更新会话数据；没有播放时保留旧值
    const int playingTab = m_playlist->playingTabIndex();
    if (playingTab > 0) {
        settings.setValue(QStringLiteral("lastTab"), playingTab);
        if (m_playlistPos >= 0) {
            settings.setValue(QStringLiteral("lastPos"), m_playlistPos);
            if (m_timePos > 0.0)
                settings.setValue(QStringLiteral("lastTimePos"), m_timePos);
            else
                settings.remove(QStringLiteral("lastTimePos"));
        } else {
            settings.remove(QStringLiteral("lastPos"));
            settings.remove(QStringLiteral("lastTimePos"));
        }
    }
}

void MainWindow::restoreSettings()
{
    QSettings settings(m_configFile, QSettings::IniFormat);

    // 循环模式
    const QString loopMode = settings.value(QStringLiteral("loopMode"), QStringLiteral("none")).toString();
    applyLoopMode(loopMode);

    // 硬件解码
    const QString hwdec = settings.value(QStringLiteral("hwdec")).toString();
    if (!hwdec.isEmpty())
        m_controller->setProperty(QStringLiteral("hwdec"), hwdec);

    // 音量
    if (settings.contains(QStringLiteral("volume"))) {
        const double vol = qBound(0.0, settings.value(QStringLiteral("volume")).toDouble(), 130.0);
        m_controller->setProperty(QStringLiteral("volume"), vol);
        QSignalBlocker blocker(m_volumeSlider);
        m_volumeSlider->setValue(int(vol + 0.5));
    }

    // 窗口尺寸 / 最大化
    if (settings.value(QStringLiteral("windowMaximized"), false).toBool()) {
        setWindowState(Qt::WindowMaximized);
    } else if (settings.contains(QStringLiteral("windowWidth"))) {
        resize(settings.value(QStringLiteral("windowWidth"), 960).toInt(),
               settings.value(QStringLiteral("windowHeight"), 600).toInt());
    } else {
        resize(960, 600);
    }

    // 播放列表面板可见性
    if (settings.value(QStringLiteral("playlistExpanded"), false).toBool())
        setPlaylistVisible(true);

    // 上次播放 tab 和位置（仅非默认 tab；只选中高亮，不自动播放）
    const int lastTab = settings.value(QStringLiteral("lastTab"), -1).toInt();
    const int lastPos = settings.value(QStringLiteral("lastPos"), -1).toInt();
    if (lastTab > 0 && lastTab < m_playlist->count() && lastPos >= 0) {
        m_playlist->selectItem(lastTab, lastPos);
        // 标记为"点击播放时应恢复会话"：进度 >0 表示有待恢复的播放进度，0 表示只恢复选中项
        if (settings.contains(QStringLiteral("lastTimePos")))
            m_restoreTimePos = settings.value(QStringLiteral("lastTimePos")).toDouble();
        else
            m_restoreTimePos = 0.0;
    }
}

void MainWindow::updateTimeDisplay()
{
    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(formatTime(m_timePos), formatTime(m_duration)));
    if (!m_seeking && m_duration > 0.0)
        m_progressSlider->setValue(int(m_timePos / m_duration * 1000.0));
}

QString MainWindow::formatTime(double seconds)
{
    const int total = int(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    // mpv 是宿主进程：关闭 GUI 时让 mpv 一并退出
    m_controller->command({QStringLiteral("quit")});
    QMainWindow::closeEvent(event);
}

#include "MainWindow.h"

#include "MpvController.h"
#include "PlaylistWidget.h"

#include <algorithm>

#include <QApplication>
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

    // 控制行按钮不接受焦点（C2）：Space 归全局快捷键，避免焦点按钮被 Space 二次触发
    for (QPushButton *b : {m_playButton, stopButton, prevButton, nextButton, settingsButton, m_playlistToggle})
        b->setFocusPolicy(Qt::NoFocus);

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
        // per-tab 记忆：持续刷新播放中 tab 的进度（纯内存，落盘发生在保存时）。
        // v>0 守卫：STOP 的 time-pos 失效事件（归零 0.0）先于 playlist-pos 失效
        // 到达时不得把记忆覆写为 0，否则停止动作本身会销毁续播进度。
        // 默认 tab 也记录（pt>=0），但仅进程内有效——持久化侧天然跳过索引 0
        const int pt = m_playlist->playingTabIndex();
        if (pt >= 0 && m_playlistPos >= 0 && v > 0.0)
            m_playlist->recordPlayback(pt, m_playlistPos, v);
    });
    connect(m_controller, &MpvController::durationChanged, this, [this](double v) {
        m_duration = v;
        updateTimeDisplay();
        if (v <= 0.0) {
            // 停止/卸载后时长失效，进度条归零（C1）；时间显示由 updateTimeDisplay 处理
            QSignalBlocker blocker(m_progressSlider);
            m_progressSlider->setValue(0);
        }
        // 会话恢复的挂起 seek：仅当就绪的 duration 属于目标条目时执行一次，
        // 避免被其他条目（如加载过程中的瞬态条目）的 duration 事件误导
        if (m_pendingSeekRow >= 0 && v > 0.0 && m_playlistPos == m_pendingSeekRow) {
            const double target = m_pendingSeekTime;
            m_pendingSeekRow = -1;
            m_pendingSeekTime = -1.0;
            m_controller->command({QStringLiteral("seek"), QString::number(target), QStringLiteral("absolute")});
        }
    });
    connect(m_controller, &MpvController::pauseChanged, this, [this](bool paused) {
        m_playButton->setIcon(style()->standardIcon(paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
    });
    connect(m_controller, &MpvController::volumeChanged, this, [this](double v) {
        QSignalBlocker blocker(m_volumeSlider);
        m_volumeSlider->setValue(int(v + 0.5));
    });
    connect(m_controller, &MpvController::playlistChanged, m_playlist, &PlaylistWidget::setPlaylist);
    connect(m_controller, &MpvController::playlistPosChanged, this, [this](int pos) {
        m_playlistPos = pos;
        if (pos >= 0) {
            m_playedThisSession = true; // A13：标记本进程发生过播放
            // per-tab 记忆：记录播放目标 tab 的最后播放项（默认 tab 仅进程内）
            const int pt = m_playlist->playingTabIndex();
            if (pt >= 0)
                m_playlist->recordPlayback(pt, pos, m_timePos);
            // 启动后首次检测到播放（如命令行参数打开文件）：切换到正在播放的 tab
            if (!m_startupPlaybackCheckDone) {
                m_startupPlaybackCheckDone = true;
                const int pt = m_playlist->playingTabIndex();
                if (pt >= 0 && m_playlist->currentIndex() != pt)
                    m_playlist->setCurrentIndex(pt);
            }
        }
    });
    // 列表播完或停止后 mpv 进入空闲，复位播放按钮与进度条（C1）
    connect(m_controller, &MpvController::idleActive, this, [this](bool active) {
        if (active) {
            m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
            QSignalBlocker blocker(m_progressSlider);
            m_progressSlider->setValue(0);
        }
    });

    connect(m_playlist, &PlaylistWidget::playRequested, this, &MainWindow::onPlayRequested);
    connect(m_playlist, &PlaylistWidget::filesAdded, this, &MainWindow::onFilesAdded);
    connect(m_playlist, &PlaylistWidget::removeRequested, this, &MainWindow::onRemoveRequested);
    connect(m_playlist, &PlaylistWidget::clearRequested, this, &MainWindow::onClearRequested);
    // mpv 侧退出：补保存会话后再结束事件循环（A6）
    connect(m_controller, &MpvController::mpvShutdown, this, &MainWindow::onMpvShutdown);
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
    m_pendingSeekRow = -1;   // 手动播放丢弃未执行的挂起 seek，防陈旧跳转
    m_controller->setProperty(QStringLiteral("playlist-pos"), index);
    m_controller->setProperty(QStringLiteral("pause"), false);
    // 若 pause 原本就是 no，mpv 不会再推送属性变化，图标需手动更新
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
}

void MainWindow::togglePause()
{
    if (m_playlistPos < 0) {
        // 停止态点播放：目标恒为当前选中的 tab（A8）。
        // 有播放记忆（含默认 tab 的进程内记忆）→ 从记忆项与进度续播；
        // 无记忆 → 选中行优先，否则第一项；不兜底旧播放列表。
        const int cur = m_playlist->currentIndex();
        const QStringList files = m_playlist->filesOf(cur);
        if (files.isEmpty())
            return;
        const double restoreTime = m_playlist->lastTimePosOf(cur);
        const int memPos = m_playlist->lastPosOf(cur);
        int row;
        if (restoreTime >= 0.0 && memPos >= 0 && memPos < files.size()) {
            row = memPos; // 续播记忆项
        } else {
            row = m_playlist->currentSelectedRow();
            if (row < 0 || row >= files.size())
                row = 0;
        }
        // 载入即转移同步目标：mpv 列表回刷写进当前 tab，关闭时 lastTabName 随之正确
        m_playlist->markPlayingTab(cur);
        onPlayRequested(files, row);
        // 有播放进度：挂起 seek，等目标条目的 duration 就绪后在 durationChanged 中执行一次
        if (restoreTime > 0.0) {
            m_pendingSeekRow = row;
            m_pendingSeekTime = restoreTime;
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
        stepStopped(-1);
        return;
    }
    m_controller->command({QStringLiteral("playlist-prev")});
    // 始终选中正在播放的列表（A9）
    const int pt = m_playlist->playingTabIndex();
    if (pt >= 0 && m_playlist->currentIndex() != pt)
        m_playlist->setCurrentIndex(pt);
}

void MainWindow::playlistNext()
{
    if (m_playlistPos < 0) {
        stepStopped(1);
        return;
    }
    m_controller->command({QStringLiteral("playlist-next")});
    // 始终选中正在播放的列表（A9）
    const int pt = m_playlist->playingTabIndex();
    if (pt >= 0 && m_playlist->currentIndex() != pt)
        m_playlist->setCurrentIndex(pt);
}

// 停止态的上一个/下一个（A9）：以当前选中列表为基准步进并开始播放。
// base：有记忆取记忆项，否则第 0 项；越界无动作（暂不做循环）。
void MainWindow::stepStopped(int delta)
{
    const int cur = m_playlist->currentIndex();
    const QStringList files = m_playlist->filesOf(cur);
    if (files.isEmpty())
        return;
    int base = m_playlist->lastPosOf(cur);
    if (base < 0 || base >= files.size())
        base = 0;
    const int target = base + delta;
    if (target < 0 || target >= files.size())
        return;
    m_playlist->markPlayingTab(cur);
    onPlayRequested(files, target);
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
    // 三个控件的状态统一由本函数收口（A12）：启动恢复路径不经过
    // 按钮→动作链，若不同步 QAction，两者分叉会导致首次点击空转
    {
        QSignalBlocker blocker(m_playlistToggle);
        m_playlistToggle->setChecked(visible);
    }
    QSignalBlocker actionBlocker(m_playlistAction);
    m_playlistAction->setChecked(visible);
}

void MainWindow::onPlayRequested(const QStringList &files, int index)
{
    m_pendingSeekRow = -1;   // 丢弃未执行的挂起 seek
    if (files.isEmpty())
        return;
    // mpv 的 playlist-clear 会保留当前播放的条目，先 stop 才能真正清空
    m_controller->command({QStringLiteral("stop")});
    m_controller->command({QStringLiteral("playlist-clear")});
    // 全部按原顺序 append（不自动播），最后以 playlist-pos 启动目标条目：
    // 既避免 idle 下 append-play 瞬态播放 file[0]，也保持 mpv 列表顺序与界面一致
    for (int i = 0; i < files.size(); ++i)
        m_controller->command({QStringLiteral("loadfile"), files[i], QStringLiteral("append")});
    m_controller->setProperty(QStringLiteral("playlist-pos"), index);
    // pause 状态在文件切换后仍然保持，双击播放时显式恢复播放
    m_controller->setProperty(QStringLiteral("pause"), false);
    // pause 原本就是 no 时 mpv 不会再推送属性变化，图标需手动更新（同 playAt）
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
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

void MainWindow::onMpvShutdown()
{
    // mpv 侧发起退出（终端按 q、其他脚本发 quit）时 closeEvent 不会触发，
    // 在此补保存会话；窗口关闭路径随后到来的重复保存写同值幂等无害
    saveSettings();
    QApplication::quit();
}

void MainWindow::saveSettings()
{
    // A11：每次进程只落盘一次。关窗路径会保存两次（closeEvent + SHUTDOWN 补存），
    // 第二次发生在窗口隐藏后，isVisible() 等活体状态会被读错
    if (m_settingsSaved)
        return;
    m_settingsSaved = true;

    QSettings settings(m_configFile, QSettings::IniFormat);

    // 音量：读 UI 缓存而非实时查询 mpv——SHUTDOWN 后查询可能失败返回空串，
    // toDouble() 得 0 会把音量误存为 0
    settings.setValue(QStringLiteral("volume"), m_volumeSlider->value());

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

    // 会话恢复入口（Q1 方案 A）：记录播放中的非默认 tab 名。
    // 仅当本次进程发生过播放才更新/清除（A13）：未播放就关闭属于会话延续，
    // 上次保存的 lastTabName 与各 tab 记忆仍然有效，不得清空
    if (m_playedThisSession) {
        const int playingTab = m_playlist->playingTabIndex();
        if (playingTab > 0)
            settings.setValue(QStringLiteral("lastTabName"), m_playlist->tabText(playingTab));
        else
            settings.remove(QStringLiteral("lastTabName"));
    }

    // per-tab 记忆随各播放列表一并落盘
    m_playlist->savePlaylists();
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

    // 上次播放的非默认 tab：按名定位并切换，currentChanged 联动会自动选中
    // 该 tab 记忆的最后播放项；找不到则停留默认 tab。旧全局键一次性弃用不迁移。
    const QString lastTabName = settings.value(QStringLiteral("lastTabName")).toString();
    if (!lastTabName.isEmpty()) {
        const int idx = m_playlist->indexOfName(lastTabName);
        if (idx > 0)
            m_playlist->setCurrentIndex(idx);
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

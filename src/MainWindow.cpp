#include "pch.h"

#include "MainWindow.h"

#include "ActionManager.h"
#include "Constants.h"
#include "MpvController.h"
#include "NavigatorPanel.h"
#include "PlaylistWidget.h"
#include "Util.h"

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <qt_windows.h>
#endif

MainWindow::MainWindow(MpvController *controller, const QString &configFile, QWidget *parent)
    : QMainWindow(parent), m_controller(controller), m_configFile(configFile)
{
    setWindowTitle(App::name());

    m_actions = new ActionManager(this); // A15：先于任何容器构建

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(createLeftArea());

    // 视频显示区域（N3）：主窗直接子控件，脱离布局管理，几何由
    // syncVideoGeometry 手动同步。父链构造期确定后全程不动——setParent 会
    // 销毁重建原生 HWND 使 mpv wid 失效（技术事实 4）。注意：原生子窗口与
    // alien 控件矩形重叠时合成次序未定义（技术事实 3 勘误），故全屏期隐藏菜单栏
    m_videoWidget = new QWidget(this);
    m_videoWidget->setAttribute(Qt::WA_NativeWindow);
    m_videoWidget->setAttribute(Qt::WA_DontCreateNativeAncestors);
    m_videoWidget->setAutoFillBackground(true);
    QPalette videoPal = m_videoWidget->palette();
    videoPal.setColor(QPalette::Window, Qt::black);
    m_videoWidget->setPalette(videoPal);
    m_videoWidget->installEventFilter(this); // 单击/双击分流

    // 单击/双击区分定时器（N3）：间隔取系统双击间隔
    m_clickTimer = new QTimer(this);
    m_clickTimer->setSingleShot(true);
    connect(m_clickTimer, &QTimer::timeout, this, &MainWindow::onVideoClickTimeout);

    m_playlist = new PlaylistWidget(configFile, m_splitter);
    m_playlist->attachController(m_controller); // N4：四步式停止前读取 time-pos
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

    // 导航器浮窗（N3 改按需创建）：冷启动不构造对象，首次打开经
    // ensureNavigator 创建；视口映射需要视频渲染区域的真实尺寸（黑边感知）

    restoreSettings();
}

QWidget *MainWindow::createLeftArea()
{
    auto *left = new QWidget;
    auto *layout = new QVBoxLayout(left);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // 第一行：视频区占位符（N3）——真实视频区已改为主窗直接子控件，
    // 此处仅提供布局槽位与几何锚点；黑色背景保证视觉不变，
    // 最小尺寸约束必须挂在受布局管理的占位符上（对手动的视频区无效）
    m_videoPlaceholder = new QWidget;
    m_videoPlaceholder->setMinimumSize(480, 270);
    m_videoPlaceholder->setAutoFillBackground(true);
    QPalette pal = m_videoPlaceholder->palette();
    pal.setColor(QPalette::Window, Qt::black);
    m_videoPlaceholder->setPalette(pal);
    m_videoPlaceholder->installEventFilter(this); // Resize/Move → syncVideoGeometry
    layout->addWidget(m_videoPlaceholder, 1);

    // 第二行：进度条 + 音量条（行固定高度，音量条固定宽度）
    auto *seekRow = new QWidget;
    seekRow->setFixedHeight(28);
    auto *seekLayout = new QHBoxLayout(seekRow);
    seekLayout->setContentsMargins(8, 0, 8, 0);
    m_progressSlider = new QSlider(Qt::Horizontal);
    m_progressSlider->setRange(0, 1000);
    // N8 Part3：未按下也接收 move，驱动 tooltip 跟随
    m_progressSlider->setMouseTracking(true);
    m_progressSlider->installEventFilter(this);
    // N8 Part5：停止态禁用且归零（起播经 playlistPosChanged 启用）
    m_progressSlider->setEnabled(false);
    m_progressSlider->setValue(0);
    m_volumeSlider = new QSlider(Qt::Horizontal);
    // N5：上限可配置，构造期用成员默认值 100（restoreSettings 读 ini 后再调）
    m_volumeSlider->setRange(0, m_volumeMax);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(100);
    m_volumeSlider->installEventFilter(this); // N5：滚轮调节走统一入口
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

    // A15：全局按钮统一经 bindButton 引用主实例（事件来源 trigger + 启停跟随）
    bindButton(m_playButton, m_actions->action(ActionManager::Id::PlayPause));
    bindButton(stopButton, m_actions->action(ActionManager::Id::Stop));
    bindButton(prevButton, m_actions->action(ActionManager::Id::PrevFile));
    bindButton(nextButton, m_actions->action(ActionManager::Id::NextFile));
    bindButton(settingsButton, m_actions->action(ActionManager::Id::Settings));
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

// A15：无状态按钮统一接线。注意 trigger() 是编程式激活、绕过 enabled 检查
//（只有菜单点击/快捷键分发尊重禁用态），故必须同步 enabledChanged 让按钮
// 随 action 置灰，用户路径才能真正被拦住
void MainWindow::bindButton(QAbstractButton *btn, QAction *action)
{
    connect(btn, &QAbstractButton::clicked, action, &QAction::trigger);
    connect(action, &QAction::enabledChanged, btn, &QWidget::setEnabled);
    btn->setEnabled(action->isEnabled());
}

void MainWindow::createMenus()
{
    using AM = ActionManager;
    auto act = [this](AM::Id id) { return m_actions->action(id); };

    QMenu *fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    fileMenu->addAction(act(AM::Id::Open));
    connect(act(AM::Id::Open), &QAction::triggered, this, &MainWindow::openFiles);
    fileMenu->addAction(act(AM::Id::Settings));
    connect(act(AM::Id::Settings), &QAction::triggered, this, &MainWindow::showSettingsDialog);
    fileMenu->addSeparator();
    fileMenu->addAction(act(AM::Id::Quit));
    connect(act(AM::Id::Quit), &QAction::triggered, this, &QWidget::close);

    QMenu *playMenu = menuBar()->addMenu(tr("播放(&P)"));
    playMenu->addAction(act(AM::Id::PlayPause));
    connect(act(AM::Id::PlayPause), &QAction::triggered, this, &MainWindow::togglePause);
    playMenu->addAction(act(AM::Id::Stop));
    connect(act(AM::Id::Stop), &QAction::triggered, this, &MainWindow::stop);
    playMenu->addSeparator();
    playMenu->addAction(act(AM::Id::PrevFile));
    connect(act(AM::Id::PrevFile), &QAction::triggered, this, &MainWindow::playlistPrev);
    playMenu->addAction(act(AM::Id::NextFile));
    connect(act(AM::Id::NextFile), &QAction::triggered, this, &MainWindow::playlistNext);
    // N5：音量步进（动作快捷键 ↑/↓ 全局生效，此处为菜单可见入口）
    playMenu->addSeparator();
    playMenu->addAction(act(AM::Id::VolumeUp));
    connect(act(AM::Id::VolumeUp), &QAction::triggered, this,
            [this] { changeVolume(m_volumeKeyStep); });
    playMenu->addAction(act(AM::Id::VolumeDown));
    connect(act(AM::Id::VolumeDown), &QAction::triggered, this,
            [this] { changeVolume(-m_volumeKeyStep); });
    // N6：速度控制组
    playMenu->addSeparator();
    playMenu->addAction(act(AM::Id::SpeedUp));
    connect(act(AM::Id::SpeedUp), &QAction::triggered, this,
            [this] { changeSpeed(+0.25); });
    playMenu->addAction(act(AM::Id::SpeedDown));
    connect(act(AM::Id::SpeedDown), &QAction::triggered, this,
            [this] { changeSpeed(-0.25); });
    playMenu->addAction(act(AM::Id::SpeedReset));
    connect(act(AM::Id::SpeedReset), &QAction::triggered, this, &MainWindow::resetSpeed);

    QMenu *viewMenu = menuBar()->addMenu(tr("视图(&V)"));
    m_playlistAction = act(AM::Id::Playlist);
    viewMenu->addAction(m_playlistAction);
    connect(m_playlistAction, &QAction::toggled, this, &MainWindow::setPlaylistVisible);

    m_navigatorAction = act(AM::Id::Navigator);
    viewMenu->addAction(m_navigatorAction);
    connect(m_navigatorAction, &QAction::toggled, this, &MainWindow::setNavigatorVisible);

    // 全屏（N3）：F11/视图菜单/Esc/双击四路共用 toggleFullscreen 收口
    m_fullscreenAction = act(AM::Id::Fullscreen);
    viewMenu->addAction(m_fullscreenAction);
    connect(m_fullscreenAction, &QAction::toggled, this, [this](bool on) {
        if (on != m_fullscreen)
            toggleFullscreen();
    });
    // Esc 隐绑定经管理器取用，守卫逻辑留主窗口：仅全屏态退出，
    // 弹出菜单打开时 Esc 由 popup 自行消费
    connect(m_actions->shortcut(AM::Id::Escape), &QShortcut::activated, this, [this] {
        if (m_fullscreen)
            toggleFullscreen();
    });
    // N6：±5 秒隐绑定 seek，守卫逻辑留主窗口（停止态无播放位置可寻址）
    connect(m_actions->shortcut(AM::Id::SeekForward), &QShortcut::activated, this,
            [this] { seekRelative(+5); });
    connect(m_actions->shortcut(AM::Id::SeekBackward), &QShortcut::activated, this,
            [this] { seekRelative(-5); });

    QMenu *helpMenu = menuBar()->addMenu(tr("帮助(&H)"));
    helpMenu->addAction(act(AM::Id::About));
    connect(act(AM::Id::About), &QAction::triggered, this, &MainWindow::showAboutDialog);
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
        // N7 #1：播放↔暂停 OSD（仅播放态互切，起停由 #2/#3 去重覆盖）
        if (m_playlistPos >= 0) {
            m_pendingPlayPause = paused ? tr("暂停") : tr("播放");
            if (!m_osdCoalescePending) {
                m_osdCoalescePending = true;
                QTimer::singleShot(0, this, &MainWindow::flushOsd);
            }
        }
    });
    connect(m_controller, &MpvController::volumeChanged, this, [this](double v) {
        QSignalBlocker blocker(m_volumeSlider);
        m_volumeSlider->setValue(int(v + 0.5));
        // N7 #4 + N8 Part6：音量 OSD（百分比样式），启动恢复期屏蔽
        if (m_osdReady)
            showOsd(tr("音量: %1%").arg(int(v + 0.5)));
    });
    connect(m_controller, &MpvController::playlistChanged, m_playlist, &PlaylistWidget::setPlaylist);
    // 播放态列表长度变化影响步进启停（A16）
    connect(m_controller, &MpvController::playlistChanged, this, [this](const QVariantList &) {
        updateNavigationActions();
    });
    connect(m_controller, &MpvController::playlistPosChanged, this, [this](int pos) {
        const bool switchedItem = pos != m_playlistPos; // B9：实际换条目（值不变 mpv 不推送）
        m_playlistPos = pos;
        updateNavigationActions(); // 播放/停止态切换与位置变化（A16）
        // N8 Part5：停止态禁用进度条并归位 0；播放态恢复可用
        m_progressSlider->setEnabled(pos >= 0);
        if (pos < 0) {
            QSignalBlocker progressBlocker(m_progressSlider);
            m_progressSlider->setValue(0);
        }
        if (pos >= 0) {
            m_playedThisSession = true; // A13：标记本进程发生过播放
            // per-tab 记忆：记录播放目标 tab 的最后播放项（默认 tab 仅进程内）。
            // B9 Part2：换条目瞬间 m_timePos 尚是上一条的残留值（≈其片尾），
            // 以 0 起记新槽位待新条目 time-pos 回填，防接续窗口内停止污染记忆
            const int pt = m_playlist->playingTabIndex();
            if (pt >= 0)
                m_playlist->recordPlayback(pt, pos, switchedItem ? 0.0 : m_timePos);
            // 启动后首次检测到播放（如命令行参数打开文件）：切换到正在播放的 tab
            if (!m_startupPlaybackCheckDone) {
                m_startupPlaybackCheckDone = true;
                const int pt = m_playlist->playingTabIndex();
                if (pt >= 0 && m_playlist->currentIndex() != pt)
                    m_playlist->setCurrentIndex(pt);
            }
            // N7 #2 + N8 Part1：起播/切文件 OSD 与窗口标题同源（helper 收敛，
            // 真实文件名含后缀，不用 media-title）
            QString filename = playingFilename();
            const int curPt2 = m_playlist->playingTabIndex();
            const int total = curPt2 >= 0 ? m_playlist->filesOf(curPt2).size() : pos + 1;
            if (!filename.isEmpty())
                setWindowTitle(filename); // N8 Part1：标题跟随真实文件名
            else
                filename = tr("未知文件");
            m_pendingNowPlaying = tr("[%1/%2] %3").arg(pos + 1).arg(total).arg(filename);
            if (!m_osdCoalescePending) {
                m_osdCoalescePending = true;
                QTimer::singleShot(0, this, &MainWindow::flushOsd);
            }
        }
    });
    // B9 Part1：整表自然播完（EOF）即清进度留记忆项，播放按钮即可从头播；
    // 手动停止/四步式等 reason≠eof 不触发。判定点即本事件，零事件顺序依赖；
    // 中间条目接续时清除结果旋即被 playlistPosChanged 的换条目起记覆盖，无副作用。
    // m_playlistPos>=0 守卫排除启动期与已停止态
    connect(m_controller, &MpvController::endFile, this, [this](bool eof) {
        if (eof && m_playlistPos >= 0)
            m_playlist->clearResumeProgress(m_playlist->playingTabIndex());
    });
    // 列表播完或停止后 mpv 进入空闲，复位播放按钮与进度条（C1）
    connect(m_controller, &MpvController::idleActive, this, [this](bool active) {
        if (active) {
            m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
            QSignalBlocker blocker(m_progressSlider);
            m_progressSlider->setValue(0);
            // N8 Part5：停止态禁用进度条（与 C1 归零同点）
            m_progressSlider->setEnabled(false);
            // N8 Part1：无正在播放内容，标题恢复基名
            setWindowTitle(App::name());
            // N7 #3：停止 OSD（单源去重，playlistPos -1 不触发，以 m_playlistPos>=0 为守卫）
            if (m_playlistPos >= 0) {
                m_pendingStop = tr("停止");
                if (!m_osdCoalescePending) {
                    m_osdCoalescePending = true;
                    QTimer::singleShot(0, this, &MainWindow::flushOsd);
                }
            }
        }
    });

    connect(m_playlist, &PlaylistWidget::playRequested, this, &MainWindow::onPlayRequested);
    connect(m_playlist, &PlaylistWidget::filesAdded, this, &MainWindow::onFilesAdded);
    connect(m_playlist, &PlaylistWidget::removeRequested, this, &MainWindow::onRemoveRequested);
    connect(m_playlist, &PlaylistWidget::clearRequested, this, &MainWindow::onClearRequested);
    // N4：四步式第一步——排序/清理播放中 tab 前下发停止
    connect(m_playlist, &PlaylistWidget::stopRequested, this, [this]() {
        m_controller->command({QStringLiteral("stop")});
    });
    // N4：元数据回填路由——按"当前正播路径"定位全局仓条目（推送数据覆写）。
    // 路径经 normalizePath 规范化，与条目存储键同形，防止写入幻影条目
    connect(m_controller, &MpvController::durationChanged, m_playlist,
            [this](double sec) {
                if (sec > 0)
                    m_playlist->applyDuration(
                        PlaylistWidget::normalizePath(
                            m_controller->propertyString(QStringLiteral("path"))),
                        sec);
            });
    connect(m_controller, &MpvController::mediaMetadataChanged, m_playlist,
            [this](const QVariantMap &tags) {
                m_playlist->applyMediaMetadata(
                    PlaylistWidget::normalizePath(
                        m_controller->propertyString(QStringLiteral("path"))),
                    tags);
            });
    connect(m_controller, &MpvController::streamInfoChanged, m_playlist,
            [this](const QVariantMap &info) {
                m_playlist->applyStreamInfo(
                    PlaylistWidget::normalizePath(
                        m_controller->propertyString(QStringLiteral("path"))),
                    info);
            });
    connect(m_controller, &MpvController::mediaMetadataChanged, m_playlist,
            [this](const QVariantMap &tags) {
                m_playlist->applyMediaMetadata(
                    m_controller->propertyString(QStringLiteral("path")), tags);
            });
    connect(m_controller, &MpvController::streamInfoChanged, m_playlist,
            [this](const QVariantMap &info) {
                m_playlist->applyStreamInfo(
                    m_controller->propertyString(QStringLiteral("path")), info);
            });
    // 停止态步进以选中 tab 为基准，切换 tab 需刷新启停（A16）
    connect(m_playlist, &QTabWidget::currentChanged, this, [this](int) {
        updateNavigationActions();
    });
    // mpv 侧退出：补保存会话后再结束事件循环（A6）
    connect(m_controller, &MpvController::mpvShutdown, this, &MainWindow::onMpvShutdown);
}

void MainWindow::openFiles()
{
    // N9：过滤器模式由全局媒体扩展名清单生成（与 DirProvider 默认值同源）
    QStringList patterns;
    for (const QString &ext : App::defaultMediaExtensions())
        patterns << QStringLiteral("*.") + ext;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("打开文件"), QString(),
        tr("媒体文件 (%1);;所有文件 (*)").arg(patterns.join(QLatin1Char(' '))));
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
        // N9：路径+序号双存解析（序号校验失败按路径寻位）
        const int cur = m_playlist->currentIndex();
        const QStringList files = m_playlist->filesOf(cur);
        if (files.isEmpty())
            return;
        const double restoreTime = m_playlist->lastTimePosOf(cur);
        int row = m_playlist->resolveResumeRow(cur);
        if (row < 0) {
            row = m_playlist->currentSelectedRow();
            if (row < 0 || row >= files.size())
                row = 0;
        }
        // 载入即转移同步目标：mpv 列表回刷写进当前 tab，关闭时 lastTabName 随之正确；
        // 续播进度经统一入口的挂起 seek 机制恢复
        m_playlist->markPlayingTab(cur);
        onPlayRequested(files, row, restoreTime > 0.0 ? restoreTime : -1.0);
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
        stepStopped(false);
        return;
    }
    m_controller->command({QStringLiteral("playlist-prev")});
    // 步进隐含开始播放（A9/A14）：暂停态下新条目继承 pause=yes，显式恢复播放
    m_controller->setProperty(QStringLiteral("pause"), false);
    // 始终选中正在播放的列表（A9）
    const int pt = m_playlist->playingTabIndex();
    if (pt >= 0 && m_playlist->currentIndex() != pt)
        m_playlist->setCurrentIndex(pt);
}

void MainWindow::playlistNext()
{
    if (m_playlistPos < 0) {
        stepStopped(true);
        return;
    }
    m_controller->command({QStringLiteral("playlist-next")});
    // 同上（A9/A14）
    m_controller->setProperty(QStringLiteral("pause"), false);
    // 始终选中正在播放的列表（A9）
    const int pt = m_playlist->playingTabIndex();
    if (pt >= 0 && m_playlist->currentIndex() != pt)
        m_playlist->setCurrentIndex(pt);
}

// 停止态的上一个/下一个（A9/A16）：以当前选中列表为基准步进并开始播放。
// 起点：有记忆取记忆项，否则第 0 项；列表循环开启时首尾互绕，非循环越界无动作
void MainWindow::stepStopped(bool next)
{
    const int cur = m_playlist->currentIndex();
    const QStringList files = m_playlist->filesOf(cur);
    if (files.isEmpty())
        return;
    int base = m_playlist->resolveResumeRow(cur);
    if (base < 0 || base >= files.size())
        base = 0;
    int target = base + (next ? 1 : -1);
    if (currentLoopMode() == QLatin1String("playlist")) {
        if (target < 0)
            target = files.size() - 1; // 首项上一个 → 绕尾项
        else if (target >= files.size())
            target = 0;                // 尾项下一个 → 绕首项
    } else if (target < 0 || target >= files.size()) {
        return;
    }
    m_playlist->markPlayingTab(cur);
    onPlayRequested(files, target, -1.0);
}

// A16：上一/下一动作三路联动启停（矩阵见 specs/A16.md）。
// 仅操作这两个动作，不得触碰其他动作的禁用逻辑（如全屏期导航器守卫）
void MainWindow::updateNavigationActions()
{
    const bool loop = currentLoopMode() == QLatin1String("playlist");
    QAction *prevAction = m_actions->action(ActionManager::Id::PrevFile);
    QAction *nextAction = m_actions->action(ActionManager::Id::NextFile);

    if (m_playlistPos >= 0) {
        // 播放/暂停态：以 mpv 实际列表为准（长度取正在播放 tab 的镜像）
        const int pt = m_playlist->playingTabIndex();
        const int count = pt >= 0 ? m_playlist->filesOf(pt).size() : m_playlistPos + 1;
        prevAction->setEnabled(loop || m_playlistPos > 0);
        nextAction->setEnabled(loop || m_playlistPos < count - 1);
        return;
    }

    // 停止态：以当前选中 tab 为准；空列表无视 LOOP 双禁用
    const int cur = m_playlist->currentIndex();
    const QStringList files = m_playlist->filesOf(cur);
    if (files.isEmpty()) {
        prevAction->setEnabled(false);
        nextAction->setEnabled(false);
        return;
    }
    int base = m_playlist->resolveResumeRow(cur);
    if (base < 0 || base >= files.size())
        base = 0; // 与 stepStopped 起点归一规则一致
    prevAction->setEnabled(loop || base > 0);
    nextAction->setEnabled(loop || base < files.size() - 1);
}

void MainWindow::seekReleased()
{
    m_seeking = false;
    const double percent = m_progressSlider->value() / 10.0;
    m_controller->command({QStringLiteral("seek"), QString::number(percent), QStringLiteral("absolute-percent")});
    // N7 #7：进度跳转 OSD（固定 hh:mm:ss）。N8 Part5：duration<=0 分支已删——
    // 停止态滑条禁用后不可达，残余场景（直播流）下 absolute-percent 本就无效
    if (m_duration > 0.0)
        showOsd(tr("进度跳转: %1").arg(Util::formatTimePercent(m_duration * percent / 100.0, m_duration)));
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

// 导航器面板显隐（N1 显隐语义 + N3 按需创建生命周期）：零持久化——
// 每次打开都新建实例并定位到主窗口右下角；关闭（菜单/系统关闭钮）
// 统一经 destroyNavigator 复位画面并销毁
void MainWindow::setNavigatorVisible(bool visible)
{
    if (visible) {
        ensureNavigator();
        return;
    }
    destroyNavigator();
}

// 首次需要时构造导航器浮窗并接好信号，再走标准显示流程（N1）
void MainWindow::ensureNavigator()
{
    if (!m_navigator) {
        m_navigator = new NavigatorPanel(m_controller, videoWidget(), this);
        connect(m_navigator, &NavigatorPanel::panelClosed, this, &MainWindow::destroyNavigator);
    }
    m_navigator->adjustSize();
    m_navigator->show(); // 先显示再定位：定位函数带 isVisible 守卫，隐藏态会被跳过
    repositionNavigator();
    m_navigator->raise();
}

// 统一关闭收口（N3 口径 b，幂等）：复位画面 → 取消菜单勾选 → 销毁置 null。
// 系统关闭钮路径 closeEvent 内已做过一次 requestReset，此处 resetAndHide
// 再触发一次属写零属性幂等空转。QSignalBlocker 防止取消勾选递归回本函数
void MainWindow::destroyNavigator()
{
    if (!m_navigator)
        return;
    m_navigator->resetAndHide();
    {
        QSignalBlocker blocker(m_navigatorAction);
        m_navigatorAction->setChecked(false);
    }
    m_navigator->deleteLater();
    m_navigator = nullptr;
}

// 浮窗不随属主联动，主窗口移动/缩放时手动跟随并刷新映射。
// 用 frameGeometry（含标题栏外框）锚定：move() 定位的是外框左上角，
// 按客户区尺寸计算会让外框越过主窗口底边约一个标题栏高度
void MainWindow::repositionNavigator()
{
    if (!m_navigator || !m_navigator->isVisible())
        return;
    const QPoint corner = mapToGlobal(QPoint(width(), height()));
    const QRect frame = m_navigator->frameGeometry();
    QPoint pos(corner.x() - frame.width() - 12,
               corner.y() - frame.height() - 12);
    const QRect avail = m_navigator->screen()->availableGeometry();
    pos.setX(qBound(avail.left(), pos.x(), avail.right() - frame.width()));
    pos.setY(qBound(avail.top(), pos.y(), avail.bottom() - frame.height()));
    m_navigator->move(pos);
    m_navigator->refresh(); // 渲染区域尺寸可能已变，视口矩形需重算
}

// ---- N7：VideoOverlay 统一 OSD（A3 顶级 Tool 浮窗）----
namespace {
class OsdLabel : public QLabel
{
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        const QString text = this->text();
        if (text.isEmpty())
            return;
        QFont f = font();
        f.setPointSize(20);
        f.setBold(true);
        p.setFont(f);
        QFontMetrics fm(f);
        const int padX = 10;
        const int padY = 6;
        QPainterPath path;
        path.addText(padX, padY + fm.ascent(), f, text);
        QPen pen(Qt::black, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(path, pen);
        p.fillPath(path, QBrush(Qt::white));
    }
    QSize sizeHint() const override
    {
        QFont f = font();
        f.setPointSize(20);
        f.setBold(true);
        QFontMetrics fm(f);
        const QString t = text();
        const int w = fm.horizontalAdvance(t) + 20;
        const int h = fm.height() + 12;
        return QSize(w, h);
    }
};
} // namespace

void MainWindow::ensureVideoOverlay()
{
    if (m_videoOverlay)
        return;
    m_videoOverlay = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus
                                      | Qt::WindowTransparentForInput);
    m_videoOverlay->setAttribute(Qt::WA_TranslucentBackground);
    m_videoOverlay->setAttribute(Qt::WA_ShowWithoutActivating);
    m_videoOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // 容器透明，内部控件自绘
    auto *layout = new QVBoxLayout(m_videoOverlay);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(0);
    m_osdLabel = new OsdLabel(m_videoOverlay);
    m_osdLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // N7：无背景，仅描边白字（20pt），不使用 style sheet
    m_osdLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_osdLabel->hide();
    layout->addWidget(m_osdLabel, 0, Qt::AlignTop | Qt::AlignLeft);
    layout->addStretch(1);
    m_osdTimer = new QTimer(this);
    m_osdTimer->setSingleShot(true);
    connect(m_osdTimer, &QTimer::timeout, this, [this] {
        if (m_osdLabel)
            m_osdLabel->hide();
        if (m_videoOverlay && !m_osdLabel->isVisible())
            m_videoOverlay->hide();
    });
}

void MainWindow::repositionVideoOverlay()
{
    if (!m_videoOverlay || !m_videoWidget)
        return;
    const QPoint globalPos = m_videoWidget->mapToGlobal(QPoint(0, 0));
    m_videoOverlay->setGeometry(QRect(globalPos, m_videoWidget->size()));
}

void MainWindow::showOsd(const QString &text)
{
    ensureVideoOverlay();
    m_osdLabel->setText(text);
    m_osdLabel->adjustSize();
    repositionVideoOverlay();
    m_videoOverlay->show();
    m_osdLabel->show();
    m_videoOverlay->raise();
    m_osdTimer->start(2000);
}

void MainWindow::flushOsd()
{
    m_osdCoalescePending = false;
    // 优先级：#2 起播/切文件 > #3 停止 > #1 播放/暂停
    QString text;
    if (!m_pendingNowPlaying.isEmpty())
        text = m_pendingNowPlaying;
    else if (!m_pendingStop.isEmpty())
        text = m_pendingStop;
    else if (!m_pendingPlayPause.isEmpty())
        text = m_pendingPlayPause;
    m_pendingNowPlaying.clear();
    m_pendingStop.clear();
    m_pendingPlayPause.clear();
    if (!text.isEmpty())
        showOsd(text);
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    QMainWindow::moveEvent(event);
    repositionNavigator();
    repositionVideoOverlay();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    repositionNavigator();
    repositionVideoOverlay();
    // N3：showFullScreen/showNormal 改变的是主窗尺寸，主窗 resize 是
    // 全屏/还原后视频区几何同步的可靠最终触发点（幂等）
    syncVideoGeometry();
}

// 视频区几何手动同步（N3）：按窗口模式分支计算目标几何。
// 全屏铺满整个客户区；普通态对齐占位符——占位符 geometry 相对其父容器，
// 须 mapTo 主窗坐标系
void MainWindow::syncVideoGeometry()
{
    if (!m_videoWidget || !m_videoPlaceholder)
        return;
    if (m_fullscreen) {
        m_videoWidget->setGeometry(rect());
    } else {
        const QPoint origin = m_videoPlaceholder->mapTo(this, QPoint(0, 0));
        m_videoWidget->setGeometry(QRect(origin, m_videoPlaceholder->size()));
    }
    repositionVideoOverlay();
}

// 覆盖式全屏切换（N3）：不隐藏控件、不重父化、wid 不重设、零黑闪
void MainWindow::toggleFullscreen()
{
    if (!m_fullscreen) {
        // 全屏态落盘守卫：快照进全屏前几何（saveSettings 落盘 + 退出恢复共用）。
        // A18：快照扩为整矩形（含位置），无框方案的普通态退出恢复需要
        m_preFsMaximized = isMaximized();
        m_preFsRect = m_preFsMaximized ? normalGeometry() : geometry();
        // 导航器 zoom/pan 先存后写零：复位=保持比例填充全屏（第三节）
        m_savedZoom = m_controller->propertyDouble(QStringLiteral("video-zoom"), 0.0);
        m_savedPanX = m_controller->propertyDouble(QStringLiteral("video-pan-x"), 0.0);
        m_savedPanY = m_controller->propertyDouble(QStringLiteral("video-pan-y"), 0.0);
        m_controller->setProperty(QStringLiteral("video-zoom"), 0.0);
        m_controller->setProperty(QStringLiteral("video-pan-x"), 0.0);
        m_controller->setProperty(QStringLiteral("video-pan-y"), 0.0);
        // 菜单栏为 alien 控件，实测会绘制在原生视频区之上形成顶部遮挡条
        // （"native 天然覆盖 alien"的假设对菜单栏不成立），全屏期间整体隐藏
        menuBar()->hide();
        if (m_navigator)
            m_navigator->hide();
        m_navigatorAction->setEnabled(false);
        m_fullscreen = true;
        // A18 无框手动几何全屏：绕开 WindowFullScreen 状态机（其进出必经
        // 普通几何中间帧并回放最大化动画）。flags 切换触发平台窗口重建并
        // 隐式隐藏，子原生 HWND 经 SetParent 平移保身份，wid 不失效（原型实测）
        m_preFsFlags = windowFlags();
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
#ifdef Q_OS_WIN
        // A19：无框 NoState 窗口不被认定为全屏，Win11 DWM 会按普通浮窗施加
        // 圆角裁切（四角透明缺口+边缘透明环）。强制方角；属性须作用于重建后
        // 的新 HWND 故紧随 flags 切换。退出无需处理——再次重建回归系统默认，
        // 且最大化态本就自动方角
        const DWORD cornerPref = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()),
                              DWMWA_WINDOW_CORNER_PREFERENCE,
                              &cornerPref, sizeof(cornerPref));
#endif
        setWindowState(Qt::WindowNoState);
        setGeometry(screen()->geometry()); // 窗口所在屏，覆盖任务栏区
        show();
        syncVideoGeometry(); // 铺满后视频区对位；随后的 resizeEvent 幂等兜底
    } else {
        m_fullscreen = false;
        // A18：恢复有框样式触发重建（自动隐藏），新窗口几何继承自全屏矩形。
        // 趁隐藏期植入快照几何——它随即成为最大化"还原位置"，取消最大化
        // 时精确回到进全屏前的尺寸；再按快照分流显示
        setWindowFlags(m_preFsFlags);
        setGeometry(m_preFsRect);
        if (m_preFsMaximized)
            showMaximized();
        else
            show();
        menuBar()->show(); // 恢复菜单栏（布局重排会经占位符事件链触发几何同步）
        syncVideoGeometry();
        m_controller->setProperty(QStringLiteral("video-zoom"), m_savedZoom);
        m_controller->setProperty(QStringLiteral("video-pan-x"), m_savedPanX);
        m_controller->setProperty(QStringLiteral("video-pan-y"), m_savedPanY);
        if (m_navigator) { // 非 null ⟹ 必为被本方案隐藏的全屏前可见面板
            m_navigator->show();
            repositionNavigator();
        }
        m_navigatorAction->setEnabled(true);
    }
    {
        QSignalBlocker blocker(m_fullscreenAction); // 防循环触发 toggleFullscreen
        m_fullscreenAction->setChecked(m_fullscreen);
    }
}

// 实时读 mpv 的 pause（缓存可能滞后一个事件周期）；读取失败按已暂停处理，
// 走延迟路径更安全
bool MainWindow::isPaused()
{
    return m_controller->propertyFlag(QStringLiteral("pause"), true);
}

// 单击定时器超时：确非双击，执行延迟的单击动作
// （仅暂停态恢复播放/停止态开播路径有实际效果）
void MainWindow::onVideoClickTimeout()
{
    if (m_clickActionPending) {
        m_clickActionPending = false;
        togglePause();
    }
}

// 占位符几何跟随 + 视频区单击/双击分流（N3）。
// 右键与 Release 不处理：右键无任何行为变化
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_videoPlaceholder) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Move)
            syncVideoGeometry();
    } else if (watched == m_videoWidget) {
        switch (event->type()) {
        case QEvent::Wheel:
            // N5：播放区滚轮调音量（上滚增大）
            changeVolume(static_cast<QWheelEvent *>(event)->angleDelta().y() > 0
                             ? m_volumeWheelStep
                             : -m_volumeWheelStep);
            return true;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() != Qt::LeftButton)
                break; // 右键无任何行为变化
            // N5：点击画面即离开列表焦点，方向键回到全局音量模式
            if (auto *fw = QApplication::focusWidget())
                fw->clearFocus();
            if (event->type() == QEvent::MouseButtonDblClick) {
                m_clickTimer->stop();
                m_clickActionPending = false;
                if (m_pendingPauseRestore) {
                    m_pendingPauseRestore = false;
                    m_controller->setProperty(QStringLiteral("pause"), false); // 恢复播放
                }
                toggleFullscreen();
                break;
            }
            // "定时器未活跃"守卫：双击间隔内带位移的第二 press 不重启流程
            if (m_clickTimer->isActive())
                break;
            // 入口无条件复位两标志：状态只在单轮 press→(双击|超时)
            // 生命周期内存活，杜绝上轮残留泄漏进本轮
            m_pendingPauseRestore = false;
            m_clickActionPending = false;
            if (m_playlistPos >= 0 && !isPaused()) {
                // 播放中：立即暂停（单击终态），双击时再恢复
                togglePause();
                m_pendingPauseRestore = true;
            } else {
                // 暂停/停止态：延迟到超时执行，避免误开播后抵消的音频闪烁
                m_clickActionPending = true;
            }
            m_clickTimer->start(QApplication::doubleClickInterval());
            break;
        }
        default:
            break;
        }
    } else if (watched == m_progressSlider) {
        // N8 Part3：tooltip 跟随鼠标实时显示所指进度；拖动期持续刷新防自动隐藏。
        // 停止态控件已禁用收不到鼠标事件，天然不显示
        switch (event->type()) {
        case QEvent::ToolTip:
        case QEvent::MouseMove: {
            // ToolTip 载荷为 QHelpEvent、MouseMove 为 QMouseEvent，先归一坐标
            QPoint pos, global;
            if (event->type() == QEvent::ToolTip) {
                const auto *he = static_cast<QHelpEvent *>(event);
                pos = he->pos();
                global = he->globalPos();
            } else {
                const auto *me = static_cast<QMouseEvent *>(event);
                pos = me->position().toPoint();
                global = me->globalPosition().toPoint();
            }
            if (m_playlistPos < 0 || m_duration <= 0.0)
                break;
            const double percent = progressPercentAt(pos);
            if (percent < 0.0)
                break;
            QToolTip::showText(global,
                               Util::formatTimePercent(m_duration * percent / 100.0, m_duration),
                               m_progressSlider);
            break;
        }
        case QEvent::Leave:
            // 拖动中移出不隐藏（保持常显），仅悬停离开时收起
            if (!m_seeking)
                QToolTip::hideText();
            break;
        default:
            break;
        }
    } else if (watched == m_volumeSlider) {
        switch (event->type()) {
        case QEvent::Wheel:
            // N5：接管滑条原生滚轮步进，与播放区同向同步长
            changeVolume(static_cast<QWheelEvent *>(event)->angleDelta().y() > 0
                             ? m_volumeWheelStep
                             : -m_volumeWheelStep);
            return true;
        case QEvent::MouseMove:
            // N8 Part4：拖动期间显示当前音量（isSliderDown 区分拖动与悬停）
            if (m_volumeSlider->isSliderDown())
                QToolTip::showText(static_cast<QMouseEvent *>(event)->globalPosition().toPoint(),
                                   QStringLiteral("%1%").arg(m_volumeSlider->value()), m_volumeSlider);
            break;
        case QEvent::MouseButtonRelease:
        case QEvent::Leave:
            QToolTip::hideText();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// N5：统一音量调节入口。以滑条缓存为基准钳制后下发 mpv；
// UI 不在此处回写，由 volumeChanged 观察回流刷新（防环已有 QSignalBlocker）
void MainWindow::changeVolume(int delta)
{
    const int target = qBound(0, m_volumeSlider->value() + delta, m_volumeMax);
    m_controller->setProperty(QStringLiteral("volume"), double(target));
}

// B11：正在播放的真实文件名（含后缀不含路径），标题与 OSD #2 同源。
// 列表镜像优先——playlist-pos 推送早于新文件装载完成，此刻读 filename 属性
// 会拿到上一个文件的残留值（概率性错名根因）；本地镜像顺序与 mpv 恒一致，
// 按 pos 取名为确定性来源。属性仅作镜像缺失时的兜底（如起播早期回刷未达）
QString MainWindow::playingFilename()
{
    const int pt = m_playlist->playingTabIndex();
    if (pt >= 0 && m_playlistPos >= 0) {
        const QStringList files = m_playlist->filesOf(pt);
        if (m_playlistPos < files.size())
            return QFileInfo(files[m_playlistPos]).fileName();
    }
    return m_controller->propertyString(QStringLiteral("filename"));
}

// N8 Part3：进度条鼠标 x → 百分比（0~100）。经样式反查 groove/handle 几何，
// 扣除滑块半宽后线性映射，与拖动定位算法同口径
double MainWindow::progressPercentAt(const QPoint &pos) const
{
    QStyleOptionSlider opt;
    opt.initFrom(m_progressSlider);
    opt.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
    opt.orientation = m_progressSlider->orientation();
    opt.minimum = m_progressSlider->minimum();
    opt.maximum = m_progressSlider->maximum();
    opt.sliderPosition = m_progressSlider->sliderPosition();
    opt.sliderValue = m_progressSlider->value();
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, m_progressSlider);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, m_progressSlider);
    const int span = groove.width() - handle.width();
    if (span <= 0)
        return -1.0;
    const int dx = pos.x() - handle.width() / 2 - groove.left();
    const int value = QStyle::sliderValueFromPosition(opt.minimum, opt.maximum, qBound(0, dx, span), span);
    return value / 10.0;
}

// N6：相对 seek。停止态（m_playlistPos<0）无播放位置可寻址，直接忽略；
// 不指定 flags 走 mpv 默认相对关键帧寻址，响应优先于逐帧精度
void MainWindow::seekRelative(int deltaSeconds)
{
    if (m_playlistPos < 0)
        return;
    m_controller->command({QStringLiteral("seek"), QString::number(deltaSeconds)});
    // N7 #6：进度跳转 OSD（固定 hh:mm:ss）
    if (m_duration > 0.0) {
        const double target = qBound(0.0, m_timePos + deltaSeconds, m_duration);
        const QString sign = deltaSeconds >= 0 ? QStringLiteral("+%1秒").arg(deltaSeconds)
                                               : QStringLiteral("%1秒").arg(deltaSeconds);
        showOsd(tr("进度跳转: %1 / %2").arg(sign, Util::formatTimePercent(target, m_duration)));
    } else {
        const QString sign = deltaSeconds >= 0 ? QStringLiteral("+%1秒").arg(deltaSeconds)
                                               : QStringLiteral("%1秒").arg(deltaSeconds);
        showOsd(tr("进度跳转: %1").arg(sign));
    }
}

// N6：速度步进——实时读 speed 属性做读改写（不设观察缓存），
// 钳制到 [0.25, 8]；±0.25 为二进制精确小数，域内累加无舍入误差
void MainWindow::changeSpeed(double delta)
{
    const double cur = m_controller->propertyDouble(QStringLiteral("speed"), 1.0);
    const double target = qBound(0.25, cur + delta, 8.0);
    m_controller->setProperty(QStringLiteral("speed"), target);
    // N7 #5：四舍五入至两位小数后去尾零
    double rounded = std::round(target * 100.0) / 100.0;
    QString num = QString::number(rounded, 'f', 2);
    while (num.contains(QLatin1Char('.')) && num.endsWith(QLatin1Char('0')))
        num.chop(1);
    if (num.endsWith(QLatin1Char('.')))
        num.chop(1);
    showOsd(tr("播放速度: %1倍").arg(num));
}

void MainWindow::resetSpeed()
{
    m_controller->setProperty(QStringLiteral("speed"), 1.0);
    showOsd(tr("播放速度: 1倍"));
}

// N5：空白区域点击清焦兜底——未被交互控件消费的鼠标按下事件一路冒泡至此，
// 清除焦点使方向键回到全局音量模式；列表/滑条/按钮等自行消费按下事件，
// 不会到达此处，语义自动正确
void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (auto *fw = QApplication::focusWidget())
            fw->clearFocus();
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::onPlayRequested(const QStringList &files, int index, double seekTo)
{
    // 统一播放入口（N4 合并）：整表载入 → 跳转 index → 挂起恢复进度。
    m_pendingSeekRow = -1;   // 丢弃未执行的挂起 seek
    m_pendingSeekTime = -1.0;
    if (files.isEmpty())
        return;
    // mpv 的 playlist-clear 会保留当前播放的条目，先 stop 才能真正清空
    m_controller->command({QStringLiteral("stop")});
    m_controller->command({QStringLiteral("playlist-clear")});
    // 全部按原顺序 append（不自动播），最后以 playlist-pos 启动目标条目：
    // 既避免 idle 下 append-play 瞬态播放 file[0]，也保持 mpv 列表顺序与界面一致
    for (int i = 0; i < files.size(); ++i)
        m_controller->command({QStringLiteral("loadfile"), files[i], QStringLiteral("append")});
    if (seekTo > 0.0) {
        // 进度恢复走挂起 seek（与会话恢复同款）：playlist-pos 触发的加载是
        // 异步的，立即 seek 会因文件未就绪而失败；先挂起再启动杜绝竞态，
        // 待目标条目 duration 推送到达且行号匹配时执行一次
        m_pendingSeekRow = index;
        m_pendingSeekTime = seekTo;
    }
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
    updateNavigationActions(); // 本地 tab 新增内容影响停止态步进（A16）；播放态由 playlistChanged 兜底
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
    updateNavigationActions(); // 循环模式切换立即刷新启停（A16）
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

    // N5：音量调节三项——上限自由填写（域 0~400），滚轮/方向键步长档位单选
    auto *maxSpin = new QSpinBox(&dialog);
    maxSpin->setRange(0, 400);
    maxSpin->setValue(m_volumeMax);
    layout->addRow(tr("音量上限"), maxSpin);

    auto makeStepCombo = [&dialog](int current) {
        auto *combo = new QComboBox(&dialog);
        for (int step : {1, 2, 5, 10})
            combo->addItem(QString::number(step), step);
        const int idx = combo->findData(current);
        if (idx >= 0)
            combo->setCurrentIndex(idx);
        return combo;
    };
    auto *wheelStepCombo = makeStepCombo(m_volumeWheelStep);
    layout->addRow(tr("滚轮步长"), wheelStepCombo);
    auto *keyStepCombo = makeStepCombo(m_volumeKeyStep);
    layout->addRow(tr("方向键步长"), keyStepCombo);

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

        // 音量上限：setRange 自动把超限旧值钳回并经既有直通连接推送 mpv
        const int newMax = maxSpin->value();
        if (newMax != m_volumeMax) {
            m_volumeMax = newMax;
            m_volumeSlider->setRange(0, m_volumeMax);
        }
        m_volumeWheelStep = wheelStepCombo->currentData().toInt();
        m_volumeKeyStep = keyStepCombo->currentData().toInt();
        settings.setValue(QStringLiteral("volumeMax"), m_volumeMax);
        settings.setValue(QStringLiteral("volumeWheelStep"), m_volumeWheelStep);
        settings.setValue(QStringLiteral("volumeKeyStep"), m_volumeKeyStep);
    }
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("关于 %1").arg(App::name()),
                       tr("%1：以 mpv C 插件形式加载的 Qt 播放器界面。\n"
                          "视频渲染由 mpv 完成，本插件仅提供图形界面。").arg(App::name()));
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

    // 窗口：最大化时额外记录普通态尺寸（B8），否则取消最大化
    // 只能回到构造期默认几何而非上次会话的尺寸。
    // N3 全屏态落盘守卫：全屏中实时宽高是屏幕尺寸，不得存为普通态几何，
    // 改用进全屏前快照写入（toggleFullscreen 进入分支已缓存）
    const bool maximized = m_fullscreen ? m_preFsMaximized : isMaximized();
    int winW = 0, winH = 0;
    if (m_fullscreen) {
        winW = m_preFsRect.width();
        winH = m_preFsRect.height();
    } else if (maximized) {
        const QRect normal = normalGeometry();
        if (normal.isValid()) {
            winW = normal.width();
            winH = normal.height();
        }
    } else {
        winW = width();
        winH = height();
    }
    settings.setValue(QStringLiteral("windowMaximized"), maximized);
    if (winW > 0 && winH > 0) {
        settings.setValue(QStringLiteral("windowWidth"), winW);
        settings.setValue(QStringLiteral("windowHeight"), winH);
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

    // 音量（N5：先恢复上限并调整滑条量程，再恢复音量值——顺序敏感，
    // 否则旧音量可能被默认量程 100 截断）
    m_volumeMax = qBound(0, settings.value(QStringLiteral("volumeMax"), m_volumeMax).toInt(), 400);
    m_volumeSlider->setRange(0, m_volumeMax);
    m_volumeWheelStep = settings.value(QStringLiteral("volumeWheelStep"), m_volumeWheelStep).toInt();
    m_volumeKeyStep = settings.value(QStringLiteral("volumeKeyStep"), m_volumeKeyStep).toInt();
    if (settings.contains(QStringLiteral("volume"))) {
        const double vol =
            qBound(0.0, settings.value(QStringLiteral("volume")).toDouble(), double(m_volumeMax));
        m_controller->setProperty(QStringLiteral("volume"), vol);
        QSignalBlocker blocker(m_volumeSlider);
        m_volumeSlider->setValue(int(vol + 0.5));
    }

    // 窗口尺寸 / 最大化
    if (settings.value(QStringLiteral("windowMaximized"), false).toBool()) {
        // B7：不在构造期直接设最大化（未显示窗口的状态会被平台层丢弃），
        // 记标志待首次 showEvent 应用。
        // B8：先按 ini 尺寸铺好普通态几何，取消最大化时才能恢复到
        // 上次会话的尺寸（否则是构造期默认值）
        m_pendingMaximize = true;
        if (settings.contains(QStringLiteral("windowWidth")))
            resize(settings.value(QStringLiteral("windowWidth")).toInt(),
                   settings.value(QStringLiteral("windowHeight")).toInt());
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

    updateNavigationActions(); // A16：启动恢复完成后的初始启停状态
    // N7：启动恢复的 volume setProperty 会异步回推 volumeChanged，
    // 需延迟至事件队列稳定后再允许 OSD，避免启动瞬间误弹“音量: xx”
    QTimer::singleShot(600, this, [this] { m_osdReady = true; });
}

void MainWindow::updateTimeDisplay()
{
    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(Util::formatTime(m_timePos), Util::formatTime(m_duration)));
    if (!m_seeking && m_duration > 0.0)
        m_progressSlider->setValue(int(m_timePos / m_duration * 1000.0));
}

QString MainWindow::formatTime(double seconds)
{
    return Util::formatTime(seconds);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    // mpv 是宿主进程：关闭 GUI 时让 mpv 一并退出
    m_controller->command({QStringLiteral("quit")});
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    // B7：首次显示时应用上次会话的最大化状态（构造期设置不可靠）
    if (m_pendingMaximize) {
        m_pendingMaximize = false;
        setWindowState(Qt::WindowMaximized);
    }
}

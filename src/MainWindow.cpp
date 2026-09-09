#include "pch.h"

#include "MainWindow.h"

#include "ActionManager.h"
#include "BookmarkEditorDialog.h"
#include "BookmarkManager.h"
#include "ChapterSlider.h"
#include "Constants.h"
#include "MpvController.h"
#include "NavigatorPanel.h"
#include "PlaylistWidget.h"
#include "SettingsDialog.h"
#include "Util.h"

// N15：章节+书签数据与合并规则的唯一持有者（MainWindow.cpp 文件内实现，
// 头文件仅前向声明）。职责：接收原始章节 → 内部严格 == 去重 + time 升序；
// 对外提供三视图——去重章节（菜单段/标记基数）、原始章节（全量）、
// 原始书签（书签按冻结口径永不查重，BookmarkManager 即原始源，此处为
// 统一读入口）。三处消费者（滑条标记 / tooltip 命中 / 章节菜单勾选）共用
// 同一合并规则。缓存键仅剩脏标志：章节/书签/文件切换均为低频信号，全部
// 经 setChapters/invalidate 置脏，path 只在脏时惰性同步查询一次——命中路径零 IPC
class MarkAggregator {
  public:
    struct Mark {
        double time = 0.0;
        QString title;
        bool isBookmark = false;
    };

    explicit MarkAggregator(MpvController *controller, BookmarkManager *bookmarks)
        : m_controller(controller), m_bookmarks(bookmarks) {}

    // 章节数据：接收原始（不去重），内部严格 == 去重（保留首项）+ time 升序。
    // 传空即清空（文件切换/停止）。去重规则与合并规则同处收口
    void setChapters(const QList<MpvController::ChapterInfo> &raw) {
        m_rawChapters = raw;
        m_chapters.clear();
        for (const auto &c : raw) {
            bool dup = false;
            for (const auto &d : m_chapters)
                if (d.time == c.time) {
                    dup = true;
                    break;
                }
            if (!dup)
                m_chapters.append(c);
        }
        std::stable_sort(m_chapters.begin(), m_chapters.end(),
                         [](const MpvController::ChapterInfo &a,
                            const MpvController::ChapterInfo &b) { return a.time < b.time; });
        m_dirty = true;
    }

    // 去重排序后的章节（菜单章节段 / 合并标记基数）
    const QList<MpvController::ChapterInfo> &chapters() const { return m_chapters; }
    // 原始章节（原样不去重；需要全量时用）
    const QList<MpvController::ChapterInfo> &rawChapters() const { return m_rawChapters; }
    // 原始书签（书签永不查重；path 归一化后查表，空路径返回空）
    QList<BookmarkManager::Bookmark> bookmarks(const QString &path) const {
        if (path.isEmpty() || !m_bookmarks)
            return {};
        return m_bookmarks->bookmarks(Util::normalizePathForMatch(path));
    }
    // 书签/装载完成等任意内容变更后调用
    void invalidate() { m_dirty = true; }

    // 合并清单：当前文件的章节 ∪ 书签，time 升序；同 time（严格 ==）书签覆盖章节。
    // 返回内部缓存 const 引用（零拷贝）；引用仅在下次 marks()/invalidate 前有效，
    // 调用方须即时消费，不得跨变更持有
    const QList<Mark> &marks() const {
        if (!m_dirty)
            return m_cache;
        const QString path =
            Util::normalizePathForMatch(m_controller->propertyString(QStringLiteral("path")));
        QList<Mark> out;
        for (const auto &c : m_chapters)
            out.append({c.time, c.title, false});
        if (!path.isEmpty() && m_bookmarks) {
            const auto bms = m_bookmarks->bookmarks(path);
            for (const auto &b : bms)
                out.append({b.time, b.title, true});
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const Mark &a, const Mark &b) { return a.time < b.time; });
        QList<Mark> merged;
        for (const auto &m : out) {
            bool found = false;
            for (int i = 0; i < merged.size(); ++i) {
                if (merged[i].time == m.time) {
                    found = true;
                    if (m.isBookmark && !merged[i].isBookmark)
                        merged[i] = m;
                    break;
                }
            }
            if (!found)
                merged.append(m);
        }
        m_cache = merged;
        m_dirty = false;
        return m_cache;
    }

    // 命中：marks 中 time ≤ timePos 的最大项；无命中返回空
    std::optional<Mark> hit(double timePos) const {
        const auto &list = marks();
        std::optional<Mark> result;
        for (const auto &m : list) {
            if (m.time <= timePos)
                result = m;
            else
                break;
        }
        return result;
    }

  private:
    MpvController *m_controller;                     // 非拥有
    BookmarkManager *m_bookmarks;                    // 非拥有
    QList<MpvController::ChapterInfo> m_rawChapters; // 原始章节（setChapters 入参原样）
    QList<MpvController::ChapterInfo> m_chapters;    // 严格 == 去重 + time 升序
    mutable QList<Mark> m_cache;
    mutable bool m_dirty = true;
};

MainWindow::MainWindow(MpvController *controller, const QString &configFile, QWidget *parent)
    : QMainWindow(parent), m_controller(controller), m_configFile(configFile) {
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
    // A20：禁止把手把播放列表拖到 0——显隐统一走专门入口（Ctrl+L/☰/菜单）并
    // 收口于 setPlaylistVisible；拖没列表会绕过收口导致勾选分叉且窗口宽度不收回，
    // 同时消除"记忆宽度可能为 0"的防御分支。不涉及 stretchFactor，
    // "窗口增量归左 pane"的既有语义不变
    m_splitter->setCollapsible(1, false);
    m_playlist->setVisible(false); // 播放列表默认隐藏
    // A20：拖动把手即时刷新记忆宽度，使"拖动后直接收缩"也记住正确宽度；
    // 已禁止拖到 0，无需零宽过滤
    connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
        if (m_playlist->isVisible())
            m_playlistWidth = m_playlist->width();
    });
    m_playlist->loadPlaylists();

    m_bookmarkMgr = new BookmarkManager(configFile, this);
    m_bookmarkMgr->load();

    m_marks = new MarkAggregator(m_controller, m_bookmarkMgr); // N15：标记聚合器

    setCentralWidget(m_splitter);
    createMenus();
    connectController();

    // 导航器浮窗（N3 改按需创建）：冷启动不构造对象，首次打开经
    // ensureNavigator 创建；视口映射需要视频渲染区域的真实尺寸（黑边感知）

    // N10：启动固定默认尺寸；窗口几何/最大化状态不再持久化
    resize(960, 600);

    restoreSettings();
    updateChapterMenuState();
    updateChapterSlider();
}

QWidget *MainWindow::createLeftArea() {
    auto *left = new QWidget;
    auto *layout = new QVBoxLayout(left);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // 第一行：视频区占位符（N3）——真实视频区已改为主窗直接子控件，
    // 此处仅提供布局槽位与几何锚点；黑色背景保证视觉不变，
    // 最小尺寸约束必须挂在受布局管理的占位符上（对手动的视频区无效）
    m_videoPlaceholder = new QWidget;
    // A20：最小尺寸按常量设置——默认尺寸即下限（480 为控制栏不裁切的实测下限，
    // 原值 480×270 的高度下限由 270 抬到 320）
    m_videoPlaceholder->setMinimumSize(App::videoDefaultSize());
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
    m_progressSlider = new ChapterSlider();
    m_progressSlider->setOrientation(Qt::Horizontal);
    m_progressSlider->setRange(0, 1000);
    // N8 Part3：未按下也接收 move，驱动 tooltip 跟随
    m_progressSlider->setMouseTracking(true);
    m_progressSlider->installEventFilter(this);
    // N8 Part5：停止态禁用且归零（起播经 playlistPosChanged 启用）
    m_progressSlider->setEnabled(false);
    m_progressSlider->setValue(0);
    m_volumeSlider = new QSlider(Qt::Horizontal);
    // R3：音量上限由 mpv 自管（volume-max 属性推送）；构造期用成员默认值 130
    // （与 mpv 默认一致），运行时经 applyVolumeMax(qRound) 同步。
    m_volumeSlider->setRange(0, m_volumeMax);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setFixedWidth(100);
    m_volumeSlider->installEventFilter(this); // N5：滚轮调节走统一入口
    seekLayout->addWidget(m_progressSlider, 1);
    seekLayout->addSpacing(8);
    seekLayout->addWidget(new QLabel(tr("Volume")));
    seekLayout->addWidget(m_volumeSlider);
    layout->addWidget(seekRow);

    // 第三行：控制按钮行（自适应高度，按钮 36×36 由布局撑开，边框与按钮间距相等）
    auto *controlRow = new QWidget;
    auto *controlLayout = new QHBoxLayout(controlRow);
    controlLayout->setContentsMargins(8, 0, 8, 8);
    controlLayout->setSpacing(8);
    m_playButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), QString());
    m_playButton->setToolTip(tr("Play/Pause"));
    m_playButton->setFixedSize(36, 36);
    auto *stopButton = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop), QString());
    stopButton->setToolTip(tr("Stop"));
    stopButton->setFixedSize(36, 36);
    auto *prevButton =
        new QPushButton(style()->standardIcon(QStyle::SP_MediaSkipBackward), QString());
    prevButton->setToolTip(tr("Previous File"));
    prevButton->setFixedSize(36, 36);
    auto *nextButton =
        new QPushButton(style()->standardIcon(QStyle::SP_MediaSkipForward), QString());
    nextButton->setToolTip(tr("Next File"));
    nextButton->setFixedSize(36, 36);
    m_timeLabel = new QLabel(QStringLiteral("00:00 / 00:00"));
    m_streamButton = new QPushButton(QStringLiteral("♪"));
    m_streamButton->setToolTip(tr("Switch Stream"));
    m_streamButton->setFixedSize(36, 36);
    m_streamButton->hide();
    auto *settingsButton = new QPushButton(QStringLiteral("⚙"));
    settingsButton->setToolTip(tr("Settings"));
    settingsButton->setFixedSize(36, 36);
    m_playlistToggle = new QPushButton(QStringLiteral("☰"));
    m_playlistToggle->setToolTip(tr("Show/Hide Playlist"));
    m_playlistToggle->setFixedSize(36, 36);
    m_playlistToggle->setCheckable(true);

    // 控制行按钮不接受焦点（C2）：Space 归全局快捷键，避免焦点按钮被 Space 二次触发
    for (QPushButton *b : {m_playButton, stopButton, prevButton, nextButton, m_streamButton,
                           settingsButton, m_playlistToggle}) {
        b->setFocusPolicy(Qt::NoFocus);
        // 点击控制按钮时把焦点重置到主窗口
        connect(b, &QPushButton::pressed, [] {
            if (auto *fw = QApplication::focusWidget())
                fw->clearFocus();
        });
    }

    controlLayout->addWidget(m_playButton);
    controlLayout->addWidget(stopButton);
    controlLayout->addWidget(prevButton);
    controlLayout->addWidget(nextButton);
    controlLayout->addSpacing(8);
    controlLayout->addWidget(m_timeLabel);
    controlLayout->addStretch(1);
    controlLayout->addWidget(m_streamButton);
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
    connect(m_streamButton, &QPushButton::clicked, this, &MainWindow::showStreamMenu);

    connect(m_progressSlider, &QSlider::sliderPressed, this, [this] { m_seeking = true; });
    connect(m_progressSlider, &QSlider::sliderReleased, this, &MainWindow::seekReleased);
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        applyVolume(
            value); // B14：用户手势统一入口（拖动/点击；回流回写被 QSignalBlocker 拦截不入此）
    });

    return left;
}

// A15：无状态按钮统一接线。注意 trigger() 是编程式激活、绕过 enabled 检查
// （只有菜单点击/快捷键分发尊重禁用态），故必须同步 enabledChanged 让按钮
// 随 action 置灰，用户路径才能真正被拦住
void MainWindow::bindButton(QAbstractButton *btn, QAction *action) {
    connect(btn, &QAbstractButton::clicked, action, &QAction::trigger);
    connect(action, &QAction::enabledChanged, btn, &QWidget::setEnabled);
    btn->setEnabled(action->isEnabled());
}

void MainWindow::createMenus() {
    using AM = ActionManager;
    auto act = [this](AM::Id id) { return m_actions->action(id); };

    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(act(AM::Id::Open));
    connect(act(AM::Id::Open), &QAction::triggered, this, &MainWindow::openFiles);
    fileMenu->addAction(act(AM::Id::Settings));
    connect(act(AM::Id::Settings), &QAction::triggered, this, &MainWindow::ensureSettingsDialog);
    fileMenu->addSeparator();
    fileMenu->addAction(act(AM::Id::Quit));
    connect(act(AM::Id::Quit), &QAction::triggered, this, &QWidget::close);

    QMenu *playMenu = menuBar()->addMenu(tr("&Play"));
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
    connect(act(AM::Id::SpeedUp), &QAction::triggered, this, [this] { changeSpeed(+0.25); });
    playMenu->addAction(act(AM::Id::SpeedDown));
    connect(act(AM::Id::SpeedDown), &QAction::triggered, this, [this] { changeSpeed(-0.25); });
    playMenu->addAction(act(AM::Id::SpeedReset));
    connect(act(AM::Id::SpeedReset), &QAction::triggered, this, &MainWindow::resetSpeed);
    // N13：视频/音频/字幕流三子菜单（视频在前），无流时整菜单禁用
    playMenu->addSeparator();
    m_videoStreamMenu = playMenu->addMenu(tr("Video"));
    m_audioStreamMenu = playMenu->addMenu(tr("Audio"));
    m_subStreamMenu = playMenu->addMenu(tr("Subtitle"));
    m_videoStreamMenu->setEnabled(false);
    m_audioStreamMenu->setEnabled(false);
    m_subStreamMenu->setEnabled(false);
    connect(m_videoStreamMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTrackMenus);
    connect(m_audioStreamMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTrackMenus);
    connect(m_subStreamMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTrackMenus);

    m_chapterMenu = menuBar()->addMenu(tr("&Chapter"));
    m_chapterGroup = new QActionGroup(this);
    m_chapterGroup->setExclusive(true);
    connect(m_chapterMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildChapterMenu);
    connect(act(AM::Id::AddBookmark), &QAction::triggered, this, &MainWindow::onAddBookmark);
    connect(act(AM::Id::BookmarkEditor), &QAction::triggered, this,
            &MainWindow::ensureBookmarkEditor);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
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

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(act(AM::Id::About));
    connect(act(AM::Id::About), &QAction::triggered, this, &MainWindow::showAboutDialog);
}

void MainWindow::connectController() {
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
    connect(m_controller, &MpvController::durationChanged, this, [this](const QString &, double v) {
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
            m_controller->command(
                {QStringLiteral("seek"), QString::number(target), QStringLiteral("absolute")});
        }
    });
    connect(m_controller, &MpvController::pauseChanged, this, [this](bool paused) {
        m_playButton->setIcon(
            style()->standardIcon(paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
        // N7 #1：播放↔暂停 OSD（仅播放态互切，起停由 #2/#3 去重覆盖）
        if (m_playlistPos >= 0) {
            // B14：视频区单击引发的回声在此一次性吞咽——暂停 OSD 由单击
            // 超时补显，双击场景（含恢复回声）全程静默；标志随按压周期复位
            if (m_suppressPlayPauseOsd) {
                m_suppressPlayPauseOsd = false;
                return;
            }
            m_pendingPlayPause = paused ? tr("Pause") : tr("Play");
            if (!m_osdCoalescePending) {
                m_osdCoalescePending = true;
                QTimer::singleShot(0, this, &MainWindow::flushOsd);
            }
        }
    });
    connect(m_controller, &MpvController::volumeChanged, this, [this](double v) {
        QSignalBlocker blocker(m_volumeSlider);
        m_volumeSlider->setValue(int(v + 0.5));
        // B14：推送仅同步滑条，OSD 已移至用户动作统一入口 applyVolume 直显
    });
    // R3：音量上限（mpv volume-max）推送，同步滑条量程
    connect(m_controller, &MpvController::volumeMaxChanged, this,
            [this](double v) { applyVolumeMax(qRound(v)); });
    connect(m_controller, &MpvController::playlistChanged, m_playlist,
            &PlaylistWidget::setPlaylist);
    // 播放态列表长度变化影响步进启停（A16）
    connect(m_controller, &MpvController::playlistChanged, this,
            [this](const QVariantList &) { updateNavigationActions(); });
    // N25：回刷差异收编后镜像重新与 mpv 序列一致，但 playlist-pos 值不变不再推送，
    // pos 推送时刻按旧镜像取的错名会挂到下一次切条目——仅错名时主动刷新标题/OSD
    // （追加等不影响正播名的收编不弹 OSD）
    connect(m_playlist, &PlaylistWidget::playingListAdopted, this, [this]() {
        const QString f = playingFilename();
        if (!f.isEmpty() && f != windowTitle())
            updateNowPlayingUi();
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
                    m_playlist->requestTab(pt); // N19：忙时排队，空闲补执行
            }
            // N7 #2 + N8 Part1：起播/切文件 OSD 与窗口标题同源（helper 收敛，
            // 真实文件名含后缀，不用 media-title）
            updateNowPlayingUi();
            // 强制重置播放暂停按钮状态
            bool paused = m_controller->propertyFlag("pause", true);
            m_playButton->setIcon(
                style()->standardIcon(paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
        }
    });
    // B9 Part1：整表自然播完（EOF）即清进度留记忆项，播放按钮即可从头播；
    // 手动停止/四步式等 reason≠eof 不触发。判定点即本事件，零事件顺序依赖；
    // 中间条目接续时清除结果旋即被 playlistPosChanged 的换条目起记覆盖，无副作用。
    // m_playlistPos>=0 守卫排除启动期与已停止态
    connect(m_controller, &MpvController::endFile, this, [this](int reason) {
        if (reason == MPV_END_FILE_REASON_EOF && m_playlistPos >= 0)
            m_playlist->clearResumeProgress(m_playlist->playingTabIndex());
    });
    // B18：文件卸载即重置会话态（所有 reason 无条件）——tracks / marks /
    // duration 绑定当前文件，卸载即失效；时序与现行 observe NONE 推送一致
    // （卸载瞬间清空，新文件 FILE_LOADED 回填），切换间隙与停止态均无陈旧残留。
    // 只动会话态，不触碰播放列表仓（档案冻结语义见规格 2.7.2）
    connect(m_controller, &MpvController::endFile, this, [this](int) {
        m_timePos = 0.0;
        m_duration = 0.0;
        updateTimeDisplay();
        {
            // 与 durationChanged(0.0) 旧复位分支同款：不触发用户感知的 moved 信号
            QSignalBlocker blocker(m_progressSlider);
            m_progressSlider->setValue(0);
        }
        m_tracks.clear();
        updateStreamControls();
        m_marks->setChapters({});
        updateChapterSlider();
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
                m_pendingStop = tr("Stop");
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
    connect(m_playlist, &PlaylistWidget::stopRequested, this,
            [this]() { m_controller->command({QStringLiteral("stop")}); });
    // N4：元数据回填路由——按"当前正播路径"定位全局仓条目（推送数据覆写）。
    // 路径经 normalizePathForMatch 规范化，与条目存储键同形，防止写入幻影条目。
    // B18：0 秒（FILE_LOADED 读不到）同其余 4 项一样走"读取失败即清空"，
    // 故不在此拦截；卸载路径不发 0.0（PropDuration 跳过 NONE），档案不受影响
    connect(m_controller, &MpvController::durationChanged, m_playlist,
            [this](const QString &p, double sec) {
                // mpv 来源路径：本地 cleanPath+原生分隔符，网络/流保持原样（B15）
                m_playlist->applyDuration(Util::resolveLocalPath(p), sec);
            });
    // N18：其余 mpv 元数据（media-metadata / file-format / tracks / chapters）
    // 不再回填 PlaylistWidget——属性对话框改读 MediaInfoHelper 缓存，
    // 轨道数据由 MainWindow::m_tracks 持有，章节由 MarkAggregator 持有。
    // MpvController 端信号保留，仍服务上述消费者。
    // N13/N14：轨道列表驱动菜单与按钮状态
    connect(m_controller, &MpvController::tracksChanged, this,
            [this](const QString &, const QList<MpvController::TrackInfo> &tracks) {
                m_tracks = tracks;
                updateStreamControls();
            });
    // N15：章节
    connect(m_controller, &MpvController::chaptersChanged, this,
            [this](const QString &, const QList<MpvController::ChapterInfo> &list) {
                // 去重+排序在聚合器内完成（严格 == 保留首项，规则单一持有）
                m_marks->setChapters(list);
                updateChapterSlider();
                // 菜单勾选：timePosChanged 常驻驱动（§3.7），重建在 aboutToShow
            });
    connect(m_bookmarkMgr, &BookmarkManager::changed, this, [this] {
        m_marks->invalidate();
        updateChapterSlider();
        updateChapterMenuState();
        if (m_bookmarkEditor)
            m_bookmarkEditor->refresh();
    });
    // duration 变更影响滑条比例与添加书签可用态（全局快捷键 P 不依赖菜单展开）。
    // 置脏：duration 是文件装载完成信号之一（path 已就位），保证切换后
    // 按新文件惰性重查一次
    connect(m_controller, &MpvController::durationChanged, this, [this](const QString &, double) {
        m_marks->invalidate();
        updateChapterSlider();
        updateChapterMenuState();
    });
    // N15 §3.7：菜单打开期间勾选随播放进度实时单选切换
    // （聚合器有缓存、菜单未展开时 actions 为空，代价可忽略）
    connect(m_controller, &MpvController::timePosChanged, this,
            [this](double) { updateChapterMenuChecked(); });
    connect(m_controller, &MpvController::playlistPosChanged, this, [this](int) {
        // 换行即换文件：章节随文件走，先清除防旧文件章节残留
        // （新文件 chapter-list 装载后经 chaptersChanged 回填；spec §4.2）
        m_marks->setChapters({});
        updateChapterMenuState();
        updateChapterSlider();
    });
    connect(m_controller, &MpvController::idleActive, this, [this](bool) {
        // 停止即无当前文件：清除章节（spec §4.2，playlistPosChanged(-1) 已先行）
        m_marks->setChapters({});
        updateChapterMenuState();
        updateChapterSlider();
    });

    // N10/B16：起播拿到分辨率即适配窗口。videoSizeChanged 由 mpv 的 width/height
    // 两独立属性分别推送合并触发，单次装载会产生中间对（如 (1920,288) 先于 (1920,1080)）；
    // 经单发定时器合并突发，仅以最后一次一致对触发一次自适应（B16 草案）
    m_videoAdaptTimer.setSingleShot(true);
    connect(&m_videoAdaptTimer, &QTimer::timeout, this,
            [this] { adaptWindowSizeToVideo(m_pendingVideoW, m_pendingVideoH); });
    connect(m_controller, &MpvController::videoSizeChanged, this, [this](int w, int h) {
        m_pendingVideoW = w;
        m_pendingVideoH = h;
        if (m_fullscreen && w > 0 && h > 0)
            m_refitAfterFullscreen = true;
        m_videoAdaptTimer.start(0);
    });
    // 停止态步进以选中 tab 为基准，切换 tab 需刷新启停（A16）
    connect(m_playlist, &QTabWidget::currentChanged, this,
            [this](int) { updateNavigationActions(); });
    // N19+A17：异步收尾/本地增删清后条目数已变（切 tab 时可能还是空表），补刷新启停；
    // 播放中 tab 另有 mpv 事件兜底，此处幂等重刷
    connect(m_playlist, &PlaylistWidget::contentsChanged, this,
            [this]() { updateNavigationActions(); });
    // mpv 侧退出：补保存会话后再结束事件循环（A6）
    connect(m_controller, &MpvController::mpvShutdown, this, &MainWindow::onMpvShutdown);
}

void MainWindow::openFiles() {
    // N9：过滤器模式由全局媒体扩展名清单生成（与 DirProvider 默认值同源）
    QStringList patterns;
    for (const QString &ext : App::defaultMediaExtensions())
        patterns << QStringLiteral("*.") + ext;
    QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Open File"), QString(),
        tr("Media files (%1);;All files (*)").arg(patterns.join(QLatin1Char(' '))));
    if (files.isEmpty())
        return;
    // Qt 来源：拿到即转原生分隔符（B15），逐文件加工
    for (QString &f : files)
        f = Util::nativePath(f);
    // N11：主菜单打开文件固定落点默认播放列表——清旧内容与续播记忆后重灌，
    // 随即整表开播（他 tab 播放中被抢占切换）；多选按对话框原始返回顺序从首项播起。
    // 有意不走 addFiles：绕开落点漂移与播放中 append 语义，拖放/右键添加不受影响
    m_playlist->setDefaultTabFiles(files);
    m_playlist->markPlayingTab(0);
    m_playlist->requestTab(0); // Q2 裁决：抢占切换时激活态同步跳到默认 tab（N19 忙时排队）
    onPlayRequested(files, 0, -1.0);
}

void MainWindow::playAt(int index) {
    m_pendingSeekRow = -1; // 手动播放丢弃未执行的挂起 seek，防陈旧跳转
    m_controller->setProperty(QStringLiteral("playlist-pos"), index);
    m_controller->setProperty(QStringLiteral("pause"), false);
    // 若 pause 原本就是 no，mpv 不会再推送属性变化，图标需手动更新
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
}

void MainWindow::togglePause() {
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

void MainWindow::enterStoppedState() {
    // stop 默认会清空播放列表，keep-playlist 标志只停止播放
    m_controller->command({QStringLiteral("stop"), QStringLiteral("keep-playlist")});
    // stop 不改变 pause 属性，按钮图标不会自动更新，手动置为“可播放”态
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
}

void MainWindow::stop() { enterStoppedState(); }

void MainWindow::playlistPrev() {
    if (m_playlistPos < 0) {
        stepStopped(false);
        return;
    }
    m_controller->command({QStringLiteral("playlist-prev")});
    // 步进隐含开始播放（A9/A14）：暂停态下新条目继承 pause=yes，显式恢复播放
    m_controller->setProperty(QStringLiteral("pause"), false);
    // 始终选中正在播放的列表（A9；N19 忙时排队）
    const int pt = m_playlist->playingTabIndex();
    if (pt >= 0 && m_playlist->currentIndex() != pt)
        m_playlist->requestTab(pt);
}

void MainWindow::playlistNext() {
    if (m_playlistPos < 0) {
        stepStopped(true);
        return;
    }
    m_controller->command({QStringLiteral("playlist-next")});
    // 同上（A9/A14）
    m_controller->setProperty(QStringLiteral("pause"), false);
    // 始终选中正在播放的列表（A9；N19 忙时排队）
    const int pt = m_playlist->playingTabIndex();
    if (pt >= 0 && m_playlist->currentIndex() != pt)
        m_playlist->requestTab(pt);
}

// 停止态的上一个/下一个（A9/A16）：以当前选中列表为基准步进并开始播放。
// 起点：有记忆取记忆项，否则第 0 项；列表循环开启时首尾互绕，非循环越界无动作
void MainWindow::stepStopped(bool next) {
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
            target = 0; // 尾项下一个 → 绕首项
    } else if (target < 0 || target >= files.size()) {
        return;
    }
    m_playlist->markPlayingTab(cur);
    onPlayRequested(files, target, -1.0);
}

// A16：上一/下一动作三路联动启停（矩阵见 specs/A16.md）。
// 仅操作这两个动作，不得触碰其他动作的禁用逻辑（如全屏期导航器守卫）
void MainWindow::updateNavigationActions() {
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

void MainWindow::seekReleased() {
    m_seeking = false;
    const double percent = m_progressSlider->value() / 10.0;
    m_controller->command(
        {QStringLiteral("seek"), QString::number(percent), QStringLiteral("absolute-percent")});
    // N7 #7：进度跳转 OSD（固定 hh:mm:ss）。N8 Part5：duration<=0 分支已删——
    // 停止态滑条禁用后不可达，残余场景（直播流）下 absolute-percent 本就无效
    if (m_duration > 0.0)
        showOsd(
            tr("Seek: %1").arg(Util::formatTimePercent(m_duration * percent / 100.0, m_duration)));
}

void MainWindow::setPlaylistVisible(bool visible) {
    if (visible) {
        // 锚点必须在改变可见性前取：setVisible 只让布局失效、重排可能异步，
        // 晚读会拿到已被挤窄的宽度（A20 坑 2）
        const int leftW = m_splitter->widget(0)->width();
        m_playlist->setVisible(true);
        applyPlaylistGeometry(true, leftW);
    } else {
        // 宽度必须在隐藏前读：隐藏后 splitter 不再排布它，读到的是失效值（A20 坑 1）
        if (const int p = m_playlist->width(); p > 0)
            m_playlistWidth = p;
        const int leftW = m_splitter->widget(0)->width();
        m_playlist->setVisible(false);
        applyPlaylistGeometry(false, leftW);
    }
    // 三个控件的状态统一由本函数收口（A12）：启动恢复路径不经过
    // 按钮→动作链，若不同步 QAction，两者分叉会导致首次点击空转
    {
        QSignalBlocker blocker(m_playlistToggle);
        m_playlistToggle->setChecked(visible);
    }
    QSignalBlocker actionBlocker(m_playlistAction);
    m_playlistAction->setChecked(visible);
}

// ---- A20：播放列表显隐几何联动 ----

// 把外框矩形 frame 按最小移动原则收进 area：两轴独立钳制，能不动就不动，
// 越界多少移多少；返回外框左上角，未越界时原样返回（调用方据此决定是否 move）。
// +1：QRect::right() == left + width - 1，合法左上角上限为区域右边界 − 外框宽度 + 1；
// qMax 兜住"框比区还大"时钳制区间颠倒的退化
QPoint MainWindow::minimalMoveTopLeft(const QRect &frame, const QRect &area) const {
    const int maxX = qMax(area.left(), area.right() - frame.width() + 1);
    const int maxY = qMax(area.top(), area.bottom() - frame.height() + 1);
    return {qBound(area.left(), frame.left(), maxX), qBound(area.top(), frame.top(), maxY)};
}

// 播放列表显隐的几何联动：左 pane（视频区）宽度保持不变，展开所需宽度全部由
// 主窗口向右扩展承担；工作区不足时先压视频区至下限，触底后再压播放列表宽度并
// 按实测值改写记忆。全屏/最大化/窗口未显示时不动几何
void MainWindow::applyPlaylistGeometry(bool expanding, int leftAnchor) {
    if (m_fullscreen || isMaximized())
        return;         // 几何由全屏/平台托管：仅显隐
    if (!isVisible()) { // 构造期（启动恢复）：外框尺寸与所在屏均不可靠（N1⑥ 同类）
        if (expanding)
            m_playlistGeometryPending = true;
        return;
    }

    // A20 修正（实测日志定位）：setVisible 引发的 splitter 重排是延迟事件
    // （LayoutRequest 入队，事件循环才派发），而主窗布局每次 activate 都会把
    // 当时的布局最小宽"钉"成主窗显式 minimumWidth（QLayout::activate 的
    // SetDefaultConstraint 行为），且该最小宽存在多跳缓存（splitter 重排 →
    // 主窗布局缓存失效逐级传播）。显隐后立即 resize 会被旧钉钳住——实测隐藏后
    // minimumWidth 仍钉着可见态的 705，窗口收不回 480、视频区被动右扩。
    // 故循环冲刷延迟布局事件直至最小尺寸稳定（每轮推进一跳），再手动 activate
    // 一次重钉新值，随后的 resize 收敛才不被旧值拦截
    for (int k = 0; k < 4; ++k) {
        const QSize minBefore = minimumSizeHint();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        if (minimumSizeHint() == minBefore)
            break;
    }
    if (layout())
        layout()->activate();

    const QRect avail = screen()->availableGeometry();
    const int chromeW = frameGeometry().width() - width(); // 外框水平装饰
    const int maxClientW = avail.width() - chromeW;        // 客户区宽度上限（桌面宽度）
    const int handle = m_splitter->handleWidth();
    const int videoMinW = App::videoDefaultSize().width(); // 480，视频区最小宽度
    const int pMin = m_playlist->minimumWidth();           // 220

    int p = m_playlistWidth > 0 ? m_playlistWidth : App::playlistDefaultWidth();
    int videoW = leftAnchor; // 左 pane（视频区）目标宽度
    int clientW;
    if (expanding) {
        clientW = qMin(leftAnchor + p + handle, maxClientW); // ① 工作区宽度封顶
        videoW = qMin(leftAnchor, clientW - p - handle);     // ② 先压视频区
        if (videoW < videoMinW) {                            // 视频区触底 → 再压播放列表
            videoW = videoMinW;
            p = qMax(pMin, qMin(p, maxClientW - videoW - handle)); // ③ 压缩播放列表宽度
            clientW = qMin(videoW + p + handle, maxClientW);
        }
    } else {
        // 隐藏后左 pane 独占客户区（把手随隐藏项隐去），窗口宽度收回
        clientW = qMin(leftAnchor, maxClientW);
        videoW = clientW;
    }

    // 迭代收敛（B16 同款）：不写死"把手宽度 / splitter 边距 / 非视频域恒定"等假设，
    // 每步读左 pane 实测宽度补残差，最多 3 步；无进展即停，避免受最小尺寸钳制时空转
    int lastAchieved = -1;
    for (int i = 0; i < 3; ++i) {
        resize(qMax(minimumWidth(), clientW), height());
        if (layout())
            layout()->activate();
        if (expanding)
            m_splitter->setSizes({videoW, p}); // 增量必须显式交给播放列表（A20 事实 2）
        if (layout())
            layout()->activate();
        const int achieved = m_splitter->widget(0)->width();
        if (achieved == videoW || achieved == lastAchieved)
            break;
        lastAchieved = achieved;
        clientW = qMin(clientW + (videoW - achieved), maxClientW);
    }

    // 记忆按收敛后的实测宽度写回：窗口被最小宽度钳制时目标与实际分叉，
    // 按目标写回会让下次展开/收缩口径错位（评审修订）
    if (expanding) {
        if (const int actual = m_playlist->width(); actual > 0)
            m_playlistWidth = actual;
    }

    // 最小移动收回：宽度变化只可能产生右向越界，仅水平轴实际生效；垂直轴不动
    const QRect fr = frameGeometry();
    const QPoint tl = minimalMoveTopLeft(fr, avail);
    if (tl != fr.topLeft())
        move(tl); // move() 对外框坐标（N1⑥），resize() 对客户区坐标（N10）
}

// 启动几何收口：视频区恰好为默认尺寸 480×320——宽度经 applyPlaylistGeometry
// 一次定死（setSizes 锁定左 pane），高度单独迭代收敛（只动高度：若顺手改宽度，
// splitter 会重新分配、视频区宽度被破坏），最后居中。只在首次 show 执行
void MainWindow::applyStartupGeometry() {
    const int wantW = App::videoDefaultSize().width();
    const int wantH = App::videoDefaultSize().height();
    if (m_playlistGeometryPending) {
        m_playlistGeometryPending = false;
        applyPlaylistGeometry(true, wantW); // 展开：视频区 480 + 播放列表 + 把手
    } else {
        applyPlaylistGeometry(false, wantW); // 隐藏：左 pane 独占，客户区宽 = 480
    }
    // 高度：以 m_videoWidget 实测高度补残差（B16 同款，通常 1 步到位）。
    // 读数依赖占位符 resize 事件链触发的 syncVideoGeometry，前置定宽流程已布局落地
    int lastAchieved = -1;
    for (int i = 0; i < 4; ++i) {
        const int delta = wantH - m_videoWidget->height();
        resize(width(), height() + delta);
        if (layout())
            layout()->activate();
        const int achieved = m_videoWidget->height();
        if (achieved == wantH || achieved == lastAchieved)
            break;
        lastAchieved = achieved;
    }
    centerOnScreen();
}

// 窗口居中到所在屏工作区：外框居中（move 定位外框左上角，故按外框尺寸算）。
// 末尾过一遍最小移动钳回，兜住"窗口比工作区还大"的退化（此时居中会让四周越界）。
// 居中必须是启动几何的最后一步：定宽/定高都会改变外框，先居中会被后续 resize 破坏
void MainWindow::centerOnScreen() {
    const QRect avail = screen()->availableGeometry();
    const QRect fr = frameGeometry();
    move(avail.left() + (avail.width() - fr.width()) / 2,
         avail.top() + (avail.height() - fr.height()) / 2);
    const QRect after = frameGeometry();
    const QPoint clamped = minimalMoveTopLeft(after, avail);
    if (clamped != after.topLeft())
        move(clamped);
}

// 导航器面板显隐（N1 显隐语义 + N3 按需创建生命周期）：零持久化——
// 每次打开都新建实例并定位到主窗口右下角；关闭（菜单/系统关闭钮）
// 统一经 destroyNavigator 复位画面并销毁
void MainWindow::setNavigatorVisible(bool visible) {
    if (visible) {
        ensureNavigator();
        return;
    }
    destroyNavigator();
}

// 首次需要时构造导航器浮窗并接好信号，再走标准显示流程（N1）
void MainWindow::ensureNavigator() {
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
void MainWindow::destroyNavigator() {
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
void MainWindow::repositionNavigator() {
    if (!m_navigator || !m_navigator->isVisible())
        return;
    const QPoint corner = mapToGlobal(QPoint(width(), height()));
    const QRect frame = m_navigator->frameGeometry();
    QPoint pos(corner.x() - frame.width() - 12, corner.y() - frame.height() - 12);
    // 可用区来源是导航器所在屏而非主窗屏，替换最小移动实现时保留口径（A20 3.8）
    const QRect avail = m_navigator->screen()->availableGeometry();
    pos = minimalMoveTopLeft(QRect(pos, frame.size()), avail); // 顺带修正旧实现缺失 +1 的 1px 过移
    m_navigator->move(pos);
    m_navigator->refresh(); // 渲染区域尺寸可能已变，视口矩形需重算
}

// ---- N7：VideoOverlay 统一 OSD（A3 顶级 Tool 浮窗）----
namespace {
class OsdLabel : public QLabel {
  public:
    using QLabel::QLabel;

  protected:
    void paintEvent(QPaintEvent *) override {
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
    QSize sizeHint() const override {
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

void MainWindow::ensureVideoOverlay() {
    if (m_videoOverlay)
        return;
    m_videoOverlay =
        new QWidget(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                              Qt::WindowTransparentForInput);
    m_videoOverlay->setAttribute(Qt::WA_TranslucentBackground);
    m_videoOverlay->setAttribute(Qt::WA_ShowWithoutActivating);
    m_videoOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // 容器透明，内部控件自绘
    auto *layout = new QVBoxLayout(m_videoOverlay);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(0);
    // B12：SetNoConstraint——浮窗几何恒由 setGeometry(视频区矩形) 决定，
    // 不被内容 sizeHint 撑大；超宽文本（长文件名）由父窗口边界自然硬切，
    // 杜绝默认约束模式把内容最小尺寸施加为顶级窗口尺寸而压过播放列表
    layout->setSizeConstraint(QLayout::SetNoConstraint);
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

void MainWindow::repositionVideoOverlay() {
    if (!m_videoOverlay || !m_videoWidget)
        return;
    const QPoint globalPos = m_videoWidget->mapToGlobal(QPoint(0, 0));
    m_videoOverlay->setGeometry(QRect(globalPos, m_videoWidget->size()));
}

void MainWindow::showOsd(const QString &text) {
    ensureVideoOverlay();
    m_osdLabel->setText(text);
    m_osdLabel->adjustSize();
    repositionVideoOverlay();
    m_videoOverlay->show();
    m_osdLabel->show();
    m_videoOverlay->raise();
    m_osdTimer->start(2000);
}

void MainWindow::flushOsd() {
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

void MainWindow::moveEvent(QMoveEvent *event) {
    QMainWindow::moveEvent(event);
    repositionNavigator();
    repositionVideoOverlay();
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    repositionNavigator();
    repositionVideoOverlay();
    // N3：showFullScreen/showNormal 改变的是主窗尺寸，主窗 resize 是
    // 全屏/还原后视频区几何同步的可靠最终触发点（幂等）
    syncVideoGeometry();
}

// A20：启动几何收口的触发点——首次 show 时 frameGeometry() 与 screen() 才可靠
// （构造期二者均不可信，N1⑥ 同类）。最小化恢复、A18 全屏退出的 show() 都会
// 再触发本事件，必须一次性守卫：重复执行会把窗口越推越大、把位置反复拉回居中
void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (m_firstShowDone)
        return;
    m_firstShowDone = true;
    applyStartupGeometry();
}

// 视频区几何手动同步（N3）：按窗口模式分支计算目标几何。
// 全屏铺满整个客户区；普通态对齐占位符——占位符 geometry 相对其父容器，
// 须 mapTo 主窗坐标系
void MainWindow::syncVideoGeometry() {
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

// N10：普通态窗口自适应当前视频——视频域尽可能物理像素 1:1（逻辑尺寸 =
// 像素 ÷ devicePixelRatio，高 DPI 下不被系统缩放拉伸），任一边超出所在屏
// 工作区即整窗等比缩到恰好放入；resize 后外框越出工作区时按最小移动原则
// 两轴独立钳回（恰好不越界）。全屏/最大化/无视频流不动作；
// 停止不复位、全屏退出维持全屏前普通尺寸（本函数仅在分辨率推送时触发）
void MainWindow::adaptWindowSizeToVideo(int videoW, int videoH) {
    if (videoW <= 0 || videoH <= 0 || m_fullscreen || isMaximized())
        return;
    const QRect work = screen()->availableGeometry();
    const qreal dpr = devicePixelRatioF();
    QSize want(qRound(videoW / dpr), qRound(videoH / dpr));
    // 双重余量：外框装饰 + 客户区内非视频域（菜单栏/进度行/控制行固定高度）。
    // 若只扣外框，缩放分支会把视频域放到"工作区−外框"上限，
    // 加上非视频域后整窗仍会撑破工作区（验收实测缺陷）
    const QSize frameChrome = frameGeometry().size() - geometry().size();
    const QSize clientReserve = geometry().size() - m_videoWidget->size();
    const QSize availVideo = work.size() - frameChrome - clientReserve;
    if (availVideo.width() > 0 && availVideo.height() > 0 &&
        (want.width() > availVideo.width() || want.height() > availVideo.height()))
        want.scale(availVideo, Qt::KeepAspectRatio);
    // B16：迭代收敛——单步增量假设"非视频域宽度恒定"不成立（右侧 splitter/播放列表
    // 会随窗口变宽吸收部分增量），故每步 resize 后强制布局落地、读真实 video 尺寸，
    // 以残差 want−实际 再补增量，1~2 步收敛到精确 want；无进展（如受占位符最小尺寸
    // 钳制无法再缩）即停，避免空转
    QSize target = size() + want - m_videoWidget->size();
    QSize lastAchieved;
    for (int i = 0; i < 4; ++i) {
        resize(target);
        if (layout())
            layout()->activate(); // 同步落实子布局（含 splitter/播放列表重分布 + 视频区对位）
        const QSize achieved = m_videoWidget->size();
        if (achieved == want || achieved == lastAchieved)
            break;
        lastAchieved = achieved;
        target = size() + want - achieved;
    }
    // 最小移动收回：仅当外框越出工作区时钳回，不动原本合法的位置（A20 复用私有辅助）
    const QRect fr = frameGeometry();
    const QPoint tl = minimalMoveTopLeft(fr, work);
    if (tl != fr.topLeft())
        move(tl);
}

// 覆盖式全屏切换（N3）：不隐藏控件、不重父化、wid 不重设、零黑闪
void MainWindow::toggleFullscreen() {
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
        // A20：全屏禁用播放列表切换（禁动作即禁快捷键；禁用不影响 setChecked 同步）
        m_playlistAction->setEnabled(false);
        m_fullscreen = true;
        m_refitAfterFullscreen = false; // 进全屏清空：仅记录"本次全屏期内是否换片"
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
        DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()), DWMWA_WINDOW_CORNER_PREFERENCE,
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
        // B16：全屏期内若切换过视频，当前尺寸与"进全屏前的小窗"已不一致；
        // 恢复普通态后按当前视频尺寸重新适配（最大化分支不走此路，保持全屏宽度）
        if (m_refitAfterFullscreen) {
            m_refitAfterFullscreen = false;
            m_videoAdaptTimer.start(0); // 复用合并定时器，布局落定后以当前 pending 触发
        }
        m_controller->setProperty(QStringLiteral("video-zoom"), m_savedZoom);
        m_controller->setProperty(QStringLiteral("video-pan-x"), m_savedPanX);
        m_controller->setProperty(QStringLiteral("video-pan-y"), m_savedPanY);
        if (m_navigator) { // 非 null ⟹ 必为被本方案隐藏的全屏前可见面板
            m_navigator->show();
            repositionNavigator();
        }
        m_navigatorAction->setEnabled(true);
        m_playlistAction->setEnabled(true); // A20：退出全屏恢复播放列表切换
    }
    {
        QSignalBlocker blocker(m_fullscreenAction); // 防循环触发 toggleFullscreen
        m_fullscreenAction->setChecked(m_fullscreen);
    }
}

// 实时读 mpv 的 pause（缓存可能滞后一个事件周期）；读取失败按已暂停处理，
// 走延迟路径更安全
bool MainWindow::isPaused() { return m_controller->propertyFlag(QStringLiteral("pause"), true); }

// 单击定时器超时：确非双击，按按压来源分流收尾。
// 播放态点击（B14）：暂停已即时生效，此处补显被吞咽的暂停 OSD；
// 暂停/停止态点击：执行延迟的恢复/开播动作
void MainWindow::onVideoClickTimeout() {
    if (!m_clickActionPending)
        return;
    m_clickActionPending = false;
    if (m_pendingPauseRestore) {
        // 实时状态校验：防窗口期内空格键反向操作后错显"暂停"
        if (isPaused())
            showOsd(tr("Pause"));
    } else {
        togglePause();
    }
}

// 占位符几何跟随 + 视频区单击/双击分流（N3）。
// 右键与 Release 不处理：右键无任何行为变化
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
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
            // 入口无条件复位各标志：状态只在单轮 press→(双击|超时)
            // 生命周期内存活，杜绝上轮残留泄漏进本轮
            m_pendingPauseRestore = false;
            m_clickActionPending = false;
            m_suppressPlayPauseOsd = false;
            if (m_playlistPos >= 0 && !isPaused()) {
                // 播放中：立即暂停（单击终态不变），OSD 延迟到超时补显；
                // 双击则由恢复回声吞咽实现全程静默（B14）。
                // 置 pending 使超时门闸放行（按来源分流补显/动作）
                togglePause();
                m_pendingPauseRestore = true;
                m_clickActionPending = true;
                m_suppressPlayPauseOsd = true;
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
        // 停止态控件已禁用收不到鼠标事件，天然不显示。
        // N15 实测定论（二分定位）：卡顿根因是自绘路径中“单独的手柄绘制调用”
        // （见 ChapterSlider 注释）；tooltip 无辜，故恢复逐事件直接处理
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
            const double hoverTime = m_duration * percent / 100.0;
            QString tip;
            if (const auto hit = m_marks->hit(hoverTime); hit && !hit->title.isEmpty())
                tip = hit->title + QStringLiteral("\n");
            tip += Util::formatTimePercent(hoverTime, m_duration);
            QToolTip::showText(global, tip, m_progressSlider);
            break;
        }
        case QEvent::Leave:
            // 拖动中移出不隐藏（保持常显），仅悬停离开时收起
            if (!m_seeking)
                QToolTip::hideText();
            break;
        case QEvent::Wheel:
            // 禁止滚轮控制进度条
            return true;
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
                                   QStringLiteral("%1%").arg(m_volumeSlider->value()),
                                   m_volumeSlider);
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

// B14：用户音量动作统一收口——钳制、下发 mpv 并直显音量 OSD。
// 三条路径汇入：changeVolume（滚轮/快捷键）、滑条 valueChanged（拖动/点击）。
// 启动恢复刻意绕行：restoreSettings 以 QSignalBlocker 直写滑条并直接
// setProperty，不经此函数——启动路径从此无任何 OSD 调用点
void MainWindow::applyVolume(int value) {
    const int v = qBound(0, value, m_volumeMax);
    m_controller->setProperty(QStringLiteral("volume"), double(v));
    showOsd(tr("Volume: %1%").arg(v));
}

// N5：统一音量调节入口。以滑条缓存为基准步进后交 applyVolume；
// UI 不在此处回写，由 volumeChanged 观察回流刷新（防环已有 QSignalBlocker）
void MainWindow::changeVolume(int delta) { applyVolume(m_volumeSlider->value() + delta); }

// B11：正在播放的真实文件名（含后缀不含路径），标题与 OSD #2 同源。
// 列表镜像优先——playlist-pos 推送早于新文件装载完成，此刻读 filename 属性
// 会拿到上一个文件的残留值（概率性错名根因）；本地镜像顺序与 mpv 恒一致，
// 按 pos 取名为确定性来源。属性仅作镜像缺失时的兜底（如起播早期回刷未达）
QString MainWindow::playingFilename() {
    const int pt = m_playlist->playingTabIndex();
    if (pt >= 0 && m_playlistPos >= 0) {
        const QStringList files = m_playlist->filesOf(pt);
        if (m_playlistPos < files.size())
            return QFileInfo(files[m_playlistPos]).fileName();
    }
    return m_controller->propertyString(QStringLiteral("filename"));
}

// N7 #2 + N8 Part1：标题与 OSD #2 同源刷新（B11 镜像取名）。
// N25：收编（外部变更落默认 tab）后镜像重新与 mpv 序列一致，但 playlist-pos
// 值不变不会再推送，pos 推送时刻按旧镜像取的错名会挂到下一次切条目——
// 收编后经 playingListAdopted 主动刷新一次
void MainWindow::updateNowPlayingUi() {
    if (m_playlistPos < 0)
        return; // 停止态无"正在播放"可言
    QString filename = playingFilename();
    const int pt = m_playlist->playingTabIndex();
    const int total = pt >= 0 ? m_playlist->filesOf(pt).size() : m_playlistPos + 1;
    if (!filename.isEmpty())
        setWindowTitle(filename); // N8 Part1：标题跟随真实文件名
    else
        filename = tr("Unknown File");
    m_pendingNowPlaying =
        QStringLiteral("[%1/%2] %3").arg(m_playlistPos + 1).arg(total).arg(filename);
    if (!m_osdCoalescePending) {
        m_osdCoalescePending = true;
        QTimer::singleShot(0, this, &MainWindow::flushOsd);
    }
}

// N8 Part3：进度条鼠标 x → 百分比（0~100）。经样式反查 groove/handle 几何，
// 扣除滑块半宽后线性映射，与拖动定位算法同口径
double MainWindow::progressPercentAt(const QPoint &pos) const {
    QStyleOptionSlider opt;
    opt.initFrom(m_progressSlider);
    opt.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
    opt.orientation = m_progressSlider->orientation();
    opt.minimum = m_progressSlider->minimum();
    opt.maximum = m_progressSlider->maximum();
    opt.sliderPosition = m_progressSlider->sliderPosition();
    opt.sliderValue = m_progressSlider->value();
    const QRect groove =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, m_progressSlider);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, m_progressSlider);
    const int span = groove.width() - handle.width();
    if (span <= 0)
        return -1.0;
    const int dx = pos.x() - handle.width() / 2 - groove.left();
    const int value =
        QStyle::sliderValueFromPosition(opt.minimum, opt.maximum, qBound(0, dx, span), span);
    return value / 10.0;
}

// N6：相对 seek。停止态（m_playlistPos<0）无播放位置可寻址，直接忽略；
// 不指定 flags 走 mpv 默认相对关键帧寻址，响应优先于逐帧精度
void MainWindow::seekRelative(int deltaSeconds) {
    if (m_playlistPos < 0)
        return;
    m_controller->command({QStringLiteral("seek"), QString::number(deltaSeconds)});
    // N7 #6：进度跳转 OSD（固定 hh:mm:ss）
    if (m_duration > 0.0) {
        const double target = qBound(0.0, m_timePos + deltaSeconds, m_duration);
        const QString sign =
            deltaSeconds >= 0 ? tr("+%1s").arg(deltaSeconds) : tr("%1s").arg(deltaSeconds);
        showOsd(tr("Seek: %1 / %2").arg(sign, Util::formatTimePercent(target, m_duration)));
    } else {
        const QString sign =
            deltaSeconds >= 0 ? tr("+%1s").arg(deltaSeconds) : tr("%1s").arg(deltaSeconds);
        showOsd(tr("Seek: %1").arg(sign));
    }
}

// N6：速度步进——实时读 speed 属性做读改写（不设观察缓存），
// 钳制到 [0.25, 8]；±0.25 为二进制精确小数，域内累加无舍入误差
void MainWindow::changeSpeed(double delta) {
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
    showOsd(tr("Speed: %1x").arg(num));
}

void MainWindow::resetSpeed() {
    m_controller->setProperty(QStringLiteral("speed"), 1.0);
    showOsd(tr("Speed: 1x"));
}

// N5：空白区域点击清焦兜底——未被交互控件消费的鼠标按下事件一路冒泡至此，
// 清除焦点使方向键回到全局音量模式；列表/滑条/按钮等自行消费按下事件，
// 不会到达此处，语义自动正确
void MainWindow::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (auto *fw = QApplication::focusWidget())
            fw->clearFocus();
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::onPlayRequested(const QStringList &files, int index, double seekTo,
                                 bool keepPaused) {
    // 统一播放入口（N4 合并）：整表载入 → 跳转 index → 挂起恢复进度。
    m_pendingSeekRow = -1; // 丢弃未执行的挂起 seek
    m_pendingSeekTime = -1.0;
    if (files.isEmpty())
        return;
    // N25：整表载入是多命令序列（stop→clear→N×append→pos），登记本地在途回显基准，
    // 使回刷把空列表/部分列表的中间态识别为本变更在途而忽略（files 即本地列表内容）
    m_playlist->noteLocalPush(files);
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
    // N30：pause 状态在文件切换后仍然保持——双击播放时显式恢复播放（keepPaused 为假）；
    // 四步式在暂停态下重推则写入 pause=yes 保持暂停。
    // 同值写入不触发属性变化推送，故无多余 pauseChanged / OSD，图标需手动更新（同 playAt）
    m_controller->setProperty(QStringLiteral("pause"), keepPaused);
    m_playButton->setIcon(
        style()->standardIcon(keepPaused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
}

void MainWindow::onRemoveRequested(QList<int> rows) {
    // 从后往前删，避免索引失效
    std::sort(rows.rbegin(), rows.rend());
    for (int row : rows)
        m_controller->command({QStringLiteral("playlist-remove"), QString::number(row)});
}

void MainWindow::onClearRequested() {
    // 同样需要先 stop，否则当前播放的条目会被保留
    m_controller->command({QStringLiteral("stop")});
    m_controller->command({QStringLiteral("playlist-clear")});
}

void MainWindow::onFilesAdded(const QStringList &files) {
    updateNavigationActions(); // 本地 tab 新增内容影响停止态步进（A16）；播放态由 playlistChanged
                               // 兜底
    // 只有添加到“正在播放”tab 的文件才同步进 mpv 播放列表；
    // 其他 tab 为本地列表，双击播放时整体载入。
    if (!m_playlist->currentIsPlayingTab())
        return;
    for (const QString &file : files)
        m_controller->command({QStringLiteral("loadfile"), file, QStringLiteral("append-play")});
}

void MainWindow::applyLoopMode(const QString &mode) {
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

QString MainWindow::currentLoopMode() const {
    if (m_controller->propertyString(QStringLiteral("loop-file")) == QLatin1String("inf"))
        return QStringLiteral("file");
    if (m_controller->propertyString(QStringLiteral("loop-playlist")) == QLatin1String("inf"))
        return QStringLiteral("playlist");
    return QStringLiteral("none");
}

// R3：按需创建/唤起设置对话框（仿 ensureBookmarkEditor 生命周期）。
// 非模态、关闭即销毁；菜单再次触发可重建并恢复已应用值。
void MainWindow::ensureSettingsDialog() {
    if (m_settingsDialog) {
        m_settingsDialog->show();
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }
    m_settingsDialog = new SettingsDialog(this, m_controller, m_configFile);
    m_settingsDialog->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(m_settingsDialog, &QObject::destroyed, this, [this] { m_settingsDialog = nullptr; });
    m_settingsDialog->show();
}

// R3：MpvController::volumeMaxChanged 回调——同步 m_volumeMax 与滑条量程。
// mpv 启动时会立即推送当前值（默认 130），无需手动初始化。
void MainWindow::applyVolumeMax(int v) {
    m_volumeMax = v;
    if (m_volumeSlider)
        m_volumeSlider->setRange(0, m_volumeMax);
}

void MainWindow::showAboutDialog() {
    QMessageBox::about(this, tr("About %1").arg(App::name()),
                       tr("%1: a Qt-based player GUI loaded as an mpv C plugin.\n"
                          "Video rendering is performed by mpv; this plugin only provides the "
                          "graphical interface.")
                           .arg(App::name()));
}

void MainWindow::onMpvShutdown() {
    // mpv 侧发起退出（终端按 q、其他脚本发 quit）时 closeEvent 不会触发，
    // 在此补保存会话；窗口关闭路径随后到来的重复保存写同值幂等无害
    saveSettings();
    QApplication::quit();
}

void MainWindow::saveSettings() {
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

    // N10：窗口几何/最大化不再落盘（启动恒为固定默认尺寸 960×600）

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

void MainWindow::restoreSettings() {
    QSettings settings(m_configFile, QSettings::IniFormat);

    // R3：循环模式 / 硬件解码 / 音量上限 改由 mpv 自管（mpv.conf + observe），
    // 插件 INI 不再读写这三项；启动期经 MpvController::volumeMaxChanged 推送
    // 经 applyVolumeMax 同步滑条量程（见 connectController）。

    // 音量步长（插件本体选项）
    m_volumeWheelStep =
        settings.value(QStringLiteral("volumeWheelStep"), m_volumeWheelStep).toInt();
    m_volumeKeyStep = settings.value(QStringLiteral("volumeKeyStep"), m_volumeKeyStep).toInt();
    if (settings.contains(QStringLiteral("volume"))) {
        const double vol =
            qBound(0.0, settings.value(QStringLiteral("volume")).toDouble(), double(m_volumeMax));
        m_controller->setProperty(QStringLiteral("volume"), vol);
        QSignalBlocker blocker(m_volumeSlider);
        m_volumeSlider->setValue(int(vol + 0.5));
    }

    // N10：窗口几何/最大化不再恢复，INI 遗留键
    // （windowMaximized/windowWidth/windowHeight）读侧删除后自然忽略

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
}

void MainWindow::updateTimeDisplay() {
    m_timeLabel->setText(
        QStringLiteral("%1 / %2").arg(Util::formatTime(m_timePos), Util::formatTime(m_duration)));
    if (!m_seeking && m_duration > 0.0)
        m_progressSlider->setValue(int(m_timePos / m_duration * 1000.0));
}

QString MainWindow::formatTime(double seconds) { return Util::formatTime(seconds); }

// ---- N13：轨道文案与切换 ----

qint64 MainWindow::trackBitrate(const MpvController::TrackInfo &t) const {
    if (t.demuxBitrate > 0)
        return t.demuxBitrate;
    if (t.metadataBPS > 0)
        return t.metadataBPS;
    return 0;
}

QString MainWindow::formatTrackText(const MpvController::TrackInfo &t) const {
    // 本地外挂流固定格式：[local] 文件名 [语言] (格式)
    if (t.external) {
        QString fn = QFileInfo(t.externalFilename).fileName();
        if (fn.isEmpty())
            fn = t.externalFilename;
        QString lang = t.lang.isEmpty() ? QStringLiteral("und") : t.lang;
        QString inner = t.codec;
        QString result = QStringLiteral("[local] %1 [%2]").arg(fn, lang);
        if (!inner.isEmpty())
            result += QStringLiteral(" (%1)").arg(inner);
        else
            result += QStringLiteral(" (%1)").arg(QStringLiteral("unknown"));
        if (t.isDefault)
            result += QStringLiteral(" [default]");
        return result;
    }

    QString namePart = t.title;
    QString lang = t.lang.isEmpty() ? QStringLiteral("und") : t.lang;
    QStringList inner;
    if (!t.codec.isEmpty())
        inner << t.codec;
    if (t.type == QLatin1String("video")) {
        if (t.demuxW > 0 && t.demuxH > 0)
            inner << QStringLiteral("%1x%2").arg(t.demuxW).arg(t.demuxH);
        if (t.demuxFps > 0.0) {
            QString s = QString::number(t.demuxFps, 'f', 2);
            if (s.contains(QLatin1Char('.'))) {
                while (s.endsWith(QLatin1Char('0')))
                    s.chop(1);
                if (s.endsWith(QLatin1Char('.')))
                    s.chop(1);
            }
            s += QStringLiteral("fps");
            inner << s;
        }
        qint64 br = trackBitrate(t);
        if (br > 0)
            inner << QString::number((br + 500) / 1000) + QStringLiteral("kbps");
    } else if (t.type == QLatin1String("audio")) {
        if (t.demuxSamplerate > 0) {
            QString s = QString::number(t.demuxSamplerate / 1000.0, 'f', 1);
            if (s.contains(QLatin1Char('.'))) {
                while (s.endsWith(QLatin1Char('0')))
                    s.chop(1);
                if (s.endsWith(QLatin1Char('.')))
                    s.chop(1);
            }
            s += QStringLiteral("kHz");
            inner << s;
        }
        if (t.demuxChannelCount > 0)
            inner << QStringLiteral("%1ch").arg(t.demuxChannelCount);
        qint64 br = trackBitrate(t);
        if (br > 0)
            inner << QString::number((br + 500) / 1000) + QStringLiteral("kbps");
    }
    // sub 仅 codec，已加入

    QString result;
    if (!namePart.isEmpty())
        result = namePart + QStringLiteral(" ");
    result += QStringLiteral("[%1]").arg(lang);
    if (!inner.isEmpty())
        result += QStringLiteral(" (%1)").arg(inner.join(QStringLiteral(", ")));
    if (t.isDefault)
        result += QStringLiteral(" [default]");
    return result;
}

void MainWindow::rebuildTrackMenus() {
    auto rebuild = [this](QMenu *menu, const QString &type) {
        if (!menu)
            return;
        menu->clear();
        QList<MpvController::TrackInfo> filtered;
        for (const auto &t : m_tracks)
            if (t.type == type)
                filtered.append(t);
        std::sort(filtered.begin(), filtered.end(),
                  [](const MpvController::TrackInfo &a, const MpvController::TrackInfo &b) {
                      return a.id < b.id;
                  });

        if (type == QLatin1String("sub")) {
            // 字幕首项"无"
            bool noSub = true;
            for (const auto &t : filtered)
                if (t.selected)
                    noSub = false;
            QAction *none = menu->addAction(tr("None"));
            none->setCheckable(true);
            none->setChecked(noSub);
            connect(none, &QAction::triggered, this, &MainWindow::disableSubtitles);
            for (const auto &t : filtered) {
                QAction *a = menu->addAction(formatTrackText(t));
                a->setCheckable(true);
                a->setChecked(t.selected);
                MpvController::TrackInfo copy = t;
                connect(a, &QAction::triggered, this, [this, copy] { switchToTrack(copy); });
            }
        } else {
            for (const auto &t : filtered) {
                QAction *a = menu->addAction(formatTrackText(t));
                a->setCheckable(true);
                a->setChecked(t.selected);
                MpvController::TrackInfo copy = t;
                connect(a, &QAction::triggered, this, [this, copy] { switchToTrack(copy); });
            }
        }
    };
    rebuild(m_videoStreamMenu, QStringLiteral("video"));
    rebuild(m_audioStreamMenu, QStringLiteral("audio"));
    rebuild(m_subStreamMenu, QStringLiteral("sub"));
}

// B18：流按钮与三个流子菜单的状态统一由 m_tracks 派生（tracksChanged 与
// END_FILE 会话态重置共用；后者清空 m_tracks 即按钮隐藏、菜单全禁用）
void MainWindow::updateStreamControls() {
    int v = 0, a = 0, s = 0;
    for (const auto &t : m_tracks) {
        if (t.type == QLatin1String("video"))
            ++v;
        else if (t.type == QLatin1String("audio"))
            ++a;
        else if (t.type == QLatin1String("sub"))
            ++s;
    }
    const bool visible = (v > 1) || (a > 1) || (s > 0);
    if (m_streamButton) {
        m_streamButton->setVisible(visible);
        m_streamButton->setEnabled(visible);
    }
    if (m_videoStreamMenu)
        m_videoStreamMenu->setEnabled(v > 0);
    if (m_audioStreamMenu)
        m_audioStreamMenu->setEnabled(a > 0);
    if (m_subStreamMenu)
        m_subStreamMenu->setEnabled(s > 0);
}

void MainWindow::showStreamMenu() {
    QMenu menu(this);
    QList<MpvController::TrackInfo> videos, audios, subs;
    for (const auto &t : m_tracks) {
        if (t.type == QLatin1String("video"))
            videos.append(t);
        else if (t.type == QLatin1String("audio"))
            audios.append(t);
        else if (t.type == QLatin1String("sub"))
            subs.append(t);
    }
    std::sort(videos.begin(), videos.end(),
              [](const MpvController::TrackInfo &a, const MpvController::TrackInfo &b) {
                  return a.id < b.id;
              });
    std::sort(audios.begin(), audios.end(),
              [](const MpvController::TrackInfo &a, const MpvController::TrackInfo &b) {
                  return a.id < b.id;
              });
    std::sort(subs.begin(), subs.end(),
              [](const MpvController::TrackInfo &a, const MpvController::TrackInfo &b) {
                  return a.id < b.id;
              });

    bool hasPrev = false;
    if (videos.size() > 1) {
        QAction *header = menu.addAction(tr("Video"));
        header->setEnabled(false);
        for (const auto &t : videos) {
            QAction *a = menu.addAction(formatTrackText(t));
            a->setCheckable(true);
            a->setChecked(t.selected);
            MpvController::TrackInfo copy = t;
            connect(a, &QAction::triggered, this, [this, copy] { switchToTrack(copy); });
        }
        hasPrev = true;
    }
    if (audios.size() > 1) {
        if (hasPrev)
            menu.addSeparator();
        QAction *header = menu.addAction(tr("Audio"));
        header->setEnabled(false);
        for (const auto &t : audios) {
            QAction *a = menu.addAction(formatTrackText(t));
            a->setCheckable(true);
            a->setChecked(t.selected);
            MpvController::TrackInfo copy = t;
            connect(a, &QAction::triggered, this, [this, copy] { switchToTrack(copy); });
        }
        hasPrev = true;
    }
    if (!subs.isEmpty()) {
        if (hasPrev)
            menu.addSeparator();
        QAction *header = menu.addAction(tr("Subtitle"));
        header->setEnabled(false);
        bool noSub = true;
        for (const auto &t : subs)
            if (t.selected)
                noSub = false;
        QAction *none = menu.addAction(tr("None"));
        none->setCheckable(true);
        none->setChecked(noSub);
        connect(none, &QAction::triggered, this, &MainWindow::disableSubtitles);
        for (const auto &t : subs) {
            QAction *a = menu.addAction(formatTrackText(t));
            a->setCheckable(true);
            a->setChecked(t.selected);
            MpvController::TrackInfo copy = t;
            connect(a, &QAction::triggered, this, [this, copy] { switchToTrack(copy); });
        }
    }
    if (menu.isEmpty())
        return;
    const QPoint pos = m_streamButton
                           ? m_streamButton->mapToGlobal(QPoint(0, m_streamButton->height()))
                           : QCursor::pos();
    menu.exec(pos);
}

void MainWindow::switchToTrack(const MpvController::TrackInfo &t) {
    QString prop;
    QString prefix;
    if (t.type == QLatin1String("video")) {
        prop = QStringLiteral("vid");
        prefix = tr("Video: ");
    } else if (t.type == QLatin1String("audio")) {
        prop = QStringLiteral("aid");
        prefix = tr("Audio: ");
    } else if (t.type == QLatin1String("sub")) {
        prop = QStringLiteral("sid");
        prefix = tr("Subtitle: ");
    } else {
        return;
    }
    m_controller->setProperty(prop, int(t.id));
    showOsd(prefix + formatTrackText(t));
}

void MainWindow::disableSubtitles() {
    m_controller->setProperty(QStringLiteral("sid"), QStringLiteral("no"));
    showOsd(tr("Subtitle: None"));
}

// ---- N15：章节与书签 ----
void MainWindow::updateChapterSlider() {
    m_progressSlider->setDuration(m_duration);
    const auto &marks = m_marks->marks();
    QList<double> times;
    times.reserve(marks.size());
    for (const auto &m : marks)
        times.append(m.time);
    m_progressSlider->setMarkTimes(times);
}

void MainWindow::updateChapterMenuState() {
    const QString path = m_controller->propertyString(QStringLiteral("path"));
    const bool hasFile = !path.isEmpty() && m_playlistPos >= 0 && m_duration > 0.0;
    if (auto *a = m_actions->action(ActionManager::Id::AddBookmark))
        a->setEnabled(hasFile);
}

void MainWindow::updateChapterMenuChecked() {
    if (!m_chapterMenu || !m_chapterGroup)
        return;
    const auto hit = m_marks->hit(m_timePos); // std::optional<Mark>
    const bool hasHit = hit.has_value();
    const bool wasExclusive = m_chapterGroup->isExclusive();
    m_chapterGroup->setExclusive(false);
    const auto actions = m_chapterMenu->actions();
    for (QAction *act : actions) {
        if (!act->isCheckable())
            continue;
        double t = act->data().toDouble();
        bool shouldCheck = hasHit && hit->time == t &&
                           ((hit->isBookmark && act->property("isBookmark").toBool()) ||
                            (!hit->isBookmark && !act->property("isBookmark").toBool()));
        if (shouldCheck) {
            if (act->property("title").toString() != hit->title)
                shouldCheck = false;
        }
        act->setChecked(shouldCheck);
    }
    m_chapterGroup->setExclusive(wasExclusive);
}

void MainWindow::rebuildChapterMenu() {
    if (!m_chapterMenu)
        return;
    m_chapterMenu->clear();
    // 菜单项由 clear() 连同销毁，QAction 析构时自动脱离 m_chapterGroup，
    // 此处重建 action 后重新入组即可
    const auto marksChapters = m_marks->chapters();
    const QString curPath =
        Util::normalizePathForMatch(m_controller->propertyString(QStringLiteral("path")));
    // 跳转需原始显示路径（大小写/分隔符原样）；curPath 仅作书签仓键匹配用（B15）
    const QString curDisplay =
        Util::resolveLocalPath(m_controller->propertyString(QStringLiteral("path")));
    // 书签经聚合器统一读入口（书签永不查重，即原始）
    const auto bms = m_marks->bookmarks(curPath);

    // 跳转目标 = 菜单构建时的当前文件（菜单条目语义上属于该文件）
    auto addSection = [this, curPath, curDisplay](const QList<MarkAggregator::Mark> &items) {
        for (const auto &it : items) {
            const QString text =
                QStringLiteral("[%1] %2").arg(Util::formatTimeHMSms(it.time), it.title);
            QAction *a = m_chapterMenu->addAction(text);
            a->setCheckable(true);
            a->setData(it.time);
            a->setProperty("isBookmark", it.isBookmark);
            a->setProperty("title", it.title);
            m_chapterGroup->addAction(a);
            const double t = it.time;
            connect(a, &QAction::triggered, this,
                    [this, curDisplay, t] { jumpToFileTime(curDisplay, t); });
        }
    };

    // 准备章节/书签的 Mark 列表用于菜单分段显示
    QList<MarkAggregator::Mark> chapterMarks;
    for (const auto &c : marksChapters)
        chapterMarks.append({c.time, c.title, false});
    QList<MarkAggregator::Mark> bookmarkMarks;
    for (const auto &b : bms)
        bookmarkMarks.append({b.time, b.title, true});

    // 去重：章节内部严格 == 已在聚合器 chapters 处理；此处仅分段显示，保持原序
    // 若书签与章节同 time，菜单两段都会显示各自项，但选中跟随以书签优先（updateChapterMenuChecked）
    // 标记层已合并，菜单允许重复显示以区分来源，符合“平铺显示所有章节/书签”
    bool hasChapter = !chapterMarks.isEmpty();
    bool hasBookmark = !bookmarkMarks.isEmpty();

    if (hasChapter) {
        addSection(chapterMarks);
    }
    if (hasChapter && hasBookmark)
        m_chapterMenu->addSeparator();
    if (hasBookmark) {
        addSection(bookmarkMarks);
    }
    if (hasChapter || hasBookmark)
        m_chapterMenu->addSeparator();

    QAction *addAct = m_actions->action(ActionManager::Id::AddBookmark);
    QAction *editorAct = m_actions->action(ActionManager::Id::BookmarkEditor);
    if (addAct)
        m_chapterMenu->addAction(addAct);
    if (editorAct)
        m_chapterMenu->addAction(editorAct);

    updateChapterMenuState();
    updateChapterMenuChecked();
}

void MainWindow::jumpToFileTime(const QString &path, double time) {
    if (path.isEmpty())
        return;
    const QString norm = Util::normalizePathForMatch(path);
    // 查当前激活列表。两侧均为 normalizePathForMatch 产物（N15.1 起 Windows 分支
    // 折叠大小写），字符串相等即路径相等，无需额外分支
    auto findInTab = [&](int tabIdx) -> int {
        if (tabIdx < 0 || tabIdx >= m_playlist->count())
            return -1;
        const QStringList files = m_playlist->filesOf(tabIdx);
        for (int i = 0; i < files.size(); ++i)
            if (Util::normalizePathForMatch(files[i]) == norm)
                return i;
        return -1;
    };
    const int curIdx = m_playlist->currentIndex();
    const int playingIdx = m_playlist->playingTabIndex();
    int targetTab = -1;
    int targetRow = -1;
    int r = findInTab(curIdx);
    if (r >= 0) {
        targetTab = curIdx;
        targetRow = r;
    } else {
        r = findInTab(playingIdx);
        if (r >= 0 && playingIdx != curIdx) {
            targetTab = playingIdx;
            targetRow = r;
        }
    }
    if (targetTab >= 0) {
        // 复用既有播放路径
        if (targetTab == playingIdx) {
            // 同一播放列表内跳转：仅切换条目或 seek
            const bool isAlreadyPlaying = (m_playlistPos == targetRow);
            if (isAlreadyPlaying) {
                m_controller->command(
                    {QStringLiteral("seek"), QString::number(time), QStringLiteral("absolute")});
            } else {
                // 同列表切文件：复用 playAt（不重建列表）。seek 走挂起机制：
                // playlist-pos 触发的加载是异步的，立即 seek 会作用于尚未
                // 卸载的旧文件，待新条目 duration 推送且行号匹配时执行一次
                playAt(targetRow);
                m_pendingSeekRow = targetRow;
                m_pendingSeekTime = time;
            }
        } else {
            // 跨列表跳转：复用 onPlayRequested 整表载入（与双击条目同路径）
            const QStringList files = m_playlist->filesOf(targetTab);
            m_playlist->markPlayingTab(targetTab);
            m_playlist->requestTab(targetTab); // N19：忙时排队
            onPlayRequested(files, targetRow, time);
        }
        return;
    }
    // 复用“打开文件”逻辑：清空默认列表后推入新文件并跳转
    m_playlist->setDefaultTabFiles({path});
    m_playlist->markPlayingTab(0);
    m_playlist->requestTab(0); // N19：忙时排队
    onPlayRequested({path}, 0, time);
}

void MainWindow::ensureBookmarkEditor() {
    if (m_bookmarkEditor) {
        m_bookmarkEditor->show();
        m_bookmarkEditor->raise();
        return;
    }
    // N16：传入当前播放文件归一化键，使编辑器打开即聚焦该文件（若存在书签）
    const QString curKey =
        Util::normalizePathForMatch(m_controller->propertyString(QStringLiteral("path")));
    m_bookmarkEditor = new BookmarkEditorDialog(
        m_bookmarkMgr, [this](const QString &p, double t) { jumpToFileTime(p, t); }, curKey, this);
    m_bookmarkEditor->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(m_bookmarkEditor, &QObject::destroyed, this, [this] { m_bookmarkEditor = nullptr; });
    m_bookmarkEditor->show();
}

void MainWindow::onAddBookmark() {
    // mpv 来源路径经 resolveLocalPath 加工为显示路径；BookmarkManager 内部归一为键，
    // 展示保原样（大小写/分隔符，B15）
    const QString rawPath =
        Util::resolveLocalPath(m_controller->propertyString(QStringLiteral("path")));
    if (rawPath.isEmpty() || m_playlistPos < 0)
        return;
    const QString path = Util::normalizePathForMatch(rawPath);
    const double t = m_timePos;
    QString title;
    if (m_bookmarkMgr) {
        const auto list = m_marks->bookmarks(path);
        QSet<QString> titles;
        for (const auto &b : list)
            titles.insert(b.title);
        int n = 1;
        while (true) {
            const QString cand = tr("Bookmark %1").arg(n);
            if (!titles.contains(cand)) {
                title = cand;
                break;
            }
            ++n;
        }
    } else {
        title = tr("Bookmark 1");
    }
    m_bookmarkMgr->addBookmark(rawPath, t, title);
    showOsd(tr("Bookmark added: [%1] %2").arg(Util::formatTimeHMSms(t), title));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveSettings();
    // mpv 是宿主进程：关闭 GUI 时让 mpv 一并退出
    m_controller->command({QStringLiteral("quit")});
    QMainWindow::closeEvent(event);
}

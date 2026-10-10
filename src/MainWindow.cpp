#include "pch.h"

#include "MainWindow.h"

#include "ActionManager.h"
#include "BookmarkEditorDialog.h"
#include "BookmarkManager.h"
#include "ChapterSlider.h"
#include "Constants.h"
#include "FilterListWidget.h"
#include "HttpClient.h"
#include "MpvBindingDialog.h" // N89：mpv 键位编辑（input.conf）
#include "MpvController.h"
#include "MpvKeyName.h" // N87：未认领按键的 mpv 键名翻译
#include "NavigatorPanel.h"
#include "Playlist.h"         // N57 §2.6：经 PlaylistWidget::listOf 取身份，需完整类型
#include "PlaylistRegistry.h" // N57 §2.3：装配顶点创建列表注册表（默认名与保留身份的出处）
#include "PlaylistWidget.h"
#include "ProviderCatalog.h" // N57 §2.3：后备提供者经登记表第 0 项出生，只此一处调用
#include "ServiceRegistry.h"
#include "SettingsDialog.h"
#include "ShortcutsDialog.h" // N86
#include "StreamRelay.h"
#include "StreamTransit.h" // N55 §2.7：书签存储串的转接形态分岔（判据与 §3.6 同一条）
#include "TraversalPolicy.h"
#include "Util.h"
#include "Version.h" // N84：About 三行版本身份（本文件只许 include 此口，不得 include revision.h）
#ifndef Q_OS_WIN
#include "X11Util.h" // N83：小窗置顶 EWMH 直发（仅非 Windows 编译，见 CMakeLists）
#endif

// N15：章节+书签数据与合并规则的唯一持有者（MainWindow.cpp 文件内实现，
// 头文件仅前向声明）。职责：接收原始章节 → 内部严格 == 去重 + time 升序；
// 对外提供三视图——去重章节（菜单段/标记基数）、原始章节（全量）、
// 原始书签（书签按冻结口径永不查重，BookmarkManager 即原始源，此处为
// 统一读入口）。三处消费者（滑条标记 / tooltip 命中 / 章节菜单勾选）共用
// 同一合并规则。缓存键仅剩脏标志：章节/书签/文件切换均为低频信号，全部
// 经 setChapters/invalidate 置脏，身份与 key 只在脏时惰性同步查询一次——
// 命中路径零 IPC
class MarkAggregator {
  public:
    struct Mark {
        double time = 0.0;
        QString title;
        bool isBookmark = false;
        BookmarkManager::Origin origin; // N77：书签条目逐组随条（章节条目为空 Origin）
    };

    explicit MarkAggregator(BookmarkManager *bookmarks) : m_bookmarks(bookmarks) {}

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
    // N77 §2.3：书签读入口统一收 (uuid, key)，内部按豁免分派——本地内容按 key 合并
    // 同键全部组（逐组条目带各自来源，按组序拼接），远端严格按 (uuid, key) 单组读。
    // 返回 Mark 形态（isBookmark 恒真），菜单陈列 / 自动取号 / 合并标记共用同一分派
    // 结果；空 key（无正播）返回空
    QList<Mark> bookmarks(const QUuid &listUuid, const QString &key) const {
        QList<Mark> out;
        if (key.isEmpty() || !m_bookmarks)
            return out;
        auto push = [&out](const BookmarkManager::Origin &origin,
                           const QList<BookmarkManager::Bookmark> &list) {
            for (const auto &b : list)
                out.append({b.time, b.title, true, origin});
        };
        if (m_bookmarks->allGroupsLocal(key)) {
            for (const auto &ge : m_bookmarks->groupsByKey(key))
                push(ge.origin, ge.bookmarks);
        } else {
            push(m_bookmarks->bookmarkOrigin({listUuid, key}),
                 m_bookmarks->bookmarks({listUuid, key}));
        }
        // 合并视图按组序拼接后在消费侧补一次时间序：groupsByKey 的组序是记账口径，
        // 单组本按 time 有序，跨组拼接不再保证全局有序（改前单组恒有序，菜单直接
        // 消费本查询，乱序即回归）；稳定序保持同刻条目的组内相对次序
        std::stable_sort(out.begin(), out.end(),
                         [](const Mark &a, const Mark &b) { return a.time < b.time; });
        return out;
    }
    // 书签/装载完成等任意内容变更后调用
    void invalidate() { m_dirty = true; }

    // 合并清单：当前文件的章节 ∪ 书签，time 升序；同 time（严格 ==）书签覆盖章节。
    // 返回内部缓存 const 引用（零拷贝）；引用仅在下次 marks()/invalidate 前有效，
    // 调用方须即时消费，不得跨变更持有。
    // N56 §2.7：书签查表键取自调用方给的正播实体（空指针即无正播，书签段为空），
    // 本类不再读 mpv 属性——凡"以当前内容为对象"的读点统一归正播实体。
    // N77 §2.3：签名收 (uuid, key)，书签段经豁免分派（本地合并读、远端严格读）
    const QList<Mark> &marks(const QUuid &listUuid, const QString &key) const {
        if (!m_dirty)
            return m_cache;
        QList<Mark> out;
        for (const auto &c : m_chapters)
            out.append({c.time, c.title, false, BookmarkManager::Origin()});
        if (!key.isEmpty() && m_bookmarks) {
            for (const Mark &m : bookmarks(listUuid, key))
                out.append(m);
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
    std::optional<Mark> hit(double timePos, const QUuid &listUuid, const QString &key) const {
        const auto &list = marks(listUuid, key);
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
    BookmarkManager *m_bookmarks;                    // 非拥有
    QList<MpvController::ChapterInfo> m_rawChapters; // 原始章节（setChapters 入参原样）
    QList<MpvController::ChapterInfo> m_chapters;    // 严格 == 去重 + time 升序
    mutable QList<Mark> m_cache;
    mutable bool m_dirty = true;
};

// N66 §三-4：弹层 / 模态对话框关闭观察器——app 级过滤的独立承载对象（文件内类，
// MarkAggregator 同款口径）。不用 MainWindow 自身挂 qApp 过滤：app 路径会把本类
// 对象的全部事件再送一遍 MainWindow::eventFilter，视频区 dblclick / 滚轮 / 按下等
// 分支被二次执行（实测：双击全屏连切两次、闪一下即回）。本类只做一件事：顶级弹层
// 或模态对话框关闭后补一次浮窗显隐重评；以主窗为 parent 随亡，父链保证析构期无
// 迟到回调
class PopupCloseWatch : public QObject {
  public:
    PopupCloseWatch(std::function<void()> onPopupClosed, QObject *parent)
        : QObject(parent), m_onPopupClosed(std::move(onPopupClosed)) {}

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        // Qt 无 ClosePopup 事件类型，以 Close / Hide 命中为准。观察面：顶级弹层
        // （Qt::Popup）+ 顶级对话框（QDialog——播放列表自带的列表设置 / 新建 /
        // 删除确认 / Notice / 加文件均为模态 exec，parent 在浮窗内，§二-9 的
        // "本轮全为非模态"前提对它们不成立，模态结束必须补重评）
        if (event->type() == QEvent::Hide || event->type() == QEvent::Close) {
            if (auto *w = qobject_cast<QWidget *>(watched);
                w && w->isWindow() && (w->windowType() == Qt::Popup || qobject_cast<QDialog *>(w)))
                QTimer::singleShot(0, this, m_onPopupClosed); // 完全关闭后再重评
        }
        return false;
    }

  private:
    std::function<void()> m_onPopupClosed;
};

// ---- N67：播放时视频区光标自动隐藏 ----
// 视频区光标行为机（文件内类，PopupCloseWatch 同款口径；唯一调用方是 MainWindow，
// 不另立文件）。播放中光标停在视频区 3 秒无鼠标活动即藏（Qt::BlankCursor），
// 任何活动立即恢复并重新计时；暂停 / 停止立即恢复。不区分全屏与否，但小窗模式
// 整体不布防。
// mpv 侧 cursor-autohide 不参与本行为——插件不向 mpv 透传鼠标事件，该选项实际
// 不生效（N67 §二-1），光标行为单源于本类
class CursorAutoHider : public QObject {
  public:
    explicit CursorAutoHider(QWidget *video, QObject *parent) : QObject(parent), m_video(video) {
        m_timer.setSingleShot(true);
        m_timer.setInterval(App::cursorAutoHideMs());
        connect(&m_timer, &QTimer::timeout, this, &CursorAutoHider::onTimeout);
        // WA_NativeWindow 置位即有 QWindow；A18 重建主窗平台窗口时该 QWindow 随
        // 子 HWND 平移存活，过滤器无须重装（N66 同款事实）。QPointer 兜底：失效
        // 模式退化为自动隐藏静默失灵，而非悬空比对
        m_videoWindow = m_video->windowHandle();
        Q_ASSERT(m_videoWindow);
        if (m_videoWindow)
            m_videoWindow->installEventFilter(this);
    }

    // 播放态喂入（MpvController::pauseChanged / idleActive 转发）。两属性观察均
    // 在 observe 后立即推送一次当前值，接线完成即有正确初态；即便初值未达，
    // 默认即"非播放"，漏布防只会晚藏、绝不误藏
    void setPaused(bool paused) {
        if (m_paused == paused)
            return;
        m_paused = paused;
        updateState();
    }
    void setIdle(bool idle) {
        if (m_idle == idle)
            return;
        m_idle = idle;
        updateState();
    }
    // 小窗模式喂入：小窗内视频区就是整个窗口，指针是唯一的操作凭据
    void setMini(bool mini) {
        if (m_mini == mini)
            return;
        m_mini = mini;
        updateState();
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched != m_videoWindow)
            return false; // 只服务自己的安装点
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
            onActivity();
            break;
        case QEvent::Leave:
            // 离开视频区：无条件恢复 + 停表。不走 updateState——Leave 瞬间光标
            // 坐标可能仍落在边界内，误重布防会在 3 秒后藏走已离区的光标
            showCursor();
            m_timer.stop();
            break;
        default:
            break;
        }
        return false; // 一律放行：MainWindow 为 N66 装的过滤器与 widget 层继续收
    }

  private:
    bool playing() const { return !m_paused && !m_idle; }

    bool cursorInVideo() const {
        return m_video->rect().contains(m_video->mapFromGlobal(QCursor::pos()));
    }

    // 收口：活动驱动与状态驱动同一套判定。非播放 → 恢复显示 + 停表；播放且光标
    // 在区内 → 布防（start 即从头计满时长）。隐藏只发生在到期复核通过的那一次
    void updateState() {
        showCursor();
        if (!m_mini && playing() && m_video->isVisible() && cursorInVideo())
            m_timer.start();
        else
            m_timer.stop();
    }

    void onActivity() { updateState(); }

    // 到期四条件复核（N67 §二-5）：任一不过只停表不动作，不自动续期、不轮询
    void onTimeout() {
        if (m_mini || !playing() || !m_video->isVisible() || QApplication::activePopupWidget() ||
            !cursorInVideo())
            return;
        m_video->setCursor(Qt::BlankCursor);
        m_hidden = true;
    }

    void showCursor() {
        if (!m_hidden)
            return;
        m_hidden = false;
        m_video->unsetCursor();
    }

    QTimer m_timer;
    QWidget *m_video;
    QPointer<QWindow> m_videoWindow;
    bool m_paused = true; // 初值即"暂停"：接线后由属性观察初值推送校正
    bool m_idle = true;
    bool m_mini = false; // 小窗期不布防，初值即"非小窗"（启动不进小窗）
    bool m_hidden = false;
};

MainWindow::MainWindow(MpvController *controller, const QString &configFile,
                       std::shared_ptr<StreamRelay> relay, QWidget *parent)
    : QMainWindow(parent), m_controller(controller), m_configFile(configFile),
      m_relay(std::move(relay)) {
    setWindowTitle(App::name());

    m_actions = new ActionManager(this); // A15：先于任何容器构建
    // 启动装配期收口：菜单创建前把 ini 覆盖应用到键位
    applyShortcutOverrides();

    // N56 §2.7：装配顶点——registry 由本类创建并持有，relay 的 provide<StreamRelayIf>
    // 亦在此完成（硬顺序②：必须早于 PlaylistWidget 构造期出生的第一个提供者）。
    // HttpClient / TraversalPolicy 的注册同样归本类（自 PlaylistWidget 上移）
    m_registry = std::make_shared<ServiceRegistry>();
    if (m_relay)
        m_registry->provide<StreamRelayIf>(m_relay);
    provideHttpServices();
    provideTraversalPolicy();
    // N57 §2.3：列表注册表同由装配顶点创建并持有——后备提供者在此经登记表第 0 项
    // 造好交入（"谁调提供者工厂"只此一处），注册表只收成品、不认识服务注册表
    m_listRegistry = std::make_shared<PlaylistRegistry>(createDefaultProvider(m_registry));

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
    // N66 §三-4：唤起固定走 QWindow 层——WA_NativeWindow 置位即有 QWindow，平台
    // 投递给原生 HWND 的每条 move 由 Qt 无条件转发到该 QWindow，早于 widget 层的
    // 鼠标追踪门闸，无须开追踪、无回退分支。指针落成员：安装与 eventFilter 比对
    // 同一个对象，A18 重建主窗平台窗口时该 QWindow 随子 HWND 平移存活
    m_videoWindow = m_videoWidget->windowHandle();
    Q_ASSERT(m_videoWindow);
    if (m_videoWindow)
        m_videoWindow->installEventFilter(this);
    // N67：视频区光标自动隐藏——构造即接管事件过滤；播放态由 connectController
    // 的两条转发 connect 喂入
    m_cursorHider = new CursorAutoHider(m_videoWidget, this);
    // N66 §三-4：弹层关闭补重评经 qApp 级过滤器观察——观察器为独立对象（原因见
    // PopupCloseWatch 类注释），主窗 eventFilter 只服务对象级过滤
    qApp->installEventFilter(new PopupCloseWatch([this] { reevaluateOverlays(); }, this));

    // 单击/双击区分定时器（N3）：间隔取系统双击间隔
    m_clickTimer = new QTimer(this);
    m_clickTimer->setSingleShot(true);
    connect(m_clickTimer, &QTimer::timeout, this, &MainWindow::onVideoClickTimeout);

    m_playlist = new PlaylistWidget(configFile, m_registry, m_listRegistry, m_splitter);
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

    m_marks = new MarkAggregator(m_bookmarkMgr); // N15：标记聚合器

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
    m_leftPane = left; // N66：重挂收口的挂回锚点
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
    // N38：原 "Volume" 文本标签改图标按钮——点击切换静音，图标态经 mpv 推送驱动；
    // 与 Play 菜单静音项共用同一 QAction 实例（bindButton 统一接线）
    m_muteButton = new QToolButton;
    m_muteButton->setAutoRaise(true);
    m_muteButton->setFixedSize(24, 24);
    m_muteButton->setToolTip(tr("Mute"));
    m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume)); // 初值：未静音
    m_muteAction = m_actions->action(ActionManager::Id::Mute);
    bindButton(m_muteButton, m_muteAction);
    seekLayout->addWidget(m_muteButton);
    seekLayout->addWidget(m_volumeSlider);
    // N66：进度行与控制行不再直接进左 pane 布局，收进底部条带容器
    // m_bottomBar（spacing 取左 pane 布局原值 4，视觉零变化），
    // 占位符的 stretch 不变——容器是全屏期一次 setParent 的重挂单元
    m_bottomBar = new QWidget;
    auto *bottomLayout = new QVBoxLayout(m_bottomBar);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(4);
    bottomLayout->addWidget(seekRow);

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
    auto *settingsButton = new QPushButton(QIcon(App::iconPath(App::iconSettings())), QString());
    settingsButton->setToolTip(tr("Settings"));
    settingsButton->setFixedSize(36, 36);
    // 该按钮可切换：选中底偏深，On 态换白色版素材（两态各按显示尺寸现绘，不涉缩放）
    QIcon playlistIcon(App::iconPath(App::iconPlaylist()));
    playlistIcon.addFile(App::iconPath(App::iconPlaylistOn()), QSize(), QIcon::Normal, QIcon::On);
    m_playlistToggle = new QPushButton(playlistIcon, QString());
    m_playlistToggle->setToolTip(tr("Show/Hide Playlist"));
    m_playlistToggle->setFixedSize(36, 36);
    m_playlistToggle->setCheckable(true);

    // 控制行按钮与音量行静音按钮不接受焦点（C2）：Space 归全局快捷键，
    // 避免焦点按钮被 Space 二次触发（N38：静音按钮并入本组）
    QAbstractButton *noFocusButtons[] = {m_playButton,     stopButton,     prevButton,
                                         nextButton,       m_streamButton, settingsButton,
                                         m_playlistToggle, m_muteButton};
    for (QAbstractButton *b : noFocusButtons) {
        b->setFocusPolicy(Qt::NoFocus);
        // 点击控制按钮时把焦点重置到主窗口
        connect(b, &QAbstractButton::pressed, [] {
            if (auto *fw = QApplication::focusWidget())
                fw->clearFocus();
        });
    }

    // N38：进度条 / 音量条同样不接受焦点——同理把焦点留空，方向键恒为全局音量模式，
    // 滑条不被键盘单独驱动；滑条的按下信号名与按钮不同（sliderPressed），故另起一组
    QAbstractSlider *noFocusSliders[] = {m_progressSlider, m_volumeSlider};
    for (QAbstractSlider *s : noFocusSliders) {
        s->setFocusPolicy(Qt::NoFocus);
        connect(s, &QAbstractSlider::sliderPressed, s, [] {
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
    bottomLayout->addWidget(controlRow);
    layout->addWidget(m_bottomBar); // 左 pane 布局末项（N66 容器）

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
    // N66 §三-4：滑条松手后按光标位置重评浮窗显隐（挂起条件解除；窗口态直通空操作）
    connect(m_progressSlider, &QSlider::sliderReleased, this, &MainWindow::reevaluateOverlays);
    connect(m_volumeSlider, &QSlider::sliderReleased, this, &MainWindow::reevaluateOverlays);
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

    // N85：菜单栏按下清焦的过滤挂载——QMenuBar 自行消费按下事件，主窗
    // mousePressEvent 的冒泡兜底（N5）够不到它，点击菜单栏焦点会原地留在
    // 播放列表，方向键回不到全局音量模式。分支语义见 eventFilter
    menuBar()->installEventFilter(this);

    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(act(AM::Id::Open));
    connect(act(AM::Id::Open), &QAction::triggered, this, &MainWindow::openFiles);
    fileMenu->addAction(act(AM::Id::Settings));
    connect(act(AM::Id::Settings), &QAction::triggered, this, &MainWindow::ensureSettingsDialog);
    fileMenu->addAction(act(AM::Id::Shortcuts));
    connect(act(AM::Id::Shortcuts), &QAction::triggered, this, &MainWindow::ensureShortcutsDialog);
    // N89：mpv 键位编辑器与 N86 并列相邻但完全独立——一个管 mpv 侧（input.conf），
    // 一个管插件侧（[shortcuts] 节）。不经 ActionManager 出生登记：本项无快捷键、
    // 无 [shortcuts] 持久化语义，收进键位表反而会被 N86 编辑器当成可改绑条目
    QAction *mpvBindingsAct = fileMenu->addAction(tr("&Key Bindings…"));
    connect(mpvBindingsAct, &QAction::triggered, this, &MainWindow::ensureMpvBindingDialog);
    // N78：截图组——与 Settings 间加分隔符独立成组，其后原有「分隔符 + Quit」不动
    fileMenu->addSeparator();
    fileMenu->addAction(act(AM::Id::ScreenshotCopy));
    connect(act(AM::Id::ScreenshotCopy), &QAction::triggered, this,
            &MainWindow::copyScreenshotToClipboard);
    fileMenu->addAction(act(AM::Id::ScreenshotSave));
    connect(act(AM::Id::ScreenshotSave), &QAction::triggered, this,
            &MainWindow::saveScreenshotToFile);
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
    // N38：静音——命令取 mpv 实时属性读改写（勾选态只作显示，不参与判定）；
    // OSD 与 B14 音量同构：用户动作路径直显，不入 #1/#2/#3 合并队列
    m_muteAction = act(AM::Id::Mute);
    playMenu->addAction(m_muteAction);
    connect(m_muteAction, &QAction::triggered, this, [this] {
        const bool target = !m_controller->propertyFlag(QStringLiteral("mute"), false);
        m_controller->setProperty(QStringLiteral("mute"), target);
        showOsd(target ? tr("Mute: On") : tr("Mute: Off"));
    });
    // N6：速度控制组
    playMenu->addSeparator();
    playMenu->addAction(act(AM::Id::SpeedUp));
    connect(act(AM::Id::SpeedUp), &QAction::triggered, this, [this] { changeSpeed(+0.25); });
    playMenu->addAction(act(AM::Id::SpeedDown));
    connect(act(AM::Id::SpeedDown), &QAction::triggered, this, [this] { changeSpeed(-0.25); });
    playMenu->addAction(act(AM::Id::SpeedReset));
    connect(act(AM::Id::SpeedReset), &QAction::triggered, this, &MainWindow::resetSpeed);
    // N43：视频/音频滤镜子菜单（视频在前，与流选择组同序），位于速度组与流选择组
    // 之间。重建挂父菜单的 aboutToShow——子菜单被禁用时不可能被展开、自身
    // aboutToShow 永不触发，靠它自己翻转不了"从无滤镜到有滤镜"的启用态
    playMenu->addSeparator();
    m_videoFilterMenu = playMenu->addMenu(tr("Video Filters"));
    m_audioFilterMenu = playMenu->addMenu(tr("Audio Filters"));
    m_videoFilterMenu->setEnabled(false);
    m_audioFilterMenu->setEnabled(false);
    connect(playMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildFilterMenus);
    // N13：视频/音频/字幕流子菜单（视频在前），无流时整菜单禁用
    // N94：追加第二字幕流，位于字幕流之下
    playMenu->addSeparator();
    m_videoStreamMenu = playMenu->addMenu(tr("Video"));
    m_audioStreamMenu = playMenu->addMenu(tr("Audio"));
    m_subStreamMenu = playMenu->addMenu(tr("Subtitle"));
    m_secondarySubStreamMenu = playMenu->addMenu(tr("Secondary Subtitle"));
    m_videoStreamMenu->setEnabled(false);
    m_audioStreamMenu->setEnabled(false);
    m_subStreamMenu->setEnabled(false);
    m_secondarySubStreamMenu->setEnabled(false);
    connect(m_videoStreamMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTrackMenus);
    connect(m_audioStreamMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTrackMenus);
    connect(m_subStreamMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTrackMenus);
    connect(m_secondarySubStreamMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTrackMenus);

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
    // 小窗（N68）：I/视图菜单/Esc/关闭钮多路共用 toggleMini 收口；全屏与小窗互斥
    m_miniAction = act(AM::Id::MiniWindow);
    viewMenu->addAction(m_miniAction);
    connect(m_miniAction, &QAction::toggled, this, [this](bool on) {
        if (on != m_mini)
            toggleMini();
    });
    // 状态信息叠加层：转发 script-message 给 mpv 侧 stats 脚本，插件不持有开关状态
    viewMenu->addAction(act(AM::Id::ToggleStats));
    connect(act(AM::Id::ToggleStats), &QAction::triggered, this, [this] {
        m_controller->command({QStringLiteral("script-message-to"), QStringLiteral("stats"),
                               QStringLiteral("display-stats-toggle")});
    });

    // Esc 隐绑定经管理器取用，守卫逻辑留主窗口：全屏或小窗态退出（N68），
    // 弹出菜单打开时 Esc 由 popup 自行消费
    connect(m_actions->shortcut(AM::Id::Escape), &QShortcut::activated, this, [this] {
        if (m_fullscreen)
            toggleFullscreen();
        else if (m_mini)
            toggleMini();
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
    // N38：静音推送仅作状态联动（勾选 + 图标），OSD 同 B14 由用户动作路径直显
    connect(m_controller, &MpvController::muteChanged, this, &MainWindow::applyMuteState);
    // R3：音量上限（mpv volume-max）推送，同步滑条量程
    connect(m_controller, &MpvController::volumeMaxChanged, this,
            [this](double v) { applyVolumeMax(qRound(v)); });
    connect(m_controller, &MpvController::playlistChanged, m_playlist,
            &PlaylistWidget::setPlaylist);
    // N60 §2.2：屏障 reply 的终点对账**直投 playlist**（与上一行同一接收者）——
    // 吞域与对账的保序前提系于"两者投进同一 GUI 队列、按投递序处理"，不得绕本窗转调
    connect(m_controller, &MpvController::pushBarrierResult, m_playlist,
            &PlaylistWidget::onPushBarrier);
    // 播放态列表长度变化影响步进启停（A16）
    connect(m_controller, &MpvController::playlistChanged, this,
            [this](const QVariantList &) { updateNavigationActions(); });
    // N25：回刷差异收编后镜像重新与 mpv 序列一致，但 playlist-pos 值不变不再推送，
    // pos 推送时刻按旧镜像取的错名会挂到下一次切条目——仅错名时主动刷新标题/OSD
    // （追加等不影响正播名的收编不弹 OSD）
    connect(m_playlist, &PlaylistWidget::playingListAdopted, this, [this]() {
        const QString f = playingDisplayName();
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
            // N45 §2.9：切曲归位——手动上一 / 下一与自动续播一律把浏览路径归位到
            // 队列根层并滚动到正播项（仅实际换条目时；两次切曲之间可自由浏览）
            if (switchedItem && pt >= 0)
                m_playlist->revealPlayingItem();
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
            // N8 Part1：无正在播放内容，标题恢复基名（N91：退出区间内不夺走"正在关闭"标题）
            if (!m_closing)
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

    // N67：播放态喂给光标自动隐藏（另起 connect，不并入既有 lambda——两机制互不纠缠）
    connect(m_controller, &MpvController::pauseChanged, m_cursorHider, &CursorAutoHider::setPaused);
    connect(m_controller, &MpvController::idleActive, m_cursorHider, &CursorAutoHider::setIdle);

    connect(m_playlist, &PlaylistWidget::playRequested, this, &MainWindow::onPlayRequested);
    // N54：书签层级定位的两种结局——播放决策三档与失败策略都在本类（widget 只切层
    // 与定位，不知道 mpv 现状，也不报错）
    connect(m_playlist, &PlaylistWidget::levelItemReady, this, &MainWindow::onLevelItemReady);
    connect(m_playlist, &PlaylistWidget::levelItemFailed, this, &MainWindow::onLevelItemFailed);
    connect(m_playlist, &PlaylistWidget::filesAdded, this, &MainWindow::onFilesAdded);
    connect(m_playlist, &PlaylistWidget::removeRequested, this, &MainWindow::onRemoveRequested);
    connect(m_playlist, &PlaylistWidget::clearRequested, this, &MainWindow::onClearRequested);
    // N4：四步式第一步——排序/清理播放中 tab 前下发停止
    connect(m_playlist, &PlaylistWidget::stopRequested, this,
            [this]() { m_controller->command({QStringLiteral("stop")}); });
    // N4：元数据回填路由——按正播实体定位全局仓条目（推送数据覆写）。
    // N56 §2.7：键取正播实体的 key()（与条目存储键同形，防止写入幻影条目）；
    // playlist-pos 推送先于文件装载完成，duration 到达时镜像必已就位
    // B18：0 秒（FILE_LOADED 读不到）同其余 4 项一样走"读取失败即清空"，
    // 故不在此拦截；卸载路径不发 0.0（PropDuration 跳过 NONE），档案不受影响
    connect(m_controller, &MpvController::durationChanged, m_playlist,
            [this](const QString &, double sec) {
                if (const auto e = playingEntity())
                    m_playlist->applyDuration(e->key(), sec);
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

    // N10/B16/N40：起播拿到分辨率即适配窗口。尺寸信号经控制器去重收口（事件驱动 +
    // 与上次已发布值比对），只在真实变化时到达、且每次都是一致对——原"单发定时器
    // 合并两属性突发"的适配缓冲已随之删除，此处直连触发
    connect(m_controller, &MpvController::videoSizeChanged, this, [this](int w, int h) {
        m_pendingVideoW = w;
        m_pendingVideoH = h;
        if (m_mini) {
            // N68：小窗期换片就地适配（与全屏"退出再适配"不同——小窗本身就该跟着
            // 视频走，无须 refit 标志）。停止进 idle 与换片至无流的 (0,0) 推送不动作：
            // 维持小窗形态与当前尺寸不变（尺寸稳定性优先，进入时的 16:9 仅针对无流进场）
            if (w > 0 && h > 0)
                QTimer::singleShot(0, this, [this] { refitMiniToVideo(); });
            return;
        }
        if (m_fullscreen && w > 0 && h > 0)
            m_refitAfterFullscreen = true;
        else if (isMaximized() && w > 0 && h > 0)
            m_refitAfterMaximize = true; // N40 变更：还原最大化时重适配（跟随全屏逻辑）
        adaptWindowSizeToVideo(w, h);    // (0,0) 与全屏/最大化在入口内守卫，无动作
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
    // N87：console 开关 → 键盘让位（控制器观察 user-data/mpv/console/open）
    connect(m_controller, &MpvController::consoleOpenChanged, this,
            &MainWindow::onConsoleOpenChanged);
    // N88：fullscreen 属性镜像分派——外部写入（终端 / IPC / 默认键 f）与注册初值
    // （mpv.conf 写 fullscreen=yes 即启动全屏，期望行为）同走此路。与 Qt 侧真相
    // 比对：相等即不动（Qt 自身写入的回灌被守卫吸收，不新增标志）；不符时小窗态
    // 只按真相回正（N68 互斥即忽略，回写对象从 QAction 扩到 mpv 属性，不切换），
    // 普通态驱动 toggleFullscreen() 切换。镜像值按窗口真实状态定义（铁律）：
    // 全屏 true、普通与小窗 false——小窗态绝不映射成 true
    connect(m_controller, &MpvController::fullscreenChanged, this, [this](bool fs) {
        if (fs == m_fullscreen)
            return;
        if (m_mini)
            m_controller->setProperty(QStringLiteral("fullscreen"), m_fullscreen);
        else
            toggleFullscreen();
    });
}

// N56：注册表维护（自 PlaylistWidget 上移装配顶点）。读 ini 两项构建 HttpClient
// 缺省参数并 provide。初装配（构造期）与重装配（设置变更）共用：覆盖式更新，
// 注册表身份恒定——实体持有的 registry 指针不变，下一次求值 resolve 即得新 client；
// 在途单请求不受影响（旧 client 副本随任务存活），条目粒度切换，可接受（N35 §2.3）
void MainWindow::provideHttpServices() {
    int timeoutSec = App::remoteTimeoutSecDefault();
    bool ignoreSsl = App::ignoreHttpsErrorsDefault();
    if (!m_configFile.isEmpty()) {
        QSettings s(m_configFile, QSettings::IniFormat);
        bool ok = false;
        const int v = s.value(QStringLiteral("remoteTimeoutSec"), timeoutSec).toInt(&ok);
        if (ok && v >= 1 && v <= 120) // 与设置目录 range 同界，读侧自钳
            timeoutSec = v;
        ignoreSsl = s.value(QStringLiteral("ignoreHttpsErrors"), ignoreSsl).toBool();
    }
    HttpClient::Config cfg;
    cfg.timeoutMs = timeoutSec * 1000; // ini 记秒，Config 记毫秒（N34）
    cfg.ignoreSslErrors = ignoreSsl;
    m_registry->provide<HttpClient>(std::make_shared<HttpClient>(cfg));
}

// N36：遍历策略同路——读 ini 构建 TraversalPolicy 快照 provide 进同一注册表；
// 取值侧在 DirProvider::enumerate 起始处 resolve
void MainWindow::provideTraversalPolicy() {
    int depthLimit = App::recursiveDepthLimitDefault();
    if (!m_configFile.isEmpty()) {
        QSettings s(m_configFile, QSettings::IniFormat);
        bool ok = false;
        const int v = s.value(QStringLiteral("recursiveDepthLimit"), depthLimit).toInt(&ok);
        // 读侧自钳：上界与设置目录 range 同源（见 Constants.h）；下界恒 0
        if (ok && v >= 0 && v <= App::recursiveDepthLimitMax())
            depthLimit = v;
    }
    auto policy = std::make_shared<TraversalPolicy>();
    policy->depthLimit = depthLimit;
    m_registry->provide<TraversalPolicy>(std::move(policy));
}

// N56：设置对话框 ini 键变更入口（自 PlaylistWidget 上移）——命中相关键时重读
// ini 覆盖注册表内部实例，下一次求值即生效
void MainWindow::onPluginIniKeysChanged(const QStringList &keys) {
    if (keys.contains(QStringLiteral("remoteTimeoutSec")) ||
        keys.contains(QStringLiteral("ignoreHttpsErrors")))
        provideHttpServices();
    if (keys.contains(QStringLiteral("recursiveDepthLimit")))
        provideTraversalPolicy();
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
        f = Util::Url::toNative(f);
    // N11：主菜单打开文件固定落点默认播放列表——清旧内容与续播记忆后重灌，
    // 随即整表开播（他 tab 播放中被抢占切换）；多选按对话框原始返回顺序从首项播起。
    // 有意不走 addFiles：绕开落点漂移与播放中 append 语义，拖放/右键添加不受影响。
    // N56 §2.7：setDefaultTabFiles 回传实体表，播放入口改传实体表
    const PlaylistEntity::List entities = m_playlist->setDefaultTabFiles(files);
    m_playlist->markPlayingTab(0);
    m_playlist->requestTab(0); // Q2 裁决：抢占切换时激活态同步跳到默认 tab（N19 忙时排队）
    onPlayRequested(entities, 0, 0, -1.0);
}

QImage MainWindow::grabScreenshotOrNotify() {
    // N78：取图失败（空闲态无视频 / 命令报错 / 载荷异常）统一在此出 OSD，
    // 两个入口槽只看空图短路
    const QImage img = m_controller->screenshotRaw();
    if (img.isNull())
        showOsd(tr("No screenshot available"));
    return img;
}

void MainWindow::copyScreenshotToClipboard() {
    const QImage img = grabScreenshotOrNotify();
    if (img.isNull())
        return;
    QApplication::clipboard()->setImage(img); // 与 FilterListWidget / PlaylistWidget 同源
    showOsd(tr("Screenshot copied to clipboard"));
}

void MainWindow::saveScreenshotToFile() {
    // 先取图：失败即 OSD 报错，不弹对话框
    const QImage img = grabScreenshotOrNotify();
    if (img.isNull())
        return;

    // 预填名：<在播文件名去后缀>_<yyyyMMdd_HHmmss_zzz>。在播路径走 N61 唯一出口
    // currentPlayingPath()，末段经 Util::Url::fileName（http(s) 按既有语义解码），
    // completeBaseName 剥一个后缀；末段为空回退 screenshot
    QString base =
        QFileInfo(Util::Url::fileName(m_controller->currentPlayingPath())).completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("screenshot");
    const QString prefill =
        base + u'_' + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));

    // 固定三档过滤器；首选后缀形如 "*.png" → 取首个。静态 Qt 构建无 webp 编码、
    // qgif 仅解码不可写，故只列该构建可写的三档（png/bmp 内建、jpg 已实测可写）
    const QString filters = QStringLiteral("PNG (*.png);;JPEG (*.jpg *.jpeg);;BMP (*.bmp)");
    // 解析假设依赖本文件内硬编码的过滤器写法：indexOf('(') 取描述里首个括号、
    // 首选后缀恒为 "*.xxx" 两字符前缀（下面的 mid(2) 与之配套）——若改成动态
    // 生成过滤器串，此二处须一并重审
    const auto preferredSuffix = [](const QString &filter) -> QString {
        const int open = filter.indexOf(QLatin1Char('('));
        const int close = filter.indexOf(QLatin1Char(')'), open);
        if (open < 0 || close < 0)
            return QString();
        const QStringList exts =
            filter.mid(open + 1, close - open - 1).split(u' ', Qt::SkipEmptyParts);
        return exts.value(0);
    };
    // 后缀与格式名的映射收口：jpg 与 QImageWriter 的 jpeg 格式名在此对齐
    const auto suffixToFormat = [](const QString &suffix) -> QByteArray {
        if (suffix == QLatin1String("png"))
            return QByteArrayLiteral("png");
        if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg"))
            return QByteArrayLiteral("jpeg");
        if (suffix == QLatin1String("bmp"))
            return QByteArrayLiteral("bmp");
        return QByteArray();
    };

    // 栈上实例（非静态便捷函数）：要连 filterSelected 与读 selectedNameFilter。
    // 起始目录不设置、用系统默认；预填名只含文件名。后缀补齐双保险——Qt 自绘
    // 对话框不自动补后缀，经 defaultSuffix 补；Windows 原生由系统按选中过滤器补
    // （同"无后缀才补"语义，两路不冲突不双补）
    QFileDialog dlg(this, tr("Save Screenshot"), prefill, filters);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    const auto applyDefaultSuffix = [&dlg, &preferredSuffix](const QString &filter) {
        const QString suffix = preferredSuffix(filter).mid(2); // 去 "*."
        if (!suffix.isEmpty())
            dlg.setDefaultSuffix(suffix);
    };
    applyDefaultSuffix(dlg.selectedNameFilter());
    connect(&dlg, &QFileDialog::filterSelected, &dlg, applyDefaultSuffix);
    if (dlg.exec() != QDialog::Accepted)
        return; // 取消静默返回（无 OSD）
    QString path = dlg.selectedFiles().value(0);
    if (path.isEmpty())
        return;

    // 后缀即格式：以最终后缀定编码器；不受支持 → 按所选过滤器首选后缀补齐，
    // 兜住手删后缀 / 手输怪后缀等全部路径
    QByteArray format = suffixToFormat(QFileInfo(path).suffix().toLower());
    if (format.isEmpty()) {
        const QString preferred = preferredSuffix(dlg.selectedNameFilter()).mid(2);
        if (preferred.isEmpty())
            return; // 理论不可达：过滤器三档的首选后缀均在映射表内
        path += QLatin1Char('.') + preferred;
        format = suffixToFormat(preferred);
    }

    QImageWriter writer(path, format);
    if (!writer.write(img)) {
        qDebug() << "[oxbow] screenshot write failed:" << writer.errorString() << path;
        showOsd(tr("Failed to save screenshot"));
        return;
    }
    showOsd(tr("Screenshot saved"));
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
        // N45 §2.9：先锚定队列根 = 当前浏览层（双击层即队列层），再取队列内容与
        // 续播定位——mpv 侧语义一律实体序
        const int cur = m_playlist->currentIndex();
        m_playlist->markPlayingTab(cur);
        const PlaylistEntity::List entities = m_playlist->queueEntitiesOf(cur);
        if (entities.empty())
            return;
        const double restoreTime = m_playlist->lastTimePosOf(cur);
        int idx = m_playlist->resumeEntityIndexOf(cur);
        if (idx < 0) {
            idx = m_playlist->currentSelectedEntityIndex();
            if (idx < 0 || idx >= int(entities.size()))
                idx = 0;
        }
        // 载入即转移同步目标：mpv 列表回刷写进当前 tab，关闭时 lastListUuid 随之正确；
        // 续播进度经统一入口的挂起 seek 机制恢复
        onPlayRequested(entities, cur, idx, restoreTime > 0.0 ? restoreTime : -1.0);
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
// 起点：有记忆取记忆项，否则第 0 项；列表循环开启时首尾互绕，非循环越界无动作。
// N45 §2.9：先锚定队列根 = 当前浏览层，再按队列根层实体序步进
void MainWindow::stepStopped(bool next) {
    const int cur = m_playlist->currentIndex();
    m_playlist->markPlayingTab(cur);
    const PlaylistEntity::List entities = m_playlist->queueEntitiesOf(cur);
    if (entities.empty())
        return;
    int base = m_playlist->resumeEntityIndexOf(cur);
    if (base < 0 || base >= int(entities.size()))
        base = 0;
    int target = base + (next ? 1 : -1);
    if (currentLoopMode() == QLatin1String("playlist")) {
        if (target < 0)
            target = int(entities.size()) - 1; // 首项上一个 → 绕尾项
        else if (target >= int(entities.size()))
            target = 0; // 尾项下一个 → 绕首项
    } else if (target < 0 || target >= int(entities.size())) {
        return;
    }
    onPlayRequested(entities, cur, target, -1.0);
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
        const int count = pt >= 0 ? int(m_playlist->queueEntitiesOf(pt).size()) : m_playlistPos + 1;
        prevAction->setEnabled(loop || m_playlistPos > 0);
        nextAction->setEnabled(loop || m_playlistPos < count - 1);
        return;
    }

    // 停止态：以当前选中 tab 为准；空列表无视 LOOP 双禁用。
    // N45：基准按队列根层实体序（resumeEntityIndexOf）
    const int cur = m_playlist->currentIndex();
    const PlaylistEntity::List entities = m_playlist->queueEntitiesOf(cur);
    if (entities.empty()) {
        prevAction->setEnabled(false);
        nextAction->setEnabled(false);
        return;
    }
    int base = m_playlist->resumeEntityIndexOf(cur);
    if (base < 0 || base >= int(entities.size()))
        base = 0; // 与 stepStopped 起点归一规则一致
    prevAction->setEnabled(loop || base > 0);
    nextAction->setEnabled(loop || base < int(entities.size()) - 1);
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
    if (m_mini) {
        // N68：小窗期显隐切换整体忽略——面板是隐藏态，若照走 A20 联动会把小窗向右
        // 撑宽（applyPlaylistGeometry 的守卫只挡全屏/最大化），比例与几何单源全破。
        // 真相成员不动，退出仍按进窗前的可见性恢复（§8）；勾选态按真相回写，
        // 免得 Ctrl+L 把动作勾成与实况分叉（同互斥早退的纠正口径）
        QSignalBlocker toggleBlocker(m_playlistToggle);
        m_playlistToggle->setChecked(m_playlistVisibleTruth);
        QSignalBlocker actionBlocker(m_playlistAction);
        m_playlistAction->setChecked(m_playlistVisibleTruth);
        return;
    }
    if (m_fullscreen) {
        // N66 §二-7：全屏期 ☰ / Ctrl+L 改义——不触碰 splitter、不改窗口态面板
        // 可见性、不写 playlistExpanded，只翻右缘自动显示允许位。按钮→动作→本函数
        // 三条入口在此单点汇聚分流
        setFullscreenPlaylistEdgeEnabled(visible);
        return;
    }
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
    m_playlistVisibleTruth = visible; // N66：窗口态可见性真相成员（唯一写点，saveSettings 读此）
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
    // N68：小窗期不显示 OSD（客户区过小，叠加层可读性差）；被抑制的消息不排队、
    // 不补显——暂停提示 / 书签确认等均有时效，退出小窗后补显已失意义，故
    // 退出路径不涉及 OSD 恢复
    if (m_mini)
        return;
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
    repositionFullscreenOverlays(); // N66：全屏期两条带贴边跟随
#if !defined(Q_OS_WIN) || defined(OXBOW_PUREQT)
    if (m_mini)
        scheduleMiniRefit(/*positionAroused=*/true); // 降级路径：位置变化算外部唤醒
#endif
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    repositionNavigator();
    repositionVideoOverlay();
    repositionFullscreenOverlays(); // N66：全屏期两条带贴边跟随
#if !defined(Q_OS_WIN) || defined(OXBOW_PUREQT)
    if (m_mini)
        scheduleMiniRefit(); // 降级路径：尺寸变化按回声判别定夺是否重新武装
#endif
    // N3：showFullScreen/showNormal 改变的是主窗尺寸，主窗 resize 是
    // 全屏/还原后视频区几何同步的可靠最终触发点（幂等）
    syncVideoGeometry();
}

// N40 变更：还原最大化时按当前视频重新适配（跟随全屏退出重适配逻辑）。
// 最大化期间的尺寸推送被 adaptWindowSizeToVideo 的 isMaximized 守卫拦下、
// 只更新 pending 与 m_refitAfterMaximize；此处捕获"最大化 → 普通态"的还原
// 瞬间补触发。singleShot(0) 与全屏退出路径同款：等还原几何落定后按最近
// 一次发布的尺寸（pending）适配
void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    // m_fullscreen / m_mini 守卫（N68）：从最大化进入覆盖式全屏或小窗都会先
    // setWindowState(NoState)，该转移同样表现为"最大化 → 普通态"，须排除；
    // 退出全屏/小窗时标志已先置假，但旧态是 NoState，不会误入本分支
    if (event->type() != QEvent::WindowStateChange || m_fullscreen || m_mini)
        return;
    auto *stateEvent = static_cast<QWindowStateChangeEvent *>(event);
    const bool wasMaximized = stateEvent->oldState() & Qt::WindowMaximized;
    const bool nowNormal =
        !(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized));
    if (wasMaximized && nowNormal && m_refitAfterMaximize) {
        m_refitAfterMaximize = false;
        QTimer::singleShot(0, this,
                           [this] { adaptWindowSizeToVideo(m_pendingVideoW, m_pendingVideoH); });
    }
}

// A20：启动几何收口的触发点——首次 show 时 frameGeometry() 与 screen() 才可靠
// （构造期二者均不可信，N1⑥ 同类）。最小化恢复、A18 全屏退出的 show() 都会
// 再触发本事件，必须一次性守卫：重复执行会把窗口越推越大、把位置反复拉回居中
void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    // N90：让位态下补装窗口级过滤器——平台窗口经 setWindowFlags 重建后，旧句柄上的
    // 过滤器已随之没掉而让位态不变；本函数在 flags 切换必经的 hide→show 里触发，新句柄已就位
    if (m_inputYielded)
        attachYieldFilter();
    if (m_firstShowDone)
        return;
    m_firstShowDone = true;
    applyStartupGeometry();
}

// 视频区几何手动同步（N3）：按窗口模式分支计算目标几何。
// 全屏与小窗（N68）铺满整个客户区（小窗客户区即视频，无边距）；普通态对齐
// 占位符——占位符 geometry 相对其父容器，须 mapTo 主窗坐标系
void MainWindow::syncVideoGeometry() {
    if (!m_videoWidget || !m_videoPlaceholder)
        return;
    if (m_fullscreen || m_mini) {
        m_videoWidget->setGeometry(rect());
    } else {
        const QPoint origin = m_videoPlaceholder->mapTo(this, QPoint(0, 0));
        m_videoWidget->setGeometry(QRect(origin, m_videoPlaceholder->size()));
    }
    repositionVideoOverlay();
    repositionFullscreenOverlays(); // N66：两条带几何与视频区对位同源（§三-3 挂载点并列）
}

// ---- N66：全屏浮窗控制栏与播放列表 ----

// §三-3：全屏态 ☰ 语义的唯一收口——翻转右缘自动显示允许位 + 重评显隐 + 按 A12
// 口径同步 ☰ 与动作勾选态。允许位与窗口态可见性互不污染：本函数不碰 splitter
// 与 setPlaylistVisible
void MainWindow::setFullscreenPlaylistEdgeEnabled(bool enabled) {
    if (m_fsPlaylistEdgeEnabled == enabled)
        return;
    m_fsPlaylistEdgeEnabled = enabled;
    // §二-7：关闭且正在显示 → 立即收起；开启 → 不自动显示，等边缘触发
    if (!enabled)
        setOverlayShown(Overlay::Playlist, false);
    {
        QSignalBlocker toggleBlocker(m_playlistToggle);
        m_playlistToggle->setChecked(enabled);
    }
    {
        QSignalBlocker actionBlocker(m_playlistAction);
        m_playlistAction->setChecked(enabled);
    }
    reevaluateOverlays(); // 关闭且未显示 / 开启两档：按光标位置与新允许位定态
}

// §三-2：两个宿主浮窗按需创建（同 ensureNavigator / ensureVideoOverlay 的生命周期：
// 首次进全屏才构造，成员指针为 null 即未创建）。焦点标志两窗分叉（§二-1）：控制栏带
// WindowDoesNotAcceptFocus（控件全 NoFocus 无须键盘）；播放列表不带——搜索框与列表
// 键盘导航需要激活窗口，点它成为活动窗口后快捷键经 Qt::Tool + parent 回落仍作用主窗
void MainWindow::ensureFullscreenOverlays() {
    if (m_controlOverlay)
        return;
    // bypassWm：是否绕开 WM（override-redirect，仅非 Windows 生效）。控制栏要啃住
    // 屏底、覆盖面板区域，必须绕；播放列表不绕——它是自家模态对话框的宿主，留在
    // 受管体系内才能换回 WM 的堆叠序与焦点指派（N83 §10）
    const auto makeOverlay = [this](bool acceptFocus, bool bypassWm) {
        Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint;
#ifndef Q_OS_WIN
        // N83：X11 下受 WM 管理的浮窗几何按 strut 被约束（要给停靠面板让位）——主窗
        // 带 _NET_WM_STATE_FULLSCREEN 豁免，浮窗没有，于是被顶起一条面板高、底边留空。
        // 控制栏必须覆盖面板区域，故走 override-redirect：WM 不参与定框，setGeometry
        // 直落且与具体 WM 无关，堆叠序靠 setOverlayShown 的 raise 维持。播放列表不走
        // OR：右键属性 / 列表设置等模态对话框都从它弹出，而 OR 窗恒在所有受管窗之上，
        // WM 无力把对话框提到它上面；留在受管体系内则按「transient 压在其父之上」由
        // WM 堆叠——Windows 侧本就是这一机制。附带收益：播放列表需要键盘焦点（搜索框
        // / 列表导航），受管即恢复 WM 的焦点指派。标志只在构造期给，此后不碰任何
        // flags（§8 重建律：StaysOnTop 经 Qt 通道变更实测触发平台窗口销毁重建）
        if (bypassWm)
            flags |= Qt::X11BypassWindowManagerHint;
#else
        Q_UNUSED(bypassWm);
#endif
        auto *w = new QWidget(
            this, flags | (acceptFocus ? Qt::WindowFlags() : Qt::WindowDoesNotAcceptFocus));
        w->setAttribute(Qt::WA_ShowWithoutActivating);
        auto *lay = new QVBoxLayout(w);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->setSizeConstraint(QLayout::SetNoConstraint); // B12：几何不被内容 sizeHint 反推
        w->installEventFilter(this);                      // §三-4：Leave 收起
        return w;
    };
    m_controlOverlay = makeOverlay(false, /*bypassWm=*/true);  // 啃屏底，绕开 WM
    m_playlistOverlay = makeOverlay(true, /*bypassWm=*/false); // 回受管，换回堆叠与焦点
    // §二-8：左缘拖宽热区——宿主被面板子控件全覆盖、自身收不到鼠标事件，以宿主内
    // 手动几何的透明子条承载 6px 热区（按下即隐式鼠标 grab，拖出浮窗事件仍可达）
    m_fsResizeStrip = new QWidget(m_playlistOverlay);
    m_fsResizeStrip->setCursor(Qt::SizeHorCursor);
    m_fsResizeStrip->installEventFilter(this);
}

// §三-3：重挂收口。QLayout::addWidget 代办 setParent，但 Qt 契约的重挂隐式隐藏
// 必须由调用方显式恢复——重挂后控件自身保持 visible，显隐由宿主浮窗唯一控制
// （§二-2 "两浮窗均 Hidden" 的 Hidden 落在宿主上：宿主不 show，控件就不上屏）
void MainWindow::reparentIntoOverlay(QWidget *what, QWidget *host) {
    host->layout()->addWidget(what);
    what->setVisible(true);
}

// §三-3：挂回原位——播放列表回 splitter（末位追加即索引 1，左 pane 恒在 0，
// 只恢复对象树形状，可见性由 §二-2 ④ 按窗口态真相恢复）；控制栏回左 pane 布局
// 末项（控制行窗口态常显，随挂回直接恢复）
void MainWindow::restoreOriginalParent(QWidget *what) {
    if (what == m_playlist) {
        m_splitter->addWidget(m_playlist);
        m_playlist->setVisible(false);
    } else {
        m_leftPane->layout()->addWidget(what);
        what->setVisible(true);
    }
}

// §三-3：浮窗显隐唯一出口（§三-5 末条：显隐只经此，不在别处直接 setVisible/show）。
// 播放列表浮窗隐藏前按焦点归还纪律把键盘焦点送回主窗（§二-1），杜绝焦点滞留已隐藏窗口
void MainWindow::setOverlayShown(Overlay which, bool shown) {
    QWidget *overlay = which == Overlay::ControlBar ? m_controlOverlay : m_playlistOverlay;
    if (!overlay || overlay->isVisible() == shown)
        return;
    if (!shown && which == Overlay::Playlist) {
        if (auto *fw = QApplication::focusWidget(); fw && overlay->isAncestorOf(fw)) {
            setFocus();
            activateWindow();
        }
    }
    overlay->setVisible(shown);
    if (shown)
        overlay->raise(); // §二-6：各浮窗 show 时自行 raise，互不干预对方 Z 序
}

// §三-4：按当前光标位置重评一次（滑条松手 / 拖宽松手 / 弹层关闭 / 对话框关闭共用）
void MainWindow::reevaluateOverlays() {
    if (m_fullscreen)
        evaluateOverlayVisibility(mapFromGlobal(QCursor::pos()));
}

// §二-4：边缘触发区分区判据的唯一实现，一次判定同时定两态，不可能双命中
// （底区与右区不共享角落）。判定顺序先挂起后互斥切换（§二-5：被挂起的浮窗
// 不被另一条触发区的唤起动作顶掉）
void MainWindow::evaluateOverlayVisibility(const QPoint &clientPos) {
    if (!m_fullscreen || !m_controlOverlay)
        return;
    const int W = width();
    const int H = height();
    const int t = App::fullscreenEdgeTriggerWidth();
    const bool inBottom = clientPos.y() >= H - m_bottomBarHeight && clientPos.x() < W - t;
    const bool inRight = clientPos.x() >= W - t && clientPos.y() < H - m_bottomBarHeight;
    // §二-5 挂起条件：滑条按下挂起控制栏（收起会让用户看不见正在拖的目标）；拖宽
    // 进行中整个状态机挂起——光标随缘扫过底区不应意外唤出控制栏，播放列表同理不收；
    // 活动弹层两浮窗一并挂起（菜单项落在浮窗之外，光标移向菜单会先触发 Leave）
    const bool popup = QApplication::activePopupWidget() != nullptr;
    const bool holdControl =
        m_progressSlider->isSliderDown() || m_volumeSlider->isSliderDown() || popup || m_fsResizing;
    const bool holdPlaylist = m_fsResizing || popup;
    // §二-3 "光标停在浮窗内无限期保持显示"：已显示的浮窗以自身矩形保活（Leave 收起
    // 的等价判据），触发带只负责唤出——否则拖宽松手 / 弹层关闭等重评点会因光标不在
    // 16px 触发带内而把浮窗误收起。保活兼作让位规则：光标已被某浮窗占住时，另一条
    // 带在该位置不再唤出——两浮窗矩形在底部重叠带确实相交，"不同时可见"靠这条让位
    // 维持。浮窗 geometry() 为全局坐标，光标须先转全局再比对（本地坐标混比在副屏即错）
    const QPoint globalPos = mapToGlobal(clientPos);
    const bool cursorOnControl =
        m_controlOverlay->isVisible() && m_controlOverlay->geometry().contains(globalPos);
    const bool cursorOnPlaylist =
        m_playlistOverlay->isVisible() && m_playlistOverlay->geometry().contains(globalPos);
    const bool controlTarget = (inBottom && !cursorOnPlaylist) || cursorOnControl;
    const bool playlistTarget =
        !controlTarget && ((inRight && m_fsPlaylistEdgeEnabled) || cursorOnPlaylist);
    if (!holdControl)
        setOverlayShown(Overlay::ControlBar, controlTarget);
    if (!holdPlaylist)
        setOverlayShown(Overlay::Playlist, playlistTarget);
}

// §三-3：两条带几何——控制栏贴底满宽高 h；播放列表贴右满高宽 m_fsPlaylistWidth
// （钳制在面板下限与客户区宽 50%，§二-8）。挂载点与 repositionVideoOverlay 完全并列。
// 顶级浮窗 setGeometry 取全局屏幕坐标（repositionVideoOverlay 先 mapToGlobal 同口径），
// 故按主窗客户区的全局矩形定位——本地 rect 只在全屏所在屏原点恰为 (0,0) 时数值巧合
// 相等，副屏下两条带会整体漂到主屏原点
void MainWindow::repositionFullscreenOverlays() {
    if (!m_fullscreen || !m_controlOverlay)
        return;
    const QRect client(mapToGlobal(QPoint(0, 0)), rect().size());
    const int h = m_bottomBarHeight;
    m_controlOverlay->setGeometry(QRect(client.left(), client.bottom() - h + 1, client.width(), h));
    if (m_playlistOverlay) {
        const int w = qBound(m_playlist->minimumWidth(), m_fsPlaylistWidth, client.width() / 2);
#ifndef Q_OS_WIN
        // N83：播放列表回受管体系后几何会被 WM 按 strut 约束到工作区——若仍按全屏高
        // 下发，WM 会把它压回来、进出各有一段可见回弹。直接按所在屏工作区与主窗客户区
        // 的交叠下发：所求即所得（不再满屏高，底部让给面板）
        const QRect work = screen()->availableGeometry();
        const int top = qMax(client.top(), work.top());
        const int bottom = qMin(client.bottom(), work.bottom());
        m_playlistOverlay->setGeometry(
            QRect(client.right() - w + 1, top, w, qMax(1, bottom - top + 1)));
#else
        m_playlistOverlay->setGeometry(
            QRect(client.right() - w + 1, client.top(), w, client.height()));
#endif
        if (m_fsResizeStrip) { // 热区条随宿主高度走（手动几何，不进布局）
            m_fsResizeStrip->setGeometry(0, 0, App::fullscreenResizeHotspotWidth(),
                                         m_playlistOverlay->height());
            m_fsResizeStrip->raise(); // 面板重挂后可能盖住热区条，恒置顶兜底
        }
    }
}

// N10：普通态窗口自适应当前视频——视频域尽可能物理像素 1:1（逻辑尺寸 =
// 像素 ÷ devicePixelRatio，高 DPI 下不被系统缩放拉伸），任一边超出所在屏
// 工作区即整窗等比缩到恰好放入；resize 后外框越出工作区时按最小移动原则
// 两轴独立钳回（恰好不越界）。全屏/最大化/无视频流不动作；
// 停止不复位、全屏退出维持全屏前普通尺寸（本函数仅在分辨率推送时触发）
void MainWindow::adaptWindowSizeToVideo(int videoW, int videoH) {
    // m_mini 守卫（N68）：小窗几何由 toggleMini / 换片自适应单源负责，
    // 尺寸推送不得把小窗重排成默认尺寸
    if (videoW <= 0 || videoH <= 0 || m_fullscreen || m_mini || isMaximized())
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

// N68：小窗期换片自适应（videoSizeChanged 经 singleShot(0) 触发，等几何落定后
// 就地调整）——保持当前客户区宽不变、高按新比例重算，尺寸与位置不回默认；
// §6 要求换片后整窗不越出工作区，故两路径同补钳回（越出屏外只在**用户拖动**时允许）
void MainWindow::refitMiniToVideo() {
    if (!m_mini || m_pendingVideoW <= 0 || m_pendingVideoH <= 0)
        return;
    refitMiniToAspect();
    clampMiniIntoWorkArea();
}

// N68 小窗几何收口（保宽调高）：保持当前客户区宽不变、高按视频比例（无流按 16:9）
// 重算。幂等——尺寸已合比时空转。两条路径共用、触发时机不同：主路径只经换片自适应
// （refitMiniToVideo）进来，缩放与拖动的定框由 WM_SIZING / WM_MOVING 在原生域内完成；
// 降级路径经结果驱动收口轮询（自由缩放在定态收敛回等比形态）。本函数同时记录下发的
// 客户区尺寸（m_miniRefitRequested），供收口触发器区分自家修正的回声与外部几何变化。
// 越界钳回**不在**本函数：降级松手不得把用户推到屏外的窗拽回（第 8 条同一裁决），
// 只有程序性场景（换片、进窗贴角）按需另经 clampMiniIntoWorkArea
void MainWindow::refitMiniToAspect() {
    if (!m_mini)
        return;
    const double ratio = (m_pendingVideoW > 0 && m_pendingVideoH > 0)
                             ? double(m_pendingVideoW) / double(m_pendingVideoH)
                             : 16.0 / 9.0;
    const int clientW = width();
    const int clientH = qRound(clientW / ratio);
#if !defined(Q_OS_WIN) || defined(OXBOW_PUREQT)
    // 记录仅服务降级路径收口触发器的回声判别：成员随本守卫声明，主路径既不记录
    // 也不持有，故赋值须与声明同守卫
    m_miniRefitRequested = QSize(clientW, clientH);
#endif
    resize(clientW, clientH);
}

// N68 外框越出工作区即按最小移动原则收回（越界多少移多少，未越界空转）。
// Qt 的 frameGeometry 已剥掉不可见缩放边框，与主路径的可见外框口径同（第 5 条）
void MainWindow::clampMiniIntoWorkArea() {
    if (!m_mini)
        return;
    const QRect fr = frameGeometry();
    const QPoint tl = minimalMoveTopLeft(fr, screen()->availableGeometry());
    if (tl != fr.topLeft())
        move(tl); // 与既有普通态钳回同款：move 传外框左上角，Qt 按 frameMargins 折算
}

#if !defined(Q_OS_WIN) || defined(OXBOW_PUREQT)
// N68 降级路径的定态收口触发器（纯 Qt，不借任何平台消息）：原生模态拖动/缩放循环期间
// Qt 持续收到 move/resize 事件，每条事件重置去抖定时器，静默达 miniRefitDebounceMs
// 触发一轮。
// N83 结果驱动：不判松手、只判收敛——触发时查「客户区高是否符合比例」（±1px 容差防
// WM 舍入往返），符合即停；不符合调一次（refitMiniToAspect）并续轮询。手势中的停顿
// 同样触发一轮修正（闪回一跳，N68 第 11 条既有让步），被循环覆写也无所谓：下一轮
// 发现仍不符会再调，真松手后必然收敛。重试预算：位置变化（拖动）视为外部唤醒；
// 尺寸事件则区分回声——与上次请求一致的尺寸是自家修正的回声，不清预算（否则自家
// resize 的回声把预算清零，上限形同虚设），与请求不一致的尺寸（手势覆写 / WM 钳制
// 修正）重新武装。循环覆写与 WM 拒绝在纯 Qt 下不可区分，故不做停滞判定、偏置为
// 重试；WM 钳制修正也构成外部唤醒，该场景下轮询不设上界（罕见且每轮代价极小），
// 预算的实际身份是「无外部唤醒时的空转保险丝」。
// 降级形态**不做吸附，也不做拖动/松手的越界钳回**：拖动归原生模态循环所有，循环外改
// 窗位会被下一条鼠标消息按循环自己的轨迹覆写（实测乱跳、方向不定，第 10 条）。主路径
// 能全程吸附，靠的是改循环自己的提案（WM_MOVING 的 lParam），这条通道纯 Qt 没有；
// KDE 默认对所有窗口吸附，降级无需自理
void MainWindow::scheduleMiniRefit(bool positionAroused) {
    if (!m_miniRefitTimer) {
        m_miniRefitTimer = new QTimer(this);
        m_miniRefitTimer->setSingleShot(true);
        m_miniRefitTimer->setInterval(App::miniRefitDebounceMs());
        connect(m_miniRefitTimer, &QTimer::timeout, this, [this] {
            // N83：进窗贴角待办先行消费——无论收敛与否，首条收口都要贴角（装饰尺寸
            // 此刻才可信，读 frameGeometry 不偏）。只动位置不改尺寸，不影响下方判定。
            // 这是进入小窗那一刻的一次性初始落位（程序性场景，N68 明文保留），不是
            // 拖动吸附——后者降级路径一律不做（见上方块注释）
            if (m_miniEntryGluePending) {
                m_miniEntryGluePending = false;
                clampMiniIntoWorkArea(); // 外框越出即收回，即贴右/下边
            }
            const int clientW = width();
            const double ratio = (m_pendingVideoW > 0 && m_pendingVideoH > 0)
                                     ? double(m_pendingVideoW) / double(m_pendingVideoH)
                                     : 16.0 / 9.0;
            if (qAbs(height() - qRound(clientW / ratio)) <= 1)
                return; // 收敛（±1px 容差防 WM 舍入往返）：停
            if (++m_miniRefitAttempts > App::miniRefitMaxAttempts())
                return; // 自上次外部唤醒起未收敛轮数达预算：停，等下一次外部唤醒重新武装
            refitMiniToAspect();
            m_miniRefitTimer->start(); // 未收敛：继续轮询，直到结果符合要求
        });
    }
    // 预算清零规则：位置变化（拖动）视为外部唤醒（自家 clampMiniIntoWorkArea 的
    // move 也会走到，实害为零：贴角待办一次性、换片自适应本就属合理唤醒）；尺寸
    // 事件则区分回声——与上次请求一致的尺寸是自家修正的回声，不清预算（否则自家
    // resize 的回声把预算清零，上限形同虚设），与请求不一致的尺寸是外部几何变化
    // （手势覆写 / WM 钳制修正），重新武装。注意：WM 对请求的钳制修正也构成外部
    // 唤醒，故该场景下轮询不设上界（罕见且每轮代价极小，见规格实测口径）
    if (positionAroused || size() != m_miniRefitRequested)
        m_miniRefitAttempts = 0;
    m_miniRefitTimer->start();
}
#endif

// 覆盖式全屏切换（N3）：不隐藏控件、不重父化、wid 不重设、零黑闪
void MainWindow::toggleFullscreen() {
    if (m_mini) {
        // N68：全屏与小窗互斥，小窗态忽略。早退前按真相纠正勾选项——本函数由
        // 勾选项 toggled 与视频区双击两条路进来，动作已翻成 checked 而状态没进
        // 全屏，会让退出小窗后的"全屏"菜单项显示失真，且 F11 须按两次才生效
        QSignalBlocker blocker(m_fullscreenAction);
        m_fullscreenAction->setChecked(m_fullscreen);
        return;
    }
    if (!m_fullscreen) {
        // 全屏态落盘守卫：快照进全屏前几何（saveSettings 落盘 + 退出恢复共用）。
        // A18：快照扩为整矩形（含位置），无框方案的普通态退出恢复需要
        m_preFsMaximized = isMaximized();
        m_preFsRect = m_preFsMaximized ? normalGeometry() : geometry();
        // N66 §二-2：窗口态真相快照——顶端快照点（与 m_preFsRect 同点，主窗仍是
        // 普通几何）。show() 之后主窗已重排成全屏尺度，左 pane 宽度那时读到的是
        // "屏宽 − 列表宽"，拿它作退出锚点会把窗口重新撑大。面板可见性读真相成员
        // （与 isVisible() 等值，见成员注释）
        m_preFsLeftWidth = m_splitter->widget(0)->width();
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
        // N66 §二-7：全屏期播放列表动作保持可用（原 A20 禁用口径作废，改义见
        // setPlaylistVisible 的全屏分流）
        m_fullscreen = true;
        m_refitAfterFullscreen = false; // 进全屏清空：仅记录"本次全屏期内是否换片"
        // A18 无框手动几何全屏（Windows 口径；Linux 不走手动几何——flags 切换后经
        // showFullScreen 走状态机，见下方 #else）：绕开 WindowFullScreen 状态机（其进出必经
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
        setWindowState(Qt::WindowNoState);
        setGeometry(screen()->geometry()); // 窗口所在屏，覆盖任务栏区
        show();
#else
        // N83：X11 下上面这套手动几何盖不住底部面板——EWMH 里停靠面板由 strut 托底，
        // WM 恒把它压在"非全屏"窗口之上，没有 Windows 那套"无边框窗口铺满整屏即自动
        // 收起任务栏"的启发式。走全屏态：Qt 发 _NET_WM_STATE_FULLSCREEN，实测经
        // showFullScreen 原地生效、不重建平台窗口，wid 与 mpv 嵌入不受连坐（会崩的
        // 那条只发生在 Qt flags 通道上的 StaysOnTop 变更，见 §8）；几何交由 WM 按
        // 整屏定，不再自己 setGeometry 与 strut 拉扯
        showFullScreen();
#endif
        syncVideoGeometry(); // 铺满后视频区对位；随后的 resizeEvent 幂等兜底
        // N66 §二-2：主窗落定后装配浮窗（A18 flags 重建已收敛，owned 浮窗可安全
        // 装配）——重挂控制栏、记高度与全屏宽度初值、定几何；两浮窗恒 Hidden 进全屏
        ensureFullscreenOverlays();
        reparentIntoOverlay(m_bottomBar, m_controlOverlay);
        reparentIntoOverlay(m_playlist, m_playlistOverlay);            // §二-2：面板同批重挂
        m_bottomBarHeight = qMax(1, m_bottomBar->sizeHint().height()); // 记录时钳位，判据与几何同源
        if (m_fsPlaylistWidth <= 0) // 进程内记忆首取窗口态宽度（为零用默认值，§二-8）
            m_fsPlaylistWidth = m_playlistWidth > 0 ? m_playlistWidth : App::playlistDefaultWidth();
        repositionFullscreenOverlays();
        // N66 §二-7：☰ / 动作勾选态切到允许位（QSignalBlocker 挡在回调外，A12 口径；
        // 退出时置回窗口态真相，两向同步都在回调外完成）
        {
            QSignalBlocker toggleBlocker(m_playlistToggle);
            m_playlistToggle->setChecked(m_fsPlaylistEdgeEnabled);
        }
        {
            QSignalBlocker actionBlocker(m_playlistAction);
            m_playlistAction->setChecked(m_fsPlaylistEdgeEnabled);
        }
    } else {
        m_fullscreen = false;
        // N66 §二-2 ①：改 flags 前只收浮窗、把两控件挂回原位（②只恢复对象树形状，
        // 播放列表可见性与宽度联动留到 ④ 主窗几何落定之后；控制行窗口态常显，
        // 可见性随挂回直接恢复）
        setOverlayShown(Overlay::ControlBar, false);
        setOverlayShown(Overlay::Playlist, false);
        restoreOriginalParent(m_bottomBar);
        restoreOriginalParent(m_playlist);
        // A18：恢复有框样式触发重建（自动隐藏），新窗口几何继承自全屏矩形。
        // 趁隐藏期植入快照几何——它随即成为最大化"还原位置"，取消最大化
        // 时精确回到进全屏前的尺寸；再按快照分流显示
        setWindowFlags(m_preFsFlags);
#ifndef Q_OS_WIN
        // N83：与进入侧 showFullScreen 成对——show() 不清窗口状态（只有
        // showNormal/showMaximized 会），全屏态若残留，WM 继续按整屏摆窗、面板不回归
        setWindowState(Qt::WindowNoState);
#endif
        setGeometry(m_preFsRect);
        if (m_preFsMaximized)
            showMaximized();
        else
            show();
        menuBar()->show(); // 恢复菜单栏（布局重排会经占位符事件链触发几何同步）
        syncVideoGeometry();
        // N66 §二-2 ④：主窗几何落定后按窗口态恢复面板可见性与宽度。锚点用进全屏前
        // 记下的左 pane 宽度直调（挂回后现读必被新默认分配污染，A20 坑 2）；调用时机
        // 必须在 flags / setGeometry / show() 之后（该函数读当前外框与所在屏，全屏
        // 矩形上算出的目标宽度是错的）。最大化分支经既有 isMaximized 守卫早退，
        // 与 A20 窗口态语义一致
        m_playlist->setVisible(m_playlistVisibleTruth);
        if (m_playlistVisibleTruth)
            applyPlaylistGeometry(true, m_preFsLeftWidth);
        // N66 §二-7：☰ 与 m_playlistAction 勾选态置回窗口态真相（A12 口径，
        // QSignalBlocker 挡在回调外，否则勾选态被钉在错误来源上）
        {
            QSignalBlocker toggleBlocker(m_playlistToggle);
            m_playlistToggle->setChecked(m_playlistVisibleTruth);
        }
        {
            QSignalBlocker actionBlocker(m_playlistAction);
            m_playlistAction->setChecked(m_playlistVisibleTruth);
        }
        // B16：全屏期内若切换过视频，当前尺寸与"进全屏前的小窗"已不一致；
        // 恢复普通态后按当前视频尺寸重新适配（最大化分支不走此路，保持全屏宽度）
        if (m_refitAfterFullscreen) {
            m_refitAfterFullscreen = false;
            // N40：适配缓冲定时器已删，零延迟转发语义等价改走 singleShot——
            // 布局落定后按最近一次收到的发布值（pending）触发
            QTimer::singleShot(
                0, this, [this] { adaptWindowSizeToVideo(m_pendingVideoW, m_pendingVideoH); });
        }
        m_controller->setProperty(QStringLiteral("video-zoom"), m_savedZoom);
        m_controller->setProperty(QStringLiteral("video-pan-x"), m_savedPanX);
        m_controller->setProperty(QStringLiteral("video-pan-y"), m_savedPanY);
        if (m_navigator) { // 非 null ⟹ 必为被本方案隐藏的全屏前可见面板
            m_navigator->show();
            repositionNavigator();
        }
        m_navigatorAction->setEnabled(true);
        // N66 §二-7：全屏期动作未禁用，退出无需恢复可用态（原 A20 恢复行随之作废）
    }
    {
        QSignalBlocker blocker(m_fullscreenAction); // 防循环触发 toggleFullscreen
        m_fullscreenAction->setChecked(m_fullscreen);
    }
    // N88：镜像 mpv 的 fullscreen 属性。写入时序铁律——m_fullscreen 已在两分支
    // 落定后才写，回灌回来时与真相必等、被分派守卫吸收（顺序反了会被判成外部
    // 变更二次驱动本函数，递归只从这一处来）。wid 嵌入下该属性在 mpv 侧是惰性
    // 的（值变而无窗口动作），写值安全
    m_controller->setProperty(QStringLiteral("fullscreen"), m_fullscreen);
}

// N68：小窗态等比缩放与边缘吸附（Windows 主路径）。两项增强在原生缩放/拖动
// 循环内部修正 RECT——每次鼠标消息的定框即已含约束，无追帧、无回弹；贴边/钳制
// 一律以**可见外框**（DWM EFB）为准：原生外框含不可见缩放边框（贴边留缝、缝中
// 是阴影），客户区不含发丝边框（贴边裁边），均非人眼可见框。inset 在循环公共
// 起点（WM_ENTERSIZEMOVE，窗口静止）测一次缓存，EXITSIZEMOVE 作废，循环内复用；
// WM_SIZING 定框另经工作区钳制收口（尺寸上限 + 最小移动钳位），右下角贴边起步、
// 拖左/上边扩大时整窗不再越出工作区。降级路径（非 Windows 或 OXBOW_PUREQT）整段不在
// 此处处理消息，两项增强一概不做：等比缩放只留定态保比（结果驱动收口 → refitMiniToAspect），
// 吸附连"松手贴齐"也不做——循环外改窗位会被原生模态循环按自己的轨迹覆写（逐条事件贴齐
// 实测乱跳，第 10 条），退到松手时点仍会在拖拽中途停顿时闪回（第 11 条撤销第 9 条的扩大）。
// 两路径同一条纪律：都不把已被用户拖出工作区的窗拽回来（第 8 条裁决），越界收回只在
// 程序性场景经 clampMiniIntoWorkArea——换片自适应与降级进窗贴角。
// DPI 纪律：消息的 RECT 均为物理像素，运算一律在物理域——逻辑常量与逻辑工作区
// 统一按窗口 devicePixelRatioF 换算（同一标度），装饰差值现读、不读 Qt 缓存几何
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
#if defined(Q_OS_WIN) && !defined(OXBOW_PUREQT)
    auto *msg = static_cast<MSG *>(message);
    // 拖动/缩放模态循环的起点锚：只在此刻记一次，整轮拖动都以此为原始位置基准
    if (m_mini && msg->message == WM_ENTERSIZEMOVE) {
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        RECT now{};
        GetWindowRect(hwnd, &now);
        m_miniDragOriginRect = QRect(now.left, now.top, now.right - now.left, now.bottom - now.top);
        POINT cur{};
        GetCursorPos(&cur);
        m_miniDragCursorOrigin = QPoint(cur.x, cur.y);
        // 可见框 inset 在循环公共起点测一次缓存（窗口静止、几何可信）：DWM 可见外框
        // 相对原生 RECT 的差值——原生外框左/右/下含不可见缩放边框（阴影带，贴边留缝）、
        // 客户区不含发丝边框（贴边裁边），均非人眼可见框（第 5 条）
        m_miniFrameInsetsValid = false;
        RECT dwm{};
        if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &dwm, sizeof(dwm)) == S_OK) {
            m_miniFrameInsets = QMargins(dwm.left - now.left, dwm.top - now.top,
                                         dwm.right - now.right, dwm.bottom - now.bottom);
            m_miniFrameInsetsValid = true;
        }
    }
    if (m_mini && msg->message == WM_EXITSIZEMOVE) {
        m_miniDragOriginRect = QRect(); // 作废：杜绝跨循环误用旧锚点
        m_miniFrameInsetsValid = false; // inset 同随循环作废（装饰循环期内不变）
    }
    if (m_mini && msg->message == WM_SIZING) {
        // lParam 指向拖动中的 RECT（屏幕物理坐标），wParam 为拖动边（WMSZ_*）
        auto *rc = reinterpret_cast<RECT *>(msg->lParam);
        const UINT edge = static_cast<UINT>(msg->wParam);
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        RECT wr;
        RECT cr;
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        // WM_SIZING 期间 Qt 缓存几何陈旧，装饰差值只用现读值
        const LONG chromeW = (wr.right - wr.left) - (cr.right - cr.left);
        const LONG chromeH = (wr.bottom - wr.top) - (cr.bottom - cr.top);
        const qreal dpr = devicePixelRatioF();
        const LONG cw = rc->right - rc->left - chromeW;
        const LONG ch = rc->bottom - rc->top - chromeH;
        // 视频比例无量纲直接用；无流按 16:9（与进入路径同源）
        const double ratio = (m_pendingVideoW > 0 && m_pendingVideoH > 0)
                                 ? double(m_pendingVideoW) / double(m_pendingVideoH)
                                 : 16.0 / 9.0;
        const LONG minW = qRound(App::miniMinWidth() * dpr);
        // 工作区物理矩形：定框钳入工作区，贴边起步拖边扩大时整窗不越界（实测：
        // 拖左下角顶边为锚、整窗向下探出屏外）。判定与钳制以**可见外框**为准——
        // 原生外框含不可见缩放边框（贴边留缝）、客户区不含发丝边框（贴边裁边），
        // 均非人眼可见框；inset 取循环起点缓存，异常序缺失时退化外框口径（第 5 条）
        const QRect area = screen()->availableGeometry();
        // 换算只用窗口 DPR：RECT 与窗口同域，逻辑工作区与逻辑常量按同一因子才自洽
        // （同屏时屏因子恒等于窗因子；跨屏不同缩放下窗口因子才是正确标度）
        const LONG physL = qRound(area.left() * dpr);
        const LONG physT = qRound(area.top() * dpr);
        const LONG physR = qRound((area.left() + area.width()) * dpr);
        const LONG physB = qRound((area.top() + area.height()) * dpr);
        const QMargins insets = m_miniFrameInsetsValid ? m_miniFrameInsets : QMargins();
        // 可见框上限换到客户域（可见宽 = native 宽 + inR − inL）：真值维先钳上限
        // 再反推，反推维才不破比例；上限取"两轴可见框都不超工作区"的紧值
        const LONG maxClientW = (physR - physL) - (insets.right() - insets.left()) - chromeW;
        const LONG maxClientH = (physB - physT) - (insets.bottom() - insets.top()) - chromeH;
        const bool verticalEdge = edge == WMSZ_LEFT || edge == WMSZ_RIGHT || edge == WMSZ_TOPLEFT ||
                                  edge == WMSZ_BOTTOMLEFT || edge == WMSZ_TOPRIGHT ||
                                  edge == WMSZ_BOTTOMRIGHT;
        const bool horizontalEdge = edge == WMSZ_TOP || edge == WMSZ_BOTTOM ||
                                    edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT ||
                                    edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT;
        // 被拖的那条边给什么就以什么为准（宽或高跟随鼠标），另一边按视频比例反推。
        // §4 原文是反过来的（拖左右边锁高反推宽），按原文实现永远到不了新尺寸——
        // 小窗恒正好是那个比例，"锁高反推宽"推出的就是原宽，于是四边拖拽全成 no-op
        // （实测只有四角能动，四角走的是"以宽为准"分支）。偏离论证见规格变更记录第 6 条。
        // 宽下限必须在反推**之前**钳住真值维，否则钳后两边失配
        LONG cwTrue;
        LONG chTrue;
        if (horizontalEdge && !verticalEdge) { // 拖上/下边：高为准，反推宽
            chTrue = qMax(ch, static_cast<LONG>(qRound(minW / ratio)));
            chTrue = qMin(chTrue, qMin(maxClientH, static_cast<LONG>(qRound(maxClientW / ratio))));
            cwTrue = qRound(chTrue * ratio);
        } else { // 拖左右边与四角：宽为准，反推高
            cwTrue = qMax(cw, minW);
            cwTrue = qMin(cwTrue, qMin(maxClientW, static_cast<LONG>(qRound(maxClientH * ratio))));
            chTrue = qRound(cwTrue / ratio);
        }
        // 锚点两轴分治：被拖的轴以**对边**为锚（对边不动，符合拖边直觉）；被反推的
        // 轴以**中线**为锚（两侧对称伸缩，免得拖下边时整窗只会往下长、画面甩偏）
        const LONG cx = (rc->left + rc->right) / 2;
        const LONG cy = (rc->top + rc->bottom) / 2;
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
            rc->left = rc->right - chromeW - cwTrue;
        else if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT)
            rc->right = rc->left + chromeW + cwTrue;
        else { // 纯上/下边：宽由中线两侧对称展开
            rc->left = cx - (chromeW + cwTrue) / 2;
            rc->right = rc->left + chromeW + cwTrue;
        }
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
            rc->top = rc->bottom - chromeH - chTrue;
        else if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT)
            rc->bottom = rc->top + chromeH + chTrue;
        else { // 纯左/右边：高由中线两侧对称展开
            rc->top = cy - (chromeH + chTrue) / 2;
            rc->bottom = rc->top + chromeH + chTrue;
        }
        // 位置钳制：可见外框按最小移动原则收进工作区（越界多少整窗平移多少），
        // 被拖边在触到工作区边界前仍跟随鼠标；尺寸已先钳上限，钳制区间不颠倒
        const LONG fx = qBound(physL - insets.left(), rc->left,
                               physR - insets.right() - (rc->right - rc->left));
        const LONG fy =
            qBound(physT - insets.top(), rc->top, physB - insets.bottom() - (rc->bottom - rc->top));
        rc->right += fx - rc->left;
        rc->bottom += fy - rc->top;
        rc->left = fx;
        rc->top = fy;
        *result = TRUE;
        return true;
    }
    if (m_mini && msg->message == WM_MOVING) {
        // lParam 指向拖动中的 RECT（物理像素）。四边独立判定，一次可同时贴齐两边（角部）。
        auto *rc = reinterpret_cast<RECT *>(msg->lParam);
        // ① 判距基准换成"未吸附原始位"= 起点矩形 + 鼠标总位移。直接拿系统给的 rc
        // 判距会形成自反馈：下一条 WM_MOVING 的输入矩形建立在上一次已吸附的结果上，
        // 每轮位移又被拉回带内，慢速拖动永远脱不开（实测——只有单帧位移超过带宽的
        // 快拖才侥幸挣脱）。锚点缺失（异常序）时退回系统提案，至多退化为旧行为
        RECT raw = *rc;
        if (!m_miniDragOriginRect.isEmpty()) {
            POINT cur{};
            GetCursorPos(&cur);
            raw.left = m_miniDragOriginRect.left() + (cur.x - m_miniDragCursorOrigin.x());
            raw.top = m_miniDragOriginRect.top() + (cur.y - m_miniDragCursorOrigin.y());
            raw.right = raw.left + m_miniDragOriginRect.width();
            raw.bottom = raw.top + m_miniDragOriginRect.height();
        }
        // ② 判距口径用**可见外框**：原生外框左/右/下含不可见缩放边框（贴边留缝、
        // 缝中是 DWM 阴影），客户区不含发丝边框（贴边裁边），均非人眼可见框；
        // inset 取循环起点缓存，异常序缺失时退化外框口径（第 5 条）
        // ③ 工作区是逻辑坐标，按窗口 DPR 换算成物理矩形后才与可见框同域比距离
        //    （换算只用窗口因子：RECT 与窗口同域，与 WM_SIZING 同一标度）
        const QRect area = screen()->availableGeometry();
        const qreal dpr = devicePixelRatioF();
        const LONG physL = qRound(area.left() * dpr);
        const LONG physT = qRound(area.top() * dpr);
        const LONG physR = qRound((area.left() + area.width()) * dpr);
        const LONG physB = qRound((area.top() + area.height()) * dpr);
        const LONG snap = qRound(App::miniSnapDistancePx() * dpr);
        const QMargins insets = m_miniFrameInsetsValid ? m_miniFrameInsets : QMargins();
        const LONG dLeft = (raw.left + insets.left()) - physL;
        const LONG dRight = physR - (raw.right + insets.right());
        const LONG dTop = (raw.top + insets.top()) - physT;
        const LONG dBottom = physB - (raw.bottom + insets.bottom());
        // ④ 贴齐 = 整轴平移差值；脱离阈值即带宽本身，无须另设迟滞——基准与鼠标位移
        // 一一对应，拖过 snap 自然脱开
        LONG shiftX = 0;
        if (dLeft >= 0 && dLeft <= snap)
            shiftX = -dLeft;
        else if (dRight >= 0 && dRight <= snap)
            shiftX = dRight;
        LONG shiftY = 0;
        if (dTop >= 0 && dTop <= snap)
            shiftY = -dTop;
        else if (dBottom >= 0 && dBottom <= snap)
            shiftY = dBottom;
        rc->left = raw.left + shiftX;
        rc->top = raw.top + shiftY;
        rc->right = raw.right + shiftX;
        rc->bottom = raw.bottom + shiftY;
        // 改 RECT 即生效，无需返回值处理，落入基线 DefWindowProc
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

// N68：小窗模式唯一收口（I / Esc / 关闭钮三路共用）。进入：快照 → 收起非视频 UI 并
// 放宽占位符下限 → **可见态**冲刷布局 → 切 Tool flags（平台窗口重建并隐式隐藏，子原生
// HWND 经 SetParent 平移保身份，wid 不失效，A18 实证事实）→ 按客户区口径定几何并贴
// 工作区右下角 → show。冲刷必须在切 flags 之前：隐藏态布局重算被 Qt 推迟，陈旧最小
// 尺寸会把目标几何整块钳走（实测）。退出仿全屏退出序恢复 flags / 几何 / UI / 导航器 /
// zoom-pan。全屏态互斥忽略；勾选项在末端按真相回写，互斥早退路径自带同样回写
void MainWindow::toggleMini() {
    if (m_fullscreen) {
        // 与全屏互斥：小窗入口在全屏态忽略。回写勾选项——本函数由勾选项 toggled
        // 触发时动作已翻成 checked，不回写就与 m_mini 分叉（同 toggleFullscreen 的
        // 互斥守卫，两处一个口径）
        QSignalBlocker blocker(m_miniAction);
        m_miniAction->setChecked(m_mini);
        return;
    }
    if (!m_mini) {
        // 快照（仿全屏 N3/A18：整矩形含位置，最大化取 normalGeometry 作还原位）
        m_preMiniMaximized = isMaximized();
        m_preMiniRect = m_preMiniMaximized ? normalGeometry() : geometry();
        m_preMiniFlags = windowFlags();
        // 收起非视频 UI（只隐藏不重挂；面板可见性真相 m_playlistVisibleTruth 不动，
        // 退出按真相恢复）。mini 全程禁用 N66 浮窗机制，不触碰任何 overlay
        menuBar()->hide();
        m_bottomBar->hide();
        m_playlist->hide();
        // 导航器仿全屏自动禁用（独立顶级浮窗，退出时恢复）
        if (m_navigator)
            m_navigator->hide();
        m_navigatorAction->setEnabled(false);
        // zoom/pan 先存后写零（复用全屏快照成员：两态互斥不并存，无冲突）——
        // 小窗与全屏同为"视频铺满客户区"的覆盖形态，复位 = 保持比例填充
        m_savedZoom = m_controller->propertyDouble(QStringLiteral("video-zoom"), 0.0);
        m_savedPanX = m_controller->propertyDouble(QStringLiteral("video-pan-x"), 0.0);
        m_savedPanY = m_controller->propertyDouble(QStringLiteral("video-pan-y"), 0.0);
        m_controller->setProperty(QStringLiteral("video-zoom"), 0.0);
        m_controller->setProperty(QStringLiteral("video-pan-x"), 0.0);
        m_controller->setProperty(QStringLiteral("video-pan-y"), 0.0);
        // 占位符最小尺寸放宽（videoDefaultSize 480×320 会把 400px 宽钳住），退出恢复
        m_videoPlaceholder->setMinimumSize(QSize(0, 0));
        // 收起 UI 与放宽下限都只是"记在布局上"，必须**趁窗口仍可见**把布局重算落地：
        // 隐藏态下 QLayout::activate() 与 LayoutRequest 一律被 Qt 推迟（实测：先切
        // flags 再冲刷，minimumSizeHint 恒停在普通态的 705×400 = 480+220+把手 /
        // 320+底栏，随后的 setGeometry 被它整块钳走，小窗成不了 400 宽）。
        // 冲刷手法同 applyPlaylistGeometry：逐轮 sendPostedEvents 至最小尺寸稳定
        {
            const QSize minBefore = minimumSizeHint();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            if (minimumSizeHint() != minBefore) {
                for (int k = 0; k < 4; ++k) {
                    const QSize again = minimumSizeHint();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                    if (minimumSizeHint() == again)
                        break;
                }
            }
            if (layout())
                layout()->activate();
        }
        m_mini = true;
#if defined(Q_OS_WIN) && !defined(OXBOW_PUREQT)
        // 主路径：新一轮生命周期开始，上轮拖动的锚点与 inset 缓存先作废（正常由
        // WM_EXITSIZEMOVE 清，异常序可能压根没走到）——陈旧锚点会让首轮 WM_MOVING
        // 按上一轮的起点判距离，小窗一上手就跳到别处
        m_miniDragOriginRect = QRect();
        m_miniFrameInsetsValid = false;
#else
        // 降级：挂起"进窗贴角"一次性待办，交给首条去抖收口做——装饰尺寸要等 show 之后
        // 才落定，即时读 frameGeometry 会偏
        m_miniEntryGluePending = true;
#endif
        // N83：Linux 小窗与 Windows 同走 Qt::Tool——窗口类型变更（Qt::Window↔Qt::Tool）
        // 实测不重建平台窗口，早先"小窗必崩"的真因是 WindowStaysOnTopHint 经 Qt flags
        // 通道变更触发销毁重建、连带 mpv 嵌入窗口毁灭退出（N75 §3.9 更正）。置顶不在
        // 这里落——非 Windows 经 X11Util 直发 _NET_WM_STATE_ABOVE（见下），两平台
        // 小窗类型语义自此一致（N68 不降级）
        Qt::WindowFlags miniFlags =
            Qt::Tool | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint;
#ifdef Q_OS_WIN
        miniFlags |= Qt::WindowStaysOnTopHint;
#endif
        setWindowFlags(miniFlags);
        setWindowState(Qt::WindowNoState);
        // 目标客户区：宽 400 逻辑 px，高按视频比例（无流按 16:9 成 400×225）
        const int clientW = App::miniDefaultWidth();
        const double ratio = (m_pendingVideoW > 0 && m_pendingVideoH > 0)
                                 ? double(m_pendingVideoW) / double(m_pendingVideoH)
                                 : 16.0 / 9.0;
        const int clientH = qRound(clientW / ratio);
        const QRect work = screen()->availableGeometry();
        // 先按客户区口径给几何（show 前 Qt 装饰缓存不可信，尺寸只能客户区口径直给；
        // 实测 setGeometry 是"尺寸=客户区、位置=客户区左上角"）；贴角随后分两口径，
        // 两口径的参照物都是**可见外框**（第 5 条）：主路径 show 后 Win32 现查 EFB
        // 并按原生域算位移、经 SetWindowPos 直落原生域；降级路径全程留在 Qt 域内
        // （Qt 的 frameGeometry 已剥不可见边框，故按它钳回即贴齐，不得掺原生位移——
        // 掺了就是第 5 条实测的"垂直抵消、水平偏出一侧"）
        const QPoint tl(work.right() + 1 - clientW, work.bottom() + 1 - clientH);
        setGeometry(QRect(tl, QSize(clientW, clientH)));
        show();
#if defined(Q_OS_WIN) && !defined(OXBOW_PUREQT)
        {
            // 贴角以**可见外框**为准（第 5 条）：原生外框含不可见缩放边框（贴角
            // 留缝、缝中是阴影），客户区不含发丝边框（贴角裁角）。位移必须经
            // SetWindowPos 直落原生域——Qt 的 move() 按自身 frameMargins 折算外框
            // 坐标，而该边距剥掉了不可见边框（普通窗口钳回贴合的同一事实），与
            // 原生装饰恰好差一圈：垂直抵消、水平偏出一侧（实测：底边贴合、右边
            // 留缝）。EFB 现查一次（窗口已静止落地），不可得时退化外框口径
            const HWND hwnd = reinterpret_cast<HWND>(winId());
            RECT wr;
            RECT dwm{};
            GetWindowRect(hwnd, &wr);
            const QRect area = screen()->availableGeometry();
            const qreal dpr = devicePixelRatioF(); // 换算只用窗口因子（与 nativeEvent 同）
            const LONG targetR = qRound((area.left() + area.width()) * dpr);
            const LONG targetB = qRound((area.top() + area.height()) * dpr);
            if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &dwm, sizeof(dwm)) != S_OK)
                dwm = wr;
            const LONG shiftX = targetR - dwm.right;
            const LONG shiftY = targetB - dwm.bottom;
            if (shiftX != 0 || shiftY != 0)
                SetWindowPos(hwnd, nullptr, wr.left + shiftX, wr.top + shiftY, 0, 0,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
        }
#else
        // 降级路径的贴角排给首条去抖收口（见 scheduleMiniRefit）：那时刻平台/WM 的
        // 装饰尺寸已落定，clampMiniIntoWorkArea 按 Qt 的 frameGeometry 把外框收到
        // 工作区内即贴右/下边——全程在 Qt 域内取值与下发，不碰原生坐标
        scheduleMiniRefit();
#endif
        // Windows 主路径：flags 切换销毁并重建平台窗口，新 HWND 不被自动激活（实测：
        // 进小窗即失焦，双挂主窗的快捷键全哑）；Linux 类型变更不重建平台窗口
        // （N75 §3.9），activateWindow 仅为确保激活。两平台都 show 之后显式夺回层次
        // 与激活态
        raise();
        activateWindow();
#ifndef Q_OS_WIN
        // N83：置顶经 EWMH 直发（绕过 Qt flags 通道，见 X11Util.h）——StaysOnTop
        // 的任何 Qt 通道变更都触发平台窗口销毁重建（QWidget 层、QWindow 层均实测），
        // 连坐 mpv 嵌入窗口
        setWindowAboveEwmh(winId(), true);
#endif
        syncVideoGeometry(); // 铺满后视频区对位；随后的 resizeEvent 幂等兜底
    } else {
        m_mini = false;
#if !defined(Q_OS_WIN) || defined(OXBOW_PUREQT)
        // 降级：进窗贴角待办随小窗生命周期了结，去抖定时器可能正挂着
        m_miniEntryGluePending = false;
        if (m_miniRefitTimer)
            m_miniRefitTimer->stop();
#endif
        // 占位符最小尺寸恢复（普通态下限回归 videoDefaultSize）
        m_videoPlaceholder->setMinimumSize(App::videoDefaultSize());
#ifndef Q_OS_WIN
        // N83：退出小窗先摘置顶（EWMH 直发，同进入侧）
        setWindowAboveEwmh(winId(), false);
#endif
        // 仿全屏退出序：恢复 flags（Windows 触发重建并隐式隐藏；Linux 类型变更不
        // 重建，N75 §3.9），趁隐藏期植入快照几何，
        // 再按快照分流显示
        setWindowFlags(m_preMiniFlags);
        setGeometry(m_preMiniRect);
        if (m_preMiniMaximized)
            showMaximized();
        else
            show();
        menuBar()->show();
        m_bottomBar->show();
        m_playlist->setVisible(m_playlistVisibleTruth);
        if (m_navigator) { // 非 null ⟹ 必为被本方案隐藏的小窗前可见面板
            m_navigator->show();
            repositionNavigator();
        }
        m_navigatorAction->setEnabled(true);
        // zoom/pan 快照回写（与全屏退出同序）
        m_controller->setProperty(QStringLiteral("video-zoom"), m_savedZoom);
        m_controller->setProperty(QStringLiteral("video-pan-x"), m_savedPanX);
        m_controller->setProperty(QStringLiteral("video-pan-y"), m_savedPanY);
        // 退出同为 flags 重建，与进入同因显式夺回激活态
        raise();
        activateWindow();
        syncVideoGeometry(); // 兜底：退出后视频区对位回占位符
    }
    {
        QSignalBlocker blocker(m_miniAction); // 防循环触发 toggleMini（全屏同款）
        m_miniAction->setChecked(m_mini);     // 两分支走完后按真相统一回写
    }
    // N88：镜像 mpv 的 fullscreen 属性。进入与退出两分支的镜像值恒为 false——
    // 铁律按窗口真实状态取值，小窗态也不是全屏（不按"处在哪个模式"顺手映射成
    // true）；互斥早退分支（全屏态）不经过这里，彼时属性已由 toggleFullscreen
    // 写成 true，无需重写。m_mini 已先落定，回灌由分派守卫吸收
    m_controller->setProperty(QStringLiteral("fullscreen"), false);
    // 同一收口喂给光标行为机：进小窗即解除布防并恢复已藏的指针，退出重新计时长
    m_cursorHider->setMini(m_mini);
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

// N90：ShortcutOverride 否决在控件链侧的兜底。实测（Windows 与 X11 同）让位期的征询一律被
// 外层的 windowHandle 过滤器收下并 return true，到不了本覆写——它只在过滤器缺位的瞬间生效：
// 平台窗口经 setWindowFlags 重建后旧句柄连同过滤器一起没掉，直到 showEvent 补装才接回；
// 那段时间里征询沿控件链传到这里。
// 判据同出 yieldOwnsKeyboard，接受即跳过本次 shortcut 匹配、键照常投递；忽略则快捷键照常触发
bool MainWindow::event(QEvent *event) {
    if (event->type() == QEvent::ShortcutOverride && yieldOwnsKeyboard()) {
        event->accept();
        return true;
    }
    return QMainWindow::event(event);
}

// 占位符几何跟随 + 视频区单击/双击分流（N3）。
// 右键与 Release 不处理：右键无任何行为变化。
// N66：本过滤器只服务本类对象级安装（视频区 / 占位符 / 滑条 / 浮窗 / 热区条）；
// app 级的弹层关闭观察由独立的 PopupCloseWatch 承载，不经此函数（§三-4）
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // N90：让位期的窗口级拦截。Qt6 的 shortcut 匹配先于键事件投递，否决它的唯一先手是接受
    // 平台的 ShortcutOverride 征询（接受即跳过匹配、键照常按普通键投递）——实测两平台这条
    // 征询都走到本过滤器，故外层一并收下并 return true；KeyPress 截断分支承接被否决后投递
    // 的键与匹配幸存键（Tab 仍须拦在焦点导航之前）。焦点落在子控件时两分支都放行，键盘归属
    // 交回插件快捷键与控件的常规派发——预期行为，复位手段是任一路清焦（N5 / N85 等）
    if (watched == windowHandle()) {
        if (yieldOwnsKeyboard()) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }
            if (event->type() == QEvent::KeyPress) {
                // 全键截断：取到名即转发；取名落空者（纯修饰键按下、IME 组合期、
                // 不可映射键）同样吞掉——否则 Alt 单独按下会激活菜单栏、空名键继续
                // 走后续派发，截断不彻底
                forwardKeyToMpv(static_cast<QKeyEvent *>(event));
                return true;
            }
        }
        return false;
    }

    // N85：菜单栏按下清焦——左键按下即对焦点控件清焦（方向键回到全局音量模式），
    // 但恒返回 false 不吞事件，菜单照常打开。右键豁免（与视频区分支一致）。
    // 弹层是独立 popup 窗口不参与主窗焦点链，清焦时机在按下瞬间即已足够
    if (watched == menuBar()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                if (auto *fw = QApplication::focusWidget())
                    fw->clearFocus();
            }
        }
        return false;
    }

    if (watched == m_fsResizeStrip) {
        // N66 §二-8：列表浮窗左缘拖宽——按下后改宿主矩形（面板自身不动、不改视频区
        // 几何），钳制在面板 minimumWidth 与客户区宽 50% 之间；拖动期间 m_fsResizing
        // 即挂起条件（§二-5），松手按当前光标位置重评定态
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() != Qt::LeftButton)
                break;
            m_fsResizing = true;
            m_fsResizeStartX = me->globalPosition().toPoint().x();
            m_fsResizeStartWidth = m_playlistOverlay->width();
            return true;
        }
        case QEvent::MouseMove:
            if (m_fsResizing) {
                // 浮窗右缘锚定，宽度 = 起点宽 − dx：光标左移（dx<0）左缘随之左移即
                // 增宽，右移即缩窄
                const int dx = static_cast<QMouseEvent *>(event)->globalPosition().toPoint().x() -
                               m_fsResizeStartX;
                m_fsPlaylistWidth =
                    qBound(m_playlist->minimumWidth(), m_fsResizeStartWidth - dx, width() / 2);
                repositionFullscreenOverlays();
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (m_fsResizing) {
                m_fsResizing = false;
                reevaluateOverlays();
                return true;
            }
            break;
        default:
            break;
        }
    }
    if (watched == m_videoWindow) {
        // N66 §三-4：唤起——QWindow 层原始 move（早于 widget 层追踪门闸，无须
        // 开追踪）。非全屏早退；全屏时转主窗客户区坐标交分区判据
        if (event->type() == QEvent::MouseMove && m_fullscreen) {
            const auto *me = static_cast<QMouseEvent *>(event);
            evaluateOverlayVisibility(m_videoWidget->mapTo(this, me->position().toPoint()));
        }
    } else if (watched == m_controlOverlay || watched == m_playlistOverlay) {
        // N66 §三-4：浮窗 Leave → 按当前光标重评一次。挂起与保活判据的唯一实现在
        // evaluateOverlayVisibility（§三-3），此处不重复——光标已离开浮窗，保活
        // 分支自然为假即收起；"离开控制栏甩进右缘带"的切换也由同一处收口
        if (event->type() == QEvent::Leave && m_fullscreen)
            reevaluateOverlays();
    } else if (watched == m_videoPlaceholder) {
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
            // N77 §2.3：命中查询收（正播身份，key），豁免分派在聚合器内
            const auto cur = playingEntity();
            if (const auto hit =
                    m_marks->hit(hoverTime, playingListUuid(), cur ? cur->key() : QString());
                hit && !hit->title.isEmpty())
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

// N38：静音状态联动单一出口——菜单勾选与图标按钮同一处刷新，两者共用同一
// QAction 实例故天然同源。勾选态经 QSignalBlocker 直写，不回触发；本函数不播报
// OSD（OSD 由用户动作路径直显，与 B14 音量同构），故启动与恢复期均无 OSD 调用点
void MainWindow::applyMuteState(bool muted) {
    QSignalBlocker blocker(m_muteAction);
    m_muteAction->setChecked(muted);
    m_muteButton->setIcon(
        style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
}

// B11 / N56 §2.7：取名一律读正播实体的显示名 title()（与列表行同一字段，标题与 OSD #2
// 同源）。列表镜像优先——playlist-pos 推送早于新文件装载完成，此刻读属性会拿到上一个
// 文件的残留值（概率性错名的根因），镜像顺序与 mpv 恒一致，按 pos 取实体是确定性来源。
// 不再保留属性兜底（N51 亦不读 mpv 的 filename 属性，basename 语义依赖 mpv 实现）：回刷
// 收编 / duration 推送 / playlist-pos 变化都会再次驱动本函数，空窗期跳过一次刷新即可。
// N37：percent-decode 与 http / WebDAV 末段取名由实体构造期收口（HttpEntity），本处只读
// title() 不再自行加工。N70：分家后 title() 不再等于 fileName()——注入过显示名的表上是
// 频道名 / 服务端标题，未注入者按 fileName() 派生。
QString MainWindow::playingDisplayName() {
    if (const auto e = playingEntity(); e && !e->title().isEmpty())
        return e->title();
    return QString();
}

// N7 #2 + N8 Part1：标题与 OSD #2 同源刷新（B11 镜像取名）。
// N25：收编（外部变更落默认 tab）后镜像重新与 mpv 序列一致，但 playlist-pos
// 值不变不会再推送，pos 推送时刻按旧镜像取的错名会挂到下一次切条目——
// 收编后经 playingListAdopted 主动刷新一次
void MainWindow::updateNowPlayingUi() {
    if (m_closing)
        return; // 退出区间：标题归"正在关闭…"所有，正播项随 quit 消失后不再回刷标题与 OSD 文本
    if (m_playlistPos < 0)
        return; // 停止态无"正在播放"可言
    QString name = playingDisplayName();
    const int pt = m_playlist->playingTabIndex();
    const int total = pt >= 0 ? int(m_playlist->queueEntitiesOf(pt).size()) : m_playlistPos + 1;
    if (!name.isEmpty())
        setWindowTitle(name); // N8 Part1：标题跟随正播项显示名
    else
        name = tr("Unknown File");
    m_pendingNowPlaying = QStringLiteral("[%1/%2] %3").arg(m_playlistPos + 1).arg(total).arg(name);
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

// N87：未被认领按键的回落出口——Qt 的 shortcut 匹配先于事件投递，焦点链上控件又各取
// 所需，故冒泡到此的键必然插件没要（原本在事件链尽头直接蒸发）。按 mpv 键名注入
// keypress 交给 mpv 默认键位与用户 input.conf；只转按下，延时步进靠 autorepeat 连打。
// N90：让位常态下键已被窗口级过滤器截走、不到此处；它承接的是过滤器缺位的瞬间
// （平台窗口重建后到补装之前）——那时否决仍由 event() 覆写达成，键按普通投递落到顶级窗
void MainWindow::keyPressEvent(QKeyEvent *event) {
    forwardKeyToMpv(event);
    QMainWindow::keyPressEvent(event);
}

// N90：让位期键盘归属判据（合取式，单点）——让位生效 ∧ 焦点不属于主窗内任何子控件。
// 实测落点是 focusWidget() 归空（Windows 与 X11 一致）：主窗从未设焦点策略（QMainWindow 默认
// NoFocus），clearFocus 与点视频画面 / 点空白都不会把主窗本体设成焦点控件；"等于主窗本体"一条
// 只在将来给主窗开了焦点策略时成立，留作余量。eventFilter 的 windowHandle 分支与 event() 的
// ShortcutOverride 覆写共用；焦点在子控件时判据不成立，键盘交回常规派发
bool MainWindow::yieldOwnsKeyboard() const {
    QWidget *fw = QApplication::focusWidget();
    return m_inputYielded && (fw == nullptr || fw == this);
}

// N90：让位转发的单点——取名非空即异步注入 keypress，eventFilter 的截断分支与
// keyPressEvent 的兜底出口共用（全仓只允许这一种转发形态）。取名落空即静默返回，
// 吞还是放由调用方决定（截断分支吞、兜底出口放）；只转按下，KeyRelease 不经此
void MainWindow::forwardKeyToMpv(const QKeyEvent *event) {
    const QString keyName = MpvKey::nameFromEvent(event);
    if (!keyName.isEmpty())
        m_controller->commandAsync({QStringLiteral("keypress"), keyName});
}

void MainWindow::onPlayRequested(const PlaylistEntity::List &entities, int tab, int index,
                                 double seekTo, bool keepPaused) {
    // 统一播放入口（N4 合并）：整表载入 → 跳转 index → 挂起恢复进度。
    m_pendingSeekRow = -1; // 丢弃未执行的挂起 seek
    m_pendingSeekTime = -1.0;
    if (entities.empty())
        return;
    // N60 §2.4：整表载入是多命令序列（stop→clear→N×append→pos），以推送会话守卫
    // 覆盖整个函数体——析构关批并发屏障读，终点由 onPushBarrier 按屏障自带终值对账。
    // 守卫作用域含末尾的 pause 与图标更新：本批的迟投回显全在吞域之内
    const PlaylistWidget::PushSession session(m_playlist);
    // R（N56 §2.5）：下发取 playTargets(tab, entities)——喂 mpv 前最后一刻映射的播放
    // 形态，与回显侧的原样回显、reuse 索引键同源同形。N65：交 PlayTarget 整体，
    // 逐文件选项随串下发（合并已收口在 Playlist::playTargets）
    const QVector<PlayTarget> targets = m_playlist->playTargets(tab, entities);
    // mpv 的 playlist-clear 会保留当前播放的条目，先 stop 才能真正清空
    m_controller->command({QStringLiteral("stop")});
    m_controller->command({QStringLiteral("playlist-clear")});
    // 全部按原顺序 append（不自动播），最后以 playlist-pos 启动目标条目：
    // 既避免 idle 下 append-play 瞬态播放 file[0]，也保持 mpv 列表顺序与界面一致
    for (int i = 0; i < targets.size(); ++i)
        m_controller->loadFile(targets[i].url, QStringLiteral("append"), targets[i].options);
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
    if (rows.isEmpty())
        return; // N60 §2.4：空清单早退先于开批
    // N60 §2.4：逐行 playlist-remove 是一批多命令，会话守卫覆盖整个下发循环
    const PlaylistWidget::PushSession session(m_playlist);
    // 从后往前删，避免索引失效
    std::sort(rows.rbegin(), rows.rend());
    for (int row : rows)
        m_controller->command({QStringLiteral("playlist-remove"), QString::number(row)});
}

void MainWindow::onClearRequested() {
    // N60 §2.4：恒喂 stop→playlist-clear，无空情形；关批后的屏障终值恒为空列表，
    // 由对账走既有"空列表忽略"兜住
    const PlaylistWidget::PushSession session(m_playlist);
    // 同样需要先 stop，否则当前播放的条目会被保留
    m_controller->command({QStringLiteral("stop")});
    m_controller->command({QStringLiteral("playlist-clear")});
}

void MainWindow::onFilesAdded(const PlaylistEntity::List &entities) {
    updateNavigationActions(); // 本地 tab 新增内容影响停止态步进（A16）；播放态由 playlistChanged
                               // 兜底
    // 停止态判据与 togglePause / playlistPrev / playlistNext 同源（m_playlistPos < 0）。
    // 列表增补在停止态是插件内部行为，不推送 mpv：此刻 mpv 队列无人消费——一切播放
    // 经统一入口 onPlayRequested 整表载入（停止态点播放与步进均不兜底旧播放列表）；
    // 推送反而有实害——append-play 在 mpv idle 下会装载并启动追加的首条（N81 变更
    // 记录 1），append 则把条目塞进一份停止态无人读取的残留队列。两皆不取，直接不推
    if (m_playlistPos < 0)
        return;
    // 只有添加到“正在播放”tab 的文件才同步进 mpv 播放列表；
    // 其他 tab 为本地列表，双击播放时整体载入。
    if (!m_playlist->currentIsPlayingTab())
        return;
    // R（N56 §2.5）：下发取喂 mpv 前最后一刻映射的播放形态（与回显同形）；
    // N65：PlayTarget 整体——选项随串进 loadfile
    const QVector<PlayTarget> targets =
        m_playlist->playTargets(m_playlist->playingTabIndex(), entities);
    if (targets.isEmpty()) // N60 §2.4：空清单早退先于开批（不投屏障、不留空会话）
        return;
    const PlaylistWidget::PushSession session(m_playlist); // N60 §2.4：逐条 append-play 后关批
    for (const PlayTarget &target : targets)
        m_controller->loadFile(target.url, QStringLiteral("append-play"), target.options);
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
    // N66 §二-9：对话框关闭重评由 PopupCloseWatch 统一兜住（QDialog 在其观察面内），
    // 不再单独连线——重评点单一化
    // N35：ini 键变更转接播放列表——命中远端请求设置（remoteTimeoutSec /
    // ignoreHttpsErrors）时重读 ini 重建 client 并覆盖注册表，下一次求值即生效
    connect(m_settingsDialog, &SettingsDialog::pluginIniKeysChanged, this,
            &MainWindow::onPluginIniKeysChanged);
    m_settingsDialog->show();
}

// 键位收口：逐 Id 应用归一键位（归一单点在 ActionManager::effectiveKeys）。
// 幂等——按 ini 现状重算，调用方不负责"当前处于什么态"；两条路径同码：启动装配 /
// 对话框 Apply 即时生效
void MainWindow::applyShortcutOverrides() {
    if (!m_actions || m_configFile.isEmpty())
        return;
    const auto keys = m_actions->effectiveKeys(m_configFile);
    for (ActionManager::Id id : m_actions->orderedIds()) {
        // 不可改绑条目恒用声明默认
        m_actions->applyKey(id,
                            m_actions->modifiable(id) ? keys.value(id) : m_actions->defaultKey(id));
    }
}

// N86：落盘成功后的即时生效——从 ini 重载键位（幂等）。N90：让位分支已删，运行期
// 键位全程与 ini 一致，console 让位不再有"只落盘、关闭后才生效"的运行期中间态
void MainWindow::applyShortcutsLive() { applyShortcutOverrides(); }

// N87：console 开 → 让出全部键盘；关 → 收回。N90 改判：让位期键盘归属由焦点位置决定，
// 键位表全程不动——开时只做一次性焦点收口（console 一开即可打字，已定案 6）并装上
// 窗口级过滤器；关时拆掉即完，无任何恢复动作
void MainWindow::onConsoleOpenChanged(bool open) {
    if (open == m_inputYielded)
        return;
    m_inputYielded = open;
    if (open) {
        // 一次性焦点收口：焦点留在播放列表时方向键会被列表的 ShortcutOverride 认领
        // （N5），console 的历史导航与移光标就失效；清焦后主窗内再无焦点控件
        // （focusWidget() 归空，判据的 nullptr 臂），正入 eventFilter 的截断分支。
        // 期内不维持焦点、不做二次收口——焦点落回子控件即归属交回（设计 2 / 设计 3）
        if (auto *fw = QApplication::focusWidget())
            fw->clearFocus();
        attachYieldFilter();
    } else {
        detachYieldFilter();
    }
}

// 把主窗装成平台窗口自己的事件过滤器，让位期在窗口层截断全部按键（判据见 eventFilter）。
// 装的对象是 windowHandle() 那一个 QWindow，而它会被 setWindowFlags 重建（全屏与小窗进出
// 都走那条路），旧句柄连同过滤器一起没掉——故本函数幂等（同对象重复安装只回调一次），
// showEvent 里按让位态补装一次
void MainWindow::attachYieldFilter() {
    if (auto *w = windowHandle())
        w->installEventFilter(this);
}

void MainWindow::detachYieldFilter() {
    if (auto *w = windowHandle())
        w->removeEventFilter(this);
}

// 按需创建/唤起快捷键编辑对话框（仿 ensureSettingsDialog 生命周期）：非模态、
// 复用不重建。对话框是独立顶级窗口，与主窗快捷键天然互不干扰，无需绑定防护
void MainWindow::ensureShortcutsDialog() {
    if (m_shortcutsDialog) {
        m_shortcutsDialog->show();
        m_shortcutsDialog->raise();
        m_shortcutsDialog->activateWindow();
        return;
    }
    m_shortcutsDialog = new ShortcutsDialog(m_actions, m_configFile, this);
    m_shortcutsDialog->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(m_shortcutsDialog, &ShortcutsDialog::applied, this, &MainWindow::applyShortcutsLive);
    connect(m_shortcutsDialog, &QObject::destroyed, this, [this] { m_shortcutsDialog = nullptr; });
    m_shortcutsDialog->show();
}

// 按需创建/唤起 mpv 键位编辑对话框（N89，生命周期同 ensureShortcutsDialog）：
// 非模态、复用不重建，重开时对话框自行重读 input.conf 与 input-bindings。
// 注入 MpvController 与 ActionManager 两个只读指针：前者供 input-bindings 读取与
// inputConfPath 取值，后者供 Apply 前的键位冲突提示（只读查键位表）
void MainWindow::ensureMpvBindingDialog() {
    if (m_bindingDialog) {
        m_bindingDialog->show();
        m_bindingDialog->raise();
        m_bindingDialog->activateWindow();
        return;
    }
    m_bindingDialog = new MpvBindingDialog(m_controller, m_actions, this);
    m_bindingDialog->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(m_bindingDialog, &QObject::destroyed, this, [this] { m_bindingDialog = nullptr; });
    m_bindingDialog->show();
}

// R3：MpvController::volumeMaxChanged 回调——同步 m_volumeMax 与滑条量程。
// mpv 启动时会立即推送当前值（默认 130），无需手动初始化。
void MainWindow::applyVolumeMax(int v) {
    m_volumeMax = v;
    if (m_volumeSlider)
        m_volumeSlider->setRange(0, m_volumeMax);
}

void MainWindow::showAboutDialog() {
    // N84：正文后空一行接三行版本身份。标签为可翻译名词短语源串（写死）：
    // Version / Qt version / Build time；Qt 版本取运行期（qVersion()），构建时刻
    // 为 UTC 形态（偏移恒可推），三行取值与启动诊断行同源
    const QString versionBlock = tr("Version") + QStringLiteral(": ") + App::versionInfo() +
                                 QLatin1Char('\n') + tr("Qt version") + QStringLiteral(": ") +
                                 App::qtVersion() + QLatin1Char('\n') + tr("Build time") +
                                 QStringLiteral(": ") + App::buildDateTime();
    QMessageBox::about(this, tr("About %1").arg(App::name()),
                       tr("%1: a Qt-based player GUI loaded as an mpv C plugin.\n"
                          "Video rendering is performed by mpv; this plugin only provides the "
                          "graphical interface.")
                               .arg(App::name()) +
                           QStringLiteral("\n\n") + versionBlock);
}

void MainWindow::onMpvShutdown() {
    // mpv 侧发起退出（终端按 q、其他脚本发 quit）时 closeEvent 不会触发，
    // 在此补保存会话；窗口关闭路径随后到来的重复保存写同值幂等无害
    saveSettings();
    // 收口用 exit() 而非 quit()：本插件的 Qt 循环跑在 mpv 的 cplugin 线程（非进程主线程），
    // quit() 在这种布局下只往 qApp 投递一个 Quit 事件、实测永不兑现；exit() 当场同步退出
    // 本线程循环栈上的全部循环（模态对话框还开着时一并收掉）
    QCoreApplication::exit(0);
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

    // N38：静音同音量——读 UI 缓存（勾选态）而非实时查 mpv；无条件写键
    settings.setValue(QStringLiteral("mute"), m_muteAction->isChecked());

    // 播放列表面板可见性：读真相成员而非活体 isVisible()——全屏期面板被重挂且
    // 隐藏、A11 二次保存在窗口隐藏后触发，活体读数都会把该键错写成 false（N66）
    settings.setValue(QStringLiteral("playlistExpanded"), m_playlistVisibleTruth);

    // N66 §二-7：右缘自动显示允许位（全屏态真相源，与 playlistExpanded 互不读写）
    settings.setValue(QStringLiteral("fullscreenPlaylistEdge"), m_fsPlaylistEdgeEnabled);

    // N10：窗口几何/最大化不再落盘（启动恒为固定默认尺寸 960×600）

    // 会话恢复入口（Q1 方案 A）：记录最后播放的非默认列表。N57 §2.7 把键从名字换成身份
    // （`lastListUuid`）；N59 §3.1 把落点从"此刻同步目标 + 停留列表兜底"三级判定收成一级
    // ——只认**最后一次主动设定的播放目标**，停留位置完全不参与。于是形状 A（从未在非默认
    // 列表播过、关闭时焦点停在别处）与收编（播放目标真的搬回默认列表）两处错记消失，而 N53
    // 修订 3 要救的"刷新失败 / 祖先层整枝归位后仍回到那张表"由这份历史记录承载（§3.5）
    // 仅当本次进程发生过播放才更新/清除（A13）：未播放就关闭属于会话延续，
    // 上次保存的键与各列表记忆仍然有效，不得清空。
    if (m_playedThisSession) {
        const QUuid target = m_playlist->lastPlayingTargetUuid();
        if (target != kDefaultListUuid)
            settings.setValue(QStringLiteral("lastListUuid"),
                              target.toString(QUuid::StringFormat::WithoutBraces));
        else
            settings.remove(QStringLiteral("lastListUuid"));
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

    // N38：静音恢复——以 mpv 侧 mute 属性当前值为准：属性为真则跟随不下发，
    // 为假则以 ini 为准下发（无键即默认未静音）；判定不问属性来源（mpv.conf /
    // 命令行 / 其它脚本同效）。此处必须实时读属性：恢复早于事件线程启动，尚无推送
    if (!m_controller->propertyFlag(QStringLiteral("mute"), false))
        m_controller->setProperty(QStringLiteral("mute"),
                                  settings.value(QStringLiteral("mute"), false).toBool());

    // N10：窗口几何/最大化不再恢复，INI 遗留键
    // （windowMaximized/windowWidth/windowHeight）读侧删除后自然忽略

    // 播放列表面板可见性
    if (settings.value(QStringLiteral("playlistExpanded"), false).toBool())
        setPlaylistVisible(true);

    // N66 §二-7：右缘自动显示允许位（缺键按关闭——右缘唤出是需主动开启的能力）
    m_fsPlaylistEdgeEnabled =
        settings.value(QStringLiteral("fullscreenPlaylistEdge"), false).toBool();

    // N57 §2.7：会话键改用身份——上次播放的非默认列表按身份直查注册表，命中才换算
    // 视图坐标并切换（currentChanged 联动自动选中该表记忆的最后播放项）；查不到或
    // 停在默认列表则不动。旧键 lastTabName 不读也不删（与 windowMaximized 等遗留键
    // 同一口径：读侧删除即自然忽略）
    // N59 §3.4：键指向已删列表（删除当前列表时不设清理点）就在这道直查上自然落空——
    // 不切 tab 即落默认列表，无须警告也不须清 INI
    if (const Playlist *last = m_listRegistry->find(
            QUuid::fromString(settings.value(QStringLiteral("lastListUuid")).toString()))) {
        const int idx = m_playlist->tabIndexOfList(last->uuid());
        if (idx > 0)
            m_playlist->requestTab(idx); // 空闲时逐字等价于 setCurrentIndex，忙时自动排队
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

// 当前文件某类轨道的快照，按 id 升序——三组流子菜单与 ♪ 按钮的分组都从这份取表出生，
// 故条目顺序全仓一致
QList<MpvController::TrackInfo> MainWindow::tracksOfType(const QString &type) const {
    QList<MpvController::TrackInfo> result;
    for (const auto &t : m_tracks)
        if (t.type == type)
            result.append(t);
    std::sort(result.begin(), result.end(),
              [](const MpvController::TrackInfo &a, const MpvController::TrackInfo &b) {
                  return a.id < b.id;
              });
    return result;
}

// 各子菜单的 aboutToShow 都挂本函数，故一次弹出把四组一起重建。清空 + 追加的形态留在
// 调用点：播放菜单的字幕流子菜单日后要在这组条目之外追加"添加字幕"之类的动作，
// 分组函数本身只管选择条目
void MainWindow::rebuildTrackMenus() {
    if (m_videoStreamMenu) {
        m_videoStreamMenu->clear();
        addTrackSelectionGroup(m_videoStreamMenu, QStringLiteral("video"));
    }
    if (m_audioStreamMenu) {
        m_audioStreamMenu->clear();
        addTrackSelectionGroup(m_audioStreamMenu, QStringLiteral("audio"));
    }
    if (m_subStreamMenu) {
        m_subStreamMenu->clear();
        addSubtitleSelectionGroup(m_subStreamMenu, false);
    }
    if (m_secondarySubStreamMenu) {
        m_secondarySubStreamMenu->clear();
        addSubtitleSelectionGroup(m_secondarySubStreamMenu, true);
    }
}

// 单路组（视频 / 音频）：这类轨道没有"第二路"概念，selected 即当前轨，出口一律
// switchToTrack
void MainWindow::addTrackSelectionGroup(QMenu *menu, const QString &type) {
    for (const auto &t : tracksOfType(type)) {
        QAction *a = menu->addAction(formatTrackText(t));
        a->setCheckable(true);
        a->setChecked(t.selected);
        MpvController::TrackInfo copy = t;
        connect(a, &QAction::triggered, this, [this, copy] { switchToTrack(copy); });
    }
}

// N94：字幕选择条目的唯一体——播放菜单的两组与 ♪ 按钮的两个分组都从这里出生，故判据
// 全仓只此一份。两组都列全量字幕轨，不排除已被对方占用的那条——"同一条只能落在一组"
// 由点击时的抢占下发体现（见 switchToTrack / switchToSecondarySubtitle）
void MainWindow::addSubtitleSelectionGroup(QMenu *menu, bool secondary) {
    const int mainSelection = secondary ? 1 : 0;
    const QList<MpvController::TrackInfo> subs = tracksOfType(QStringLiteral("sub"));

    bool noneSelected = true;
    for (const auto &t : subs)
        if (t.mainSelection == mainSelection)
            noneSelected = false;

    QAction *none = menu->addAction(tr("None"));
    none->setCheckable(true);
    none->setChecked(noneSelected);
    if (secondary)
        connect(none, &QAction::triggered, this, &MainWindow::disableSecondarySubtitles);
    else
        connect(none, &QAction::triggered, this, &MainWindow::disableSubtitles);

    for (const auto &t : subs) {
        QAction *a = menu->addAction(formatTrackText(t));
        a->setCheckable(true);
        a->setChecked(t.mainSelection == mainSelection);
        MpvController::TrackInfo copy = t;
        if (secondary)
            connect(a, &QAction::triggered, this,
                    [this, copy] { switchToSecondarySubtitle(copy); });
        else
            connect(a, &QAction::triggered, this, [this, copy] { switchToTrack(copy); });
    }
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
    if (m_secondarySubStreamMenu)
        m_secondarySubStreamMenu->setEnabled(s > 0); // N94：与主字幕同规则
}

void MainWindow::showStreamMenu() {
    QMenu menu(this);
    const QList<MpvController::TrackInfo> videos = tracksOfType(QStringLiteral("video"));
    const QList<MpvController::TrackInfo> audios = tracksOfType(QStringLiteral("audio"));
    const QList<MpvController::TrackInfo> subs = tracksOfType(QStringLiteral("sub"));

    bool hasPrev = false;
    if (videos.size() > 1) {
        QAction *header = menu.addAction(tr("Video"));
        header->setEnabled(false);
        addTrackSelectionGroup(&menu, QStringLiteral("video"));
        hasPrev = true;
    }
    if (audios.size() > 1) {
        if (hasPrev)
            menu.addSeparator();
        QAction *header = menu.addAction(tr("Audio"));
        header->setEnabled(false);
        addTrackSelectionGroup(&menu, QStringLiteral("audio"));
        hasPrev = true;
    }
    if (!subs.isEmpty()) {
        if (hasPrev)
            menu.addSeparator();
        QAction *header = menu.addAction(tr("Subtitle"));
        header->setEnabled(false);
        addSubtitleSelectionGroup(&menu, false);

        menu.addSeparator();
        QAction *secHeader = menu.addAction(tr("Secondary Subtitle"));
        secHeader->setEnabled(false);
        addSubtitleSelectionGroup(&menu, true);
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
    // N94：抢占——mpv 不允许主 / 第二字幕同为一条，目标是第二字幕时先把它关掉。
    // 两条都同步下发，调用次序即 mpv 侧执行次序
    if (t.type == QLatin1String("sub") && t.mainSelection == 1)
        m_controller->setProperty(QStringLiteral("secondary-sid"), QStringLiteral("no"));
    m_controller->setProperty(prop, int(t.id));
    showOsd(prefix + formatTrackText(t));
}

void MainWindow::disableSubtitles() {
    m_controller->setProperty(QStringLiteral("sid"), QStringLiteral("no"));
    showOsd(tr("Subtitle: None"));
}

// N94：第二字幕。与 switchToTrack 的 sub 分支对称——抢占方向反过来（目标是主字幕
// 时先关 sid），关闭只动 secondary-sid、不碰 sid
void MainWindow::switchToSecondarySubtitle(const MpvController::TrackInfo &t) {
    if (t.mainSelection == 0)
        m_controller->setProperty(QStringLiteral("sid"), QStringLiteral("no"));
    m_controller->setProperty(QStringLiteral("secondary-sid"), int(t.id));
    showOsd(tr("Secondary Subtitle: ") + formatTrackText(t));
}

void MainWindow::disableSecondarySubtitles() {
    m_controller->setProperty(QStringLiteral("secondary-sid"), QStringLiteral("no"));
    showOsd(tr("Secondary Subtitle: None"));
}

// ---- N43：播放菜单的滤镜子菜单 ----

// 弹出时整体重建（挂父菜单 Play 的 aboutToShow，见 createMenus）：条目与勾选态
// 每次按属性实时读重算，故菜单打开时所见即当前生效状态；无条目则整菜单禁用
void MainWindow::rebuildFilterMenus() {
    auto rebuild = [this](QMenu *menu, const QString &property) {
        if (!menu)
            return;
        menu->clear();
        const QList<FilterEntry> entries = m_controller->filterEntries(property);
        const int count = int(entries.size());
        for (int i = 0; i < count; ++i) {
            QAction *a = menu->addAction(FilterListWidget::entryDisplayText(entries[i].text));
            a->setCheckable(true);
            a->setChecked(entries[i].enabled);
            connect(a, &QAction::triggered, this,
                    [this, property, i] { toggleFilterEntry(property, i); });
        }
        menu->setEnabled(count > 0);
    };
    rebuild(m_videoFilterMenu, QStringLiteral("vf"));
    rebuild(m_audioFilterMenu, QStringLiteral("af"));
}

// 点击条目：翻转启用位后整串重写——禁停位就是编译串上的 ! 前缀，与设置对话框
// 共用 compileEntries 一条写侧链路（不用 mpv 的 af/vf toggle 命令：那套语义是
// 运行期增删链上滤镜，会与 ! 位这一唯一启停真相分叉）。只改运行期属性，不写
// conf——持久配置的唯一写入口仍是设置对话框的 Apply
void MainWindow::toggleFilterEntry(const QString &property, int index) {
    QList<FilterEntry> entries = m_controller->filterEntries(property);
    if (index < 0 || index >= int(entries.size()))
        return; // 菜单弹出后链路被外部改动：按下标找不到对应条目则不动
    entries[index].enabled = !entries[index].enabled;
    m_controller->setProperty(property, FilterListWidget::compileEntries(entries));
}

// ---- N15：章节与书签 ----
void MainWindow::updateChapterSlider() {
    m_progressSlider->setDuration(m_duration);
    // N77 §2.3：标记查询收（正播身份，key），豁免分派在聚合器内
    const auto cur = playingEntity();
    const auto &marks = m_marks->marks(playingListUuid(), cur ? cur->key() : QString());
    QList<double> times;
    times.reserve(marks.size());
    for (const auto &m : marks)
        times.append(m.time);
    m_progressSlider->setMarkTimes(times);
}

void MainWindow::updateChapterMenuState() {
    // N56 §2.7：有无正播内容以正播实体为准（键非空即有），不再读 mpv 属性
    const bool hasFile = playingEntity() && m_duration > 0.0;
    if (auto *a = m_actions->action(ActionManager::Id::AddBookmark))
        a->setEnabled(hasFile);
}

void MainWindow::updateChapterMenuChecked() {
    if (!m_chapterMenu || !m_chapterGroup)
        return;
    // N77 §2.3：命中查询收（正播身份，key），豁免分派在聚合器内
    const auto cur = playingEntity();
    const auto hit = m_marks->hit(m_timePos, playingListUuid(),
                                  cur ? cur->key() : QString()); // std::optional<Mark>
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
    // N56 §2.7：以当前内容为对象的取串点统一取正播实体——key 作书签仓键匹配（B15），
    // path 作跳转用的原始显示路径；无正播实体（停止态）两者为空，菜单只剩章节段。
    // N77 §2.3：书签读入口现取正播 tab 身份一并传入，豁免分派（本地合并 / 远端严格）
    // 在聚合器内，条目 Origin 逐组随条
    const auto playing = playingEntity();
    const QUuid curUuid = playingListUuid();
    const QString curKey = playing ? playing->key() : QString();
    const QString curDisplay = playing ? playing->path() : QString();
    // 书签经聚合器统一读入口（书签永不查重，即原始）
    const QList<MarkAggregator::Mark> bookmarkMarks = m_marks->bookmarks(curUuid, curKey);

    // 跳转目标 = 菜单构建时的当前文件（菜单条目语义上属于该文件）
    auto addSection = [this, curDisplay](const QList<MarkAggregator::Mark> &items) {
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
            // N54：书签条目带上来源走"回绑定列表"的跳转；N77：合并视图内各组来源
            // 不同，每条捕获自己组的来源，不再整文件取一次。章节条目属"当前文件的
            // 这一时刻"，恒在现有列表里解决，不传来源
            if (it.isBookmark) {
                const BookmarkManager::Origin origin = it.origin;
                connect(a, &QAction::triggered, this,
                        [this, curDisplay, t, origin] { jumpToBookmark(curDisplay, t, origin); });
            } else {
                connect(a, &QAction::triggered, this,
                        [this, curDisplay, t] { jumpToChapter(curDisplay, t); });
            }
        }
    };

    // 章节段列表（书签段已在聚合器返回处成形）
    QList<MarkAggregator::Mark> chapterMarks;
    for (const auto &c : marksChapters)
        chapterMarks.append({c.time, c.title, false, BookmarkManager::Origin()});

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

// N54：章节跳转。原 jumpToFileTime 函数体逐字沿用（改名 + 末尾四行收进
// fallbackPlayInDefaultTab，零行为变化）——章节语义本就是“当前文件的这一时刻”，
// 只能在手上现有的列表里解决，那套“先扫当前 tab、再扫播放 tab、末尾兜底默认 tab”
// 的扫描对它成立且无可替代
void MainWindow::jumpToChapter(const QString &path, double time) {
    if (path.isEmpty())
        return;
    // 查当前激活列表。N56 §2.5：键级判等经实体自己的 matches 出口——形态漂移
    // （大小写 / 分隔符）仍认得出同一条内容（N30），外部不假设键的形态
    auto findInTab = [&](int tabIdx) -> int {
        if (tabIdx < 0 || tabIdx >= m_playlist->count())
            return -1;
        const PlaylistEntity::List entities = m_playlist->queueEntitiesOf(tabIdx);
        for (int i = 0; i < int(entities.size()); ++i)
            if (entities.at(i) && entities.at(i)->matches(path))
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
            const PlaylistEntity::List entities = m_playlist->queueEntitiesOf(targetTab);
            m_playlist->markPlayingTab(targetTab);
            m_playlist->requestTab(targetTab); // N19：忙时排队
            onPlayRequested(entities, targetTab, targetRow, time);
        }
        return;
    }
    // 复用“打开文件”逻辑：清空默认列表后推入新文件并跳转
    fallbackPlayInDefaultTab(path, time);
}

// N54 §2.2：书签跳转。只认书签自带的身份；N77 §2.4：目标在位 ⇒ 原样发请求（先记
// 源组）；否则 nil 与"真列表已删"并入同一分流——本地过户落默认组后按既有请求路径走
// （播放入口同路），远端不过户、弹窗中止。不再有"先在别的列表里找一把"的扫描
void MainWindow::jumpToBookmark(const QString &path, double time,
                                const BookmarkManager::Origin &origin) {
    if (path.isEmpty())
        return;
    // 第 1 步·本地缺失闸门：文件不在磁盘上，两条路都放不出来，直接说明并终止——
    // 不切 tab、不加载列表、不回退（判据与书签失效清理同源，Util::Url::isMissingLocal）
    if (Util::Url::isMissingLocal(path)) {
        warnBookmarkJump(tr("The bookmarked file no longer exists on disk."), path, QString());
        return;
    }
    const QString key = Util::Url::key(path);
    // 主判据是书签承诺的归属。真列表在位 ⇒ 回绑该列表与层（现状语义），先记源组
    const bool targetAlive = origin.isValid() && m_listRegistry->find(origin.listUuid);
    if (targetAlive) {
        PlaylistWidget::LevelJump req;
        req.itemKey = key;
        req.time = time;
        req.listUuid = origin.listUuid;
        req.levelIds = origin.levelIds;
        m_jumpSourceGroup = {origin.listUuid, key};
        m_playlist->requestLevelItemJump(req);
        return;
    }
    // N77 §2.4/§2.5：本地 ⇒ 源组过户（默认组恒在位，过户源恒非默认），随后按默认组
    // 走既有请求路径——过户组自此与原生默认组无异，播放入口也同路：项已在默认 tab
    // 队列根层 ⇒ onLevelItemReady 原样复用（同队列只 seek / 切条目，默认 tab 内容
    // 不动）；不在 ⇒ onLevelItemFailed 默认支补项播。远端 ⇒ 不过户，弹窗中止——归属
    // 纪律而非可播性判断（N55 §3.4 同调），nil 远端组同样到此（不再走"与默认同路"）
    if (Util::Url::isLocal(path)) {
        m_bookmarkMgr->transferToDefault(origin.listUuid, key);
        PlaylistWidget::LevelJump req;
        req.itemKey = key;
        req.time = time;
        req.listUuid = kDefaultListUuid;
        req.levelIds = m_playlist->queueRootIdsOf(0);
        m_jumpSourceGroup = {kDefaultListUuid, key};
        m_playlist->requestLevelItemJump(req);
        return;
    }
    warnBookmarkJump(tr("The playlist this bookmark belongs to no longer exists."), path,
                     QString());
}

// N54 §2.2 第 3 步：命中即播，三档全走既有出口（与 jumpToChapter 的三档同一形状）。
// widget 已把目标层换入前台（不变式：目标列表的显示层 == req.levelIds），故新队列直取
// 当前层出口；entityIndex 是实体序，即 mpv playlist-pos 语义；tabIndex 是视图在发射
// 时刻由 req.listUuid 现换算的坐标（N57 §2.8），本槽是它唯一的消费者
void MainWindow::onLevelItemReady(const PlaylistWidget::LevelJump &req, int tabIndex,
                                  int entityIndex) {
    // N77 §2.3：展示解析改按组——（源组，itemKey）取 displayPath（自含凭据串与干净
    // 身份同键异展，按组取才无歧义）
    const QString path = m_bookmarkMgr->displayPath({m_jumpSourceGroup.listUuid, req.itemKey});
    const bool sameQueue = tabIndex == m_playlist->playingTabIndex() &&
                           req.levelIds == m_playlist->queueRootIdsOf(tabIndex);
    if (sameQueue) {
        // 目标层就是此刻这份队列：内容基准没变，重推纯属自扰（重载文件、转接流重开、
        // 可听中断）——同列表内只切条目
        if (entityIndex == m_playlistPos) {
            m_controller->command(
                {QStringLiteral("seek"), QString::number(req.time), QStringLiteral("absolute")});
        } else {
            playAt(entityIndex);
            m_pendingSeekRow = entityIndex;
            m_pendingSeekTime = req.time;
        }
        return;
    }
    m_playlist->markPlayingTab(tabIndex); // 队列根在此重锚到该层（N45 §2.1）
    m_playlist->requestTab(tabIndex);
    onPlayRequested(m_playlist->currentLayerEntitiesOf(tabIndex), tabIndex, entityIndex, req.time);
}

// N54 §2.2 第 4 步 + §2.5 + N55 §3.4：失败策略按三件原料分流——req.listUuid 是否
// 保留身份、JumpFailure 是哪一档、Util::Url::isLocal（三件原料本槽本来就都有，
// 不需要任何新载荷）。归属默认列表的失败支：默认 tab 的根层内容只存在于层缓存、
// 其删除点只有清扫一处 ⇒ "目标层恒已加载"，ListGone 与 LevelLoadFailed 在默认 tab
// 上实际不可达，"定位失败"等价于"这项不在默认 tab 里"——本地补项播（现状），远端
// 的直接形态（含凭据快照，§2.7 落盘的那份）同样补项播：P10 的解，书签承诺的
// "把这一项送回默认列表"在此兑现。真列表在位但该层失败 / 该层中无指定文件：本地项
// 在磁盘上，落默认 tab 必然可播，静默回退；远端项回不去所属列表就没有可靠的播放
// 形态（N52 的转接 URI 依赖所属列表的端点），故一律弹窗中止并按档说准原因，
// 不做注定失败的尝试
void MainWindow::onLevelItemFailed(const PlaylistWidget::LevelJump &req,
                                   PlaylistWidget::JumpFailure why, const QString &detail) {
    // N77 §2.3：展示解析改按组——（源组，itemKey）取 displayPath
    const QString path = m_bookmarkMgr->displayPath({m_jumpSourceGroup.listUuid, req.itemKey});
    if (req.listUuid == kDefaultListUuid) {
        fallbackPlayInDefaultTab(path, req.time);
        return;
    }
    if (Util::Url::isLocal(path)) {
        fallbackPlayInDefaultTab(path, req.time);
        return;
    }
    QString reason;
    switch (why) {
    case PlaylistWidget::JumpFailure::ListGone:
        reason = tr("The playlist this bookmark belongs to no longer exists.");
        break;
    case PlaylistWidget::JumpFailure::LevelLoadFailed:
        // 正文不提"层级 / 层"这类内部概念（用户视角只有"这个列表打不开"）；detail 是
        // 提供者回传的原文（widget 只在本类失败时填），交 warnBookmarkJump 进正文之外
        reason = tr("Failed to load the playlist.");
        break;
    case PlaylistWidget::JumpFailure::ItemNotFound:
        reason = tr("The bookmarked item could not be found in its playlist.");
        break;
    }
    warnBookmarkJump(reason, path, detail);
}

// 把单个文件作为会话镜像推入默认 tab（默认 tab 不落盘、可随时被替换）。
// N54：唯一调用方是书签跳转的两条兜底（本地项回退、章节跳转扫不到时的旧路径）。
// N56 §2.7：setDefaultTabFiles 回传实体表，播放入口改传实体表
void MainWindow::fallbackPlayInDefaultTab(const QString &path, double time) {
    const PlaylistEntity::List entities = m_playlist->setDefaultTabFiles({path});
    m_playlist->markPlayingTab(0);
    m_playlist->requestTab(0); // N19：忙时排队
    onPlayRequested(entities, 0, 0, time);
}

// N54：书签跳转的报错出口。正文恒为一句人话（不含"层级"这类内部词），文件路径与
// 提供者回传的原文进 informativeText——长 URL 不挤进正文（破版），排查信息也不丢
void MainWindow::warnBookmarkJump(const QString &reason, const QString &path,
                                  const QString &detail) {
    QStringList info;
    if (!path.isEmpty())
        info << path;
    if (!detail.isEmpty())
        info << detail;
    QMessageBox box(QMessageBox::Warning, tr("Bookmark Jump"), reason, QMessageBox::Ok, this);
    if (!info.isEmpty())
        box.setInformativeText(info.join(QLatin1Char('\n')));
    box.exec();
}

void MainWindow::ensureBookmarkEditor() {
    if (m_bookmarkEditor) {
        m_bookmarkEditor->show();
        m_bookmarkEditor->raise();
        return;
    }
    // N16：传入当前播放文件使编辑器打开即聚焦该文件（若存在书签）。
    // N56 §2.7：键取正播实体，不再读 mpv 属性；N77 §2.3：聚焦改传组（正播身份 +
    // key），无正播不聚焦
    const auto cur = playingEntity();
    BookmarkManager::Group focusGroup;
    if (cur)
        focusGroup = {playingListUuid(), cur->key()};
    m_bookmarkEditor = new BookmarkEditorDialog(
        m_bookmarkMgr,
        [this](const QString &p, double t, const BookmarkManager::Origin &origin) {
            jumpToBookmark(p, t, origin); // N54：来源由对话框按选中项的键现取后随行
        },
        // N57 §2.6：一个出口同时供"叫什么名字"与"还在不在"（对话框就地派生存活判据），
        // 按身份直查注册表——名字住列表对象，改名即刻生效，不经视图反查 tab
        [this](const QUuid &listUuid) { return m_listRegistry->nameOf(listUuid); }, focusGroup,
        this);
    m_bookmarkEditor->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(m_bookmarkEditor, &QObject::destroyed, this, [this] { m_bookmarkEditor = nullptr; });
    // N66 §二-9：同设置对话框——关闭重评由 PopupCloseWatch 统一兜住，不单独连线
    m_bookmarkEditor->show();
}

// N56 §2.7：正播实体——凡"以当前内容为对象"的书签 / 聚合器 / OSD / duration 读点
// 统一经此取实体，不再读 mpv 属性。正播实体 = 播放 tab 队列根层第 m_playlistPos 项
// （镜像与 mpv 恒一致）；坐标对（playingTabIndex, m_playlistPos）在此收口，
// playlist-pos 的真相源是本类镜像。返回空 Ptr 即无正播（停止态 / 镜像未就位）
PlaylistEntity::Ptr MainWindow::playingEntity() const {
    if (m_playlistPos < 0)
        return nullptr;
    return m_playlist->playingEntityOf(m_playlist->playingTabIndex(), m_playlistPos);
}

QUuid MainWindow::playingListUuid() const {
    if (const Playlist *playing = m_playlist->listOf(m_playlist->playingTabIndex()))
        return playing->uuid();
    return {};
}

void MainWindow::onAddBookmark() {
    // N76 §2.1：书签的存储串恒为身份串（entity->path()）。真列表直接形态从此存干净
    // 身份，提供者注入的凭据退出书签 INI（N55 §2.7"下发形态落盘"的口径废止）——回跳
    // 真列表后由提供者以当前凭据现算重铸，快照陈旧一类失败面（改密码后 401）随之消失。
    // 默认归属项（收编重造 / 命令行 / 手工）StaticProvider 恒等下发，path() 与下发串
    // 同值，自含串照旧。键侧不受影响（BookmarkManager 内部归一为键，Util::Url::key
    // 先剥凭据——存储串换干净身份后同一内容的键不变），展示保原样（大小写/分隔符，B15）
    const auto entity = playingEntity();
    if (!entity)
        return;
    // N55 §2.7 / N76 §2.1：拒收分支保持——判据落在下发形态上（现算一次 playUrl()，
    // 仅作判据用；转接项的 path() 是干净身份、不带协议前缀，判据落不到身份串上）：
    // 形态带 oxbow:// 且正播 tab 是默认列表 ⇒ 拒收——那条 URI 的端点是会话内的，
    // 跨会话必死，它本来就不该有书签（N76 §2.2 后，默认 tab 里唯一可能播着 oxbow://
    // 的项就是收编来的转接项）。此处"归属"取书签承诺的归属（正播 tab 的列表，即
    // 下面 origin 的算法），不是实体的 m_ownerUuid——N55 变更记录 2 的钉定，N76 维持。
    // 拒收时经 showOsd 给一条失败提示（与成功那条同一通道、同一强度——用户按了键
    // 就得当场听到回音，静默早退等于没响应），不调 addBookmark、不发成功 OSD
    if (StreamTransit::isTransitUri(entity->playUrl())) {
        const int playingTab = m_playlist->playingTabIndex();
        const Playlist *playing = m_playlist->listOf(playingTab);
        if (!playing || playing->uuid() == kDefaultListUuid) {
            showOsd(tr("This item does not support bookmarks."));
            return;
        }
    }
    const QString rawPath = entity->path();
    const QString curKey =
        entity->key(); // 归一化键，与 jumpToBookmark 的显示串形参同名异物，改名防混
    const double t = m_timePos;
    // 自动命名：取"本文件既有书签名的最大序号 +1"，与播放列表自动命名共用同一份算法
    // （Util::nextSequenceNumber）。原实现找的是"第一个空号"——已有 Bookmark 1 与 3 时
    // 会补出 2，删掉中间一条后新条目又能复用其号，序号不随时间单调、读不出先后。
    // 作用域仍是"本文件的书签集"（各文件各自从 1 起属既有语义，本轮不动）。
    // N77 §2.3：取号与显示同口径——本地按合并视图取号（命名空间不分组，接受历史
    // 重名），远端组内取号，豁免分派在聚合器内
    QStringList titles;
    for (const auto &m : m_marks->bookmarks(playingListUuid(), curKey))
        titles << m.title;
    const QString title = tr("Bookmark %1").arg(Util::nextSequenceNumber(tr("Bookmark "), titles));
    // N54：来源 = 播放 tab 的列表身份 + 其队列根层 id 序列。此刻正播项必在播放 tab 的
    // 队列根层（凡 mpv 侧语义一律在队列根层上工作，N45 §2.4.2），所以"这条书签在哪一层"
    // 是现成且无歧义的，无需推断。
    // N57 §2.5（P3 修复，随守卫删除生效）：默认 tab 出生的书签**不再记空身份**——默认
    // 列表带恒定保留身份 kDefaultListUuid，于是这类书签不再被 removeInvalid 当数据异常
    // 清除，回跳按身份命中默认列表、换算到 tab 0、在其队列根层定位。"归属默认列表"由此
    // 是一个真实身份，既不在 uuid 命名空间里塞哨兵，也不另设显式编码位
    const int playingTab = m_playlist->playingTabIndex();
    BookmarkManager::Origin origin;
    if (const Playlist *playing = m_playlist->listOf(playingTab))
        origin = {playing->uuid(), m_playlist->queueRootIdsOf(playingTab)};
    m_bookmarkMgr->addBookmark(rawPath, t, title, origin);
    showOsd(tr("Bookmark added: [%1] %2").arg(Util::formatTimeHMSms(t), title));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // N91：窗口不随关闭钮消失（见下方 ignore 的理由），退出区间内它照常可点，故只认第一次
    // 关闭钮——重复点按不再落盘、不再重发 quit。置于小窗分支之前：退出区间内小窗态的点按
    // 也不该去跑 toggleMini（重挂与改尺寸正是要避开的动作）
    if (m_closing) {
        event->ignore();
        return;
    }
    // N68：小窗态关闭钮 = 退出小窗回普通模式，不是退出程序——ignore 并转调
    // toggleMini 退出分支（不落盘、不向 mpv 发 quit），立即 return，不得落进
    // 下方 saveSettings + quit 流程
    if (m_mini) {
        event->ignore();
        toggleMini();
        return;
    }
    m_closing = true;
    setWindowTitle(tr("Closing…")); // 退出区间的可见反馈（idleActive 与切条目的标题回刷已被挡）
    // N66 §二-2：全屏态先无条件收回原位再进入退出生序——把对象树恢复成既有形状
    // （N57 纪律：重挂改变子对象删除次序，不留到 ~MainWindow）。播放列表可见性
    // 属窗口态真相，saveSettings 读真相成员，不受重挂影响
    if (m_fullscreen) {
        setOverlayShown(Overlay::ControlBar, false);
        setOverlayShown(Overlay::Playlist, false);
        restoreOriginalParent(m_bottomBar);
        restoreOriginalParent(m_playlist);
    }
    saveSettings();
    // mpv 是宿主进程：关闭 GUI 时让 mpv 一并退出。命令走异步通道（GUI 线程不等核心），
    // 且此处 ignore 而非 accept：窗口须保持 mapped 到 vo 拆完，提前 unmap 会让 Mesa 的
    // swap_buffers 永远等不到 Present 事件，vo 线程卡死、核心拆解随之挂住。
    // 事件循环的出口只有 onMpvShutdown 收到 SHUTDOWN 那一路
    m_controller->commandAsync({QStringLiteral("quit")});
    event->ignore();
}

#include "pch.h"

#include "MainWindow.h"

#include "ActionManager.h"
#include "BookmarkManager.h"
#include "Constants.h"
#include "CustomToolManager.h"
#include "HttpClient.h"
#include "Logging.h"
#include "ServiceRegistry.h"
#include "TraversalPolicy.h"
#include "Util.h"
#include "Version.h" // N84：版本身份口（构造期填 App::setMpvVersion；本文件只许 include 此口，不得 include revision.h）
#include "mpvbridge/MpvController.h"
#include "mpvbridge/MpvKeyName.h"                    // N87：未认领按键的 mpv 键名翻译
#include "playbackreport/PlaybackReportDriver.h"     // N74：上报驱动器装配与退出序
#include "playbackreport/PlaybackReporterRegistry.h" // N74：报告者注册表 provide 与判据现查
#include "playbackreport/ReporterSwitchCatalog.h"    // N99：上报开关账目（登记与聚合单点）
#include "playlist/Playlist.h"         // N57 §2.6：经 PlaylistWidget::listOf 取身份，需完整类型
#include "playlist/PlaylistRegistry.h" // N57 §2.3：装配顶点创建列表注册表（默认名与保留身份的出处）
#include "playlist/PlaylistWidget.h"
#include "providers/ProviderCatalog.h" // N57 §2.3：后备提供者经登记表第 0 项出生，只此一处调用
#include "streams/StreamRelay.h"
#include "streams/StreamTransit.h" // N55 §2.7：书签存储串的转接形态分岔（判据与 §3.6 同一条）
#include "thumb/ThumbEngine.h"     // N107：缩略图第二 core 引擎
#include "ui/AboutDialog.h"
#include "ui/BookmarkEditorDialog.h"
#include "ui/ChapterSlider.h"
#include "ui/CustomToolListDialog.h"
#include "ui/FilterListWidget.h"
#include "ui/MpvBindingDialog.h" // N89：mpv 键位编辑（input.conf）
#include "ui/NavigatorPanel.h"
#include "ui/OpenUrlDialog.h" // N131
#include "ui/OsdController.h" // N113：三形态 OSD 控制器
#include "ui/SettingsDialog.h"
#include "ui/ShortcutsDialog.h" // N86
#include "ui/ThemedIconEngine.h"
#include "ui/ThumbWindow.h" // N107：缩略图宿主窗
#ifndef Q_OS_WIN
#include "platform/X11Util.h" // N83：小窗置顶 EWMH 直发（仅非 Windows 编译，见 CMakeLists）
#endif

namespace {

// ——文件级调档值——唯一使用点均在本文件，故不入 Constants.h（那里只放跨文件共享的
// 全局常量与默认清单）。集中置顶：调档只动这一处

// N107 悬停缩略图：待裁决 1 的实测调档就改这一族
// dwell：光标进滑条后停留此时长才建窗建核（快速掠过整条进度条根本不建）
constexpr int kHoverDwellMs = 200;
// 宽限：离开后保留此时长，期间回落即续、不重建；到期复核光标，仍在热区即自续
// 同一节拍（复核节拍即本值，不另立）
constexpr int kHoverGraceMs = 500;
// 会话期光标轮询间隔：热区的上下外扩段都不在任何带 tracking 的控件内，光标在那
// 两段移动不产生 MouseMove，seek 与重定位靠本节拍的轮询驱动
constexpr int kHoverPollMs = 30;
// 热区纵向：滑条上下各外扩的宽度（逻辑 px，两向同值——纵向正是用户找落点的那一度）
constexpr int kHoverBandPx = 8;
// 缩略图窗下沿与热区上沿的间隙（逻辑 px）：窗锚在带顶之上，热区因此不与窗矩形重叠，
// 光标不会停在"自己刚建出来的窗口"上
constexpr int kHoverGapPx = 2;
// 缩略图内接框边长（逻辑 px，固定一档）：等于 miniDefaultWidth() 的一半，不随主窗
// 尺寸与全屏切换变化
constexpr int kThumbBoxPx = 200;

// N110 画面比例内建档：比值档常量表——value 与 mpv 对同一写法的解析同源（C 双
// 精度直除，double 可精确比），勾选比对只认 double（读回串是归一化十进制，串
// 比较完全不可用，含"no"在内）。N123：档名与写入 token 都归 ActionManager，本表
// 只承担业务数值与身份，不复制文案。档间最小间距 |16/9 − 1.85| = 0.0722，勾选
// 容差 0.01 远小于其半，至多命中一档
struct AspectPreset {
    ActionManager::Id id;
    double value;
};
constexpr AspectPreset kAspectPresets[] = {
    {ActionManager::Id::Aspect4x3, 4.0 / 3.0},     {ActionManager::Id::Aspect16x9, 16.0 / 9.0},
    {ActionManager::Id::Aspect16x10, 16.0 / 10.0}, {ActionManager::Id::Aspect1_85, 1.85},
    {ActionManager::Id::Aspect2_35, 2.35},
};

// N123：窗口尺寸倍率档——Id 与倍率，供连接与启用态刷新；菜单文案与注释归登记表，
// 此处不复制 token
struct WindowScale {
    ActionManager::Id id;
    double factor;
};
constexpr WindowScale kWindowScales[] = {
    {ActionManager::Id::WindowSize0_25x, 0.25}, {ActionManager::Id::WindowSize0_5x, 0.5},
    {ActionManager::Id::WindowSize1x, 1.0},     {ActionManager::Id::WindowSize1_5x, 1.5},
    {ActionManager::Id::WindowSize2x, 2.0},
};

// N119：菜单描述表在 ui/MenuSpec.h（唯一来源）；本文件只做渲染与两个入口

// 进度条悬停 tooltip 合并节流：悬停 move 按鼠标事件率到达，而 tooltip 文本逐像素
// 就变（hh:mm:ss 只到秒，长片源 1px ≈ 数秒），Qt 的"文本未变即复用"形同虚设——
// 逐事件直发时每次都在重排 + 重定位顶层 tooltip 窗，实测悬停即吃满单核（注释掉
// showText 立即回落）。窗口内的多条 move 只按最后一条位置算一次
constexpr int kSeekTipCoalesceMs = 30;

} // namespace

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
// mpv 侧 cursor-autohide 不参与本行为：N119 起入口全平台显式置 no——核心侧的自动隐藏
// 不需要鼠标事件做前提就会向 VO 预推"藏"，留着它等于把本类搭在上游实现的偶然上。
// 光标行为单源于本类（N67 §二-1）
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

// N107：进度条悬停缩略图会话机（文件内类，PopupCloseWatch / CursorAutoHider 同款
// 口径；唯一调用方是 MainWindow，不另立文件）。
// 会话只由进度条控件的鼠标事件驱动——Enter / Leave / MouseMove 三个事件即会话的
// 全部输入，事件域就是控件自身，不往外装 tracking；过滤器只观察不消费，tooltip
// 与常规派发不受影响。dwell 到期才建窗建核（快速掠过整条进度条根本不建）；离开
// 不销毁，宽限到期复核光标：仍在热区内即自续同一节拍（不 seek 不重建），不在才
// 收会话。一切"在不在"都是 QCursor::pos() 与热区矩形的比对（N67 cursorInVideo /
// N66 右缘唤起同源）。热区以滑条 frameGeometry 实时推得（浮窗贴边跟随与重挂都不
// 影响），纵向上下各外扩同一宽度、与宿主的容器边距无关；缩略图窗锚在热区上沿之
// 上，故带顶到窗下沿恰为让开的间隙。会话期另有低频轮询兜事件盲区：热区的上下外
// 扩段都不在任何带 tracking 的控件内，光标在那两段移动不产生 MouseMove，seek 与
// 重定位由轮询驱动（滑条上的 MouseMove 仍即时响应）。
// 播放侧事实（源串、能力位、时长、视频比例、开关位）由 MainWindow 一次性下发；
// 引擎调用为跨线程队列投递，本类不做任何阻塞等待，sessionEnded 回执按会话代次
// 校验后才隐藏窗口（N103 销毁顺序：core 拆完 → 窗口隐藏；代次用于挡掉上一场
// core 的迟到回执误杀刚建好的新一场）。
class ThumbHoverWatch : public QObject {
  public:
    ThumbHoverWatch(ChapterSlider *slider, ThumbWindow *window, ThumbEngine *engine,
                    QWidget *mainWindow)
        : QObject(mainWindow), m_slider(slider), m_window(window), m_engine(engine),
          m_main(mainWindow) {
        m_slider->installEventFilter(this);
        m_dwellTimer = new QTimer(this);
        m_dwellTimer->setSingleShot(true);
        m_dwellTimer->setInterval(kHoverDwellMs);
        connect(m_dwellTimer, &QTimer::timeout, this, [this] {
            if (m_state != State::DwellPending)
                return;
            m_state = State::Idle;
            buildSession(); // 条件不成立即静默不建
        });
        m_graceTimer = new QTimer(this);
        m_graceTimer->setSingleShot(true);
        m_graceTimer->setInterval(kHoverGraceMs);
        connect(m_graceTimer, &QTimer::timeout, this, [this] {
            if (m_state != State::Active)
                return;
            if (hotBand().contains(QCursor::pos())) {
                m_graceTimer->start(); // 自续同一节拍：画面保持，不 seek 不重建
                return;
            }
            m_pollTimer->stop();
            m_state = State::Idle;
            QMetaObject::invokeMethod(
                m_engine, [e = m_engine] { e->endSession(); }, Qt::QueuedConnection);
        });
        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(kHoverPollMs);
        connect(m_pollTimer, &QTimer::timeout, this, [this] {
            if (m_state != State::Active)
                return;
            // 轮询兜事件盲区：热区的上下外扩段都不在任何带 tracking 的控件内，光标
            // 在那两段移动不产生 MouseMove，seek 与重定位只靠轮询驱动
            const QPoint cursor = QCursor::pos();
            // 滑条被隐藏（小窗态整条底部条 hide）：宿主窗不随之隐藏，而复核读到的
            // 是陈旧几何、判"仍在带内"会自续成永不销毁 —— 当场收
            if (!m_slider->isVisible()) {
                invalidateSession();
                return;
            }
            if (!hotBand().contains(cursor))
                return;
            const double t = m_slider->timeAtGlobalX(cursor.x());
            if (t >= 0.0) {
                QMetaObject::invokeMethod(
                    m_engine, [e = m_engine, t] { e->seekTo(t); }, Qt::QueuedConnection);
            }
            positionWindow(cursor);
        });
    }

    // ——播放侧事实下发（MainWindow 各通路调用；判据本身收在本类）——
    void setEnabled(bool on) {
        m_enabled = on;
        if (!on)
            invalidateSession(); // 关：当场收掉在飞会话；开：下一次悬停即生效
    }
    void setSource(const QString &path, bool allowed) {
        m_sourcePath = path;
        m_allowed = allowed;
    }
    void setDuration(double v) { m_duration = v; }
    void setVideoSize(int w, int h) {
        m_videoW = w;
        m_videoH = h;
    }
    // 换文件 / 停止 / 空闲：当场收掉在飞会话（dwell 一并取消）——旧条目的画面
    // 不跨文件残留；引擎调用幂等，重复调用安全
    void invalidateSession() {
        m_dwellTimer->stop();
        m_graceTimer->stop();
        m_pollTimer->stop();
        if (m_state != State::Idle) {
            m_state = State::Idle;
            QMetaObject::invokeMethod(
                m_engine, [e = m_engine] { e->endSession(); }, Qt::QueuedConnection);
        }
    }
    // 无事件的"已离开"（全屏控制带浮窗收起、滑条被隐藏或重挂、主窗挪位）：
    // dwell 取消；会话期起宽限复核——带随几何实时重算，光标若仍在带内自续
    void notifyLeft() {
        if (m_state == State::DwellPending) {
            m_dwellTimer->stop();
            m_state = State::Idle;
        } else if (m_state == State::Active) {
            m_graceTimer->start();
        }
    }
    bool isSessionActive() const { return m_state == State::Active; }
    // 引擎 sessionEnded 回执：core 已拆完，此刻才可隐藏窗口。回执带代次，只认
    // 当前在飞那一场——上一场 core 的迟到回执（拆解阻塞、投递无序）不得收掉刚
    // show 的新窗口，否则新场 core 会在已 unmap 的宿主窗上继续渲染（N103 纪律②）
    void handleSessionEnded(quint64 seq) {
        if (seq != m_sessionSeq)
            return;
        m_window->hide();
        m_pollTimer->stop();
        if (m_state == State::Active)
            m_state = State::Idle;
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched != m_slider)
            return QObject::eventFilter(watched, event);
        switch (event->type()) {
        case QEvent::Enter:
            OXBOW_LOG_DEBUG() << "thumb enter state=" << int(m_state);
            if (m_state == State::Active)
                m_graceTimer->stop(); // 回到滑条即继续
            else if (m_state == State::Idle) {
                m_state = State::DwellPending;
                m_dwellTimer->start(); // dwell 只在 Enter 那一刻起，不重复起
            }
            break;
        case QEvent::Leave:
            if (m_state == State::DwellPending) {
                m_dwellTimer->stop();
                m_state = State::Idle;
            } else if (m_state == State::Active) {
                m_graceTimer->start(); // 到期复核兜底，期间回落即取消
            }
            break;
        case QEvent::MouseMove:
            if (m_state == State::Active) {
                const QPoint cursor = QCursor::pos();
                const double t = m_slider->timeAtGlobalX(cursor.x());
                if (t >= 0.0) {
                    QMetaObject::invokeMethod(
                        m_engine, [e = m_engine, t] { e->seekTo(t); }, Qt::QueuedConnection);
                }
                positionWindow(cursor);
            }
            break;
        default:
            break;
        }
        return false;
    }

  private:
    enum class State { Idle, DwellPending, Active };

    // 热区：滑条全局矩形纵向上下各外扩 bandPx、横向不扩。上下对称是有意的——纵向
    // 正是用户找落点的那一度（§5），往上挪光标不该退出会话；锚点只从滑条自身几何
    // 推得，与宿主容器的边距无关
    QRect hotBand() const {
        const int band = kHoverBandPx;
        return QRect(m_slider->mapToGlobal(QPoint(0, 0)), m_slider->size())
            .adjusted(0, -band, 0, band);
    }

    void buildSession() {
        // 诊断日志（thumb）：一次悬停一行，列全部闸口取值，失败定位用
        OXBOW_LOG_DEBUG() << "thumb build"
                          << "enabled=" << m_enabled << "allowed=" << m_allowed
                          << "path=" << m_sourcePath << "dur=" << m_duration
                          << "slider=" << m_slider->isVisible() << m_slider->isEnabled();
        if (!m_enabled || !m_allowed || m_sourcePath.isEmpty() || m_duration <= 0.0)
            return;
        if (!m_slider->isVisible() || !m_slider->isEnabled())
            return; // 小窗 / 停止态滑条不可见不可用，悬停无从发生
        const QPoint cursor = QCursor::pos();
        if (!hotBand().contains(cursor))
            return; // dwell 期光标可能已离开
        const double start = m_slider->timeAtGlobalX(cursor.x());
        if (start < 0.0)
            return;
        const int box = kThumbBoxPx;
        QSize size(box, box);
        if (m_videoW > 0 && m_videoH > 0) {
            // 按片源宽高比内接、不留黑边；极宽片源缩成扁条是正确结果（N68 纪律同源）
            const double scale = qMin(double(box) / m_videoW, double(box) / m_videoH);
            size = QSize(qMax(1, qRound(m_videoW * scale)), qMax(1, qRound(m_videoH * scale)));
        }
        m_window->resize(size);
        positionWindow(cursor);
        m_window->show();  // show 必须早于 VO 开始渲染（N103 纪律①），引擎调用为
        m_window->raise(); // 队列投递，show 先于 loadfile 落地有保证
        // 引擎调用一律队列投递（引擎 affine 专属线程，mpv_command 的同步阻塞
        // 不得落 GUI 线程）；functor 形态免 metatype 注册。代次随会话递增，回执
        // 按号比对
        const quint64 seq = ++m_sessionSeq;
        QMetaObject::invokeMethod(
            m_engine,
            [e = m_engine, seq, wid = m_window->hostWid(), path = m_sourcePath, start] {
                e->startSession(seq, wid, path, start);
            },
            Qt::QueuedConnection);
        m_state = State::Active;
        m_pollTimer->start();
    }

    // 水平以光标为中心夹在主窗内；垂直锚在热区上沿之上——窗下沿 = 带顶 −
    // kHoverGapPx（让开的间隙，与 §5"热区不与窗口矩形重叠"同源）；frameless
    // 窗 move() 定位即外框左上角（N1⑥ 的 frameGeometry 语义在此等价）
    void positionWindow(const QPoint &cursor) {
        const QRect frame = m_main->frameGeometry();
        const int x = qBound(frame.left(), cursor.x() - m_window->width() / 2,
                             frame.right() - m_window->width() + 1);
        const int y = hotBand().top() - kHoverGapPx - m_window->height();
        if (m_window->geometry().topLeft() == QPoint(x, y))
            return; // 光标静止的轮询节拍不空转 SetWindowPos
        m_window->move(x, y);
    }

    ChapterSlider *m_slider;
    ThumbWindow *m_window;
    ThumbEngine *m_engine;
    QWidget *m_main;
    QTimer *m_dwellTimer = nullptr;
    QTimer *m_graceTimer = nullptr;
    QTimer *m_pollTimer = nullptr; // 会话期光标轮询（热区外扩段的事件盲区）
    State m_state = State::Idle;
    quint64 m_sessionSeq = 0; // 会话代次：引擎回执按号比对，0 = 从未建场
    bool m_enabled = true;    // INI 键 hoverThumbnail（restoreSettings / 运行期切换下发）
    bool m_allowed = false;   // 条目能力位（fileLoaded 一次性下发）
    QString m_sourcePath;     // 在播身份串（fileLoaded 一次性下发）
    double m_duration = 0.0;
    int m_videoW = 0;
    int m_videoH = 0;
};

MainWindow::MainWindow(MpvController *controller, AppearanceManager *appearance,
                       const QString &configFile, std::shared_ptr<StreamRelay> relay,
                       QWidget *parent)
    : QMainWindow(parent), m_controller(controller), m_configFile(configFile),
      m_relay(std::move(relay)), m_appearance(appearance) {
    setWindowTitle(App::name());

    // N102：mpv 版本一次性填充，About 对话框等消费者此后不依赖 MpvController
    App::setMpvVersion(m_controller->propertyString(QStringLiteral("mpv-version")));

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
    // N74：报告者注册表作为一个服务递送（显式注入、GUI 线程写 / 任意线程读、寿命
    // 随会话）。必须早于 PlaylistRegistry 与 PlaylistWidget 的构造——注册点在**列表
    // 绑定点**（Playlist 构造与换源两处，那里同时握着列表身份与提供者实例），句柄由
    // 本类经 PlaylistRegistry 下传给每个列表；取不到（测试自建 / 未装配）即不注册、
    // 不报错。提供者在构造期只造实例、不自注册（规格 §2）
    m_reporters = std::make_shared<PlaybackReporterRegistry>();
    m_registry->provide<PlaybackReporterRegistry>(m_reporters);
    // N74：驱动器对象——构造期出生、连好全部信号、不启动线程（按需启动见
    // maybeBootReportDriver；启动一次即不再销毁，进程生命周期存活、不 delete）。
    // 五条直连推送 AutoConnection：接收者已在工作线程 → queued 投递，与 GUI 侧同序；
    // 起播段那条经本类转发（要模型侧现算归属），另有两个 queued 调用由别处投
    // （setKeyEnabled / beginCollecting）。线程未起期间事件堆在其队列里，由折叠语义收敛
    m_reportDriver = new PlaybackReportDriver(m_controller, m_reporters);
    // 起播段经本类转发（不是直连）：来源列表身份只有模型知道——正播实体的归属，
    // 而播放串在直发形态下根本不带这个信息。连接顺序有讲究：本条必须早于下面
    // maybeBootReportDriver 那条，beginSegment 才先于 beginCollecting 入队
    connect(m_controller, &MpvController::fileLoaded, this, &MainWindow::forwardReportSegment);
    connect(m_controller, &MpvController::endFile, m_reportDriver,
            &PlaybackReportDriver::onEndFile);
    connect(m_controller, &MpvController::pauseChanged, m_reportDriver,
            &PlaybackReportDriver::onPauseChanged);
    connect(m_controller, &MpvController::idleActive, m_reportDriver,
            &PlaybackReportDriver::onIdleActive);
    connect(m_controller, &MpvController::mediaMetadataChanged, m_reportDriver,
            &PlaybackReportDriver::onMediaMetadataChanged);
    connect(m_controller, &MpvController::durationChanged, m_reportDriver,
            &PlaybackReportDriver::onDurationChanged);
    // 判据的唯一重查点（规格 §3）：起播即查，判据不成立则下一次起播自然补上
    connect(m_controller, &MpvController::fileLoaded, this, &MainWindow::maybeBootReportDriver);
    // 启动期 ini 初值经同一变更通路落表（queued；事件堆在 beginCollecting 之前，
    // 启动后按新表运作——陈旧表不得覆盖启动前的变更）
    syncReporterSwitches();
    // N57 §2.3：列表注册表同由装配顶点创建并持有——后备提供者在此经登记表第 0 项
    // 造好交入（"谁调提供者工厂"只此一处），注册表只收成品、不认识服务注册表。
    // N74：另把报告者注册表过一道手给每个列表（注册点在 Playlist 一侧，那里有
    // 列表身份；本行已在 provide<PlaybackReporterRegistry> 之后）
    m_listRegistry =
        std::make_shared<PlaylistRegistry>(createDefaultProvider(m_registry), m_reporters);

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
    // N97：视频区接受本地文件拖放（三类事件在同一 eventFilter 分支内受理与分派）
    m_videoWidget->setAcceptDrops(true);
    // N117：只作 Tab 落点（ClickFocus 会让点击置焦与 N5 清焦同轮竞争）；持焦与
    // 归空在键盘语义上等价，FocusIn 由本类 eventFilter 按 reason 自清。Tab 能否
    // 走到 TabFocus 单挂的控件由 styleHints()->tabFocusBehavior() 决定（Windows
    // 缺省 TabFocusAllControls，判据即 TabFocus；macOS 缺省只认 StrongFocus）
    m_videoWidget->setFocusPolicy(Qt::TabFocus);
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

    // N113：OSD 控制器——紧跟视频区装配就位。必须早于 ThumbWindow 创建（其内部
    // hostWid() 会触发主窗 QMoveEvent，moveEvent 里的 reposition 要求 m_osd 已
    // 存在）；这些调用点不做空指针容错，装配序就是防线，错序靠崩溃显影
    m_osd = new OsdController(this, m_videoWidget, m_controller);

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

    // N134
    m_toolMgr = new CustomToolManager(configFile, this);
    m_toolMgr->load();

    m_marks = new MarkAggregator(m_bookmarkMgr); // N15：标记聚合器

    // N107：缩略图装配——引擎跑专属线程（无 parent，随 finished 的 deleteLater 收尾，
    // 析构在引擎线程内执行使 core 拆解不落 GUI 线程）；窗口与会话机留 GUI 线程，
    // 窗口复用一套，hide/show 切换。relay 与主 core 同一实例（端点表不分份）
    m_thumbThread = new QThread(this);
    m_thumbThread->setObjectName(QStringLiteral("thumb-engine"));
    m_thumbEngine = new ThumbEngine(m_relay);
    m_thumbEngine->moveToThread(m_thumbThread);
    connect(m_thumbThread, &QThread::finished, m_thumbEngine, &QObject::deleteLater);
    m_thumbThread->start();
    m_thumbWindow = new ThumbWindow(this);
    m_thumbWatch = new ThumbHoverWatch(m_progressSlider, m_thumbWindow, m_thumbEngine, this);
    connect(m_thumbEngine, &ThumbEngine::sessionEnded, this,
            [this](quint64 seq) { m_thumbWatch->handleSessionEnded(seq); });

    setCentralWidget(m_splitter);
    // N119：连接先于装配——渲染只按描述表摆条目，不连任何信号
    connectActions();
    createMenuBar();
    connectController();

    // 导航器浮窗（N3 改按需创建）：冷启动不构造对象，首次打开经
    // ensureNavigator 创建；视口映射需要视频渲染区域的真实尺寸（黑边感知）

    // N10：启动固定默认尺寸；窗口几何/最大化状态不再持久化
    resize(960, 600);

    restoreSettings();
    updateChapterMenuState();
    updateChapterSlider();
}

MainWindow::~MainWindow() {
    // N107：N103 销毁顺序——先阻塞拆缩略图 core 再停引擎线程，子控件（宿主窗）
    // 随基类析构在其后销毁。常规退出已由 onMpvShutdown 收口（幂等），本处兜底
    if (m_thumbEngine)
        QMetaObject::invokeMethod(
            m_thumbEngine, [e = m_thumbEngine] { e->endSession(); }, Qt::BlockingQueuedConnection);
    if (m_thumbThread) {
        m_thumbThread->quit();
        m_thumbThread->wait();
    }
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
    // 悬停 tooltip 合并节流：move 只记最后位置，到期按该位置算一次
    // （逐事件直发的实测结论见 eventFilter 同名分支）
    m_seekTipTimer = new QTimer(this);
    m_seekTipTimer->setSingleShot(true);
    m_seekTipTimer->setInterval(kSeekTipCoalesceMs);
    connect(m_seekTipTimer, &QTimer::timeout, this, &MainWindow::flushSeekToolTip);
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
    m_muteAction = m_actions->action(ActionManager::Id::Mute);
    bindButton(m_muteButton, m_muteAction);
    applyMuteState(false); // 初值：未静音
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
    m_playButton = new QPushButton(QString());
    m_playButton->setToolTip(tr("Play/Pause"));
    m_playButton->setFixedSize(36, 36);
    m_stopButton = new QPushButton(QString());
    m_stopButton->setToolTip(tr("Stop"));
    m_stopButton->setFixedSize(36, 36);
    m_prevButton = new QPushButton(QString());
    m_prevButton->setToolTip(tr("Previous File"));
    m_prevButton->setFixedSize(36, 36);
    m_nextButton = new QPushButton(QString());
    m_nextButton->setToolTip(tr("Next File"));
    m_nextButton->setFixedSize(36, 36);
    m_timeLabel = new QLabel(QStringLiteral("00:00 / 00:00"));
    m_streamButton = new QPushButton(QString());
    m_streamButton->setToolTip(tr("Switch Stream"));
    m_streamButton->setFixedSize(36, 36);
    m_streamButton->hide();
    m_settingsButton = new QPushButton(QString());
    m_settingsButton->setToolTip(tr("Settings"));
    m_settingsButton->setFixedSize(36, 36);
    m_playlistToggle = new QPushButton(QString());
    m_playlistToggle->setToolTip(tr("Show/Hide Playlist"));
    m_playlistToggle->setFixedSize(36, 36);
    m_playlistToggle->setCheckable(true);
    // 控制行图标钉 24：样式默认取 16，而这批素材的细笔画只有画幅的 1/12——摊到 16px
    // 不足一整像素，覆盖率封顶在约 0.7（实测峰值 alpha 180/255），观感就是"发灰"；
    // 笔画摊到 2 整像素才实心。音量行的静音钮在 24 的容器里保持默认尺寸，不入本组
    QPushButton *controlIcons[] = {m_playButton,   m_stopButton,     m_prevButton,    m_nextButton,
                                   m_streamButton, m_settingsButton, m_playlistToggle};
    for (QPushButton *b : controlIcons)
        b->setIconSize(QSize(24, 24));
    retintIcons(); // 控制行各钮与静音钮的图标初值——与主题切换后的重染同一个出口

    // 控制行按钮与音量行静音按钮不接受焦点（C2）：Space 归全局快捷键，
    // 避免焦点按钮被 Space 二次触发（N38：静音按钮并入本组）
    QAbstractButton *noFocusButtons[] = {m_playButton,     m_stopButton,   m_prevButton,
                                         m_nextButton,     m_streamButton, m_settingsButton,
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
    controlLayout->addWidget(m_stopButton);
    controlLayout->addWidget(m_prevButton);
    controlLayout->addWidget(m_nextButton);
    controlLayout->addSpacing(8);
    controlLayout->addWidget(m_timeLabel);
    controlLayout->addStretch(1);
    controlLayout->addWidget(m_streamButton);
    controlLayout->addWidget(m_settingsButton);
    controlLayout->addWidget(m_playlistToggle);
    bottomLayout->addWidget(controlRow);
    layout->addWidget(m_bottomBar); // 左 pane 布局末项（N66 容器）

    // A15：全局按钮统一经 bindButton 引用主实例（事件来源 trigger + 启停跟随）
    bindButton(m_playButton, m_actions->action(ActionManager::Id::PlayPause));
    bindButton(m_stopButton, m_actions->action(ActionManager::Id::Stop));
    bindButton(m_prevButton, m_actions->action(ActionManager::Id::PrevFile));
    bindButton(m_nextButton, m_actions->action(ActionManager::Id::NextFile));
    bindButton(m_settingsButton, m_actions->action(ActionManager::Id::Settings));
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
        // 拖动 tooltip 与 OSD 同值同刻：挂 valueChanged 取的是滑条已消化的新值
        // （eventFilter 的 MouseMove 先于滑条处理事件，彼处读 value() 只能拿到
        // 上一次的值）；回流回写被 QSignalBlocker 拦截，不会从这里冒出 tooltip
        if (m_volumeSlider->isSliderDown())
            QToolTip::showText(QCursor::pos(), QStringLiteral("%1%").arg(value), m_volumeSlider);
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

// N119：动作连接唯一出生处，启动调一次——容器渲染永不连接，反复展开菜单因此不叠加
// 连接。结构归描述表与 createMenuBar，连接归本函数
void MainWindow::connectActions() {
    using AM = ActionManager;
    auto act = [this](AM::Id id) { return m_actions->action(id); };

    connect(act(AM::Id::Open), &QAction::triggered, this, &MainWindow::openFiles);
    connect(act(AM::Id::OpenUrl), &QAction::triggered, this, &MainWindow::openUrls);
    connect(act(AM::Id::Settings), &QAction::triggered, this, &MainWindow::ensureSettingsDialog);
    connect(act(AM::Id::Shortcuts), &QAction::triggered, this, &MainWindow::ensureShortcutsDialog);
    // N89：mpv 键位编辑器与 N86 并列相邻但完全独立——一个管 mpv 侧（input.conf），
    // 一个管插件侧（[shortcuts] 节）。登记在表里只为与其他菜单入口同处一处（默认不
    // 绑键、可改绑、禁用态处处生效），配置与键位仍互不共享
    connect(act(AM::Id::MpvKeyBindings), &QAction::triggered, this,
            &MainWindow::ensureMpvBindingDialog);
    // N134：工具列表编辑入口
    connect(act(AM::Id::CustomizeTools), &QAction::triggered, this, &MainWindow::customizeTools);
    // N78：截图组
    connect(act(AM::Id::ScreenshotCopy), &QAction::triggered, this,
            &MainWindow::copyScreenshotToClipboard);
    connect(act(AM::Id::ScreenshotSave), &QAction::triggered, this,
            &MainWindow::saveScreenshotToFile);
    connect(act(AM::Id::Quit), &QAction::triggered, this, &QWidget::close);

    connect(act(AM::Id::PlayPause), &QAction::triggered, this, &MainWindow::togglePause);
    connect(act(AM::Id::Stop), &QAction::triggered, this, &MainWindow::stop);
    connect(act(AM::Id::PrevFile), &QAction::triggered, this, &MainWindow::playlistPrev);
    connect(act(AM::Id::NextFile), &QAction::triggered, this, &MainWindow::playlistNext);
    // N5：音量步进（动作快捷键 ↑/↓ 全局生效，菜单为可见入口）
    connect(act(AM::Id::VolumeUp), &QAction::triggered, this,
            [this] { changeVolume(m_volumeKeyStep); });
    connect(act(AM::Id::VolumeDown), &QAction::triggered, this,
            [this] { changeVolume(-m_volumeKeyStep); });
    // N38：静音——命令取 mpv 实时属性读改写（勾选态只作显示，不参与判定）；
    // OSD 与 B14 音量同构：用户动作路径直显，不入 #1/#2/#3 合并队列
    m_muteAction = act(AM::Id::Mute);
    connect(m_muteAction, &QAction::triggered, this, [this] {
        const bool target = !m_controller->propertyFlag(QStringLiteral("mute"), false);
        m_controller->setProperty(QStringLiteral("mute"), target);
        m_osd->show(target ? tr("Mute: On") : tr("Mute: Off"));
    });
    // N6：速度控制组
    connect(act(AM::Id::SpeedUp), &QAction::triggered, this, [this] { changeSpeed(+0.25); });
    connect(act(AM::Id::SpeedDown), &QAction::triggered, this, [this] { changeSpeed(-0.25); });
    connect(act(AM::Id::SpeedReset), &QAction::triggered, this, &MainWindow::resetSpeed);
    // N15：章节与书签入口（菜单落点在章节段的动态填充里）
    connect(act(AM::Id::AddBookmark), &QAction::triggered, this, &MainWindow::onAddBookmark);
    connect(act(AM::Id::BookmarkEditor), &QAction::triggered, this,
            &MainWindow::ensureBookmarkEditor);

    m_playlistAction = act(AM::Id::Playlist);
    connect(m_playlistAction, &QAction::toggled, this, &MainWindow::setPlaylistVisible);

    m_navigatorAction = act(AM::Id::Navigator);
    connect(m_navigatorAction, &QAction::toggled, this, &MainWindow::setNavigatorVisible);

    // 全屏（N3）：F11/视图菜单/Esc/双击四路共用 toggleFullscreen 收口
    m_fullscreenAction = act(AM::Id::Fullscreen);
    connect(m_fullscreenAction, &QAction::toggled, this, [this](bool on) {
        if (on != m_fullscreen)
            toggleFullscreen();
    });
    // 小窗（N68）：I/视图菜单/Esc/关闭钮多路共用 toggleMini 收口；全屏与小窗互斥
    m_miniAction = act(AM::Id::MiniWindow);
    connect(m_miniAction, &QAction::toggled, this, [this](bool on) {
        if (on != m_mini)
            toggleMini();
    });
    // N119：菜单栏开关——勾选态即偏好，可见性收口见 applyMenuBarVisibility
    connect(act(AM::Id::MenuBar), &QAction::toggled, this, [this](bool on) {
        m_menuBarVisibleTruth = on;
        applyMenuBarVisibility();
    });
    // 状态信息叠加层：转发 script-message 给 mpv 侧 stats 脚本，插件不持有开关状态
    connect(act(AM::Id::ToggleStats), &QAction::triggered, this, [this] {
        m_controller->command({QStringLiteral("script-message-to"), QStringLiteral("stats"),
                               QStringLiteral("display-stats-toggle")});
    });
    // N123：画面比例七项——写入形态取该档声明的菜单文案（"4:3" 等即合法写入形态，
    // 原样交 mpv 归一化收账）；命中档的勾选态由下一次菜单装配按属性回填
    connect(act(AM::Id::AspectOriginal), &QAction::triggered, this, [this] {
        m_controller->setProperty(QStringLiteral("video-aspect-override"), QStringLiteral("no"));
    });
    for (const AspectPreset &p : kAspectPresets) {
        connect(act(p.id), &QAction::triggered, this, [this, p] {
            m_controller->setProperty(QStringLiteral("video-aspect-override"),
                                      m_actions->menuText(p.id));
        });
    }
    connect(act(AM::Id::AspectCustom), &QAction::triggered, this, &MainWindow::onCustomAspectRatio);
    // N123：五档窗口尺寸倍率（一次性命令；全屏 / 无视频的早退在 applyWindowScale 内）
    for (const WindowScale &s : kWindowScales) {
        connect(act(s.id), &QAction::triggered, this, [this, s] { applyWindowScale(s.factor); });
    }
    // 收尾：首个尺寸推送前一律禁用，避免启动瞬间误启用
    updateWindowScaleActions();

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

    // N102：About 由主窗自建、不经登记表——判据见 ActionManager.h 头注释（无改绑
    // 需求的入口不进键位表，免得在快捷键编辑器里白占一行）
    m_aboutAction = new QAction(tr("&About"), this);
    connect(m_aboutAction, &QAction::triggered, this, [this] { AboutDialog(this).exec(); });
}

// N119：菜单栏装配——六个顶级菜单是常驻实例（既有语义与 N85 的过滤都挂在实例上），
// 只换内容不重建容器；内容挂 aboutToShow，打开时所见即当前状态
void MainWindow::createMenuBar() {
    // N85：菜单栏按下清焦的过滤挂载——QMenuBar 自行消费按下事件，主窗
    // mousePressEvent 的冒泡兜底（N5）够不到它，点击菜单栏焦点会原地留在
    // 播放列表，方向键回不到全局音量模式。分支语义见 eventFilter
    menuBar()->installEventFilter(this);

    for (const MenuSpec::Group &group : MenuSpec::kGroups) {
        QMenu *menu = menuBar()->addMenu(tr(group.title));
        connect(menu, &QMenu::aboutToShow, this,
                [this, menu, group] { renderMenuGroup(*menu, group); });
    }
}

// N119：按描述表装配一个菜单段——先 clear 再逐条填，不连任何信号（动作的连接住
// connectActions，动态段的连接随条目现连、随 clear() 消亡）。两个渲染面同一份判定
void MainWindow::renderMenuGroup(QMenu &menu, const MenuSpec::Group &group) {
    menu.clear();
    renderMenuItems(menu, group.items);
}

void MainWindow::renderMenuItems(QMenu &menu, std::span<const MenuSpec::Item> items) {
    using Kind = MenuSpec::Item::Kind;
    for (const MenuSpec::Item &item : items) {
        if (item.hiddenIn(m_fullscreen, m_mini))
            continue;
        switch (item.kind) {
        case Kind::Action:
            menu.addAction(m_actions->action(item.action));
            break;
        case Kind::About:
            menu.addAction(m_aboutAction);
            break;
        case Kind::Separator:
            menu.addSeparator();
            break;
        case Kind::Filters:
            buildFilterSection(menu);
            break;
        case Kind::Tracks:
            buildTrackSection(menu);
            break;
        case Kind::Chapter:
            buildChapterSection(menu);
            break;
        case Kind::Aspect:
            buildAspectSection(menu);
            break;
        case Kind::Tools:
            buildToolSection(menu);
            break;
        case Kind::SubMenu:
            // N123：条目全为主实例，递归只建容器；子菜单与父菜单共用同一套裁剪与渲染
            renderMenuItems(*menu.addMenu(tr(item.title)), item.children);
            break;
        }
    }
}

// N119：视频区右键菜单入口——整棵现建、建完即渲染、exec 即毁，动作仍是共享主实例。
// 弹出前按 N5 清焦；弹层父指针挂 window()（pitfalls：transient parent 链）
void MainWindow::showVideoContextMenu(const QPoint &globalPos) {
    if (auto *fw = QApplication::focusWidget())
        fw->clearFocus();
    QMenu root(window());
    for (const MenuSpec::Group &group : MenuSpec::kGroups)
        renderMenuGroup(*root.addMenu(tr(group.title)), group);
    root.exec(globalPos);
}

// 进出覆盖态的四处调用点全走这里，不再盲 show / hide（隐藏偏好不被退出推翻）
void MainWindow::applyMenuBarVisibility() {
    menuBar()->setVisible(m_menuBarVisibleTruth && !m_fullscreen && !m_mini);
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
        applyPlayIcon(paused);
        // N7 #1：播放↔暂停 OSD（仅播放态互切，起停由 #2/#3 去重覆盖）。单击引发的
        // 回声同样入队，OSD 与属性回声同刻出现（N113）
        if (m_playlistPos >= 0)
            m_osd->queue(OsdController::Priority::PlayPause, paused ? tr("Pause") : tr("Play"));
    });
    connect(m_controller, &MpvController::volumeChanged, this, [this](double v) {
        QSignalBlocker blocker(m_volumeSlider);
        // N108：勾选曲线时回流反算为显示百分比——下发 f(P) 后收到同一 V，反算回
        // P（幂等），滑条恒显示等价百分比
        m_volumeSlider->setValue(qRound(m_quadraticVolumeCurve ? Util::Volume::mpvToMpc(v) : v));
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
    // pos 推送时刻按旧镜像取的错名会挂到下一次切条目——收编即刷新一次标题与 OSD
    connect(m_playlist, &PlaylistWidget::playingListAdopted, this, &MainWindow::updateNowPlayingUi);
    connect(m_controller, &MpvController::playlistPosChanged, this, [this](int pos) {
        const bool switchedItem = pos != m_playlistPos; // B9：实际换条目（值不变 mpv 不推送）
        m_playlistPos = pos;
        updateNavigationActions(); // 播放/停止态切换与位置变化（A16）
        // 流子菜单启用态派生自正播实体（N96），而实体正是随本信号落定的：控件状态必须
        // 随其派生输入一起刷新。只靠 endFile 那次调用会停在"实体尚未更新"的瞬间，
        // 停止态能否复位就全押在 mpv 各属性推送的先后上（无依据）
        updateStreamButton();
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
            applyPlayIcon(paused);
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
        updateStreamButton();
        m_marks->setChapters({});
        updateChapterSlider();
    });
    // 列表播完或停止后 mpv 进入空闲，复位播放按钮与进度条（C1）
    connect(m_controller, &MpvController::idleActive, this, [this](bool active) {
        if (active) {
            applyPlayIcon(true);
            QSignalBlocker blocker(m_progressSlider);
            m_progressSlider->setValue(0);
            // N8 Part5：停止态禁用进度条（与 C1 归零同点）
            m_progressSlider->setEnabled(false);
            // N8 Part1：无正在播放内容，标题恢复基名（N91：退出区间内不夺走"正在关闭"标题）
            if (!m_closing)
                setWindowTitle(App::name());
            // N7 #3：停止 OSD（单源去重，playlistPos -1 不触发，以 m_playlistPos>=0 为守卫）
            if (m_playlistPos >= 0)
                m_osd->queue(OsdController::Priority::Stop, tr("Stop"));
        }
    });

    // N67：播放态喂给光标自动隐藏（另起 connect，不并入既有 lambda——两机制互不纠缠）
    connect(m_controller, &MpvController::pauseChanged, m_cursorHider, &CursorAutoHider::setPaused);
    connect(m_controller, &MpvController::idleActive, m_cursorHider, &CursorAutoHider::setIdle);

    // N107：悬停缩略图的播放侧事实下发与会话收口（各通路独立 connect，不并入
    // 既有 lambda——判据与生命周期互不纠缠）
    connect(m_controller, &MpvController::fileLoaded, this, [this] {
        // N107 §4：装载完成查条目能力位并一次性下发，此后该文件生命周期内不再查询。
        // 行号与 tab 现读（镜像时序不可靠，与 forwardReportSegment 同款口径）
        const int pos = int(m_controller->propertyInt64(QStringLiteral("playlist-pos"), -1));
        const int tab = m_playlist->playingTabIndex();
        const PlaylistEntity::Ptr e = pos >= 0 ? m_playlist->playingEntityOf(tab, pos) : nullptr;
        m_thumbWatch->setSource(m_controller->currentPlayingPath(),
                                e && e->supportsSecondaryDecode());
    });
    connect(m_controller, &MpvController::durationChanged, this,
            [this](const QString &, double v) { m_thumbWatch->setDuration(v); });
    // N110：喂缩略图的是**片源像素尺寸**，不是信号载荷的有效尺寸——第二 core
    // 呈现未拉伸的像素帧（N110 §四-3.1），故经源尺寸出口现读再喂
    connect(m_controller, &MpvController::videoSizeChanged, this, [this](int, int) {
        const QSize src = m_controller->sourceVideoSize();
        m_thumbWatch->setVideoSize(src.width(), src.height());
    });
    connect(m_controller, &MpvController::endFile, this,
            [this](int) { m_thumbWatch->invalidateSession(); });
    connect(m_controller, &MpvController::idleActive, this, [this](bool active) {
        if (active)
            m_thumbWatch->invalidateSession();
    });

    connect(m_playlist, &PlaylistWidget::playRequested, this, &MainWindow::onPlayRequested);
    // N54：书签层级定位的两种结局——播放决策三档与失败策略都在本类（widget 只切层
    // 与定位，不知道 mpv 现状，也不报错）
    connect(m_playlist, &PlaylistWidget::levelItemReady, this, &MainWindow::onLevelItemReady);
    connect(m_playlist, &PlaylistWidget::levelItemFailed, this, &MainWindow::onLevelItemFailed);
    connect(m_playlist, &PlaylistWidget::filesAdded, this, &MainWindow::onFilesAdded);
    connect(m_playlist, &PlaylistWidget::removeRequested, this, &MainWindow::onRemoveRequested);
    connect(m_playlist, &PlaylistWidget::clearRequested, this, &MainWindow::onClearRequested);
    // N104：失败显影的标与清——按串口收在 PlaylistWidget（unwrap 在口内做），
    // 发射方在事件线程，AutoConnection 落 queued 到 GUI 线程
    connect(m_controller, &MpvController::playFailed, m_playlist, &PlaylistWidget::markPlayFailed);
    connect(m_controller, &MpvController::fileLoaded, m_playlist, &PlaylistWidget::clearPlayFailed);
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
                updateStreamButton();
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
    // 合并两属性突发"的适配缓冲已随之删除，此处直连触发。
    // N121：on_preloaded 的预估值提前走同一入口；预估值由 GUI 线程直连发射，
    // hook 等它完成才放行，权威值随后照常校正
    connect(m_controller, &MpvController::videoSizeChanged, this, &MainWindow::applyVideoSize);
    connect(m_controller, &MpvController::preloadVideoSizeHint, this, &MainWindow::applyVideoSize,
            Qt::DirectConnection);
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

// N115：style / colorScheme 两键的运行期读取点（初始 apply 在插件入口完成）
void MainWindow::applyAppearanceFromIni() {
    if (m_configFile.isEmpty())
        return;
    QSettings settings(m_configFile, QSettings::IniFormat);
    m_appearance->apply(
        settings.value(QStringLiteral("style"), App::interfaceStyleDefault()).toString(),
        settings.value(QStringLiteral("colorScheme"), App::colorSchemeDefault()).toString());
}

// N56：设置对话框 ini 键变更入口（自 PlaylistWidget 上移）——命中相关键时重读
// ini 覆盖注册表内部实例，下一次求值即生效
void MainWindow::onPluginIniKeysChanged(const QStringList &keys) {
    if (keys.contains(QStringLiteral("remoteTimeoutSec")) ||
        keys.contains(QStringLiteral("ignoreHttpsErrors")))
        provideHttpServices();
    if (keys.contains(QStringLiteral("recursiveDepthLimit")))
        provideTraversalPolicy();
    // 上报开关不再逐键枚举（N99）：账目行命中的键在同步通路内过滤，新增行零改动
    syncReporterSwitches(keys);
    // 音量族这两项都只读 ini：配置文件为空串即整段跳过（与同函数邻居
    // provideHttpServices / provideTraversalPolicy 同一守卫口径）
    if (!m_configFile.isEmpty()) {
        QSettings settings(m_configFile, QSettings::IniFormat);
        // 滚轮 / 方向键音量步长（N5）改动即时生效：变更后重读，下一次步进即按新值
        if (keys.contains(QStringLiteral("volumeWheelStep")) ||
            keys.contains(QStringLiteral("volumeKeyStep"))) {
            m_volumeWheelStep =
                settings.value(QStringLiteral("volumeWheelStep"), App::volumeWheelStepDefault())
                    .toInt();
            m_volumeKeyStep =
                settings.value(QStringLiteral("volumeKeyStep"), App::volumeKeyStepDefault())
                    .toInt();
        }
        // N108：曲线开关——信号只带确有变化的键，仍先读值比对（读不到即保持当前态，
        // 零副作用）；套用后重算量程（越界由 setRange 钳制，那一格经 applyVolume 播报
        // 钳后值），再以钳后的当前显示值按新曲线直发（不经 applyVolume——曲线开关自身
        // 不播 OSD、不弹提示）
        if (keys.contains(QStringLiteral("quadraticVolumeCurve"))) {
            const bool on = settings
                                .value(QStringLiteral("quadraticVolumeCurve"),
                                       App::quadraticVolumeCurveDefault())
                                .toBool();
            if (on != m_quadraticVolumeCurve) {
                m_quadraticVolumeCurve = on;
                updateVolumeSliderRange();
                pushVolume(m_volumeSlider->value());
            }
        }
        // N107：悬停缩略图开关运行期切换——关：会话机当场收掉在飞会话并停定时器；
        // 开：下一次悬停即生效，无须重启。开关不参与主播放路径的任何判断
        if (keys.contains(QStringLiteral("hoverThumbnail"))) {
            const bool on =
                settings.value(QStringLiteral("hoverThumbnail"), App::hoverThumbnailDefault())
                    .toBool();
            if (on != m_hoverThumbnailEnabled) {
                m_hoverThumbnailEnabled = on;
                m_thumbWatch->setEnabled(on);
            }
        }
        // N113：OSD 形态运行期切换——同值 setMode 内部即返（零副作用）；离开当前
        // 形态的清除由控制器自管，立即生效、无须重启
        if (keys.contains(QStringLiteral("osdMode"))) {
            m_osd->setMode(OsdController::modeFromIniValue(
                settings.value(QStringLiteral("osdMode"), App::osdModeDefault()).toString()));
        }
        // N115：样式 / 配色即时生效（与启动期同一 helper；分支不设门禁——低版本
        // 触发后配色步不在编，无副作用）
        if (keys.contains(QStringLiteral("style")) || keys.contains(QStringLiteral("colorScheme")))
            applyAppearanceFromIni();
    }
}

// N99：上报开关的变更通路与启动期初值共用——遍历 ReporterSwitchCatalog（登记、读值与
// 聚合的单点），命中 changedKeys 的行（空表即全读，启动期那一路）后 queued 改驱动器
// 启用表、切闸门。这条通路一律不判断据、不起线程——判据只在起播那一处重查
// （maybeBootReportDriver）；线程未起时这次调用只是把表改掉，启动后按新表运作。
// queued 投递走 functor 形态：改名 / 改签名交给编译器，不落字符串查找
void MainWindow::syncReporterSwitches(const QStringList &changedKeys) {
    const bool filter = !changedKeys.isEmpty();
    const QHash<QString, bool> values = readReporterSwitchValues(m_configFile);
    for (const ReporterSwitch &row : reporterSwitches()) {
        if (filter && !changedKeys.contains(QLatin1String(row.iniKey)))
            continue; // 本次变更未命中该行：表值不变，不重投
        const QString key = QLatin1String(row.reporterKey);
        const bool enabled = values.value(key);
        QMetaObject::invokeMethod(
            m_reportDriver, [d = m_reportDriver, key, enabled] { d->setKeyEnabled(key, enabled); },
            Qt::QueuedConnection);
    }
}

// N74：起播段的转发。投递路由要的"在播哪一册"是模型知识——正播实体的归属（收编
// 进默认 tab 的项交**原列表**身份，那才是加工这条播放形态、持凭据、在册报告者所在的
// 一册；与书签那种"承诺归属 = 正播 tab"不同物）。不从播放串反推：直发形态（不经转接）
// 的串里没有列表身份，靠串就等于只对转接条目成立。行号取 playlist-pos **现读**而非
// 缓存的 m_playlistPos——后者随 playlistPosChanged 的 queued 推送更新，与该信号同源
// 的 FILE_LOADED 先后无文档保证，取晚了会认错条目归属（认错即串账，正是本路由要防的）。
// 归属解析的判据（实体键级判等防"缓存载荷 + 现读行号"错配、非默认 tab 回落、默认 tab
// 不猜）收在 PlaybackReporterRegistry::resolveSourceUuid——与 claimsSource 同族的纯
// 谓词，随注册表进测试库（N99），本函数只剩取数与投递
void MainWindow::forwardReportSegment(const QString &path) {
    const QString identity = StreamTransit::unwrap(path);
    const int pos = int(m_controller->propertyInt64(QStringLiteral("playlist-pos"), -1));
    const int tab = m_playlist->playingTabIndex();
    const PlaylistEntity::Ptr entity = pos >= 0 ? m_playlist->playingEntityOf(tab, pos) : nullptr;
    const Playlist *playing = m_playlist->listOf(tab);
    const QUuid listUuid = PlaybackReporterRegistry::resolveSourceUuid(
        entity, identity, playing ? playing->uuid() : QUuid());
    // functor 形态投递（N99）：改名 / 改签名交给编译器，不落字符串查找；队列语义
    // 与入队次序不变
    QMetaObject::invokeMethod(
        m_reportDriver, [d = m_reportDriver, path, listUuid] { d->beginSegment(path, listUuid); },
        Qt::QueuedConnection);
}

// N74：线程启动判据的唯一重查点。判据 = 注册表里有在册报告者 且 账目聚合判"有任一
// 启用"（ReporterSwitchCatalog::anyEnabled，与驱动器 keyEnabled 同口径——未登记的
// 键按启用处理，"判据判全关而驱动器按启用投"的分裂从规则单点消除）；两条件齐且闸门
// 仍是 Unbooted 才起线程——只看键会在人人常驻一条线程（绝大多数用户没配任何 Jellyfin
// 列表），只看注册表则配置被关的用户也白常驻。起线程后 queued 投一次 beginCollecting，
// 折叠快照当场收敛成"此刻在播的这一条"，Start 即刻发出。
// 两条已知且刻意保留的形状：
// ① 判据读的是工作线程置位的闸门，beginCollecting 又是 queued 的——工作线程还没
//    处理第一条时连续两次起播，两次都读到 Unbooted、各调一次 start()。不另设本类
//    私有的"已起旗"：Qt 对 QThread::start() 明写"线程已在运行即无操作"，重复的
//    beginCollecting 亦自带闸门守卫，两处都落在无害；加一枚旗只是把一个已被文档
//    担保的幂等再担保一遍。
// ② hasEntry 廉价（无 I/O）排在 ini 现读之前：为假的多数用户连 ini 都不读。判据
//    现读 ini、不设缓存成员（只在起播前执行几次，少一份状态少一处漂移）；读法经
//    readReporterSwitchValues 与变更通路同源（ini 不可用即整表落各行 defaultValue）
void MainWindow::maybeBootReportDriver() {
    if (m_reportDriver->gate() != PlaybackReportDriver::Gate::Unbooted)
        return; // 已启动（Running / Gated / Dead）：启动一次即不再销毁
    if (!m_reporters->hasEntry())
        return; // 无在册报告者：无损失可谈（服务器续播点靠 Progress 与超时收尾）
    const QHash<QString, bool> values = readReporterSwitchValues(m_configFile);
    std::vector<QString> registeredKeys; // 未经任何过滤的全部在册键（anyEnabled 的契约）
    for (const auto &entry : m_reporters->snapshot())
        registeredKeys.push_back(entry.key);
    if (!anyEnabled(values, registeredKeys))
        return;
    // 启动协议（start + queued 投 beginCollecting）收进驱动器（N99）：两步必须同在，
    // 如今同一个方法保证；m_thread 自此不出本类
    m_reportDriver->boot();
}

void MainWindow::openFiles() {
    // N9：过滤器模式由全局媒体扩展名清单生成（与 DirProvider 默认值同源）
    const QString patterns = Util::extensionPatterns(App::defaultMediaExtensions()).join(u' ');
    QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Open File"), QString(), tr("Media files (%1);;All files (*)").arg(patterns));
    if (files.isEmpty())
        return; // 对话框取消
    // Qt 来源：拿到即转原生分隔符（B15），逐文件加工
    for (QString &f : files)
        f = Util::Url::toNative(f);
    playFilesFromPaths(files);
}

// N131：与 openFiles 共用尾部收口，差别只在来源——条目可以是本地路径也可以是任意
// 链接 / 协议，故不按后缀过滤（输入的 .m3u、edl:// 之类要原样喂进去）
void MainWindow::openUrls() {
    OpenUrlDialog dlg(tr("Open URL"), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    playFilesFromPaths(dlg.entries());
}

// N97：对话框入口与视频区拖放共用的播放收口（原 openFiles 尾部四步）。
// 集合为空一步都不进：切 tab 的两步排在 onPlayRequested 之前，不先判空会"什么都没播"
// 却把前台 tab 切到默认列表
void MainWindow::playFilesFromPaths(const QStringList &files) {
    if (files.isEmpty())
        return;
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
        m_osd->show(tr("No screenshot available"));
    return img;
}

void MainWindow::copyScreenshotToClipboard() {
    const QImage img = grabScreenshotOrNotify();
    if (img.isNull())
        return;
    QApplication::clipboard()->setImage(img); // 与 FilterListWidget / PlaylistWidget 同源
    m_osd->show(tr("Screenshot copied to clipboard"));
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
        OXBOW_LOG_DEBUG() << "screenshot write failed:" << writer.errorString() << path;
        m_osd->show(tr("Failed to save screenshot"));
        return;
    }
    m_osd->show(tr("Screenshot saved"));
}

void MainWindow::playAt(int index) {
    m_pendingSeekRow = -1; // 手动播放丢弃未执行的挂起 seek，防陈旧跳转
    m_controller->setProperty(QStringLiteral("playlist-pos"), index);
    m_controller->setProperty(QStringLiteral("pause"), false);
    // 若 pause 原本就是 no，mpv 不会再推送属性变化，图标需手动更新
    applyPlayIcon(false);
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
    applyPlayIcon(true);
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
        m_osd->show(
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
        // N119 §3.1：全屏期同一开关只写真相，不碰 splitter 与窗口几何；勾上不立即显示，
        // 等右缘触发。取消须无条件立即收起——重评的保活判据 cursorOnPlaylist 会让光标
        // 停在面板上时不收，"取消 = 两态都关"就此落空。按钮与动作是两条独立勾选链
        // （bindButton 只同步 enabled），按真相各回写一次（A12）
        m_playlistVisibleTruth = visible;
        if (!visible)
            setOverlayShown(Overlay::Playlist, false);
        {
            QSignalBlocker toggleBlocker(m_playlistToggle);
            m_playlistToggle->setChecked(visible);
        }
        {
            QSignalBlocker actionBlocker(m_playlistAction);
            m_playlistAction->setChecked(visible);
        }
        reevaluateOverlays();
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

void MainWindow::moveEvent(QMoveEvent *event) {
    QMainWindow::moveEvent(event);
    repositionNavigator();
    m_osd->reposition();
    repositionFullscreenOverlays(); // N66：全屏期两条带贴边跟随
    if (m_thumbWatch)
        m_thumbWatch->notifyLeft(); // N107 §2：主窗挪位使热区挪位，无事件按"已离开"起复核
#if !defined(Q_OS_WIN) || defined(OXBOW_PUREQT)
    if (m_mini)
        scheduleMiniRefit(/*positionAroused=*/true); // 降级路径：位置变化算外部唤醒
#endif
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    repositionNavigator();
    m_osd->reposition();
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
    // N107 §2：主窗最小化同属"没有事件的状态改变"——缩略图宿主窗是主窗的 owned
    // 窗，随 owner 一起隐掉，而带内复核读到的仍是旧几何、判"在"即自续，会话与
    // core 会挂在已 unmap 的窗口上永不销毁（N103 纪律②），故当场收
    if (event->type() == QEvent::WindowStateChange && (windowState() & Qt::WindowMinimized) &&
        m_thumbWatch)
        m_thumbWatch->invalidateSession();
    // N116：外观事件走重染，不走下面的窗口态分支（以最后到达的 PaletteChange 收口）
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        retintIcons();
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
    m_osd->reposition();
    repositionFullscreenOverlays(); // N66：两条带几何与视频区对位同源（§三-3 挂载点并列）
}

// ---- N66：全屏浮窗控制栏与播放列表 ----

// §三-2：两个宿主浮窗按需创建（同 ensureNavigator / OsdController::ensureOverlay 的生命周期：
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
    if (what == m_bottomBar && m_thumbWatch)
        m_thumbWatch->notifyLeft(); // N107 §2：滑条随带重挂，无事件按"已离开"处理
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
    if (what == m_bottomBar && m_thumbWatch)
        m_thumbWatch->notifyLeft(); // N107 §2：滑条随带重挂，无事件按"已离开"处理
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
    // N107 §2：控制带浮窗收起即无事件的"已离开"——当场起宽限复核收掉缩略图
    if (!shown && which == Overlay::ControlBar && m_thumbWatch)
        m_thumbWatch->notifyLeft();
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
    const bool holdControl = m_progressSlider->isSliderDown() || m_volumeSlider->isSliderDown() ||
                             popup || m_fsResizing ||
                             (m_thumbWatch && m_thumbWatch->isSessionActive()); // N107 §5②
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
        !controlTarget &&
        ((inRight && m_playlistVisibleTruth) || cursorOnPlaylist); // N119 §3.1：右缘门控读同一真相
    if (!holdControl)
        setOverlayShown(Overlay::ControlBar, controlTarget);
    if (!holdPlaylist)
        setOverlayShown(Overlay::Playlist, playlistTarget);
}

// §三-3：两条带几何——控制栏贴底满宽高 h；播放列表贴右满高宽 m_fsPlaylistWidth
// （钳制在面板下限与客户区宽 50%，§二-8）。挂载点与 OSD 叠加窗重挂（m_osd->reposition）完全并列。
// 顶级浮窗 setGeometry 取全局屏幕坐标（OsdController::reposition 先 mapToGlobal 同口径），
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

// N119：比例动态段——无 QActionGroup 成员，勾选按 video-aspect-override 现读回填。
// 只认 double：读回的归一化十进制串不可用于比较（§三-3）；容差 0.01 内算同档（档间
// 最小间距 0.0722，容差远小于其半，至多命中一档）。不中档 → 全不勾、Custom 缀当前值；
// -1 / 0 废弃哨兵 → 全不勾、Custom 不带值。
// N123：条目改引用 ActionManager 主实例——不再当场新建、不再逐次连接（连接住
// connectActions，反复展开菜单不叠加），文案与注释的唯一来源随之前移到登记表
void MainWindow::buildAspectSection(QMenu &parent) {
    using AM = ActionManager;
    QMenu *aspectMenu = parent.addMenu(tr("Aspect Ratio"));
    // 主实例指针先取再挂：QMenu::addAction(QAction *) 无返回值
    QAction *original = m_actions->action(AM::Id::AspectOriginal);
    aspectMenu->addAction(original);
    for (const AspectPreset &p : kAspectPresets)
        aspectMenu->addAction(m_actions->action(p.id));
    QAction *custom = m_actions->action(AM::Id::AspectCustom);
    aspectMenu->addAction(custom);
    // 连接住 connectActions（N123）：装配只引用主实例，反复展开菜单不再叠加连接

    // 勾选回填（原 updateAspectCheckedStates 主体平移）
    const double v = m_controller->propertyDouble(QStringLiteral("video-aspect-override"), -2.0);
    QAction *checked = nullptr;
    QString customText;
    if (v == -2.0) {
        checked = original; // "no" 的哨兵，读回恒 -2（§三-3）
    } else if (v > 0.0) {
        for (const AspectPreset &p : kAspectPresets) {
            if (qAbs(v - p.value) <= 0.01) {
                checked = m_actions->action(p.id);
                break;
            }
        }
        // 基串取 menuText：displayText 返回的是注释，action()->text() 可能已是带值形态
        if (!checked)
            customText = m_actions->menuText(AM::Id::AspectCustom) + QStringLiteral(" (") +
                         QString::number(v) + QStringLiteral(")");
    }
    // v == -1.0 / 0.0 落到此处：全部不勾、自定义条目不带值
    original->setChecked(checked == original);
    for (const AspectPreset &p : kAspectPresets) {
        QAction *a = m_actions->action(p.id);
        a->setChecked(a == checked);
    }
    // 自定义档恒不勾：不中档时只把当前值缀在条目上（N110 口径），命中判据里没有它
    custom->setChecked(false);
    custom->setText(customText.isEmpty() ? m_actions->menuText(AM::Id::AspectCustom) : customText);
}

// N110：自定义画面比例输入（自定义档条目触发）。循环到合法或取消：非法 → 警告
// 一行说明合法写法，随即重弹同一输入框并预填刚才的输入；取消 / 空输入不改原值，
// 上一次的有效值保留。受理写法（trim 后）：w:h（两段各自为正的有限 double，分母
// > 0）与正 double，且须是紧凑串（内部含空白一律判非法，见函数体内注释）；统一
// 校验 0 < 比值 ≤ 10——上限与 mpv 数值域一致（§三-3 实测 10 受理、10.0001 拒收），
// 放行更大值会被 mpv 静默拒收成"校验通过、画面纹丝不动"的死区，10 同时挡住 e 记法
// 误输。校验通过写入 trim 后原样（不改写法、不归一化成 double），mpv 归一化收账，
// 写法不影响状态与勾选
void MainWindow::onCustomAspectRatio() {
    // 预填：当前值 > 0 交其十进制串；哨兵态（-2 / -1 / 0）为空
    const double v = m_controller->propertyDouble(QStringLiteral("video-aspect-override"), -2.0);
    QString prefill = v > 0.0 ? QString::number(v) : QString();
    while (true) {
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("Custom Aspect Ratio"),
                                                   tr("Aspect ratio (e.g. 16:9, 2.35:1 or 1.85):"),
                                                   QLineEdit::Normal, prefill, &ok);
        if (!ok || text.trimmed().isEmpty())
            return; // 取消 / 空输入：不改原值
        const QString trimmed = text.trimmed();
        bool valid = false;
        double ratio = 0.0;
        const qsizetype colon = trimmed.indexOf(QLatin1Char(':'));
        if (colon > 0) {
            bool wOk = false, hOk = false;
            const double rw = trimmed.left(colon).toDouble(&wOk);
            const double rh = trimmed.mid(colon + 1).toDouble(&hOk);
            valid = wOk && hOk && qIsFinite(rw) && qIsFinite(rh) && rw > 0.0 && rh > 0.0;
            if (valid)
                ratio = rw / rh;
        } else {
            ratio = trimmed.toDouble(&valid);
            valid = valid && qIsFinite(ratio);
        }
        // 紧凑性把关：QString::toDouble 自带吞首尾空白，"16 : 9" 这类含内部空白的写法
        // 两段各自解析得通过，随后又原样写进 mpv——mpv 对它的宽容度未实测，一旦不容
        // 就正好落进上面要避开的"插件受理、mpv 静默拒收、画面纹丝不动"死区。故写值前
        // 先判非法（不动写入形态，只把受理面收到紧凑串）
        const bool compact =
            !std::any_of(trimmed.cbegin(), trimmed.cend(), [](QChar c) { return c.isSpace(); });
        if (compact && valid && ratio > 0.0 && ratio <= 10.0) {
            m_controller->setProperty(QStringLiteral("video-aspect-override"), trimmed);
            return;
        }
        QMessageBox::warning(this, tr("Invalid aspect ratio"),
                             tr("Enter a positive ratio up to 10, as w:h or a decimal "
                                "(e.g. 16:9 or 2.35)."));
        prefill = text; // 刚才的输入原样带回输入框
    }
}

// N121：尺寸事实的唯一 GUI 入口——权威 videoSizeChanged（事件线程排队）与
// on_preloaded 预估值（GUI 线程直连）共用。预估值可能略偏，权威值随后校正；
// 两路都只写既有状态机，不引入第二套适配语义
void MainWindow::applyVideoSize(int videoW, int videoH) {
    m_pendingVideoW = videoW;
    m_pendingVideoH = videoH;
    // N123：倍率启用态随尺寸事实刷新（先于形态分支早退，小窗 / 全屏同样成立）
    updateWindowScaleActions();
    if (m_mini) {
        // N68：小窗期换片就地适配（与全屏"退出再适配"不同——小窗本身就该跟着
        // 视频走，无须 refit 标志）。停止进 idle 与换片至无流的 (0,0) 推送不动作：
        // 维持小窗形态与当前尺寸不变（尺寸稳定性优先，进入时的 16:9 仅针对无流进场）
        if (videoW > 0 && videoH > 0)
            QTimer::singleShot(0, this, [this] { refitMiniToVideo(); });
        return;
    }
    if (m_fullscreen && videoW > 0 && videoH > 0)
        m_refitAfterFullscreen = true;
    else if (isMaximized() && videoW > 0 && videoH > 0)
        m_refitAfterMaximize = true;        // N40 变更：还原最大化时重适配（跟随全屏逻辑）
    adaptWindowSizeToVideo(videoW, videoH); // (0,0) 与全屏/最大化在入口内守卫，无动作
}

// N122：普通态视频区尺寸收口，N10 自适应与倍率命令共用；want 为目标视频区逻辑尺寸，
// 调用方须保证普通态且各维 > 0。低于 videoDefaultSize 按两维覆盖系数的较大者等比放大
// 到恰好覆盖（N114，保比无黑边），超出工作区视频域整窗等比缩回——上限须同时扣外框
// 装饰与客户区非视频域，只扣外框会撑破工作区；B16 迭代收敛与最小移动钳回沿用 N10
void MainWindow::resizeVideoAreaTo(const QSize &wantLogical) {
    const QRect work = screen()->availableGeometry();
    QSize want = wantLogical;
    const QSize videoMin = App::videoDefaultSize();
    if (want.width() < videoMin.width() || want.height() < videoMin.height()) {
        const double s = qMax(double(videoMin.width()) / want.width(),
                              double(videoMin.height()) / want.height());
        want = QSize(qRound(want.width() * s), qRound(want.height() * s));
    }
    const QSize frameChrome = frameGeometry().size() - geometry().size();
    const QSize clientReserve = geometry().size() - m_videoWidget->size();
    const QSize availVideo = work.size() - frameChrome - clientReserve;
    if (availVideo.width() > 0 && availVideo.height() > 0 &&
        (want.width() > availVideo.width() || want.height() > availVideo.height()))
        want.scale(availVideo, Qt::KeepAspectRatio);
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
    const QRect fr = frameGeometry();
    const QPoint tl = minimalMoveTopLeft(fr, work);
    if (tl != fr.topLeft())
        move(tl);
}

// N10：普通态按当前视频自适应——视频域物理像素 1:1（逻辑尺寸 = 像素 ÷ DPR，高 DPI 下
// 不被系统缩放拉伸）；全屏 / 最大化 / 小窗 / 无视频流不动作，入参为 N110 有效尺寸
void MainWindow::adaptWindowSizeToVideo(int videoW, int videoH) {
    // m_mini 守卫（N68）：小窗几何单源在 toggleMini 与换片自适应，尺寸推送不得把它重排
    if (videoW <= 0 || videoH <= 0 || m_fullscreen || m_mini || isMaximized())
        return;
    const qreal dpr = devicePixelRatioF();
    resizeVideoAreaTo(QSize(qRound(videoW / dpr), qRound(videoH / dpr)));
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

// N123：五档倍率的启用态只看"有没有有效视频"——快捷键与两个菜单渲染面因此读到同一
// 状态，不再依赖菜单是否展开。全屏态由描述表裁掉条目，命令入口的全屏 / 无视频早退
// 另行守卫（启用态不表达窗口形态）
void MainWindow::updateWindowScaleActions() {
    const bool hasVideo = m_pendingVideoW > 0 && m_pendingVideoH > 0;
    for (const WindowScale &s : kWindowScales)
        m_actions->action(s.id)->setEnabled(hasVideo);
}

// N122：窗口尺寸倍率命令——以视频有效尺寸为基准的一次性定态尺寸，不记忆、不置勾选；
// 无有效视频尺寸沿用 N10 早退。最大化先清 N40 还原重适配待办再 showNormal（否则还原
// 落地的重适配按 1x 盖掉本命令），尺寸排到还原几何落地后的下一轮事件循环
void MainWindow::applyWindowScale(double factor) {
    if (m_fullscreen || m_pendingVideoW <= 0 || m_pendingVideoH <= 0)
        return;
    if (m_mini) {
        applyMiniWindowScale(factor);
        return;
    }
    const qreal dpr = devicePixelRatioF();
    const QSize want(qRound(m_pendingVideoW * factor / dpr),
                     qRound(m_pendingVideoH * factor / dpr));
    if (isMaximized()) {
        m_refitAfterMaximize = false;
        showNormal();
        QTimer::singleShot(0, this, [this, want] { resizeVideoAreaTo(want); });
        return;
    }
    resizeVideoAreaTo(want);
}

// N122：倍率的小窗路径——客户区即视频区，宽按有效宽乘倍率、低于 miniMinWidth 覆盖放大
// 到恰好覆盖（不套 N114 普通态下限，否则 0.25x 被抬回大窗尺寸），高恒由宽反推保比，
// 故下限只需钳宽。工作区上限优先于下限：紧维钳上限、另维反推（同 WM_SIZING 手法）。
// 不写 m_preMiniRect，退出小窗按进窗前快照恢复
void MainWindow::applyMiniWindowScale(double factor) {
    const qreal dpr = devicePixelRatioF();
    const double ratio = double(m_pendingVideoW) / double(m_pendingVideoH);
    int clientW = qRound(m_pendingVideoW * factor / dpr);
    if (clientW < App::miniMinWidth())
        clientW = App::miniMinWidth();
    int clientH = qRound(clientW / ratio);
    const QRect work = screen()->availableGeometry();
    const QSize chrome = frameGeometry().size() - geometry().size();
    const QSize availClient = work.size() - chrome;
    if (availClient.width() > 0 && availClient.height() > 0 &&
        (clientW > availClient.width() || clientH > availClient.height())) {
        const double sW = double(availClient.width()) / clientW;
        const double sH = double(availClient.height()) / clientH;
        if (sW <= sH) { // 宽是紧维：钳宽反推高
            clientW = availClient.width();
            clientH = qRound(clientW / ratio);
        } else { // 高是紧维：钳高反推宽
            clientH = availClient.height();
            clientW = qRound(clientH * ratio);
        }
    }
    resize(clientW, clientH);
    clampMiniIntoWorkArea();
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
        m_fullscreen = true;
        applyMenuBarVisibility();       // 幂等复算（菜单栏已在上方按原位先藏）
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
        applyMenuBarVisibility(); // 按偏好恢复——隐藏偏好不得被退出全屏推翻
        syncVideoGeometry();
        // N66 §二-2 ④：主窗几何落定后按窗口态恢复面板可见性与宽度。锚点用进全屏前
        // 记下的左 pane 宽度直调（挂回后现读必被新默认分配污染，A20 坑 2）；调用时机
        // 必须在 flags / setGeometry / show() 之后（该函数读当前外框与所在屏，全屏
        // 矩形上算出的目标宽度是错的）。最大化分支经既有 isMaximized 守卫早退，
        // 与 A20 窗口态语义一致
        m_playlist->setVisible(m_playlistVisibleTruth);
        if (m_playlistVisibleTruth)
            applyPlaylistGeometry(true, m_preFsLeftWidth);
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
        // N117：激活指派晚于本分支的显示序列，兜底会把焦点落进已重新可见的播放
        // 列表——预置视频区为顶层 focus_child 抢在指派前，FocusIn 处自清归 null。
        // 让位期跳过，维持 N90 的 null 基线（N95 组字不受扰）
        if (!m_inputYielded)
            m_videoWidget->setFocus(Qt::OtherFocusReason);
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
#ifdef Q_OS_WIN
    // N137：吞掉键盘来源的 appcommand——不截则落 DefWindowProc 被系统转给同进程 mpv
    // core 的 SMTC，与伴随 keydown 经回落注入的 keypress 各执行一次。判据与 Qt 同源
    // （QTBUG-62838）：键盘来源且非 BROWSER_HOME 的命令必有 keydown，键事件仍走既有
    // 回落单源；鼠标 / OEM 来源无 keydown 伴随，不落本判据。主窗不在前台时消息不达
    // 本窗，媒体键仍由 SMTC 响应；OXBOW_PUREQT 不改媒体键处理，门禁只用 Q_OS_WIN
    if (const MSG *m = static_cast<MSG *>(message);
        m->message == WM_APPCOMMAND && GET_DEVICE_LPARAM(m->lParam) == FAPPCOMMAND_KEY &&
        GET_APPCOMMAND_LPARAM(m->lParam) != APPCOMMAND_BROWSER_HOME) {
        *result = TRUE;
        return true;
    }
#endif
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
        // QWidget::updateGeometry 只失效直接父布局：占位符到主窗布局隔着左 pane 与
        // splitter，须逐层补失效，主窗布局才会重算最小尺寸。播放列表此刻若已隐藏，
        // hide() 无事件可发，这条链无人打通，主窗最小宽会停在普通态的 480，把下面的
        // 400 宽整块钳走（实测成窗 480）
        for (QWidget *w = m_videoPlaceholder; w && w != this; w = w->parentWidget())
            w->updateGeometry();
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
        applyMenuBarVisibility(); // 幂等复算（上方已先藏，布局冲刷依赖其不含菜单栏）
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
        applyMenuBarVisibility(); // 按偏好恢复——隐藏偏好不得被退出小窗推翻
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
        // N117：焦点复位同 toggleFullscreen 退出分支（预置 + FocusIn 自清归 null）
        if (!m_inputYielded)
            m_videoWidget->setFocus(Qt::OtherFocusReason);
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
    // N68/N113：小窗进出同一收口喂 OSD 控制器——抑制期消息照收照丢、不补显
    m_osd->setSuppressed(m_mini);
}

// 单击定时器超时：确非双击，执行 press 记下的动作意图（N113：press 不碰 mpv）。
// InTrackToggle：仅当此刻仍在播放条目上下文才互切——窗口期内文件播完或被停止的，
// "暂停"意图已失效，什么都不做（否则会走停止态开播路径意外重新开播）；窗口期内
// pause 被别的路径翻转的，照旧再切一次（与空格 / 按钮的互切同形，不为此加守卫）。
// StoppedStart：停止态开播；窗口期内已被起播则等价"再切一次"
void MainWindow::onVideoClickTimeout() {
    if (m_clickAction == ClickAction::None)
        return;
    const ClickAction action = m_clickAction;
    m_clickAction = ClickAction::None;
    switch (action) {
    case ClickAction::InTrackToggle:
        if (m_playlistPos >= 0)
            togglePause();
        break;
    case ClickAction::StoppedStart:
        togglePause();
        break;
    case ClickAction::None:
        break;
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

// 占位符几何跟随 + 视频区单击 / 双击分流（N3）+ 右键菜单（N119）。
// Release 不处理；右键走 ContextMenu 分支，不进这套按压 / 双击计时。
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
        // N97：视频区拖放三类。一律消费（return true）——Qt 的父链传播看的是
        // isAccepted()，不受理又不消费，事件就沿控件链上溯到主窗
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
            return handleVideoDropEvent(static_cast<QDropEvent *>(event));
        case QEvent::Wheel:
            // N5：播放区滚轮调音量（上滚增大）
            changeVolume(static_cast<QWheelEvent *>(event)->angleDelta().y() > 0
                             ? m_volumeWheelStep
                             : -m_volumeWheelStep);
            return true;
        case QEvent::ContextMenu: {
            // N119：右键抬起时由平台层投递本事件，不必自判左右键；清焦与弹层收口在
            // showVideoContextMenu。键盘唤出不在本方案（非目标），命中即放行
            auto *ce = static_cast<QContextMenuEvent *>(event);
            if (ce->reason() == QContextMenuEvent::Keyboard)
                break;
            showVideoContextMenu(ce->globalPos());
            return true;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() != Qt::LeftButton)
                break; // 右键不进这套按压 / 双击计时（菜单归 ContextMenu 分支）
            // N5：点击画面即离开列表焦点，方向键回到全局音量模式
            if (auto *fw = QApplication::focusWidget())
                fw->clearFocus();
            if (event->type() == QEvent::MouseButtonDblClick) {
                m_clickTimer->stop();
                m_clickAction = ClickAction::None;
                toggleFullscreen();
                break;
            }
            // "定时器未活跃"守卫：双击间隔内带位移的第二 press 不重启流程
            if (m_clickTimer->isActive())
                break;
            // 入口无条件复位动作意图：状态只在单轮 press→(双击|超时)
            // 生命周期内存活，杜绝上轮残留泄漏进本轮。
            // press 不碰 mpv（N113）：意图按缓存 m_playlistPos 记账（零 mpv 调用），
            // 动作一律等满双击间隔由超时收口
            m_clickAction =
                m_playlistPos < 0 ? ClickAction::StoppedStart : ClickAction::InTrackToggle;
            m_clickTimer->start(QApplication::doubleClickInterval());
            break;
        }
        case QEvent::FocusIn: {
            // N117：程序性置焦当场自清归 null；Tab / Backtab 是真焦点导航，放行
            const auto reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason != Qt::TabFocusReason && reason != Qt::BacktabFocusReason)
                m_videoWidget->clearFocus();
            break;
        }
        default:
            break;
        }
    } else if (watched == m_progressSlider) {
        // N8 Part3：tooltip 跟随鼠标显示所指进度；拖动期持续刷新防自动隐藏。
        // 停止态控件已禁用收不到鼠标事件，天然不显示。
        // 逐事件直发已被实测否定：MouseMove 按鼠标事件率到达，而 tooltip 文本逐像素
        // 就变（hh:mm:ss 只到秒，长片源 1px ≈ 数秒），Qt 的"文本未变即复用"形同虚设
        // ——每个事件都演变成一次顶层 tooltip 窗的重排 + 重定位 + 暴露区重绘，悬停即
        // 吃满单核（注释掉 showText 立即回落）。故 move 只记最后位置，由单次定时器
        // 合并后算一次；ToolTip 事件稀有，直通同一出口
        switch (event->type()) {
        case QEvent::ToolTip: {
            m_seekTipGlobal = static_cast<QHelpEvent *>(event)->globalPos();
            flushSeekToolTip();
            break;
        }
        case QEvent::MouseMove: {
            m_seekTipGlobal = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
            if (!m_seekTipTimer->isActive())
                m_seekTipTimer->start(); // 合并窗口：期间只更新位置，到期算一次
            break;
        }
        case QEvent::Leave:
            // 拖动中移出不隐藏（保持常显），仅悬停离开时收起；挂起的那次一并作废
            m_seekTipTimer->stop();
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
        case QEvent::MouseButtonRelease:
        case QEvent::Leave:
            // 拖动 tooltip 挂在 valueChanged 上（见音量 valueChanged 处注释），此处只管收起
            QToolTip::hideText();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// 进度条悬停 tooltip 的唯一出口：合并节流的落点（eventFilter 只记最后位置）。
// 判据与逐事件路径一字不差——停止态 / 时长未知 / 换算失败一律不显示
void MainWindow::flushSeekToolTip() {
    if (m_playlistPos < 0 || m_duration <= 0.0)
        return;
    // N107：悬停时刻换算全仓一份算法——tooltip 与缩略图共用滑条成员，同值同刻
    // （滑条时长镜像由 updateChapterSlider 随 durationChanged 同批刷）
    const double hoverTime = m_progressSlider->timeAtGlobalX(m_seekTipGlobal.x());
    if (hoverTime < 0.0)
        return;
    QString tip;
    // N77 §2.3：命中查询收（正播身份，key），豁免分派在聚合器内
    const auto cur = playingEntity();
    if (const auto hit = m_marks->hit(hoverTime, playingListUuid(), cur ? cur->key() : QString());
        hit && !hit->title.isEmpty())
        tip = hit->title + QStringLiteral("\n");
    tip += Util::formatTimePercent(hoverTime, m_duration);
    QToolTip::showText(m_seekTipGlobal, tip, m_progressSlider);
}

// N97：视频区拖放。受理判据只有一条——`mimeData()` 有 urls 或有纯文本即受理（N132 起
// 与播放列表同形）：不看后缀、不看有无正播、不枚举目录，"这批放下会发生什么"一律推迟到
// 落下那一刻算一次。三类事件一律消费（Qt 的父链传播以 `isAccepted()` 为准，不受理又不
// 消费，事件会沿控件链上溯到主窗）。`DragLeave` 不属本函数处理面：它携的是
// `QDragLeaveEvent`，不属 `QDropEvent` 家族，取 `position()` / `mimeData()` 即读非法偏移
bool MainWindow::handleVideoDropEvent(QDropEvent *event) {
    const QMimeData *mime = event->mimeData();
    if (!mime->hasUrls() && !mime->hasText())
        return true; // 不受理，也照常消费
    if (event->type() != QEvent::Drop) {
        // 拖动经过（DragEnter / DragMove）：只受理让光标显示可放，不触碰文件系统、
        // 不往返 mpv
        event->acceptProposedAction();
        return true;
    }
    // N132/N133：来源 → 已定形串表（本地与链接一视同仁，纯文本只收已定形串，
    // 转接形态与未定形串一并丢）
    const QStringList entries = Util::Url::entriesFromDrop(*mime);
    if (entries.isEmpty())
        return true;
    const QStringList mediaExts = App::defaultMediaExtensions();
    // 分派门：恰一条且是本地文件才试字幕——混入任何链接即整体走播放集合
    if (entries.size() == 1 && !Util::Url::isNetworkStream(entries.first())) {
        const QFileInfo fi(entries.first());
        if (fi.isFile()) {
            const QString suffix = fi.suffix().toLower();
            // 字幕判据与"添加字幕"对话框同一张表（每次现读 mpv），且有正播才成立；
            // 不区分在播的是本地文件还是远程流。先判正播，停止态不付那次属性往返
            if (playingEntity() && trackExtensions(true).contains(suffix)) {
                applySubtitleFile(entries.first(), false);
                event->acceptProposedAction(); // 确实执行了动作才受理
                return true;
            }
            if (mediaExts.contains(suffix)) {
                playFilesFromPaths(entries);
                event->acceptProposedAction();
                return true;
            }
            return true; // 两者都不是 → 静默忽略（落下后无事发生，不预示不报错）
        }
    }
    // 播放集合：按原序逐条处理，目录在其原位展开一层，网络/流形态直接收下
    QStringList files;
    for (const QString &entry : entries) {
        if (Util::Url::isNetworkStream(entry)) {
            files << entry;
            continue;
        }
        const QFileInfo fi(entry);
        if (fi.isFile()) {
            if (mediaExts.contains(fi.suffix().toLower()))
                files << entry;
        } else if (fi.isDir()) {
            for (const QFileInfo &found : Util::listFiles(entry, mediaExts))
                files << Util::Url::toNative(found.absoluteFilePath()); // 入口定形（B15）
        }
    }
    if (files.isEmpty())
        return true; // 集合空 → 落下后无事发生（列表、正播、前台 tab 都不动）
    playFilesFromPaths(files);
    event->acceptProposedAction();
    return true;
}

// B14：用户音量动作统一收口——钳制、下发 mpv 并直显音量 OSD。
// 三条路径汇入：changeVolume（滚轮/快捷键）、滑条 valueChanged（拖动/点击）。
// 启动恢复刻意绕行：restoreSettings 以 QSignalBlocker 直写滑条并直接 pushVolume，
// 不经此函数——启动路径从此无任何 OSD 调用点
// N108：钳制上界取滑条量程上界（勾选曲线时它大于 m_volumeMax，沿用旧界会把量程
// 顶端钳没），先在显示域钳制、再交 pushVolume 换算下发；OSD 直显钳制后的显示
// 百分比。关态下量程上界即 m_volumeMax、换算恒等
void MainWindow::applyVolume(int value) {
    const int p = qBound(0, value, m_volumeSlider->maximum());
    pushVolume(p);
    m_osd->show(tr("Volume: %1%").arg(p));
}

// N108：显示百分比 → mpv volume 的唯一换算点（用户动作、曲线切换与启动恢复共用）。
// 不含 OSD：用户动作路径由 applyVolume 追加，曲线切换与启动恢复刻意不播
void MainWindow::pushVolume(int displayValue) {
    m_controller->setProperty(QStringLiteral("volume"), m_quadraticVolumeCurve
                                                            ? Util::Volume::mpcToMpv(displayValue)
                                                            : double(displayValue));
}

// N5：统一音量调节入口。以滑条缓存为基准步进后交 applyVolume；
// UI 不在此处回写，由 volumeChanged 观察回流刷新（防环已有 QSignalBlocker）
void MainWindow::changeVolume(int delta) { applyVolume(m_volumeSlider->value() + delta); }

// 播放 / 暂停图标的唯一赋值点（paused 为真出"播放"三角）；现态记入 m_playIconPaused
// 供 retintIcons 重灌
void MainWindow::applyPlayIcon(bool paused) {
    m_playIconPaused = paused;
    m_playButton->setIcon(Ui::themedIcon(App::iconPath(paused ? App::iconPlay() : App::iconPause()),
                                         palette().color(QPalette::WindowText)));
}

// 控制行与静音 / 开关 / 齿轮图标按当前调色板重灌——图标内容不随调色板自绘，故初值与
// 外观事件共用这一出口，幂等。开关钮 On 态取 HighlightedText：勾选态底色是样式的
// Highlight，图标取其配套前景。首行守卫兜住控件树建成前到达的样式事件
void MainWindow::retintIcons() {
    if (!m_playButton)
        return;
    const QColor tint = palette().color(QPalette::WindowText);
    applyPlayIcon(m_playIconPaused);
    m_stopButton->setIcon(Ui::themedIcon(App::iconPath(App::iconStop()), tint));
    m_prevButton->setIcon(Ui::themedIcon(App::iconPath(App::iconSkipBackward()), tint));
    m_nextButton->setIcon(Ui::themedIcon(App::iconPath(App::iconSkipForward()), tint));
    m_streamButton->setIcon(Ui::themedIcon(App::iconPath(App::iconStream()), tint));
    m_settingsButton->setIcon(Ui::themedIcon(App::iconPath(App::iconSettings()), tint));
    m_playlistToggle->setIcon(Ui::themedIcon(App::iconPath(App::iconPlaylist()), tint,
                                             palette().color(QPalette::HighlightedText)));
    applyMuteState(m_muteAction->isChecked());
}

// N38：静音状态联动单一出口——菜单勾选与图标按钮同一处刷新，两者共用同一
// QAction 实例故天然同源。勾选态经 QSignalBlocker 直写，不回触发；本函数不播报
// OSD（OSD 由用户动作路径直显，与 B14 音量同构），故启动与恢复期均无 OSD 调用点
void MainWindow::applyMuteState(bool muted) {
    QSignalBlocker blocker(m_muteAction);
    m_muteAction->setChecked(muted);
    m_muteButton->setIcon(
        Ui::themedIcon(App::iconPath(muted ? App::iconVolumeMuted() : App::iconVolume()),
                       palette().color(QPalette::WindowText)));
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
        // N8 Part1：标题跟随正播项显示名；多项列表另带当前序号，与 OSD #2 同源
        setWindowTitle(
            total > 1 && m_playlistPos < total
                ? QStringLiteral("[%1/%2] %3").arg(m_playlistPos + 1).arg(total).arg(name)
                : name);
    else
        name = tr("Unknown File");
    m_osd->queue(OsdController::Priority::NowPlaying,
                 QStringLiteral("[%1/%2] %3").arg(m_playlistPos + 1).arg(total).arg(name));
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
        m_osd->show(tr("Seek: %1 / %2").arg(sign, Util::formatTimePercent(target, m_duration)));
    } else {
        const QString sign =
            deltaSeconds >= 0 ? tr("+%1s").arg(deltaSeconds) : tr("%1s").arg(deltaSeconds);
        m_osd->show(tr("Seek: %1").arg(sign));
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
    m_osd->show(tr("Speed: %1x").arg(num));
}

void MainWindow::resetSpeed() {
    m_controller->setProperty(QStringLiteral("speed"), 1.0);
    m_osd->show(tr("Speed: 1x"));
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
// 只在将来给主窗开了焦点策略时成立，留作余量。N117 起列表侧之外视频区也可接焦（链上首个
// Tab 落点，让位期激活兜底会短暂落它再自清），计入判据。eventFilter 的 windowHandle 分支与
// event() 的 ShortcutOverride 覆写共用；焦点在播放列表侧控件时判据不成立，键盘交回常规派发
bool MainWindow::yieldOwnsKeyboard() const {
    QWidget *fw = QApplication::focusWidget();
    return m_inputYielded && (fw == nullptr || fw == this || fw == m_videoWidget);
}

// N90：让位转发的单点——取名非空即异步注入 keypress，eventFilter 的截断分支与
// keyPressEvent 的兜底出口共用（全仓只允许这一种转发形态）。取名落空即静默返回，
// 吞还是放由调用方决定（截断分支吞、兜底出口放）；只转按下，KeyRelease 不经此
void MainWindow::forwardKeyToMpv(const QKeyEvent *event) {
    // N95：组字期间静默——组字串由 IME 消费，转发会把无关键名夹带进 mpv。Windows
    // 上组字期本无键事件（QWindowsInputContext 不发），此守卫兜底 Esc / Enter 一类
    // 仍以键事件到达者
    if (m_imeComposing)
        return;
    const QString keyName = MpvKey::nameFromEvent(event);
    if (!keyName.isEmpty())
        m_controller->commandAsync({QStringLiteral("keypress"), keyName});
}

// N95：文本转发单点——提交串按 UCS-4 码点拆开逐个注入 keypress（多字符串会被当
// 键名解析失败报 "is not a valid input name"；按码点不按 UTF-16 迭代，代理对拆成
// 孤立 code unit 会各自变成非法字符）。码点 < 32 不发：mpv 的 MP_KEY_IS_UNICODE
// 不认，且这些本就有键名（Tab / ENTER）。仅让位期生效——console 已关时迟到的提交
// 字符不下发，否则字符落成全局键名误触 mpv 绑定
void MainWindow::forwardTextToMpv(const QString &text) {
    if (!m_inputYielded)
        return;
    const auto codePoints = text.toUcs4();
    for (char32_t cp : codePoints) {
        if (cp < 32)
            continue;
        m_controller->commandAsync({QStringLiteral("keypress"), QString::fromUcs4(&cp, 1)});
    }
}

// N95：让位期输入法承接——组字串维护守卫标志，提交串经文本转发单点逐码点下发。
// console 开启期间主窗置 WA_InputMethodEnabled（onConsoleOpenChanged），事件经
// focusObject（焦点归空即主窗本体）到达此处。标志只在让位期置真：非让位期收到
// 输入法事件（主窗焦点策略将来若有）不得让它卡住键名转发
void MainWindow::inputMethodEvent(QInputMethodEvent *event) {
    m_imeComposing = m_inputYielded && !event->preeditString().isEmpty();
    forwardTextToMpv(event->commitString());
}

// N95：候选窗 / 组字窗位置的单一应答点——写死视频区左上角，跟随 mpv（其 IME 弹出
// 窗总是窗口左上角），不做几何推导、不猜 console 输入行位置。查询矩形语义是 focus
// object 自身局部坐标：落点主窗即主窗局部坐标，m_videoWidget 是主窗直接子件、
// geometry() 即主窗坐标，直接可用（视频区将来挪进容器层须改 mapTo 主窗）。
// 平台侧拿到矩形后还要按 inputItemTransform 映射到窗口坐标，而控件层只在 FocusIn /
// Move / Resize 维护该变换：让位期焦点落回子控件再归空，它仍停在最后一个焦点控件的
// 偏移上，候选窗随之偏到"视频区左上角 + 旧偏移"（实测偶发跑到窗口右上）。故先按逆
// 变换折一次，平台映射回来即恒等于视频区左上角——与变换当下漂在哪一值无关。
// 除这两项外一律回落基类
QVariant MainWindow::inputMethodQuery(Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImCursorRectangle: {
        const QTransform tf = QGuiApplication::inputMethod()->inputItemTransform();
        const QPoint wanted = m_videoWidget->geometry().topLeft();
        const QPointF p = tf.isInvertible() ? tf.inverted().map(wanted) : QPointF(wanted);
        return QRect(p.toPoint(), QSize(1, fontMetrics().height()));
    }
    // 实测：属性已置真时基类这一项仍答成 false，而 §3 的口径是这一项决定平台侧是否
    // 关联输入法——按属性如实答，开关时机才由本方案说了算
    case Qt::ImEnabled:
        return testAttribute(Qt::WA_InputMethodEnabled);
    default:
        return QMainWindow::inputMethodQuery(query);
    }
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
    for (int i = 0; i < targets.size(); ++i) {
        // N104：命令被拒的条目不进 mpv 队列、没有它的 end-file，就地按指针对回
        // 实体标红（targets 与 entities 同序同长；null 实体在口内跳过）
        if (m_controller->loadFile(targets[i].url, QStringLiteral("append"), targets[i].options) !=
            0)
            m_playlist->markPlayRejected(tab, entities.at(i).get());
    }
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
    applyPlayIcon(keepPaused);
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
    // N104：标红定位的 tab 与 targets 同源一次取值，循环内不重取（其间无事件循环介入）
    const int tab = m_playlist->playingTabIndex();
    const QVector<PlayTarget> targets = m_playlist->playTargets(tab, entities);
    if (targets.isEmpty()) // N60 §2.4：空清单早退先于开批（不投屏障、不留空会话）
        return;
    const PlaylistWidget::PushSession session(m_playlist); // N60 §2.4：逐条 append-play 后关批
    for (int i = 0; i < targets.size(); ++i) {
        // N104：命令被拒就地标红（append-play 的被拒项同样没有 end-file）
        if (m_controller->loadFile(targets[i].url, QStringLiteral("append-play"),
                                   targets[i].options) != 0)
            m_playlist->markPlayRejected(tab, entities.at(i).get());
    }
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
        // N95：让位期启用输入法（mpv console 打开时置 input-ime 的插件侧对形）。
        // 运行期翻转属性平台侧不自动感知，须 update(ImEnabled) 触发重新关联——
        // 不通知要等下一次焦点变动才生效
        setAttribute(Qt::WA_InputMethodEnabled, true);
        QGuiApplication::inputMethod()->update(Qt::ImEnabled);
        // N95 偏离：候选窗 / 组字窗落在哪儿由 inputItemTransform（输入项局部坐标 →
        // 窗口坐标的变换）与本方案答的固定矩形共同决定，控件层只在 FocusIn / Move /
        // Resize 维护该变换。让位期只有 clearFocus、无 FocusIn，它可能停在让位前焦点
        // 控件的偏移上；而此刻 focusObject 即主窗本体，窗口坐标就是主窗局部坐标，
        // 恒等才是正确变换——启用时主动复位，变换有变化即触发 cursorRectangleChanged，
        // 令平台侧按新矩形重摆
        QGuiApplication::inputMethod()->setInputItemTransform(QTransform());
    } else {
        detachYieldFilter();
        // N95：解除输入法。update(ImEnabled) 与开臂对称（缺了要等下一次焦点变动才
        // 解除）；reset() 撤掉未提交的组字，对应 mpv 关闭时恢复 input-ime 原值——
        // 迟到的提交字符另由文本出口的让位门挡住；组字标志一并清零
        setAttribute(Qt::WA_InputMethodEnabled, false);
        QGuiApplication::inputMethod()->update(Qt::ImEnabled);
        QGuiApplication::inputMethod()->reset();
        m_imeComposing = false;
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
    updateVolumeSliderRange();
}

// N108：滑条量程重算单点（applyVolumeMax 推送与曲线开关切换共用）。勾选曲线时
// 顶端取 mpv 上限的等价百分比（Util::Volume::mpvToMpc），使 P↔V 在整条量程上
// 双向一一对应；关态即 m_volumeMax。setRange 自带钳制：缩量程时越界的当前值
// 被钳回，调用方按需以钳后值重下发
void MainWindow::updateVolumeSliderRange() {
    if (!m_volumeSlider)
        return;
    const int sliderMax =
        m_quadraticVolumeCurve ? qRound(Util::Volume::mpvToMpc(m_volumeMax)) : m_volumeMax;
    m_volumeSlider->setRange(0, sliderMax);
}

void MainWindow::onMpvShutdown() {
    // mpv 侧发起退出（终端按 q、其他脚本发 quit）时 closeEvent 不会触发，
    // 在此补保存会话；窗口关闭路径随后到来的重复保存写同值幂等无害
    saveSettings();
    // N74 退出序（规格 §8）：queued 收口（最后一条 Stop）→ 有界 join（200ms）→
    // 超时软停。协议收进驱动器（N99 flushAndJoin）：Unbooted 整体短路在驱动器内判
    // （判据只看闸门、不看指针），线程对象不再出本类。以下必须排在 exit(0) 之前：
    // aboutToQuit 的传输闸门（g_appQuitting）一旦落位，最后那条 Stop 会被 HttpClient
    // 静默吞掉且无任何日志
    m_reportDriver->flushAndJoin(200);
    // N107：先阻塞拆缩略图 core（引擎线程循环仍在跑，BlockingQueuedConnection 可达）
    // 并收起窗口——不留孤悬窗口进退出生序，第二 core 也不与 mpv 宿主收尾并发
    // （N103 销毁顺序同源；无在飞会话时为瞬时空调用）
    QMetaObject::invokeMethod(
        m_thumbEngine, [e = m_thumbEngine] { e->endSession(); }, Qt::BlockingQueuedConnection);
    m_thumbWindow->hide();
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

    // N119：菜单栏偏好读真相成员——覆盖态 menuBar()->isVisible() 恒假，读活体会把偏好
    // 错存成 false（A11 在 playlistExpanded 上踩过的同一坑）
    settings.setValue(QStringLiteral("menuBarVisible"), m_menuBarVisibleTruth);

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

    // 音量步长（插件本体选项，缺键回落常量默认）
    m_volumeWheelStep =
        settings.value(QStringLiteral("volumeWheelStep"), App::volumeWheelStepDefault()).toInt();
    m_volumeKeyStep =
        settings.value(QStringLiteral("volumeKeyStep"), App::volumeKeyStepDefault()).toInt();

    // N108：曲线开关先于音量块——恢复钳制与量程都须按曲线取等价百分比；缺键 false。
    // 此刻 m_volumeMax 尚为构造默认 130（volume-max 推送晚于恢复），推送到达后
    // 经 updateVolumeSliderRange 按同一曲线重算，自然对齐
    m_quadraticVolumeCurve =
        settings.value(QStringLiteral("quadraticVolumeCurve"), App::quadraticVolumeCurveDefault())
            .toBool();
    updateVolumeSliderRange();
    // N107：悬停缩略图开关（缺键开）；读法与音量族同形（先于任何装配动作——
    // 会话机已在构造期装配，此处置位即生效）
    m_hoverThumbnailEnabled =
        settings.value(QStringLiteral("hoverThumbnail"), App::hoverThumbnailDefault()).toBool();
    m_thumbWatch->setEnabled(m_hoverThumbnailEnabled);
    if (settings.contains(QStringLiteral("volume"))) {
        // N108：INI 的 volume 键含义恒为"滑条显示值"——钳到当前量程（滑条上界，即
        // 等价百分比量程）后按曲线换算下发，滑条写同值（仍带 QSignalBlocker）
        const int p = qRound(qBound(0.0, settings.value(QStringLiteral("volume")).toDouble(),
                                    double(m_volumeSlider->maximum())));
        pushVolume(p);
        QSignalBlocker blocker(m_volumeSlider);
        m_volumeSlider->setValue(p);
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

    // N119：菜单栏偏好（缺键 true）——挡回调回放勾选态，再收口一次可见性。时点在首次
    // show 之前，布局重排经占位符事件链触发 syncVideoGeometry，与退出全屏同款
    m_menuBarVisibleTruth = settings.value(QStringLiteral("menuBarVisible"), true).toBool();
    QAction *menuBarAction = m_actions->action(ActionManager::Id::MenuBar);
    {
        QSignalBlocker blocker(menuBarAction);
        menuBarAction->setChecked(m_menuBarVisibleTruth);
    }
    applyMenuBarVisibility();

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

    // N113：OSD 形态（缺键回落常量默认；非法值经 modeFromIniValue 落缺省形态）
    m_osd->setMode(OsdController::modeFromIniValue(
        settings.value(QStringLiteral("osdMode"), App::osdModeDefault()).toString()));

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

// 当前文件某类轨道的快照，按 id 升序——三组流子菜单与流选择按钮的分组都从这份取表出生，
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

// N119：轨道动态段——四个子菜单当场新建、现读现填。追加"添加"项的形态留在调用点
// （分组函数只管选择条目）。启用判据在构建时判定：视频组看视频轨，其余三组看正播
// 实体——0 轨恰是"添加"的用武之地，不按轨道数判禁用
void MainWindow::buildTrackSection(QMenu &parent) {
    QMenu *video = parent.addMenu(tr("Video"));
    addTrackSelectionGroup(video, QStringLiteral("video"));
    video->setEnabled(!tracksOfType(QStringLiteral("video")).isEmpty());

    const bool hasPlaying = playingEntity() != nullptr;
    QMenu *audio = parent.addMenu(tr("Audio"));
    addTrackSelectionGroup(audio, QStringLiteral("audio"));
    appendTrackAddAction(audio, false);
    audio->setEnabled(hasPlaying);

    QMenu *sub = parent.addMenu(tr("Subtitle"));
    addSubtitleSelectionGroup(sub, false);
    appendTrackAddAction(sub, true);
    sub->setEnabled(hasPlaying);

    QMenu *secondary = parent.addMenu(tr("Secondary Subtitle"));
    addSubtitleSelectionGroup(secondary, true);
    appendTrackAddAction(secondary, true, true);
    secondary->setEnabled(hasPlaying);
}

// N96：流子菜单"添加"项装配单点——条件分隔线（菜单非空才加，0 轨时添加项顶格、不留
// 悬空分隔线）+ 动作。动作不可勾选、不进 tracksOfType 的任何分组函数，与"选择条目"
// 分属两件事。整菜单每次 aboutToShow 重建，动作连同其连接一起随 clear() 销毁，
// 无累积连接之虞
void MainWindow::appendTrackAddAction(QMenu *menu, bool subtitle, bool secondary) {
    if (!menu->isEmpty())
        menu->addSeparator();
    QAction *add = menu->addAction(subtitle ? tr("Add Subtitle…") : tr("Add Audio…"));
    if (subtitle)
        connect(add, &QAction::triggered, this, [this, secondary] { addSubtitleTrack(secondary); });
    else
        connect(add, &QAction::triggered, this, &MainWindow::addAudioTrack);
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

// N94：字幕选择条目的唯一体——播放菜单的两组与流选择按钮的两个分组都从这里出生，故判据
// 全仓只此一份。两组都列全量字幕轨，不排除已被对方占用的那条——"同一条只能落在一组"
// 由点击时的抢占下发体现（见 switchToTrack / switchToSecondarySubtitle）
void MainWindow::addSubtitleSelectionGroup(QMenu *menu, bool secondary) {
    const int mainSelection = secondary ? 1 : 0;
    const QList<MpvController::TrackInfo> subs = tracksOfType(QStringLiteral("sub"));

    // N96：0 轨时 None 无所指（没有轨可关），不添加——字幕组与音频组一样可只剩"添加"项
    if (!subs.isEmpty()) {
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
    }

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

// B18：流选择按钮的显隐与启用由 m_tracks 派生。刷新点随输入走：tracksChanged（轨道
// 表）/ playlistPosChanged（正播实体）/ END_FILE 会话态重置（清空 m_tracks 即隐藏）
void MainWindow::updateStreamButton() {
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
    m_osd->show(prefix + formatTrackText(t));
}

void MainWindow::disableSubtitles() {
    m_controller->setProperty(QStringLiteral("sid"), QStringLiteral("no"));
    m_osd->show(tr("Subtitle: None"));
}

// N94：第二字幕。与 switchToTrack 的 sub 分支对称——抢占方向反过来（目标是主字幕
// 时先关 sid），关闭只动 secondary-sid、不碰 sid
void MainWindow::switchToSecondarySubtitle(const MpvController::TrackInfo &t) {
    if (t.mainSelection == 0)
        m_controller->setProperty(QStringLiteral("sid"), QStringLiteral("no"));
    m_controller->setProperty(QStringLiteral("secondary-sid"), int(t.id));
    m_osd->show(tr("Secondary Subtitle: ") + formatTrackText(t));
}

void MainWindow::disableSecondarySubtitles() {
    m_controller->setProperty(QStringLiteral("secondary-sid"), QStringLiteral("no"));
    m_osd->show(tr("Secondary Subtitle: None"));
}

// ——N96 手动添加音频流 / 字幕流——

// N97：清单取值单点——对话框过滤器与视频区拖放的字幕判据共用，读取形态一字不动
QStringList MainWindow::trackExtensions(bool subtitle) const {
    // 扩展名现读 mpv 的 --sub-auto-exts / --audio-exts（运行期可读，值形如逗号紧拼，
    // 维护者实测确认），跟随用户 mpv.conf 的自定义走，比写死更准；读失败或读到空才回退
    // Constants.h 的内置清单（读空回退的分歧已裁决接受）。值按 mpv 的形态原样采信——
    // mpv 自己也不裁空格，插件替它裁只会比 mpv 更宽容。主字幕与第二字幕同一张表
    QStringList exts = m_controller
                           ->propertyString(subtitle ? QStringLiteral("sub-auto-exts")
                                                     : QStringLiteral("audio-exts"))
                           .split(u',', Qt::SkipEmptyParts);
    if (exts.isEmpty())
        exts = subtitle ? App::defaultSubtitleExtensions() : App::defaultAudioExtensions();
    return exts;
}

// "添加"项对话框过滤器（N96）
QString MainWindow::trackFileFilter(bool subtitle) const {
    const QString patterns = Util::extensionPatterns(trackExtensions(subtitle)).join(u' ');
    return subtitle ? tr("Subtitle files (%1);;All files (*)").arg(patterns)
                    : tr("Audio files (%1);;All files (*)").arg(patterns);
}

// 选文件 → 下发 → 成败 OSD（同步 command 的返回码即成败）。取消对话框静默、无任何下发；
// 成功播文件名（B12：不带路径防 OSD 长文本越界），失败播 mpv 错误文本。
// Qt 来源的路径拿到即转原生分隔符（B15）
void MainWindow::addAudioTrack() {
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Add Audio"), QString(), trackFileFilter(false));
    if (path.isEmpty())
        return;
    // audio-add 默认 <select>，加完即成当前音频流——"添加并设为选中"由 mpv 的默认行为
    // 兑现，插件不额外下发；不传 title：mpv 自行解析文件、用媒体元数据作轨道名，
    // 插件拿不到更准的
    const int err = m_controller->command({QStringLiteral("audio-add"), Util::Url::toNative(path)});
    if (err != 0) {
        m_osd->show(tr("Failed to add audio: %1").arg(MpvController::errorString(err)));
        return;
    }
    m_osd->show(tr("Audio: ") + QFileInfo(path).fileName());
}

void MainWindow::addSubtitleTrack(bool secondary) {
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Add Subtitle"), QString(), trackFileFilter(true));
    if (path.isEmpty())
        return; // 对话框取消
    applySubtitleFile(path, secondary);
}

// N97：对话框入口与视频区拖放共用的下发收口（拖放传 secondary = false，即替换主字幕）。
// 成功播文件名（B12：不带路径防 OSD 长文本越界），失败播 mpv 错误文本
void MainWindow::applySubtitleFile(const QString &path, bool secondary) {
    // 主字幕走默认 <select>，加完即成当前字幕；第二字幕传 auto 止于"加进来"——sub-add
    // 没有"加为第二字幕"的形态，而用默认 select 会先抢主字幕，随后转第二字幕时撞上 N94
    // 的"同一条不可兼具"。选中由用户随后点条目走 switchToSecondarySubtitle 的既有出口
    QStringList cmd{QStringLiteral("sub-add"), Util::Url::toNative(path)};
    if (secondary)
        cmd << QStringLiteral("auto");
    const int err = m_controller->command(cmd);
    if (err != 0) {
        m_osd->show(tr("Failed to add subtitle: %1").arg(MpvController::errorString(err)));
        return;
    }
    // 第二字幕此刻还只是条尚未归属槽位的字幕轨，播"字幕"而非"第二字幕"（要等用户点选、
    // 真占上 secondary-sid 时由那条路径播轨道名）
    m_osd->show(tr("Subtitle: ") + QFileInfo(path).fileName());
}

// ——N134：Tools 菜单——

// 分隔线防重单点：菜单为空或上一条已是分隔线时不追加（N135 的 menu-data 段同用）
static void appendMenuSeparator(QMenu &menu) {
    const QList<QAction *> actions = menu.actions();
    if (!actions.isEmpty() && !actions.constLast()->isSeparator())
        menu.addSeparator();
}

// 键为 joinPath 拼出的规范化前缀路径；调用方按列表序推进，父菜单尾恰为首个命中
// 条目的落位，同前缀后续条目经缓存并入
static QMenu *ensureToolSubmenu(QMenu &menu, QHash<QString, QMenu *> &submenus,
                                const QString &prefixKey, const QString &title) {
    if (auto it = submenus.constFind(prefixKey); it != submenus.constEnd())
        return it.value();
    QMenu *sub = menu.addMenu(title);
    submenus.insert(prefixKey, sub);
    return sub;
}

// N134：名称经 pathSegments 分段，最后一段是叶子文本，前面的段依次找 / 建子菜单。
// 条目与连接都是渲染期对象，随 clear() 消亡。N135 起输出顺序为 mpv menu-data 段、
// 分隔线、插件工具、分隔线——两段皆空时只剩描述表追加的编辑入口（前导分隔线由
// 助手判据自然压住）
void MainWindow::buildToolSection(QMenu &menu) {
    buildMpvMenuItems(menu, m_controller->menuData());
    appendMenuSeparator(menu);
    QHash<QString, QMenu *> submenus;
    for (const CustomTool &tool : m_toolMgr->tools()) {
        const QStringList segments = CustomToolManager::pathSegments(tool.name);
        if (segments.isEmpty())
            continue;
        QMenu *parent = &menu;
        for (int i = 0; i + 1 < segments.size(); ++i) {
            const QString prefixKey = CustomToolManager::joinPath(segments.mid(0, i + 1));
            parent = ensureToolSubmenu(*parent, submenus, prefixKey, segments[i]);
        }
        QAction *act = parent->addAction(segments.constLast());
        connect(act, &QAction::triggered, this,
                [this, name = tool.name, cmd = tool.command] { runToolCommand(name, cmd); });
    }
    appendMenuSeparator(menu);
}

// N134：同步下发；失败走 OSD 与日志，日志只记 label 与错误码——不记命令原文
// （可能含凭据）
void MainWindow::runToolCommand(const QString &label, const QString &command) {
    const int err = m_controller->command(command);
    if (err == 0)
        return;
    OXBOW_LOG_WARNING() << "tool failed:" << label << "err =" << err;
    m_osd->show(tr("Failed to run tool \"%1\": %2").arg(label, MpvController::errorString(err)));
}

// N135：menu-data 的 state → QAction——disabled 置灰、checked 原生勾选（菜单每次
// 展开重建，点击造成的瞬时翻转不留存）、未知值忽略。子菜单条目施加在其 menuAction
// 上，与命令条目同一条口径
static void applyMenuDataState(QAction &action, const QStringList &state) {
    action.setEnabled(!state.contains(QLatin1String("disabled")));
    if (state.contains(QLatin1String("checked"))) {
        action.setCheckable(true);
        action.setChecked(true);
    }
}

// N135：mpv menu-data 段的递归渲染。hidden 整条跳过（含子菜单）；separator 走
// 防重助手（前导 / 连续分隔线由此折叠）；带非空 submenu 的条目按子菜单渲染（填完
// 为空即摘除，避免空弹层；title 纯空白按坏数据跳过），否则按命令条目渲染（cmd
// 非空白才连接执行，空命令条目照常显示、点击无动作）。title 原样显示不转义 '&'，
// shortcut 字段完全忽略
void MainWindow::buildMpvMenuItems(QMenu &menu, const QList<MpvController::MenuDataItem> &items) {
    for (const MpvController::MenuDataItem &item : items) {
        if (item.state.contains(QLatin1String("hidden")))
            continue;
        if (item.type == QLatin1String("separator")) {
            appendMenuSeparator(menu);
            continue;
        }
        if (!item.submenu.isEmpty()) {
            if (item.title.trimmed().isEmpty()) // 坏数据：无标题的子菜单
                continue;
            QMenu *sub = menu.addMenu(item.title);
            applyMenuDataState(*sub->menuAction(), item.state);
            buildMpvMenuItems(*sub, item.submenu);
            if (sub->isEmpty()) {
                menu.removeAction(sub->menuAction());
                delete sub;
            }
            continue;
        }
        if (item.type == QLatin1String("submenu"))
            continue; // 坏数据：type 为 submenu 但 submenu 为空
        if (item.title.trimmed().isEmpty())
            continue;
        QAction *act = menu.addAction(item.title);
        applyMenuDataState(*act, item.state);
        if (!item.cmd.trimmed().isEmpty()) {
            connect(act, &QAction::triggered, this,
                    [this, label = item.title, cmd = item.cmd] { runToolCommand(label, cmd); });
        }
    }
}

void MainWindow::customizeTools() {
    CustomToolListDialog dlg(m_toolMgr->tools(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (!m_toolMgr->replaceTools(dlg.tools()))
        OXBOW_LOG_WARNING() << "failed to save custom tool list to ini";
}

// N43：滤镜动态段——两个子菜单按 af / vf 现读重建，勾选态即启用位；无条目则该子菜单
// 禁用。填充挂父段（本段展开时一次建齐）：被禁用的子菜单永不展开，靠它自己翻转不了
// "从无滤镜到有滤镜"的启用态
void MainWindow::buildFilterSection(QMenu &parent) {
    auto build = [this](QMenu *menu, const QString &property) {
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
    build(parent.addMenu(tr("Video Filters")), QStringLiteral("vf"));
    build(parent.addMenu(tr("Audio Filters")), QStringLiteral("af"));
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

// N119：章节 / 书签的命中判据单点（时间 / 类别 / 标题齐等）——构建时的勾选初值与菜单
// 可见期的逐 tick 回写同调此处，分写两处迟早分叉
static bool markHits(const MarkAggregator::Mark &hit, double time, bool isBookmark,
                     const QString &title) {
    return hit.time == time && hit.isBookmark == isBookmark && hit.title == title;
}

void MainWindow::updateChapterMenuChecked() {
    QMenu *menu = m_liveChapterMenu.data();
    if (!menu || !menu->isVisible())
        return; // 无活体 UI（尚未构建或右键弹层已毁），或菜单不可见
    // N77 §2.3：命中查询收（正播身份，key），豁免分派在聚合器内
    const auto cur = playingEntity();
    const auto hit = m_marks->hit(m_timePos, playingListUuid(),
                                  cur ? cur->key() : QString()); // std::optional<Mark>
    // 无 QActionGroup：逐条显式回写即互斥（命中者勾上，其余落下）
    for (QAction *act : menu->actions()) {
        if (!act->isCheckable())
            continue; // 两个登记动作不在命中面上
        const bool checked = hit.has_value() && markHits(*hit, act->data().toDouble(),
                                                         act->property("isBookmark").toBool(),
                                                         act->property("title").toString());
        act->setChecked(checked);
    }
}

void MainWindow::buildChapterSection(QMenu &target) {
    // N119：登记活体章节菜单——updateChapterMenuChecked 据此在菜单可见时逐 tick 跟随
    // 播放位置；右键弹层即建即毁，QPointer 自动归空
    m_liveChapterMenu = &target;
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
    // 勾选初值按构建时刻现算（判据同出 markHits，至多一条勾上）
    const auto hit = m_marks->hit(m_timePos, curUuid, curKey);

    // 跳转目标 = 菜单构建时的当前文件（菜单条目语义上属于该文件）
    auto addSection = [&](const QList<MarkAggregator::Mark> &items) {
        for (const auto &it : items) {
            const QString text =
                QStringLiteral("[%1] %2").arg(Util::formatTimeHMSms(it.time), it.title);
            QAction *a = target.addAction(text);
            a->setCheckable(true);
            a->setData(it.time);
            a->setProperty("isBookmark", it.isBookmark);
            a->setProperty("title", it.title);
            a->setChecked(hit.has_value() && markHits(*hit, it.time, it.isBookmark, it.title));
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
    // 书签与章节同 time 时两段各显示自己那条（标记层已合并，菜单允许重复显示以区分
    // 来源），谁被勾由 markHits 定
    bool hasChapter = !chapterMarks.isEmpty();
    bool hasBookmark = !bookmarkMarks.isEmpty();

    if (hasChapter) {
        addSection(chapterMarks);
    }
    if (hasChapter && hasBookmark)
        target.addSeparator();
    if (hasBookmark) {
        addSection(bookmarkMarks);
    }
    if (hasChapter || hasBookmark)
        target.addSeparator();

    QAction *addAct = m_actions->action(ActionManager::Id::AddBookmark);
    QAction *editorAct = m_actions->action(ActionManager::Id::BookmarkEditor);
    if (addAct)
        target.addAction(addAct);
    if (editorAct)
        target.addAction(editorAct);

    updateChapterMenuState();
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
    // 拒收时经 m_osd->show 给一条失败提示（与成功那条同一通道、同一强度——用户按了键
    // 就得当场听到回音，静默早退等于没响应），不调 addBookmark、不发成功 OSD
    if (StreamTransit::isTransitUri(entity->playUrl())) {
        const int playingTab = m_playlist->playingTabIndex();
        const Playlist *playing = m_playlist->listOf(playingTab);
        if (!playing || playing->uuid() == kDefaultListUuid) {
            m_osd->show(tr("This item does not support bookmarks."));
            return;
        }
    }
    // N105 §6：无身份条目拒收——空 path 行（loadfile "" 经回显收编落 UnknownEntity）
    // 的 key 为空串，书签跳转对空串首行早退、失效清理对空串全放行，落 INI 即死账目。
    // 插件不主动写入异常数据：走既有文案回音，不调 addBookmark、不发成功 OSD
    if (entity->path().isEmpty()) {
        m_osd->show(tr("This item does not support bookmarks."));
        return;
    }
    // N124：stdin 是一次性数据流，回跳不可兑现
    if (Util::Url::isMpvStdin(entity->path())) {
        m_osd->show(tr("This item does not support bookmarks."));
        return;
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
    m_osd->show(tr("Bookmark added: [%1] %2").arg(Util::formatTimeHMSms(t), title));
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

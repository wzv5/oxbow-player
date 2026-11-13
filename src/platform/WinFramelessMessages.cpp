#include "pch.h"

#include "platform/WinFramelessMessages.h"

#include "Constants.h"
#include "WindowStateHost.h"
#include "ui/FrameMetrics.h"
#include "ui/WindowTitleBar.h"

namespace {

// OXBOW_PUREQT 是唯一的自驱动回退
#if defined(OXBOW_PUREQT)
constexpr bool kNativeGestures = false;
#else
constexpr bool kNativeGestures = true;
#endif

// 边组合 → Win32 边框命中码（缩放热区与光标共用）
LRESULT hitTestForEdges(Qt::Edges edges) {
    const bool left = edges & Qt::LeftEdge;
    const bool right = edges & Qt::RightEdge;
    const bool top = edges & Qt::TopEdge;
    const bool bottom = edges & Qt::BottomEdge;
    if (top && left)
        return HTTOPLEFT;
    if (top && right)
        return HTTOPRIGHT;
    if (bottom && left)
        return HTBOTTOMLEFT;
    if (bottom && right)
        return HTBOTTOMRIGHT;
    if (left)
        return HTLEFT;
    if (right)
        return HTRIGHT;
    if (top)
        return HTTOP;
    return HTBOTTOM;
}

// 屏幕物理坐标（消息 lParam / POINT）→ 主窗客户区逻辑坐标。原点取 ClientToScreen
// （客户区原点）而非 GetWindowRect（窗口原点）：最大化态 WM_NCCALCSIZE 把客户区置为
// 工作区，窗口矩形却仍外扩一圈，两原点不再重合——用窗口原点换算会把 NC 命中区整体
// 平移、与自绘按钮错位（还原钮只中心出 Snap 浮窗、左邻最小化钮悬停残留）。普通态
// 无框窗口矩形 == 客户区，两原点重合，取值不变。物理↔逻辑只用窗口 DPR（N68 纪律）
QPoint windowPosFromScreen(HWND window, POINT screen, qreal dpr) {
    POINT origin{0, 0};
    ClientToScreen(window, &origin);
    return QPoint(qRound((screen.x - origin.x) / dpr), qRound((screen.y - origin.y) / dpr));
}

QPoint windowPosFromScreen(HWND window, LPARAM screenLParam, qreal dpr) {
    return windowPosFromScreen(
        window,
        POINT{static_cast<short>(LOWORD(screenLParam)), static_cast<short>(HIWORD(screenLParam))},
        dpr);
}

} // namespace

WinFramelessMessages::WinFramelessMessages(QWidget *mainWindow, WindowTitleBar *titleBar,
                                           WindowStateHost *host, QObject *parent)
    : QObject(parent), m_main(mainWindow), m_titleBar(titleBar), m_host(host) {
    qApp->installNativeEventFilter(this);
}

WinFramelessMessages::~WinFramelessMessages() { detach(); }

void WinFramelessMessages::detach() {
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(this);
}

void WinFramelessMessages::clearDragAnchors() {
    m_dragOriginRect = QRect();
    m_dragCursorOrigin = QPoint();
}

bool WinFramelessMessages::maximizeButtonHit(const QPoint &windowPos) const {
    // 标题栏重挂进全屏浮窗期间 window() 指向浮窗、mapTo 不成立——fullscreen 与
    // window()==m_main 双保险；Mini 无此按钮。最大化态同答：该按钮此时是还原钮，
    // 系统在 restore 态也认 HTMAXBUTTON 弹浮窗
    if (!m_titleBar || m_host->fullscreen() || m_host->mini() || m_titleBar->window() != m_main)
        return false;
    const QRect r = m_titleBar->maximizeButtonRect();
    if (r.isEmpty())
        return false;
    return QRect(m_titleBar->mapTo(m_main, r.topLeft()), r.size()).contains(windowPos);
}

// 应用级过滤器：子 HWND 的命中测试 / 光标与最大化钮的 NC 鼠标 / leave。视频区与两
// 容器都是原生子窗、铺满客户区，系统只把 WM_NCHITTEST / WM_SETCURSOR 发给鼠标下最
// 顶层的子 HWND——子窗在边缘热区返回 HTTRANSPARENT 让出命中测试，由主窗 handleMessage
// 回答边框码。只在 Native 形态、非全屏 / 最大化时启用；自驱动形态走子控件事件转发
bool WinFramelessMessages::nativeEventFilter(const QByteArray &eventType, void *message,
                                             qintptr *result) {
    Q_UNUSED(eventType);
    auto *msg = static_cast<MSG *>(message);
    // Qt 6 的应用级过滤器按消息类别分两趟：输入消息（NC 鼠标 / leave 等）只在
    // 派发前那趟回调，result 为空、返回 true 即吞掉不再派发；窗过程那趟按
    // isInputMessage 跳过输入消息，不会再来一遍。非输入消息（WM_NCHITTEST /
    // WM_SETCURSOR）只走窗过程那趟，result 有效。据此：命中 / 光标照旧写
    // *result；NC 按钮与 leave 在空 result 那趟处理，且绝不写 *result
    const bool maxButtonMsg = msg->message == WM_NCLBUTTONDOWN || msg->message == WM_NCLBUTTONUP ||
                              msg->message == WM_NCLBUTTONDBLCLK || msg->message == WM_MOUSELEAVE ||
                              msg->message == WM_NCMOUSELEAVE;
    if (msg->message != WM_NCHITTEST && msg->message != WM_SETCURSOR && !maxButtonMsg)
        return false;
    if (!m_main->windowHandle())
        return false;
    const HWND main = reinterpret_cast<HWND>(m_main->winId());
    if (GetAncestor(msg->hwnd, GA_ROOT) != main)
        return false;
    const qreal dpr = m_main->devicePixelRatioF();
    if (msg->message == WM_NCHITTEST) {
        if (!result)
            return false; // 非输入消息只在窗过程那趟到达，result 不应为空
        // 缩放热区优先（与标准窗口一致：按钮顶边热区仍是窗口上边）；其余位置
        // 命中最大化钮即答 NC。子窗一律让出（HTTRANSPARENT），由主窗回答——
        // 直接在子窗上返回边框码 / 按钮码，系统会去动那个子窗
        const bool edgesActive = !m_host->fullscreen() && !m_main->isMaximized() && kNativeGestures;
        const QPoint local = windowPosFromScreen(main, msg->lParam, dpr);
        const Qt::Edges edges =
            edgesActive ? FrameMetrics::hotZoneEdges(m_main->size(), local) : Qt::Edges();
        const bool overMax = !edges && maximizeButtonHit(local);
        if (m_titleBar) {
            m_titleBar->setMaximizeButtonHovered(overMax);
            if (!overMax)
                m_titleBar->setMaximizeButtonDown(false);
            // 光标落入系统接管的 NC 区（最大化钮 / 边缘热区）后，该片仍在容器 HWND
            // 客户矩形内——Qt 不发 Leave 也不再投递客户区 move，邻钮（最小化 / 关闭）
            // 悬停冻结。整条重画让按钮 paintEvent 的 widgetAt 现判清掉残留
            if (overMax || edges)
                m_titleBar->update();
        }
        if (overMax) {
            if (msg->hwnd != main) {
                *result = HTTRANSPARENT;
                return true;
            }
            // 光标离开窗口（进浮窗 / 其它应用）后没有后续命中测试，靠 leave 清悬停
            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, main, 0};
            TrackMouseEvent(&tme);
            *result = HTMAXBUTTON;
            return true;
        }
        if (msg->hwnd == main)
            return false; // 主窗边缘由 handleMessage 回答，其余交 Qt
        if (!edges)
            return false;
        *result = HTTRANSPARENT;
        return true;
    }
    if (msg->message != WM_SETCURSOR && msg->hwnd != main)
        return false;
    if (msg->message == WM_NCLBUTTONDOWN) {
        if (msg->wParam != HTMAXBUTTON)
            return false;
        if (m_titleBar && maximizeButtonHit(windowPosFromScreen(main, msg->lParam, dpr)))
            m_titleBar->setMaximizeButtonDown(true);
        // 一律吞掉：落 DefWindowProc 会进它自己的 NC 按钮跟踪、并画出系统
        // 自己的最大化钮（实测）；派发前那趟 result 为空，不得解引用
        if (result)
            *result = 0;
        return true;
    }
    if (msg->message == WM_NCLBUTTONUP) {
        if (msg->wParam != HTMAXBUTTON)
            return false;
        const bool pressed = m_titleBar && m_titleBar->maximizeButtonDown();
        if (m_titleBar)
            m_titleBar->setMaximizeButtonDown(false);
        if (pressed && maximizeButtonHit(windowPosFromScreen(main, msg->lParam, dpr)))
            m_host->toggleMaximizeRestore();
        if (result)
            *result = 0;
        return true;
    }
    if (msg->message == WM_NCLBUTTONDBLCLK) {
        if (msg->wParam != HTMAXBUTTON)
            return false;
        if (result)
            *result = 0; // 首击已收口，双击不再落系统默认（避免二连切换）
        return true;
    }
    if (msg->message == WM_MOUSELEAVE || msg->message == WM_NCMOUSELEAVE) {
        POINT cur{};
        GetCursorPos(&cur);
        const bool over = maximizeButtonHit(windowPosFromScreen(main, cur, dpr));
        if (m_titleBar) {
            m_titleBar->setMaximizeButtonHovered(over);
            if (!over)
                m_titleBar->setMaximizeButtonDown(false);
            // 光标离开窗口 / 客户区：整条重画清掉邻钮残留悬停（从 NC 区移出时 Qt
            // 未必投递 Leave，按钮 widgetAt 现判需一次重画才生效）
            m_titleBar->update();
        }
        return false; // leave 语义照常交 Qt
    }
    if (m_host->fullscreen() || m_main->isMaximized() || !kNativeGestures)
        return false;
    if (!result)
        return false; // WM_SETCURSOR 同属非输入消息
    // WM_SETCURSOR：lParam 低字是命中码，边框码才答 resize 光标；客户区放行给 Qt
    LPCTSTR id = nullptr;
    switch (LOWORD(msg->lParam)) {
    case HTLEFT:
    case HTRIGHT:
        id = IDC_SIZEWE;
        break;
    case HTTOP:
    case HTBOTTOM:
        id = IDC_SIZENS;
        break;
    case HTTOPLEFT:
    case HTBOTTOMRIGHT:
        id = IDC_SIZENWSE;
        break;
    case HTTOPRIGHT:
    case HTBOTTOMLEFT:
        id = IDC_SIZENESW;
        break;
    default:
        break;
    }
    if (!id)
        return false;
    SetCursor(LoadCursor(nullptr, id));
    *result = TRUE;
    return true;
}

// 窗过程那趟的消息回答：物理↔逻辑换算在此完成（消息 RECT 均物理像素）。
// 循环外的几何回写会被原生模态循环覆写，故不走原生通道的形态由主窗自驱动环路承担
bool WinFramelessMessages::handleMessage(void *message, qintptr *result) {
    auto *msg = static_cast<MSG *>(message);
    // 非客户区恒为 0——WS_CAPTION 族 + WS_THICKFRAME 只为让 DWM 按标准窗口处理
    // （最大化 / 还原动画、系统阴影、最大化自动方角），视觉边框由本层抹掉。
    // 最大化态客户区取所在屏工作区：WM_GETMINMAXINFO 已把最大化窗口尺寸钉成
    // 工作区（窗口不外扩），再按 frame 度量内缩会露出一圈可见 NC，系统随即在右上角
    // 补画自己的标题栏按钮，压住自绘按钮且点不动（实测）；直接取工作区同时覆盖
    // 系统仍外扩的形态——外扩部分留在工作区之外（屏幕外 / 任务栏下）。
    // 全屏期仍是最大化态，客户区改取 monitor，否则底部留黑带
    if (msg->message == WM_NCCALCSIZE) {
        if (msg->wParam) {
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
            RECT &rc = params->rgrc[0];
            if (IsZoomed(msg->hwnd)) { // IsZoomed 即查 WS_MAXIMIZE
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &mi))
                    rc = m_host->fullscreen() ? mi.rcMonitor : mi.rcWork;
            }
        }
        *result = 0;
        return true;
    }
    // 自绘标题栏的按钮矩形只能由我们上报。DefWindowProc 会按标准 caption 度量填这组
    // 矩形（本机 SM_CXSIZE/SM_CYSIZE = 36×22，与自绘 46×32 按钮不重合），系统的 Snap
    // Layouts 悬停判定按它取最大化钮矩形——结果只有与标准矩形相交的一小块能出浮窗
    // （实测）。这里按真实自绘按钮回答（屏幕物理坐标）
    if (msg->message == WM_GETTITLEBARINFOEX) {
        auto *info = reinterpret_cast<TITLEBARINFOEX *>(msg->lParam);
        if (!info || info->cbSize < sizeof(TITLEBARINFOEX))
            return false;
        for (int i = 0; i <= CCHILDREN_TITLEBAR; ++i) {
            info->rgstate[i] = STATE_SYSTEM_INVISIBLE;
            SetRectEmpty(&info->rgrect[i]);
        }
        SetRectEmpty(&info->rcTitleBar);
        if (m_titleBar && !m_host->fullscreen() && m_titleBar->window() == m_main) {
            const qreal dpr = m_main->devicePixelRatioF();
            const auto toPhysical = [dpr](const QRect &logicalRect) {
                return RECT{qRound(logicalRect.left() * dpr), qRound(logicalRect.top() * dpr),
                            qRound((logicalRect.left() + logicalRect.width()) * dpr),
                            qRound((logicalRect.top() + logicalRect.height()) * dpr)};
            };
            const QRect tbRect(m_titleBar->mapToGlobal(QPoint(0, 0)), m_titleBar->size());
            info->rcTitleBar = toPhysical(tbRect);
            info->rgstate[0] = 0;
            info->rgrect[0] = info->rcTitleBar;
            const auto setButton = [&](int index, const QRect &localRect) {
                if (localRect.isEmpty())
                    return;
                const QRect screenRect(m_titleBar->mapToGlobal(localRect.topLeft()),
                                       localRect.size());
                info->rgstate[index] = 0;
                info->rgrect[index] = toPhysical(screenRect);
            };
            // 索引按 TITLEBARINFOEX 文档布局：0 标题栏自身、1 保留、2 最小化、
            // 3 最大化、4 帮助、5 关闭（不可见按钮上面已统一置 STATE_SYSTEM_INVISIBLE）
            setButton(2, m_titleBar->minimizeButtonRect());
            setButton(3, m_titleBar->maximizeButtonRect());
            setButton(5, m_titleBar->closeButtonRect());
        }
        *result = 0;
        return true;
    }
    // 主窗侧的命中测试——子窗在边缘热区返回 HTTRANSPARENT 后转到这里，由主窗回答
    // 边框码，系统才会 resize 主窗而不是子窗
    if (msg->message == WM_NCHITTEST && !m_host->fullscreen() && !m_main->isMaximized() &&
        kNativeGestures) {
        const QPoint local =
            windowPosFromScreen(msg->hwnd, msg->lParam, m_main->devicePixelRatioF());
        const Qt::Edges edges = FrameMetrics::hotZoneEdges(m_main->size(), local);
        if (edges) {
            *result = hitTestForEdges(edges);
            return true;
        }
    }
    // 最大化摆位与拖缩下限（OXBOW_PUREQT 形态同需——它管最大化摆位与拖缩下限，与
    // 手势后端无关）：下限 = minimumSizeHint() 物理值；最大化铺所在屏工作区
    // （ptMaxPosition / ptMaxSize 为 workspace 坐标，按 rcWork 减 rcMonitor 原点口径）
    if (msg->message == WM_GETMINMAXINFO) {
        auto *mmi = reinterpret_cast<MINMAXINFO *>(msg->lParam);
        const qreal dpr = m_main->devicePixelRatioF();
        const QSize hint = m_main->minimumSizeHint();
        mmi->ptMinTrackSize.x = qMax(mmi->ptMinTrackSize.x, qRound(hint.width() * dpr));
        mmi->ptMinTrackSize.y = qMax(mmi->ptMinTrackSize.y, qRound(hint.height() * dpr));
        const HWND hwnd = reinterpret_cast<HWND>(m_main->winId());
        if (const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                // 全屏期报 monitor 覆盖任务栏，其余形态报工作区
                const RECT &target = m_host->fullscreen() ? mi.rcMonitor : mi.rcWork;
                mmi->ptMaxPosition.x = target.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = target.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = target.right - target.left;
                mmi->ptMaxSize.y = target.bottom - target.top;
            }
        }
        *result = 0;
        return true;
    }
#if !defined(OXBOW_PUREQT)
    // 拖动/缩放模态循环的起点锚只在此刻记一次，整轮拖动都以此为原始位置基准
    if (m_host->mini() && msg->message == WM_ENTERSIZEMOVE) {
        RECT now{};
        GetWindowRect(reinterpret_cast<HWND>(m_main->winId()), &now);
        m_dragOriginRect = QRect(now.left, now.top, now.right - now.left, now.bottom - now.top);
        POINT cur{};
        GetCursorPos(&cur);
        m_dragCursorOrigin = QPoint(cur.x, cur.y);
    }
    if (m_host->mini() && msg->message == WM_EXITSIZEMOVE) {
        m_dragOriginRect = QRect(); // 作废：杜绝跨循环误用旧锚点
    }
    if (m_host->mini() && msg->message == WM_SIZING) {
        // lParam 指向拖动中的 RECT（屏幕物理坐标），wParam 为拖动边（WMSZ_*）。
        // 判定数学收进 FrameMetrics::miniResizedRect
        auto *rc = reinterpret_cast<RECT *>(msg->lParam);
        const UINT edge = static_cast<UINT>(msg->wParam);
        const qreal dpr = m_main->devicePixelRatioF();
        const QRect raw(
            QPoint(qRound(rc->left / dpr), qRound(rc->top / dpr)),
            QSize(qRound((rc->right - rc->left) / dpr), qRound((rc->bottom - rc->top) / dpr)));
        Qt::Edges edges;
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
            edges |= Qt::LeftEdge;
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
            edges |= Qt::TopEdge;
        if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT)
            edges |= Qt::RightEdge;
        if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT)
            edges |= Qt::BottomEdge;
        const QRect fixed =
            FrameMetrics::miniResizedRect(raw, edges, m_host->videoRatio(), App::miniMinWidth(),
                                          m_main->screen()->availableGeometry());
        rc->left = qRound(fixed.x() * dpr);
        rc->top = qRound(fixed.y() * dpr);
        rc->right = qRound((fixed.x() + fixed.width()) * dpr);
        rc->bottom = qRound((fixed.y() + fixed.height()) * dpr);
        *result = TRUE;
        return true;
    }
    if (m_host->mini() && msg->message == WM_MOVING) {
        // lParam 指向拖动中的 RECT（物理像素）。判距基准用"起点矩形 + 鼠标总位移"，
        // 拿系统给的 rc 判距会自反馈，慢速拖动永远脱不开；锚点缺失时退回系统提案
        auto *rc = reinterpret_cast<RECT *>(msg->lParam);
        RECT rawP = *rc;
        if (!m_dragOriginRect.isEmpty()) {
            POINT cur{};
            GetCursorPos(&cur);
            rawP.left = m_dragOriginRect.left() + (cur.x - m_dragCursorOrigin.x());
            rawP.top = m_dragOriginRect.top() + (cur.y - m_dragCursorOrigin.y());
            rawP.right = rawP.left + m_dragOriginRect.width();
            rawP.bottom = rawP.top + m_dragOriginRect.height();
        }
        const qreal dpr = m_main->devicePixelRatioF();
        const QRect raw(
            QPoint(qRound(rawP.left / dpr), qRound(rawP.top / dpr)),
            QSize(qRound((rawP.right - rawP.left) / dpr), qRound((rawP.bottom - rawP.top) / dpr)));
        const QPoint tl = FrameMetrics::miniSnappedTopLeft(
            raw, m_main->screen()->availableGeometry(), App::miniSnapDistancePx());
        const LONG shiftX = qRound(tl.x() * dpr) - rawP.left;
        const LONG shiftY = qRound(tl.y() * dpr) - rawP.top;
        rc->left = rawP.left + shiftX;
        rc->top = rawP.top + shiftY;
        rc->right = rawP.right + shiftX;
        rc->bottom = rawP.bottom + shiftY;
        // 改 RECT 即生效，无需返回值处理，落入基线 DefWindowProc
    }
#endif
    return false;
}
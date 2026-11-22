#include "pch.h"

#include "platform/X11FrameBackend.h"

#include "WindowStateHost.h"
#include "platform/X11Util.h"

#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>

namespace {

// _NET_WM_MOVERESIZE 方向码：0-7 缩放、8 移动
int x11MoveResizeDirection(Qt::Edges edges) {
    if (edges == (Qt::TopEdge | Qt::LeftEdge))
        return 0;
    if (edges == Qt::TopEdge)
        return 1;
    if (edges == (Qt::TopEdge | Qt::RightEdge))
        return 2;
    if (edges == Qt::RightEdge)
        return 3;
    if (edges == (Qt::RightEdge | Qt::BottomEdge))
        return 4;
    if (edges == Qt::BottomEdge)
        return 5;
    if (edges == (Qt::BottomEdge | Qt::LeftEdge))
        return 6;
    if (edges == Qt::LeftEdge)
        return 7;
    return 8;
}

// EWMH 要求客户端发送 _NET_WM_MOVERESIZE 之前放掉所有 grab。这条按键隐式 grab 属于 Qt 的
// xcb 连接，只能从那条连接放，X11Util 自建的那条放不掉。真 X 服务下窗口管理器要先抓到指针
// 才能起交互，撞上这条 grab 就退避到按钮松开之后；XWayland 由合成器接管指针，这一步等于空操作
void releasePointerForWm() {
    auto *app = qApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!app)
        return;
    xcb_connection_t *conn = app->connection();
    if (!conn)
        return;
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_flush(conn);
}

} // namespace

X11FrameBackend::X11FrameBackend(QWidget *window, WindowStateHost *host)
    : m_window(window), m_host(host) {}

void X11FrameBackend::enterFullscreenNative() { setWindowFullscreenEwmh(m_window->winId(), true); }

void X11FrameBackend::exitFullscreenNative() { setWindowFullscreenEwmh(m_window->winId(), false); }

// 窗口态转移分两拍抵达，中间帧几何不可信
bool X11FrameBackend::restoreGeometryDeferred() const { return true; }

bool X11FrameBackend::nativeGestures() const {
    // 普通态交窗口管理器，迷你态自驱动
    return !m_host->mini();
}

void X11FrameBackend::beginSystemMove(const QPoint &globalPos) {
    releasePointerForWm();
    // mutter 对窗内坐标只认左上角
    const qreal dpr = m_window->devicePixelRatioF();
    sendMoveResizeEwmh(m_window->winId(), qRound(globalPos.x() * dpr), qRound(globalPos.y() * dpr),
                       8);
}

void X11FrameBackend::beginSystemResize(Qt::Edges edges, const QPoint &globalPos) {
    if (m_window->isMaximized())
        return; // 最大化态不接缩放
    releasePointerForWm();
    // 缩放坐标须投影到窗口外边界
    const QRect g = m_window->geometry();
    QPoint root = globalPos;
    if (edges & Qt::LeftEdge)
        root.setX(g.left());
    if (edges & Qt::RightEdge)
        root.setX(g.right());
    if (edges & Qt::TopEdge)
        root.setY(g.top());
    if (edges & Qt::BottomEdge)
        root.setY(g.bottom());
    const qreal dpr = m_window->devicePixelRatioF();
    sendMoveResizeEwmh(m_window->winId(), qRound(root.x() * dpr), qRound(root.y() * dpr),
                       x11MoveResizeDirection(edges));
}

void X11FrameBackend::applyTopmost(bool topmost) { setWindowAboveEwmh(m_window->winId(), topmost); }

void X11FrameBackend::enterMiniShell(bool fromMaximized) {
    // 撤最大化须先于几何下发（N141）
    if (fromMaximized)
        m_window->setWindowState(Qt::WindowNoState);
    setWindowSkipTaskbarEwmh(m_window->winId(), true);
}

void X11FrameBackend::exitMiniShell() { setWindowSkipTaskbarEwmh(m_window->winId(), false); }

std::unique_ptr<FrameBackend> createFrameBackend(QWidget *window, WindowTitleBar *,
                                                 WindowStateHost *host) {
    return std::make_unique<X11FrameBackend>(window, host);
}

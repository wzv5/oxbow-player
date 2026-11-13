#include "pch.h"

#include "platform/WinFrameBackend.h"

#include "WindowStateHost.h"
#include "platform/WinFrameless.h"
#include "platform/WinFramelessMessages.h"

WinFrameBackend::WinFrameBackend(QWidget *window, WindowTitleBar *titleBar, WindowStateHost *host)
    : m_window(window), m_host(host),
      m_messages(std::make_unique<WinFramelessMessages>(window, titleBar, host, nullptr)) {}

void WinFrameBackend::applyCreationFixups() {
    m_rounded = WinFrameless::applyCreationFixups(m_window->winId());
}

void WinFrameBackend::setRounded(bool rounded) {
    // 构造期取 winId 会提前创建原生窗
    if (!m_window->windowHandle())
        return;
    m_rounded = WinFrameless::setRounded(m_window->winId(), rounded);
}

bool WinFrameBackend::roundedCorners() const { return m_rounded; }

void WinFrameBackend::showSystemMenu(const QPoint &globalPos) {
    // 自驱动形态没有 NC 命中通道，删掉移动 / 大小项
    WinFrameless::showSystemMenu(m_window->winId(), globalPos, m_window->devicePixelRatioF(),
                                 m_host->mini(), !nativeGestures());
}

QRect WinFrameBackend::normalPlacementRect() const {
    return WinFrameless::normalPlacementRect(m_window->winId(), m_window->devicePixelRatioF());
}

// 还原几何同步落定，没有分拍抵达的中间帧
bool WinFrameBackend::restoreGeometryDeferred() const { return false; }

void WinFrameBackend::setPlacementAtomic(const QRect &normalRect, bool maximized) {
    WinFrameless::setPlacementAtomic(m_window->winId(), normalRect, m_window->devicePixelRatioF(),
                                     maximized);
}

// 直发 SetWindowPos 绕过 Qt 按窗口态的矩形钳制；不切窗口态、不动 rcNormal
void WinFrameBackend::applyNativeRect(const QRect &rect) {
    SetWindowPos(reinterpret_cast<HWND>(m_window->winId()), nullptr, rect.x(), rect.y(),
                 rect.width(), rect.height(),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_FRAMECHANGED);
}

// 最大化态客户区被钉在工作区，先去框才能铺满任务栏
void WinFrameBackend::enterFullscreenNative() {
    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_DLGFRAME | WS_THICKFRAME);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    // 去框后须先原地刷 NC 让系统重算最大化矩形；与尺寸下发挤在一拍会被按旧矩形钳住高度
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    m_fullscreenFrameless = true;
    applyNativeRect(m_window->screen()->geometry());
}

// 保持最大化态，rcNormal 不动
void WinFrameBackend::exitFullscreenNative() {
    if (m_fullscreenFrameless) {
        const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        style |= WS_CAPTION | WS_DLGFRAME | WS_THICKFRAME;
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        m_fullscreenFrameless = false;
    }
    applyNativeRect(m_window->screen()->availableGeometry());
}

bool WinFrameBackend::handleMessage(void *message, qintptr *result) {
    const MSG *msg = static_cast<MSG *>(message);
    // 键盘来源的 appcommand 落 DefWindowProc 会被转给 SMTC，与伴随 keydown 注入的
    // keypress 重复执行
    if (msg->message == WM_APPCOMMAND && GET_DEVICE_LPARAM(msg->lParam) == FAPPCOMMAND_KEY &&
        GET_APPCOMMAND_LPARAM(msg->lParam) != APPCOMMAND_BROWSER_HOME) {
        *result = TRUE;
        return true;
    }
    return m_messages->handleMessage(message, result);
}

void WinFrameBackend::clearDragAnchors() { m_messages->clearDragAnchors(); }

void WinFrameBackend::detach() { m_messages->detach(); }

bool WinFrameBackend::nativeGestures() const {
#if defined(OXBOW_PUREQT)
    return false;
#else
    return true;
#endif
}

void WinFrameBackend::beginSystemMove(const QPoint &) {
    if (auto *w = m_window->windowHandle())
        w->startSystemMove();
}

void WinFrameBackend::beginSystemResize(Qt::Edges edges, const QPoint &) {
    // 最大化态不接缩放
    if (m_window->isMaximized())
        return;
    if (auto *w = m_window->windowHandle())
        w->startSystemResize(edges);
}

void WinFrameBackend::enterMiniShell(bool fromMaximized) {
    // 先撤最大化再改壳，否则还原分两段可见
    if (fromMaximized)
        WinFrameless::clearMaximized(m_window->winId());
    WinFrameless::setMiniShell(m_window->winId(), true);
}

void WinFrameBackend::exitMiniShell() { WinFrameless::setMiniShell(m_window->winId(), false); }

std::unique_ptr<FrameBackend> createFrameBackend(QWidget *window, WindowTitleBar *titleBar,
                                                 WindowStateHost *host) {
    return std::make_unique<WinFrameBackend>(window, titleBar, host);
}

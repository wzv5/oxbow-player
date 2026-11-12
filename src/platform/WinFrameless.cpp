#include "pch.h"

#include "platform/WinFrameless.h"

// DWM 属性 id 与取值用本文件自己的常量：Win11 SDK 才引入这三组定义，自持一份
// 免得构建机 SDK 版本差异变成编译差异（dwmapi.h 已在 pch，函数声明不重复）
namespace {

constexpr DWORD kDwmwaCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmcpDoNotRound = 1;        // DWMWCP_DONOTROUND
constexpr DWORD kDwmcpRound = 2;             // DWMWCP_ROUND

HWND hwndOf(unsigned long long win) { return reinterpret_cast<HWND>(win); }

} // namespace

bool WinFrameless::applyCreationFixups(unsigned long long win) {
    const HWND hwnd = hwndOf(win);
    // WS_CAPTION 族 + WS_THICKFRAME：DWM 按标准窗口处理，最大化 / 还原动画、
    // 系统阴影与最大化自动方角由此成立；非客户区由 WM_NCCALCSIZE 恒缩到 0，
    // 视觉仍无边框（N156）。
    // WS_SYSMENU 族：无框窗口没有系统菜单就没有任务栏点击最小化/还原与
    // Alt+Space（实测项 d）；WS_EX_APPWINDOW 显式声明任务栏归属
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      style | WS_BORDER | WS_DLGFRAME | WS_THICKFRAME | WS_SYSMENU |
                          WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_APPWINDOW);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return setRounded(win, true);
}

bool WinFrameless::setRounded(unsigned long long win, bool rounded) {
    const DWORD pref = rounded ? kDwmcpRound : kDwmcpDoNotRound;
    const HRESULT hr =
        DwmSetWindowAttribute(hwndOf(win), kDwmwaCornerPreference, &pref, sizeof(pref));
    return hr == S_OK;
}

void WinFrameless::setMiniShell(unsigned long long win, bool mini) {
    const HWND hwnd = hwndOf(win);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR updated =
        mini ? (exStyle | WS_EX_TOOLWINDOW)
             : ((exStyle & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW); // 恢复任务栏归属
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, updated);
    // FRAMECHANGED 让 EXSTYLE 立即生效（原地切换不 hide/show，实测项 c）；顶置 /
    // 撤顶同一次调用落定（迷你置顶走原生通道，替代旧 StaysOnTop 的 Qt flags 通道——
    // 后者任何变更都触发平台窗口重建，N75 §3.9 的坑）
    SetWindowPos(hwnd, mini ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void WinFrameless::showSystemMenu(unsigned long long win, const QPoint &globalPosLogical, qreal dpr,
                                  bool disableMinMax, bool dropMoveSize) {
    const HWND hwnd = hwndOf(win);
    // 先重置回默认菜单再按本次判据裁剪：系统菜单句柄随窗口终身，灰置 / 删除都是
    // 累积修改——全程无框后窗口不重建，不重置则退出 mini 后最小化 / 最大化永久灰置
    GetSystemMenu(hwnd, TRUE);
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu)
        return;
    // 裁剪按调用点判据执行：删除项不占位（自驱动形态没有对应修正通道），灰置项
    // 保占位（mini 态保留菜单结构、只锁两个动作）
    if (dropMoveSize) {
        DeleteMenu(menu, SC_SIZE, MF_BYCOMMAND);
        DeleteMenu(menu, SC_MOVE, MF_BYCOMMAND);
    }
    if (disableMinMax) {
        EnableMenuItem(menu, SC_MINIMIZE, MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | MF_GRAYED);
    }
    // 屏幕坐标：逻辑入参 × dpr → 物理
    const POINT pt{qRound(globalPosLogical.x() * dpr), qRound(globalPosLogical.y() * dpr)};
    const int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x,
                                     pt.y, hwnd, nullptr);
    if (cmd > 0)
        PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(cmd), 0);
}

bool WinFrameless::setPlacementAtomic(unsigned long long win,
                                      const QRect &normalRectWorkspaceLogical, qreal dpr,
                                      bool maximized) {
    const HWND hwnd = hwndOf(win);
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp))
        return false;
    // rcNormalPosition 是 workspace 物理坐标：入参已是 workspace 逻辑坐标，乘 dpr
    wp.rcNormalPosition.left = qRound(normalRectWorkspaceLogical.x() * dpr);
    wp.rcNormalPosition.top = qRound(normalRectWorkspaceLogical.y() * dpr);
    wp.rcNormalPosition.right =
        qRound((normalRectWorkspaceLogical.x() + normalRectWorkspaceLogical.width()) * dpr);
    wp.rcNormalPosition.bottom =
        qRound((normalRectWorkspaceLogical.y() + normalRectWorkspaceLogical.height()) * dpr);
    wp.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    return SetWindowPlacement(hwnd, &wp) != FALSE;
}

QRect WinFrameless::normalPlacementRect(unsigned long long win, qreal dpr) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwndOf(win), &wp))
        return QRect();
    const RECT &r = wp.rcNormalPosition;
    return QRect(QPoint(qRound(r.left / dpr), qRound(r.top / dpr)),
                 QSize(qRound((r.right - r.left) / dpr), qRound((r.bottom - r.top) / dpr)));
}

void WinFrameless::clearMaximized(unsigned long long win) {
    const HWND hwnd = hwndOf(win);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(style & WS_MAXIMIZE))
        return;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_MAXIMIZE);
    // FRAMECHANGED 让系统按新 style 重估非客户区；NOMOVE|NOSIZE 保持最大化矩形——
    // 窗口原地变成普通窗，不回普通矩形、无还原动画
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

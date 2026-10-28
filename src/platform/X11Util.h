#pragma once

// N83：X11 专用小工具集。**源内不做任何平台守卫**——本单元是否参与构建只由
// CMakeLists 的 NOT WIN32 段决定（§1）：在错误的平台上把它编进去，会因缺 X11 头与
// 库直接失败，那是要的显影；源内再叠一道 #ifndef Q_OS_WIN 反而会把实现静默掏空成
// 空翻译单元，把编译错降级成链接期的未定义符号。
// 声明只有一份、不提供 Windows 空操作内联，故调用方（MainWindow.cpp）的 include 与
// 调用点须自持 #ifndef Q_OS_WIN 守卫——Windows 不编译本单元，符号不存在。
// Xlib 连接自建、与 Qt 的 xcb 通道互不共享（无线程/锁交互），懒开一次常驻；实现见
// X11Util.cpp

// EWMH 直发置顶：向根窗口投递 _NET_WM_STATE ClientMessage（_NET_WM_STATE_ABOVE +
// source indication，wmctrl / xdotool 同款），WM 原地更新状态、窗口本体不动。绕开
// Qt 通道——StaysOnTop 的任何 Qt 通道变更（QWidget::setWindowFlags、QWindow::setFlag
// 均实测）都触发平台窗口销毁重建，连带 mpv 嵌入窗口毁灭退出（N75 §3.9）。
// X 环境不可用时静默跳过
void setWindowAboveEwmh(unsigned long long win, bool on);

// EWMH 直发全屏态（N141）：同一条通道，atom 换 `_NET_WM_STATE_FULLSCREEN`。只给最大化态
// 进出全屏用——绕开 `QWidget::showFullScreen` 是因为它把状态差拆成 REMOVE MAXIMIZED_VERT /
// REMOVE MAXIMIZED_HORZ / ADD FULLSCREEN 多条 client message，窗口管理器逐条响应，中间必经
// 一次"缩回普通几何"的还原。这里只发一条 ADD 而留着 MAXIMIZED（EWMH 里全屏优先于最大化），
// 退出反向只发 REMOVE；代价是 Qt 侧窗口态与实际不再一致（全屏期间记作最大化），"是否全屏"
// 的真相须由调用方自持
void setWindowFullscreenEwmh(unsigned long long win, bool on);

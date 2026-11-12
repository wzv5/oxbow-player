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

// EWMH 直发隐藏任务栏 / 分页器条目（N156）：同一条 _NET_WM_STATE 通道，一条
// client message 携带 SKIP_TASKBAR + SKIP_PAGER 双 atom，迷你期任务栏不显示
// 本窗口条目。实现镜像 setWindowAboveEwmh（自建 Xlib 连接），X 环境不可用时
// 静默跳过
void setWindowSkipTaskbarEwmh(unsigned long long win, bool on);

// N156：直发 _NET_WM_MOVERESIZE（move/resize 交互抓取）。绕开 Qt xcb 通道——实测
// mutter/XWayland 对 Qt 发的消息只认左上角：Qt 发送前 xcb_ungrab_pointer 释放了按键
// 隐式 grab、resize 用窗内指针坐标，均不合 mutter 预期。本函数不 ungrab（保留按键隐式
// grab 供 WM 接管），坐标与方向由调用方算好（resize 投影到窗口外边界）。direction: 0-7
// 缩放、8 移动。data.l[4] 是 source indication（本消息无 timestamp 字段），取 1=application。
// X 环境不可用时静默跳过
void sendMoveResizeEwmh(unsigned long long win, int rootX, int rootY, int direction);

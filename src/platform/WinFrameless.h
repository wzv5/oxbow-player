#pragma once

#include "pch.h"

// N156：Windows 无框窗口胶合层（自由函数命名空间）。与 X11Util 同纪律：源内无
// 平台守卫，本单元是否参与构建只由 CMakeLists 的 WIN32 段决定（错平台编进去就
// 在缺 Win32 / DWM 处编译失败，那是要的显影）；调用方（MainWindow.cpp）的调用
// 点自持 #ifdef Q_OS_WIN。几何入参一律逻辑坐标 + dpr，物理换算在实现内单点完成；
// 全部操作原地生效，不 withdraw、不重建 HWND。
namespace WinFrameless {

// 创建后补丁（showEvent 首次分支调用一次）：原地追加 WS_CAPTION 族
// （WS_BORDER|WS_DLGFRAME）与 WS_THICKFRAME——让 DWM 把窗口当标准窗口，最大化 /
// 还原动画、系统阴影与最大化自动方角由此成立；非客户区由 MainWindow::nativeEvent
// 的 WM_NCCALCSIZE 恒缩到 0，视觉仍无边框。另追加 WS_SYSMENU|WS_MINIMIZEBOX|
// WS_MAXIMIZEBOX（原生系统菜单、任务栏点击最小化/还原所需）与 WS_EX_APPWINDOW，
// SetWindowPos SWP_FRAMECHANGED 生效；随后落 DWMWCP_ROUND 常态圆角。原地改 style
// 不 withdraw。返回 DWMWCP_ROUND 是否生效（S_OK）——MainWindow 圆角记账的初值
// 写入点（paintEvent 边框来源分流据它选路）
bool applyCreationFixups(unsigned long long win);

// DWMWCP_ROUND / DWMWCP_DONOTROUND 原地切换（不改几何、不 withdraw）。全屏方角
// 抑制四角透明缺口（A19 口径），退出回圆角——不再依赖 flags 重建回归默认。
// 返回本次调用后圆角是否生效（同步圆角记账）
bool setRounded(unsigned long long win, bool rounded);

// 迷你壳切换：迷你 = 加 WS_EX_TOOLWINDOW（任务栏按钮与 alt-tab 隐藏）+
// HWND_TOPMOST；退出 = 清 TOOLWINDOW、恢复 WS_EX_APPWINDOW + HWND_NOTOPMOST。
// 均原地生效，无 hide/show（窗口矩形 == 客户区，FRAMECHANGED 无可见副作用）
void setMiniShell(unsigned long long win, bool mini);

// 标题栏右键系统菜单（仅 Windows 接线）：GetSystemMenu + TrackPopupMenuEx
// （TPM_RETURNCMD）+ 选中项经 WM_SYSCOMMAND 下发。每次先重置回默认菜单再裁剪——
// 菜单句柄随窗口终身，灰置 / 删除是累积修改，不重置则退出 mini 后两项永久灰置。
// disableMinMax 灰掉最大化 / 最小化（mini 态）；dropMoveSize 删掉移动 / 大小项
// （GesturePolicy 判为自驱动的形态——它们依赖系统 NC 命中，自驱动没有对应修正通道）。
// 裁剪判据留在调用点由 MainWindow 算好传入，本层不读 GesturePolicy 与窗口态（平台胶合层定位）
void showSystemMenu(unsigned long long win, const QPoint &globalPosLogical, qreal dpr,
                    bool disableMinMax, bool dropMoveSize);

// SetWindowPlacement 一次落 rcNormalPosition + showCmd（SW_SHOWMAXIMIZED /
// SW_SHOWNORMAL）。对可见窗做"普通 ↔ 最大化"转移**不**免除中间帧——实测它先落
// 普通矩形再最大化，两段可见（最大化步骤还带系统动画，实测项 b 结论）。现行
// 用途只剩"已最大化窗口上的还原矩形补记"：窗已是最大化时 showCmd 无状态转移、
// 仅更新 rcNormalPosition 记录，无视觉动作（全屏 / 小窗退出到最大化后由调用点
// 先行 setWindowState 一步到位，再调本函数记账）。
// normalRectWorkspaceLogical 是 workspace 原点的逻辑坐标，与 normalPlacementRect
// 返回值同域。返回成败，调用点按需处置
bool setPlacementAtomic(unsigned long long win, const QRect &normalRectWorkspaceLogical, qreal dpr,
                        bool maximized);

// 读平台还原位（GetWindowPlacement 的 rcNormalPosition）：workspace 物理坐标
// ÷ dpr → workspace 逻辑。与 setPlacementAtomic 读写同源——不能用 normalGeometry()
// 代替：最大化态被 Qt 按 frame 折算，本仓窗口还原后客户区 == 窗口矩形、无 frame
// 可减，折算值写回即每边少 8px。失败返回空矩形，调用点兜底
QRect normalPlacementRect(unsigned long long win, qreal dpr);

// 原地解除最大化：清 WS_MAXIMIZE style + SWP_FRAMECHANGED，NOMOVE|NOSIZE——
// 窗口停在最大化矩形上原地变成普通窗，不回普通矩形、无还原动画。随后的
// setGeometry 即一拍到位（最大化 → 全屏 / 小窗几何无中间帧）。非最大化窗口
// 调用无害（style 位守卫早退）
void clearMaximized(unsigned long long win);

} // namespace WinFrameless

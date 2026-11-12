#pragma once

#include "pch.h"

// N156：手势后端组合判据——全仓唯一的（OXBOW_PUREQT / Q_OS_WIN / 窗口态）组合
// 出口。其余代码只读本函数返回值，不再自行组合平台守卫。每个构建形态下普通态 /
// mini 态的后端在编译期确定，mini 只是运行期查表输入；无运行期可用性标志、无失效
// 降级分支——无框窗口两平台进原生循环已由实测钉死。与 TraversalPolicy.h 同族留根。
namespace GesturePolicy {

enum class Backend { Native, SelfDriven };

inline Backend backend(bool mini) {
#if defined(OXBOW_PUREQT)
    Q_UNUSED(mini);
    return Backend::SelfDriven; // 纯 Qt 回退：主窗与 mini 都自驱动
#elif defined(Q_OS_WIN)
    Q_UNUSED(mini);
    return Backend::Native; // 主窗与 mini 都走原生循环（WM_SIZING / WM_MOVING 循环内修正）
#else
    return mini ? Backend::SelfDriven : Backend::Native;
#endif
}

} // namespace GesturePolicy

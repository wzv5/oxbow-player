#pragma once

#include "pch.h"

// 单色字体图标的运行期染色出口：全仓图标染色只从这两个函数出生。引用串是 Material
// Symbols 的连字名，颜色与透明度在渲染期按 tint 施加（透明度即 tint 的 A 通道）；
// 图标内容不随调色板自跟随，主题切换后由各界入点重灌。引擎是实现细节，住对应的 .cpp。
namespace Ui {

// 单色染色：任意状态同一 tint
QIcon themedIcon(const QString &name, const QColor &tint);

// 双色染色：On 态（可切换按钮的勾选态）取 onTint，其余取 offTint。
// onTint 传无效色即缺省，On 态回落 offTint
QIcon themedIcon(const QString &name, const QColor &offTint, const QColor &onTint);

} // namespace Ui

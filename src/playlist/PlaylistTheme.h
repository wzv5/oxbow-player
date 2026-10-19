#pragma once

#include "pch.h"

// 播放列表域的主题派生色单点：列表行与两处属性对话框的取色全从这族出口，派生结果
// 不写回应用调色板。判深浅只采样调色板做选择器（qGray(Window)）——windowsvista 忽略
// Dark 时调色板不变，派生自动保持浅色系；若改读 colorScheme() 提示，会出现"令牌变暗、
// 界面仍浅"的劈叉。中性色一律 Base → Text 线性混合，深色端点落在两极中间档；语义色
// 只保留色相基准，暗色下向白提亮到可读档。全部纯函数、绘制时现取、无缓存。
namespace PlaylistTheme {

// 调色板是否深色（全仓唯一写死深浅阈值的出口）
inline bool isDark(const QPalette &p) { return qGray(p.color(QPalette::Window).rgb()) < 128; }

namespace detail {

// Base → Text 按 t 线性混合（t=0 得 Base，t=1 得 Text），整数四舍五入。
// 浅色校准（浅端点是白底黑字）：0.09 → #E8E8E8、0.435 → #909090 逐位回到历史写死值；
// 0.62 → #616161 比历史的 #606060 差一档——系数回归优先于回填定值，肉眼不可辨
inline QColor mixBaseText(const QPalette &p, double t) {
    const QColor base = p.color(QPalette::Base);
    const QColor text = p.color(QPalette::Text);
    return QColor(base.red() + qRound((text.red() - base.red()) * t),
                  base.green() + qRound((text.green() - base.green()) * t),
                  base.blue() + qRound((text.blue() - base.blue()) * t));
}

// 语义色暗色档：向白提亮 0.35（浅色原值返回）
inline QColor brightenForDark(const QColor &base, const QPalette &p) {
    if (!isDark(p))
        return base;
    return QColor(base.red() + qRound((255 - base.red()) * 0.35),
                  base.green() + qRound((255 - base.green()) * 0.35),
                  base.blue() + qRound((255 - base.blue()) * 0.35));
}

} // namespace detail

// 列表选中背景：深色系数略提（0.14）保住与文字的对比
inline QColor selectionBackground(const QPalette &p) {
    return detail::mixBaseText(p, isDark(p) ? 0.14 : 0.09);
}

// 正播蓝族：正播行前景 = 属性对话框属性名前景（色相基准 #1976D2）
inline QColor accentColor(const QPalette &p) {
    return detail::brightenForDark(QColor(0x19, 0x76, 0xD2), p);
}

// 属性值前景（= 普通行前景，命名别名）
inline QColor propValueForeground(const QPalette &p) { return detail::mixBaseText(p, 0.62); }

// 分组标题前景
inline QColor groupHeadForeground(const QPalette &p) { return detail::mixBaseText(p, 0.435); }

// 列表行前景四态：失败红 > 正播蓝 > 选中（Text 原值）> 普通灰，优先级即此序
inline QColor rowForeground(const QPalette &p, bool failed, bool playing, bool selected) {
    if (failed)
        return detail::brightenForDark(QColor(0xC6, 0x28, 0x28), p);
    if (playing)
        return accentColor(p);
    if (selected)
        return p.color(QPalette::Text);
    return propValueForeground(p);
}

} // namespace PlaylistTheme

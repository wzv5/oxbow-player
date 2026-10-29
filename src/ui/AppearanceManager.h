#pragma once

#include "pch.h"

// N115：界面样式与配色方案的唯一套用点。只接收两个原始取值并套用，不读不写
// 配置（读取归 MainWindow::applyAppearanceFromIni）；非 QObject，配色驱动的
// 重绘由 Qt 自行完成。
//
// 平台默认样式在构造时捕获一次（构造先于任何 setStyle，捕获值恒为真·平台
// 默认），Auto 档的语义就是回套这个名字。
//
// N143：列表域的主题派生色单点自 PlaylistTheme 归位于此——此前只有播放列表
// 一域使用，字幕浏览器成为第二个调用者。派生结果不写回应用调色板。判深浅只
// 采样调色板做选择器（qGray(Window)）——windowsvista 忽略 Dark 时调色板不变，
// 派生自动保持浅色系；若改读 colorScheme() 提示，会出现"令牌变暗、界面仍浅"
// 的劈叉。中性色一律 Base → Text 线性混合，深色端点落在两极中间档；语义色只
// 保留色相基准，暗色下向白提亮到可读档。全部纯函数、绘制时现取、无缓存。
class AppearanceManager {
  public:
    AppearanceManager();

    // 套用两个原始取值：样式与当前同名不重套（幂等），配色无条件 setColorScheme
    // （Unknown 即系统跟随）；配色必须排在样式之后——setStyle 会重置应用调色板
    void apply(const QString &styleRaw, const QString &colorSchemeRaw);

    // 可用样式清单：QStyleFactory::keys() 原样顺序，设置目录与解析共用
    static QStringList availableStyles();

    // ——主题派生色族（N143 自 PlaylistTheme 归位）——

    // 调色板是否深色（全仓唯一写死深浅阈值的出口）
    static bool isDark(const QPalette &p) { return qGray(p.color(QPalette::Window).rgb()) < 128; }

    // 列表选中背景：深色系数略提（0.14）保住与文字的对比
    static QColor selectionBackground(const QPalette &p) {
        return mixBaseText(p, isDark(p) ? 0.14 : 0.09);
    }

    // 正播蓝族：正播行前景 = 属性对话框属性名前景（色相基准 #1976D2）
    static QColor accentColor(const QPalette &p) {
        return brightenForDark(QColor(0x19, 0x76, 0xD2), p);
    }

    // 属性值前景（= 普通行前景，命名别名）
    static QColor propValueForeground(const QPalette &p) { return mixBaseText(p, 0.62); }

    // 分组标题前景
    static QColor groupHeadForeground(const QPalette &p) { return mixBaseText(p, 0.435); }

    // 列表行前景四态：失败红 > 正播蓝 > 选中（Text 原值）> 普通灰，优先级即此序
    static QColor rowForeground(const QPalette &p, bool failed, bool playing, bool selected) {
        if (failed)
            return brightenForDark(QColor(0xC6, 0x28, 0x28), p);
        if (playing)
            return accentColor(p);
        if (selected)
            return p.color(QPalette::Text);
        return propValueForeground(p);
    }

  private:
    // trim 后精确匹配（auto 不分大小写），空串与未命中落 fallbackName
    static QString resolveStyle(const QString &raw, const QStringList &available,
                                const QString &fallbackName);
    // trim 后精确匹配 system / light / dark，空串与未识别落 Unknown
    static Qt::ColorScheme resolveColorScheme(const QString &raw);

    // Base → Text 按 t 线性混合（t=0 得 Base，t=1 得 Text），整数四舍五入。
    // 浅色校准（浅端点是白底黑字）：0.09 → #E8E8E8、0.435 → #909090 逐位回到历史写死值；
    // 0.62 → #616161 比历史的 #606060 差一档——系数回归优先于回填定值，肉眼不可辨
    static QColor mixBaseText(const QPalette &p, double t) {
        const QColor base = p.color(QPalette::Base);
        const QColor text = p.color(QPalette::Text);
        return QColor(base.red() + qRound((text.red() - base.red()) * t),
                      base.green() + qRound((text.green() - base.green()) * t),
                      base.blue() + qRound((text.blue() - base.blue()) * t));
    }

    // 语义色暗色档：向白提亮 0.35（浅色原值返回）
    static QColor brightenForDark(const QColor &base, const QPalette &p) {
        if (!isDark(p))
            return base;
        return QColor(base.red() + qRound((255 - base.red()) * 0.35),
                      base.green() + qRound((255 - base.green()) * 0.35),
                      base.blue() + qRound((255 - base.blue()) * 0.35));
    }

    QString m_platformDefaultStyle; // 构造时捕获的平台默认样式名
};

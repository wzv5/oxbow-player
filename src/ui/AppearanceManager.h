#pragma once

#include "pch.h"

// N115：界面样式与配色方案的唯一套用点。只接收两个原始取值并套用，不读不写
// 配置（读取归 MainWindow::applyAppearanceFromIni）；非 QObject，配色驱动的
// 重绘由 Qt 自行完成。
//
// 平台默认样式在构造时捕获一次（构造先于任何 setStyle，捕获值恒为真·平台
// 默认），Auto 档的语义就是回套这个名字。
class AppearanceManager {
  public:
    AppearanceManager();

    // 套用两个原始取值：样式与当前同名不重套（幂等），配色无条件 setColorScheme
    // （Unknown 即系统跟随）；配色必须排在样式之后——setStyle 会重置应用调色板
    void apply(const QString &styleRaw, const QString &colorSchemeRaw);

    // 可用样式清单：QStyleFactory::keys() 原样顺序，设置目录与解析共用
    static QStringList availableStyles();

  private:
    // trim 后精确匹配（auto 不分大小写），空串与未命中落 fallbackName
    static QString resolveStyle(const QString &raw, const QStringList &available,
                                const QString &fallbackName);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // trim 后精确匹配 system / light / dark，空串与未识别落 Unknown
    static Qt::ColorScheme resolveColorScheme(const QString &raw);
#endif

    QString m_platformDefaultStyle; // 构造时捕获的平台默认样式名
};

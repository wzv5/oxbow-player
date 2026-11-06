#include "pch.h"

#include "AppearanceManager.h"

#include "Logging.h"

namespace {

// N152：滑条左键点击跳转只靠改写两条样式提示达成——absolute 收编左中键、
// page 清空；其余提示与绘制原样转发基样式，外观不变。文件级私有类，只此一处
// 消费（apply 套用时包裹基样式），不进头文件。
class SliderClickStyle : public QProxyStyle {
  public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override {
        switch (hint) {
        case SH_Slider_AbsoluteSetButtons:
            return int(Qt::LeftButton | Qt::MiddleButton);
        case SH_Slider_PageSetButtons:
            return int(Qt::NoButton);
        default:
            return QProxyStyle::styleHint(hint, option, widget, returnData);
        }
    }
};

} // namespace

QStringList AppearanceManager::availableStyles() { return QStyleFactory::keys(); }

QString AppearanceManager::resolveStyle(const QString &raw, const QStringList &available,
                                        const QString &fallbackName) {
    const QString value = raw.trimmed();
    if (value.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0)
        return fallbackName;
    return available.contains(value) ? value : fallbackName;
}

Qt::ColorScheme AppearanceManager::resolveColorScheme(const QString &raw) {
    const QString value = raw.trimmed();
    if (value == QLatin1String("system"))
        return Qt::ColorScheme::Unknown;
    if (value == QLatin1String("light"))
        return Qt::ColorScheme::Light;
    if (value == QLatin1String("dark"))
        return Qt::ColorScheme::Dark;
    return Qt::ColorScheme::Unknown;
}

AppearanceManager::AppearanceManager()
    : m_platformDefaultStyle(QApplication::style()->objectName()) {}

void AppearanceManager::apply(const QString &styleRaw, const QString &colorSchemeRaw) {
    const QString target = resolveStyle(styleRaw, availableStyles(), m_platformDefaultStyle);
    QStyle *current = QApplication::style();
    if (target != current->objectName() || current != m_styleProxy) {
        if (QStyle *base = QStyleFactory::create(target)) {
            auto *proxy = new SliderClickStyle(base);
            proxy->setObjectName(base->objectName());
            QApplication::setStyle(proxy);
            m_styleProxy = proxy;
        } else {
            OXBOW_LOG_WARNING().noquote()
                << QStringLiteral("AppearanceManager: QStyleFactory::create(%1) failed, keeping "
                                  "current style")
                       .arg(target);
        }
    }
    QGuiApplication::styleHints()->setColorScheme(resolveColorScheme(colorSchemeRaw));
}

#include "pch.h"

#include "AppearanceManager.h"

#include "Logging.h"

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
    if (target != QApplication::style()->objectName()) {
        if (QStyle *created = QStyleFactory::create(target))
            QApplication::setStyle(created);
        else
            OXBOW_LOG_WARNING().noquote()
                << QStringLiteral("AppearanceManager: QStyleFactory::create(%1) failed, keeping "
                                  "current style")
                       .arg(target);
    }
    QGuiApplication::styleHints()->setColorScheme(resolveColorScheme(colorSchemeRaw));
}

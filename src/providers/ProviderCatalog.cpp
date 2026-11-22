#include "pch.h"

#include "ProviderCatalog.h"

#include "Logging.h"

// 本文件是具体提供者的唯一聚合点：登记表条目要引用各提供者的键常量、
// displayName 与工厂，故只有此处包含提供者头文件。
#include "DirProvider.h"
#include "ExternalProvider.h"
#include "JellyfinProvider.h"
#include "ProviderSettingsPage.h"
#if defined(OXBOW_WITH_RCLONE_CRYPT)
#include "RcloneCryptProvider.h"
#endif
#include "StaticProvider.h"
#include "WebDavProvider.h"

namespace {
// 登记表自检：键非空且互不相同、静态条目必须在册。
// 不符记一条警告，不阻断启动；Debug 再叠 Q_ASSERT，把漏配第一时间炸出来
QList<ProviderCatalog::Entry> buildCatalog() {
    QList<ProviderCatalog::Entry> table = {
        {kStaticProviderKey, &StaticProvider::displayName, &createStaticSettingsPage,
         &StaticProvider::load},
        {kDirProviderKey, &DirProvider::displayName, &createDirSettingsPage, &DirProvider::load},
        {kWebDavProviderKey, &WebDavProvider::displayName, &createWebDavSettingsPage,
         &WebDavProvider::load},
        {kExternalProviderKey, &ExternalProvider::displayName, &createExternalSettingsPage,
         &ExternalProvider::load},
        {kJellyfinProviderKey, &JellyfinProvider::displayName, &createJellyfinSettingsPage,
         &JellyfinProvider::load},
#if defined(OXBOW_WITH_RCLONE_CRYPT)
        {kRcloneCryptProviderKey, &RcloneCryptProvider::displayName, &createRcloneCryptSettingsPage,
         &RcloneCryptProvider::load},
#endif
    };
    QSet<QString> seen;
    for (int i = 0; i < table.size(); ++i) {
        const QString key = QString::fromLatin1(table.at(i).typeKey);
        if (key.isEmpty())
            OXBOW_LOG_WARNING() << "ProviderCatalog: entry" << i << "carries an empty type key";
        else if (seen.contains(key))
            OXBOW_LOG_WARNING() << "ProviderCatalog: duplicate type key" << key;
        seen.insert(key);
    }
    const QString staticKey = QString::fromLatin1(kStaticProviderKey);
    if (!seen.contains(staticKey))
        OXBOW_LOG_WARNING() << "ProviderCatalog: static provider entry is missing";
    Q_ASSERT(seen.contains(staticKey));
    return table;
}
} // namespace

const QList<ProviderCatalog::Entry> &ProviderCatalog::entries() {
    static const QList<ProviderCatalog::Entry> table = buildCatalog();
    return table;
}

std::shared_ptr<PlaylistProvider>
ProviderCatalog::createStaticProvider(const std::shared_ptr<ServiceRegistry> &registry) {
    return std::make_shared<StaticProvider>(registry);
}

std::shared_ptr<PlaylistProvider>
ProviderCatalog::loadProvider(QSettings &settings,
                              const std::shared_ptr<ServiceRegistry> &registry) {
    const QString kind = settings.value(QStringLiteral("provider")).toString();
    for (const Entry &entry : entries()) {
        if (kind == QLatin1String(entry.typeKey)) {
            if (auto provider = entry.load(settings, registry))
                return provider;
            break; // 工厂返空按未知处理，与键缺失同档
        }
    }
    // 写死内建静态列表：回落身份不随条目顺序漂移
    return StaticProvider::load(settings, registry);
}

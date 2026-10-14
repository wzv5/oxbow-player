#include "pch.h"

#include "ProviderCatalog.h"

// 本文件是具体提供者的唯一聚合点（N44 §2.7）：登记表条目要引用各提供者的
// 键常量、displayName 与工厂，故只有此处包含提供者头文件，其余源文件一律
// 经 ProviderCatalog.h 取用
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
// 登记表自检（N44 §2.7 第 ② 层守候）：键非空且互不相同、第 0 项 createDefault 非空。
// 不符即 qWarning——沿用 R4 数据目录自检的路子，不因自检失败阻断启动；
// Debug 期再叠 Q_ASSERT，把登记表漏配在第一时间炸出来
QList<ProviderEntry> buildCatalog() {
    QList<ProviderEntry> table = {
        {kStaticProviderKey, &StaticProvider::displayName, &createStaticSettingsPage,
         &StaticProvider::load, &StaticProvider::createDefault},
        {kDirProviderKey, &DirProvider::displayName, &createDirSettingsPage, &DirProvider::load,
         nullptr},
        {kWebDavProviderKey, &WebDavProvider::displayName, &createWebDavSettingsPage,
         &WebDavProvider::load, nullptr},
        {kExternalProviderKey, &ExternalProvider::displayName, &createExternalSettingsPage,
         &ExternalProvider::load, nullptr},
        {kJellyfinProviderKey, &JellyfinProvider::displayName, &createJellyfinSettingsPage,
         &JellyfinProvider::load, nullptr},
#if defined(OXBOW_WITH_RCLONE_CRYPT)
        // rclone crypt 只读后端整条特性可关（CMake 的 OXBOW_WITH_RCLONE_CRYPT）：
        // 关掉的构建里本行连同提供者与算法层都不存在，登记表按剩余条目照常工作
        {kRcloneCryptProviderKey, &RcloneCryptProvider::displayName, &createRcloneCryptSettingsPage,
         &RcloneCryptProvider::load, nullptr},
#endif
    };
    QSet<QString> seen;
    for (int i = 0; i < table.size(); ++i) {
        const QString key = QString::fromLatin1(table.at(i).typeKey);
        if (key.isEmpty())
            qWarning("ProviderCatalog: entry %d carries an empty type key", i);
        else if (seen.contains(key))
            qWarning("ProviderCatalog: duplicate type key \"%s\"", qPrintable(key));
        seen.insert(key);
    }
    Q_ASSERT(!table.isEmpty());
    Q_ASSERT(table.constFirst().createDefault != nullptr); // 第 0 项须能无参出生
    return table;
}
} // namespace

const QList<ProviderEntry> &providerCatalog() {
    static const QList<ProviderEntry> table = buildCatalog();
    return table;
}

std::shared_ptr<PlaylistProvider>
createDefaultProvider(const std::shared_ptr<ServiceRegistry> &registry) {
    const QList<ProviderEntry> &table = providerCatalog();
    Q_ASSERT(!table.isEmpty() && table.constFirst().createDefault != nullptr);
    return table.constFirst().createDefault(registry);
}

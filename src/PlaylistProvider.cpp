#include "pch.h"

#include "PlaylistProvider.h"
#include "ProviderCatalog.h"

PlaylistProvider::PlaylistProvider(const std::shared_ptr<ServiceRegistry> &registry)
    : m_registry(registry) {}

PlaylistProvider::~PlaylistProvider() = default;

void PlaylistProvider::setSortRule(int key, bool ascending) {
    m_sortKey = key;
    m_sortAscending = ascending;
}

// N44：写侧只认子类的 typeKey()，键常量与读侧（登记表条目）同源同点定义。
// Debug 期断言"写出的键必能在登记表中解开"——键写错的后果是该表重启后静默
// 退化为默认提供者，Release 下这段全编译掉
void PlaylistProvider::writeProviderKey(QSettings &settings) const {
    const QString key = typeKey();
#ifndef QT_NO_DEBUG
    bool hit = false;
    for (const ProviderEntry &entry : providerCatalog()) {
        if (key == QLatin1String(entry.typeKey)) {
            hit = true;
            break;
        }
    }
    Q_ASSERT(hit);
#endif
    settings.setValue(QStringLiteral("provider"), key);
}

// N44：分派改经登记表——键与实现的对应关系只有 providerCatalog() 一个出处，
// 新增提供者不必回改本函数
std::shared_ptr<PlaylistProvider>
PlaylistProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    const QString kind = settings.value(QStringLiteral("provider")).toString();
    for (const ProviderEntry &entry : providerCatalog()) {
        if (kind == QLatin1String(entry.typeKey))
            return entry.load(settings, registry);
    }
    // 键缺失 / 未知：回落登记表第 0 项（默认提供者）。与现状等价——现状即
    // "缺失 / 未知 → 静态"，而静态就是第 0 项
    Q_ASSERT(!providerCatalog().isEmpty());
    return providerCatalog().constFirst().load(settings, registry);
}

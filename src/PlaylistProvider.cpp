#include "pch.h"

#include "DirProvider.h"
#include "PlaylistProvider.h"
#include "StaticProvider.h"
#include "WebDavProvider.h"

PlaylistProvider::PlaylistProvider(const std::shared_ptr<ServiceRegistry> &registry)
    : m_registry(registry) {}

PlaylistProvider::~PlaylistProvider() = default;

void PlaylistProvider::setSortRule(int key, bool ascending) {
    m_sortKey = key;
    m_sortAscending = ascending;
}

std::shared_ptr<PlaylistProvider>
PlaylistProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    const QString kind = settings.value(QStringLiteral("provider")).toString();
    if (kind == QLatin1String("dir"))
        return DirProvider::load(settings, registry);
    if (kind == QLatin1String("webdav")) // N37：WebDAV 目录提供者
        return WebDavProvider::load(settings, registry);
    return StaticProvider::load(settings, registry); // 显式 static 与缺失键同路
}

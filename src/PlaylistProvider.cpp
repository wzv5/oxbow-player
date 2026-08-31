#include "pch.h"

#include "DirProvider.h"
#include "PlaylistProvider.h"
#include "StaticProvider.h"

PlaylistProvider::~PlaylistProvider() = default;

void PlaylistProvider::setSortRule(int key, bool ascending) {
    m_sortKey = key;
    m_sortAscending = ascending;
}

std::unique_ptr<PlaylistProvider> PlaylistProvider::load(QSettings &settings) {
    const QString kind = settings.value(QStringLiteral("provider")).toString();
    if (kind == QLatin1String("dir"))
        return DirProvider::load(settings);
    return StaticProvider::load(settings); // 显式 static 与缺失键同路
}

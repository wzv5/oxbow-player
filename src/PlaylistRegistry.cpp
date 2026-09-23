#include "pch.h"

#include "PlaylistRegistry.h"

QString defaultListDisplayName() {
    return QCoreApplication::translate("PlaylistRegistry", kDefaultListNameSrc);
}

PlaylistRegistry::PlaylistRegistry(std::shared_ptr<PlaylistProvider> defaultProvider) {
    Q_ASSERT(defaultProvider != nullptr); // 装配顶点交成品，本类不调提供者工厂
    m_default = std::make_unique<Playlist>(kDefaultListUuid, defaultListDisplayName(),
                                           std::move(defaultProvider));
}

Playlist *PlaylistRegistry::find(const QUuid &uuid) const {
    if (uuid.isNull())
        return nullptr; // nil 即无身份，恒不命中（本类不产生 nil 身份的列表）
    if (m_default->uuid() == uuid)
        return m_default.get();
    for (const auto &list : m_persistent)
        if (list->uuid() == uuid)
            return list.get();
    return nullptr;
}

QString PlaylistRegistry::nameOf(const QUuid &uuid) const {
    const Playlist *list = find(uuid);
    return list ? list->name() : QString();
}

QStringList PlaylistRegistry::names(const QUuid &except) const {
    QStringList out;
    out.reserve(int(m_persistent.size()) + 1);
    if (m_default->uuid() != except)
        out << m_default->name();
    for (const auto &list : m_persistent)
        if (list->uuid() != except)
            out << list->name();
    return out;
}

int PlaylistRegistry::persistentCount() const { return int(m_persistent.size()); }

Playlist *PlaylistRegistry::persistentAt(int i) const {
    Q_ASSERT(i >= 0 && i < persistentCount()); // 越界属调用方违约
    if (i < 0 || i >= persistentCount())
        return nullptr;
    return m_persistent.at(size_t(i)).get();
}

QUuid PlaylistRegistry::mintUuid() const {
    for (;;) {
        const QUuid candidate = QUuid::createUuid();
        if (!find(candidate))
            return candidate; // 与查询同一份扫描：构造性不撞车（§2.4）
    }
}

Playlist *PlaylistRegistry::append(QString name, std::shared_ptr<PlaylistProvider> provider,
                                   const QUuid &storedUuid) {
    Q_ASSERT(provider != nullptr); // 一次成型：不存在无提供者的中间态
    // 空名列表不存在——本断言与 readTabGroup 跳无名节、对话框拒空名共同钉死该前提，
    // `nameOf` 的"空返回即无此列表"判据全靠它
    Q_ASSERT(!name.isEmpty());
    // 接回或新铸：存量值为 nil（缺键）或已在册即另铸，否则接回（撞车语义与旧会话
    // 占用表同形；保留身份 kDefaultListUuid 本就恒在册，故任何接回它的请求都判撞车）
    const QUuid uuid = (storedUuid.isNull() || find(storedUuid)) ? mintUuid() : storedUuid;
    m_persistent.push_back(std::make_unique<Playlist>(uuid, std::move(name), std::move(provider)));
    return m_persistent.back().get();
}

bool PlaylistRegistry::remove(const QUuid &uuid) {
    Q_ASSERT(uuid != kDefaultListUuid); // 默认列表不可摘除（§2.5）
    for (auto it = m_persistent.begin(); it != m_persistent.end(); ++it) {
        if ((*it)->uuid() == uuid) {
            m_persistent.erase(it); // 列表对象的唯一析构点
            return true;
        }
    }
    return false;
}

#include "pch.h"

#include "Playlist.h"

#include "PlaylistRegistry.h" // kDefaultListUuid（保留身份的唯一出处）

Playlist::Playlist(const QUuid &uuid, QString name, std::shared_ptr<PlaylistProvider> provider)
    : m_uuid(uuid), m_name(std::move(name)), m_provider(std::move(provider)) {
    Q_ASSERT(!m_uuid.isNull() && m_provider != nullptr);
}

bool Playlist::isDefault() const { return m_uuid == kDefaultListUuid; }

void Playlist::setName(QString name) {
    Q_ASSERT(!isDefault()); // 默认列表不可改名（§2.5）
    m_name = std::move(name);
}

void Playlist::setProvider(std::shared_ptr<PlaylistProvider> provider) {
    Q_ASSERT(!isDefault()); // 默认列表不可换源（§2.5）
    Q_ASSERT(provider != nullptr);
    m_provider = std::move(provider); // 身份与名字不动
}

QStringList Playlist::playUrls(const PlaylistEntity::List &entities) const {
    QStringList out;
    out.reserve(int(entities.size()));
    // N55 §2.3：归属化后形态解析只剩唯一出口 entity->playUrl()——锁实体自己的归属
    // 提供者现算，"所在列表代答"的分岔整体删除（收编搬走内容后来源不再被换掉）。
    // 空槽位仍回空串：队列对位需要占位，非"恒等退化"
    for (const auto &entity : entities)
        out.append(entity ? entity->playUrl() : QString());
    return out;
}

void Playlist::save(QSettings &settings) const {
    settings.setValue(QStringLiteral("name"), name());
    // N57 §2.4：身份随列表对象落（原先由 writeProviderKey 与类型键同点写出）。
    // 键名与字面形态逐字不变，键出现顺序不作约束（可观察差分①）
    settings.setValue(QStringLiteral("uuid"), uuid().toString(QUuid::StringFormat::WithoutBraces));
}

#include "pch.h"

#include "Playlist.h"

#include "PlaylistRegistry.h" // kDefaultListUuid（保留身份的唯一出处）
#include "StreamTransit.h"    // N65：仅 Debug 断言"转接项必带 cache=yes"（头注释
                              // 允许依赖方已列明本类，L1 叶子地位不变）

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

QVector<PlayTarget> Playlist::playTargets(const PlaylistEntity::List &entities) const {
    QVector<PlayTarget> out;
    out.reserve(int(entities.size()));
    for (const auto &entity : entities) {
        // 空槽位仍回空串 + 空 map：队列对位需要占位，非"恒等退化"（N55 §2.3）
        if (!entity) {
            out.append(PlayTarget{});
            continue;
        }
        // N65：唯一合并点——提供者产出的 PlayTarget 为底（转接项的 cache=yes
        // 在铸造点随串出生），条目级选项逐键 insert 覆盖同键
        PlayTarget target = entity->playTarget();
        const QVariantMap &entryOptions = entity->playOptions();
        for (auto it = entryOptions.constBegin(); it != entryOptions.constEnd(); ++it)
            target.options.insert(it.key(), it.value());
        // N65 显影：转接项必带 cache（N52 §2.10 语义）。可达性由构造排除——
        // StreamTransit::mint 的唯一调用点就是交出 cache 的那一句；断言兜住
        // "提供者实现漂移漏带"，控制器已退成纯管道、不再有静默补齐（本类是
        // StreamTransit.h 的唯一新增消费点，仅 Debug 生效）
        Q_ASSERT(!StreamTransit::isTransitUri(target.url) ||
                 target.options.value(QStringLiteral("cache")).toString() == QLatin1String("yes"));
        out.append(std::move(target));
    }
    return out;
}

void Playlist::save(QSettings &settings) const {
    settings.setValue(QStringLiteral("name"), name());
    // N57 §2.4：身份随列表对象落（原先由 writeProviderKey 与类型键同点写出）。
    // 键名与字面形态逐字不变，键出现顺序不作约束（可观察差分①）
    settings.setValue(QStringLiteral("uuid"), uuid().toString(QUuid::StringFormat::WithoutBraces));
}

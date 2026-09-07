#include "pch.h"

#include "StaticProvider.h"

PlaylistProvider::Type StaticProvider::type() const { return Type::Static; }

QString StaticProvider::describe() const { return QObject::tr("Local static list"); }

// 从实体提取 playUrl 覆盖最小表示（未来加标题等字段不改签名）。
// 不变式：只提取标量，不保存实体指针
void StaticProvider::setItems(const EntityList &entities) {
    QStringList urls;
    urls.reserve(int(entities.size()));
    for (const auto &entity : entities)
        urls << entity->playUrl();
    m_urls = std::move(urls);
}

// 现场造实体并预热文件元数据后移交视图（静态列表永成功）
std::expected<PlaylistProvider::EntityList, QString> StaticProvider::enumerate() const {
    EntityList entities;
    entities.reserve(m_urls.size());
    for (const QString &url : m_urls) {
        auto entity = PlaylistEntity::fromPlayUrl(url);
        entity->fileMeta(); // 预热（同步填充路径在 GUI 线程执行同一惰性入口）
        entities.push_back(std::move(entity));
    }
    return entities;
}

bool StaticProvider::supportsRemove() const { return true; }

bool StaticProvider::supportsSort() const { return true; }

void StaticProvider::saveSettings(QSettings &settings) const {
    // 静态列表不持久化排序键（顺序即添加顺序）
    settings.setValue(QStringLiteral("provider"), QStringLiteral("static"));
    settings.setValue(QStringLiteral("itemCount"), m_urls.size());
    for (int j = 0; j < m_urls.size(); ++j)
        settings.setValue(QString::number(j), m_urls[j]);
}

std::unique_ptr<StaticProvider> StaticProvider::load(QSettings &settings) {
    auto provider = std::make_unique<StaticProvider>();
    // B6：按显式计数字段逐条读取。childKeys() 顺序未定义，键多时呈字典序
    // （"0","1","10","100","11"...），百项级列表会乱序呈现，不可用
    const int n = settings.value(QStringLiteral("itemCount"), 0).toInt();
    QStringList urls;
    urls.reserve(n);
    for (int j = 0; j < n; ++j) {
        const QString file = settings.value(QString::number(j)).toString();
        if (!file.isEmpty())
            urls << file;
    }
    provider->m_urls = std::move(urls);
    return provider;
}

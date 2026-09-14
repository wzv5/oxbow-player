#include "pch.h"

#include "StaticProvider.h"

namespace {
// 非 Q_OBJECT 类的用户可见串：英文源 + QT_TRANSLATE_NOOP 标注翻译上下文
// （供 lupdate 提取），运行期经 QCoreApplication::translate 取值——无译文时即
// 英文原文（N34/HttpClient 同款约定）
QString trText(const char *source) { return QCoreApplication::translate("StaticProvider", source); }
} // namespace

PlaylistProvider::Type StaticProvider::type() const { return Type::Static; }

StaticProvider::StaticProvider(const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry) {}

QString StaticProvider::describe() const {
    return trText(QT_TRANSLATE_NOOP("StaticProvider", "Local static list"));
}

// 从实体提取 playUrl 覆盖最小表示（未来加标题等字段不改签名）。
// 不变式：只提取标量，不保存实体指针
void StaticProvider::setItems(const EntityList &entities) {
    QStringList urls;
    urls.reserve(int(entities.size()));
    for (const auto &entity : entities)
        urls << entity->playUrl();
    m_urls = std::move(urls);
}

// 现场造实体后移交视图（静态列表永成功，警告恒空）；N29 起不预热元数据，
// 按需经预读骨架供给。registry 经基类转交 fromPlayUrl：持久化的 http 条目
// 重载后仍得 HttpEntity
std::expected<PlaylistProvider::Enumeration, QString> StaticProvider::enumerate() const {
    EntityList entities;
    entities.reserve(m_urls.size());
    for (const QString &url : m_urls)
        entities.push_back(PlaylistEntity::fromPlayUrl(url, registry()));
    return Enumeration{std::move(entities), {}}; // 静态列表无警告
}

bool StaticProvider::supportsModify() const { return true; }

void StaticProvider::saveSettings(QSettings &settings) const {
    // 静态列表不持久化排序键（顺序即添加顺序）
    settings.setValue(QStringLiteral("provider"), QStringLiteral("static"));
    settings.setValue(QStringLiteral("itemCount"), m_urls.size());
    for (int j = 0; j < m_urls.size(); ++j)
        settings.setValue(QString::number(j), m_urls[j]);
}

std::shared_ptr<StaticProvider>
StaticProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    auto provider = std::make_shared<StaticProvider>(registry);
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

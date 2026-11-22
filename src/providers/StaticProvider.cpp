#include "pch.h"

#include "StaticProvider.h"

#include "ProviderSettingsPage.h"
#include "Util.h"
#include "streams/StreamTransit.h"

// 静态列表无参数，但不放空页——空白页会被误认为渲染错误
class StaticSettingsPage : public ProviderSettingsPage {
    Q_OBJECT
  public:
    StaticSettingsPage(const StaticProvider *seed, const std::shared_ptr<ServiceRegistry> &registry,
                       QWidget *parent)
        : ProviderSettingsPage(parent), m_seed(seed), m_registry(registry) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *note = new QLabel(tr("No configuration required."));
        note->setAlignment(Qt::AlignCenter);
        layout->addStretch(1);
        layout->addWidget(note);
        layout->addStretch(1);
    }

    std::optional<QString> validate() const override { return std::nullopt; }
    bool hasChanges() const override { return m_seed == nullptr; }
    std::shared_ptr<PlaylistProvider> build() const override {
        return std::make_shared<StaticProvider>(m_registry);
    }

  private:
    const StaticProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
};

ProviderSettingsPage *createStaticSettingsPage(const PlaylistProvider *seed,
                                               const std::shared_ptr<ServiceRegistry> &registry,
                                               QWidget *parent) {
    return new StaticSettingsPage(seed ? static_cast<const StaticProvider *>(seed) : nullptr,
                                  registry, parent);
}

QString StaticProvider::typeKey() const { return QLatin1String(kStaticProviderKey); }

QString StaticProvider::displayName() {
    return QCoreApplication::translate("StaticProvider", kStaticProviderName);
}

StaticProvider::StaticProvider(const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry) {}

void StaticProvider::setItems(const EntityList &entities) {
    QStringList urls;
    urls.reserve(int(entities.size()));
    for (const auto &entity : entities)
        urls << entity->path();
    m_urls = std::move(urls);
}

// 平铺、不产容器——非空 parent 属调用方违约，Q_ASSERT 守候，Release 返回空结果。
// registry 转交 fromPath：持久化的 http 条目重载后仍得 HttpEntity
std::expected<PlaylistProvider::Enumeration, QString>
StaticProvider::enumerate(const QStringList &parent) const {
    if (!parent.isEmpty()) {
        Q_ASSERT(false);
        return Enumeration{};
    }
    EntityList entities;
    entities.reserve(m_urls.size());
    for (const QString &url : m_urls)
        entities.push_back(PlaylistEntity::fromPath(url, registry()));
    Enumeration out;
    out.entities = std::move(entities);
    return out;
}

bool StaticProvider::supportsModify() const { return true; }

void StaticProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // 类型键经基类取 typeKey()
    // 静态列表不持久化排序键（顺序即添加顺序）
    settings.setValue(QStringLiteral("itemCount"), m_urls.size());
    for (int j = 0; j < m_urls.size(); ++j)
        settings.setValue(QString::number(j), m_urls[j]);
}

std::shared_ptr<PlaylistProvider>
StaticProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    auto provider = std::make_shared<StaticProvider>(registry);
    // 按显式计数逐条读：childKeys() 顺序未定义，键多时呈字典序（"0","1","10"…），
    // 百项级列表会乱序。三步分拣（统一按去首尾空白后的串）：空值丢；转接串丢——
    // 端点只活在会话内，且必须排在归一之前：toIdentity 首行 unwrap，先归一等于把
    // oxbow:// 悄悄救成身份；其余归一，产物仍非定形者丢
    const int n = settings.value(QStringLiteral("itemCount"), 0).toInt();
    QStringList urls;
    urls.reserve(n);
    for (int j = 0; j < n; ++j) {
        const QString file = settings.value(QString::number(j)).toString().trimmed();
        if (file.isEmpty())
            continue;
        if (StreamTransit::isTransitUri(file))
            continue; // 跨会话必死，不进提供者
        const QString identity = Util::Url::toIdentity(file);
        if (Util::Url::isDefiniteForm(identity))
            urls << identity;
    }
    provider->m_urls = std::move(urls);
    return provider;
}

// 本文件定义 Q_OBJECT 类（StaticSettingsPage），AUTOMOC 要求显式包含同名 .moc
#include "StaticProvider.moc"

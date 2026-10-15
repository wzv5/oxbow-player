#include "pch.h"

#include "StaticProvider.h"

#include "ProviderSettingsPage.h"
#include "Util.h"
#include "streams/StreamTransit.h"

// 静态列表的参数页（N44 §2.5.1）：本提供者无参数，但**不放空页**——空白页会被
// 误认为渲染错误，一句说明既避免误判、也顺带交代"该提供者无参数"。
// 页面类只在本文件可见（不进头文件），经 createStaticSettingsPage 产出
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
        // 上下各一个伸缩项：提示在表单区内水平 + 垂直居中
        layout->addStretch(1);
        layout->addWidget(note);
        layout->addStretch(1);
    }

    // 无参数可校验
    std::optional<QString> validate() const override { return std::nullopt; }
    // 静态无参数，同类型即未变——有回显来源（存量静态列表）判未变，无则必产出
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
    // 同类型由调用方（对话框）保证：seed 非空即本类型
    return new StaticSettingsPage(seed ? static_cast<const StaticProvider *>(seed) : nullptr,
                                  registry, parent);
}

QString StaticProvider::typeKey() const { return QLatin1String(kStaticProviderKey); }

QString StaticProvider::displayName() {
    return QCoreApplication::translate("StaticProvider", kStaticProviderName);
}

StaticProvider::StaticProvider(const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry) {}

// 从实体提取身份串落最小表示（未来加标题等字段不改签名）。
// 不变式：只提取标量，不保存实体指针
void StaticProvider::setItems(const EntityList &entities) {
    QStringList urls;
    urls.reserve(int(entities.size()));
    for (const auto &entity : entities)
        urls << entity->path();
    m_urls = std::move(urls);
}

// 现场造实体后移交视图（静态列表永成功，警告恒空）；N29 起不预热元数据，
// 按需经预读骨架供给。registry 经基类转交 fromPath：持久化的 http 条目
// 重载后仍得 HttpEntity。
// N45：静态保持平铺、不产容器——非空 parent 属调用方违约，Q_ASSERT 守候，
// release 降级返回空结果（不得静默忽略，否则视图侧 bug 会被吞掉）
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
    return out; // 静态列表无警告（N64 起 Enumeration 继承 LevelData，不再位置聚合初始化）
}

bool StaticProvider::supportsModify() const { return true; }

void StaticProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // N44：类型键经基类取 typeKey()，不再写字面量
    // 静态列表不持久化排序键（顺序即添加顺序）
    settings.setValue(QStringLiteral("itemCount"), m_urls.size());
    for (int j = 0; j < m_urls.size(); ++j)
        settings.setValue(QString::number(j), m_urls[j]);
}

std::shared_ptr<PlaylistProvider>
StaticProvider::createDefault(const std::shared_ptr<ServiceRegistry> &registry) {
    return std::make_shared<StaticProvider>(registry);
}

std::shared_ptr<PlaylistProvider>
StaticProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    auto provider = std::make_shared<StaticProvider>(registry);
    // B6：按显式计数字段逐条读取。childKeys() 顺序未定义，键多时呈字典序
    // （"0","1","10","100","11"...），百项级列表会乱序呈现，不可用
    // N105 §3：读侧三步分拣（统一按去首尾空白后的串），尽量救回用户手写的正常写法：
    // ① 空值丢（手写的 "1="）；② 转接串丢——端点会话内、跨会话必死（N55 §3.6 同一句
    // 判据），且必须排在归一之前：toIdentity 首行 unwrap，先归一等于把 oxbow://
    // 悄悄救成身份；③ 其余归一（toIdentity，不访问文件系统的纯字符串变换），产物
    // 仍非定形者丢（实测该族只有裸 file:）
    const int n = settings.value(QStringLiteral("itemCount"), 0).toInt();
    QStringList urls;
    urls.reserve(n);
    for (int j = 0; j < n; ++j) {
        const QString file = settings.value(QString::number(j)).toString().trimmed();
        if (file.isEmpty())
            continue; // ① 空值
        if (StreamTransit::isTransitUri(file))
            continue;                                         // ② 转接串：跨会话必死，不进提供者
        const QString identity = Util::Url::toIdentity(file); // ③ 相对补绝对、file:// 折本地
        if (Util::Url::isDefiniteForm(identity))
            urls << identity;
    }
    provider->m_urls = std::move(urls);
    return provider;
}

// N44：本文件定义了 Q_OBJECT 类（StaticSettingsPage），CMake AUTOMOC 要求显式包含
// 同名 .moc（否则 configure 期直接报错），按源文件名而非类名
#include "StaticProvider.moc"

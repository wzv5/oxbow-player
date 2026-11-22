#include "pch.h"

#include "PlaylistProvider.h"
#include "ProviderCatalog.h"
#include "ServiceRegistry.h" // resolve<StreamRelayIf> 需完整类型

// 只依赖 L1 与转接接缝，不碰 HttpClient、不碰任何传输头
#include "Util.h"
#include "streams/StreamEndpoint.h"
#include "streams/StreamRelay.h"
#include "streams/StreamTransit.h"

PlaylistProvider::PlaylistProvider(const std::shared_ptr<ServiceRegistry> &registry)
    : m_registry(registry) {}

PlaylistProvider::~PlaylistProvider() = default;

// staging 已由调用方定位；本函数只管写读同点，不认识介质与分组形态
std::shared_ptr<PlaylistProvider> PlaylistProvider::duplicated(QSettings &staging) const {
    saveSettings(staging);
    auto copy = load(staging, m_registry);
    copy->setSortRule(sortRule());
    return copy;
}

// 三态表即本函数的定义
bool PlaylistProvider::usesTransit() const {
    switch (transitCapability()) {
    case TransitCapability::Unsupported:
        return false;
    case TransitCapability::Optional:
        return m_transitPreferred;
    case TransitCapability::Required:
        return true;
    }
    return false;
}

std::optional<bool> PlaylistProvider::transitPreference() const {
    if (transitCapability() == TransitCapability::Optional)
        return m_transitPreferred;
    return std::nullopt; // 形态不由偏好决定
}

void PlaylistProvider::setTransitPreference(bool preferred) { m_transitPreferred = preferred; }

// Unsupported 时必空；Optional/Required 返空 = 编程错误
std::shared_ptr<StreamEndpoint> PlaylistProvider::createStreamEndpoint() const { return nullptr; }

// 必须轻、零网络：列表身份按值传入，提供者不持有；串与选项同句出生
PlayTarget PlaylistProvider::playTarget(const QUuid &listUuid, const QString &path,
                                        const QString &fileName) const {
    // 端点缓存与发布只在 GUI 线程发生，故无需加锁
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    Q_ASSERT(!listUuid.isNull()); // nil 身份 = 调用方违约
    if (!usesTransit())
        return {directPlayUrl(path), {}};

    auto relay = registry() ? registry()->resolve<StreamRelayIf>() : nullptr;
    Q_ASSERT(relay); // 空 = 装配顺序错误；Release 仍铸 URI，open 必失败
    if (!m_transitEndpoint) {
        m_transitEndpoint = createStreamEndpoint();
        Q_ASSERT(m_transitEndpoint);
    }
    if (relay)
        relay->publish(listUuid, m_transitEndpoint);
    // 入参先剥凭据；L1 侧另有双保险（Release 拒铸返空串）
    return {StreamTransit::mint(listUuid, Util::Url::stripCredentials(path), fileName),
            QVariantMap{{QStringLiteral("cache"), QStringLiteral("yes")}}};
}

// Debug 断言"写出的键必能在登记表中解开"，键写错会让重启后按未知键回落内建静态列表。
// transit 只对 Optional 写出，其余清掉可能被手改塞入的存量键
void PlaylistProvider::writeProviderKey(QSettings &settings) const {
    const QString key = typeKey();
#ifndef QT_NO_DEBUG
    bool hit = false;
    for (const ProviderCatalog::Entry &entry : ProviderCatalog::entries()) {
        if (key == QLatin1String(entry.typeKey)) {
            hit = true;
            break;
        }
    }
    Q_ASSERT(hit);
#endif
    settings.setValue(QStringLiteral("provider"), key);
    if (transitCapability() == TransitCapability::Optional)
        settings.setValue(QStringLiteral("transit"), m_transitPreferred ? 1 : 0);
    else
        settings.remove(QStringLiteral("transit"));
}

// 键与实现的对应只有 entries() 一处。列表身份归 PlaylistRegistry，本函数不读 uuid；
// 无 transit 键按默认偏好（false）处理
std::shared_ptr<PlaylistProvider>
PlaylistProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    const std::shared_ptr<PlaylistProvider> entry =
        ProviderCatalog::loadProvider(settings, registry);
    if (const QVariant transit = settings.value(QStringLiteral("transit")); transit.isValid())
        entry->setTransitPreference(transit.toBool());
    return entry;
}

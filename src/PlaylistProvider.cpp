#include "pch.h"

#include "PlaylistProvider.h"
#include "ProviderCatalog.h"
#include "ServiceRegistry.h" // resolve<StreamRelayIf>（模板实例化需完整类型）

// N52：playUrl 的转接分支取用面——StreamRelayIf 为纯头抽象基（无实现体），
// 测试链接不携带 L4；StreamTransit / Util 为 L1 与纯函数。依赖方向：
// 提供者基类 → L1 + L4 取用接缝，不碰 HttpClient、不碰任何传输头
#include "StreamEndpoint.h"
#include "StreamRelay.h"
#include "StreamTransit.h"
#include "Util.h"

PlaylistProvider::PlaylistProvider(const std::shared_ptr<ServiceRegistry> &registry)
    : m_registry(registry) {}

PlaylistProvider::~PlaylistProvider() = default;

// N62 §2.3：复制 = 经唯一序列化器的一次往返。调用方已把 staging 定位在暂存组内，
// 本函数只管写与读同点、排序规则原样交入，不认识介质与分组形态
std::shared_ptr<PlaylistProvider> PlaylistProvider::duplicated(QSettings &staging) const {
    saveSettings(staging);
    auto copy = load(staging, m_registry);
    copy->setSortRule(sortRule());
    return copy;
}

// N52 §2.8：三态表即本函数的定义——全仓只有它读那个持久化偏好
bool PlaylistProvider::usesTransit() const {
    switch (transitCapability()) {
    case TransitCapability::Unsupported:
        return false; // 偏好值被忽略，写了也不算
    case TransitCapability::Optional:
        return m_transitPreferred; // 跟随偏好
    case TransitCapability::Required:
        return true; // 偏好值被忽略，手改 INI 关不掉
    }
    return false;
}

std::optional<bool> PlaylistProvider::transitPreference() const {
    if (transitCapability() == TransitCapability::Optional)
        return m_transitPreferred;
    return std::nullopt; // Required / Unsupported：形态不由偏好决定
}

void PlaylistProvider::setTransitPreference(bool preferred) { m_transitPreferred = preferred; }

// Unsupported 时必空（基类恒等实现）；Optional/Required 返空 = 编程错误（调用侧断言守候）
std::shared_ptr<StreamEndpoint> PlaylistProvider::createStreamEndpoint() const { return nullptr; }

// N52 §2.8：播放形态模板方法（非虚公开出口，约 10 行，必须轻——零网络）。
// N57 §2.4：列表身份按值入参，提供者不持有也不镜像它。
// N65：出口升为 playTarget——串与它的逐文件选项在同一句 return 里出生：直发支
// 无选项；转接支随串交出 cache=yes（N52 §2.10 的申报语义一字不改，落点从
// MpvController::loadFile 内的 scheme 反推搬回铸造点——谁知道事实谁就交选项；
// "转接项必带 cache"的显影由 Playlist::playTargets 的 Debug 断言守候）
PlayTarget PlaylistProvider::playTarget(const QUuid &listUuid, const QString &path) const {
    // 端点缓存与发布只在 GUI 线程发生（Playlist::playTargets 整表逐条调用的既有
    // 线程口径），故缓存无需加锁；demux 线程只经 relay 表中的 weak_ptr 取用
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    Q_ASSERT(!listUuid.isNull()); // nil 身份 = 调用方违约（Release 由 mint 返空显影）
    if (!usesTransit())
        return {directPlayUrl(path), {}};

    auto relay = registry() ? registry()->resolve<StreamRelayIf>() : nullptr;
    Q_ASSERT(relay); // 空 = 装配顺序错误（编程错误）；Release 仍铸 URI——open 必失败，显影不静默
    if (!m_transitEndpoint) {
        m_transitEndpoint = createStreamEndpoint();
        Q_ASSERT(m_transitEndpoint); // Optional/Required 返空 = 编程错误
    }
    if (relay)
        relay->publish(listUuid, m_transitEndpoint);
    // 入参先剥凭据（N48 纪律在协议出口的延伸）；L1 侧另有凭据双保险（Q_ASSERT +
    // Release 拒铸返空串，调用方按 open 必失败处理）
    return {StreamTransit::mint(listUuid, Util::Url::stripCredentials(path)),
            QVariantMap{{QStringLiteral("cache"), QStringLiteral("yes")}}};
}

// N44：写侧只认子类的 typeKey()，键常量与读侧（登记表条目）同源同点定义。
// Debug 期断言"写出的键必能在登记表中解开"——键写错的后果是该表重启后静默
// 退化为默认提供者，Release 下这段全编译掉。
// N57 §2.4：本函数此后只写类型键与转接偏好两键——列表身份随 Playlist 一侧落
// （uuid 键名与字面形态不变，只是改写点了家）。transit 键只对 Optional 类型
// 写出（Unsupported / Required 顺手清掉可能被手改塞入的存量键）——编辑 Required
// 列表不产生 transit 键，形态由三态表承载
void PlaylistProvider::writeProviderKey(QSettings &settings) const {
    const QString key = typeKey();
#ifndef QT_NO_DEBUG
    bool hit = false;
    for (const ProviderEntry &entry : providerCatalog()) {
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

// N44：分派改经登记表——键与实现的对应关系只有 providerCatalog() 一个出处，
// 新增提供者不必回改本函数。
// N57 §2.4：本函数此后不读 uuid——列表身份的读回归 PlaylistRegistry / readTabGroup
// 一侧（旧 INI 无 uuid 键按缺键处理，不迁移）；无 transit 键按默认偏好（false）处理
std::shared_ptr<PlaylistProvider>
PlaylistProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    const QString kind = settings.value(QStringLiteral("provider")).toString();
    std::shared_ptr<PlaylistProvider> entry;
    for (const ProviderEntry &catalogEntry : providerCatalog()) {
        if (kind == QLatin1String(catalogEntry.typeKey)) {
            entry = catalogEntry.load(settings, registry);
            break;
        }
    }
    if (!entry) {
        // 键缺失 / 未知：回落登记表第 0 项（默认提供者）。与现状等价——现状即
        // "缺失 / 未知 → 静态"，而静态就是第 0 项
        Q_ASSERT(!providerCatalog().isEmpty());
        entry = providerCatalog().constFirst().load(settings, registry);
    }
    if (const QVariant transit = settings.value(QStringLiteral("transit")); transit.isValid())
        entry->setTransitPreference(transit.toBool());
    return entry;
}

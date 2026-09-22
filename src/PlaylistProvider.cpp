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

namespace {

// N52 §2.7：listUuid 会话占用表——本篇唯一新增的进程级状态（文件内静态表 +
// mutex 持有）。构造期新铸查重与 adoptListUuid 的接回/撞车判定共用；会话级，
// 不落盘（提供者析构不释放占用：编辑换身期间新旧实例并存，身份恒被接回）。
QMutex g_uuidMutex;
QSet<QString> g_occupiedUuids;

// 新铸一个未占用身份（占用表内插入与查重同一把锁，构造性不撞车）
QString mintListUuid() {
    QMutexLocker l(&g_uuidMutex);
    for (;;) {
        const QString u = QUuid::createUuid().toString(QUuid::StringFormat::WithoutBraces);
        if (!g_occupiedUuids.contains(u)) {
            g_occupiedUuids.insert(u);
            return u;
        }
    }
}

} // namespace

PlaylistProvider::PlaylistProvider(const std::shared_ptr<ServiceRegistry> &registry)
    : m_registry(registry), m_listUuid(mintListUuid()) {}

PlaylistProvider::~PlaylistProvider() = default;

// N52 §2.7：存量身份接回。语义三档——存量复用（未占用即认领）/ INI 撞车弃存量
// 值自铸新值保新（allowBusy=false 且已占用）/ 编辑换身在 allowBusy 下接回旧身份
// （占用表幂等认领，旧实例的同名占用被覆盖）。stored 为空（旧 INI 缺键）保留
// 构造期自铸身份，不迁移
void PlaylistProvider::adoptListUuid(const QString &stored, bool allowBusy) {
    if (stored.isEmpty() || stored == m_listUuid)
        return;
    QMutexLocker l(&g_uuidMutex);
    if (g_occupiedUuids.contains(stored) && !allowBusy)
        return; // 撞车：保留构造期自铸身份（已在占用表中），不adopt
    g_occupiedUuids.remove(m_listUuid);
    g_occupiedUuids.insert(stored);
    m_listUuid = stored;
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

// N52 §2.8：播放形态模板方法（非虚公开出口，约 10 行，必须轻——零网络）
QString PlaylistProvider::playUrl(const QString &path) const {
    // 端点缓存与发布只在 GUI 线程发生（playUrls 整表逐条调用的既有线程口径），
    // 故缓存无需加锁；demux 线程只经 relay 表中的 weak_ptr 取用
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    if (!usesTransit())
        return directPlayUrl(path);

    auto relay = registry() ? registry()->resolve<StreamRelayIf>() : nullptr;
    Q_ASSERT(relay); // 空 = 装配顺序错误（编程错误）；Release 仍铸 URI——open 必失败，显影不静默
    if (!m_transitEndpoint) {
        m_transitEndpoint = createStreamEndpoint();
        Q_ASSERT(m_transitEndpoint); // Optional/Required 返空 = 编程错误
    }
    if (relay)
        relay->publish(listUuid(), m_transitEndpoint);
    // 入参先剥凭据（N48 纪律在协议出口的延伸）；L1 侧另有凭据双保险（Q_ASSERT +
    // Release 拒铸返空串，调用方按 open 必失败处理）
    return StreamTransit::mint(listUuid(), Util::Url::stripCredentials(path));
}

void PlaylistProvider::setSortRule(int key, bool ascending) {
    m_sortKey = key;
    m_sortAscending = ascending;
}

// N44：写侧只认子类的 typeKey()，键常量与读侧（登记表条目）同源同点定义。
// Debug 期断言"写出的键必能在登记表中解开"——键写错的后果是该表重启后静默
// 退化为默认提供者，Release 下这段全编译掉。
// N52 §2.7：本函数是列表身份与类型键的同点落盘处——provider / uuid / transit
// 三键同点写出（各提供者 saveSettings 首行必调的既有约定不变，写侧四家零改动）。
// transit 键只对 Optional 类型写出（Unsupported / Required 顺手清掉可能被手改
// 塞入的存量键）——编辑 Required 列表不产生 transit 键，形态由三态表承载
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
    settings.setValue(QStringLiteral("uuid"), m_listUuid); // INI 键取 uuid，不带 list 前缀
    if (transitCapability() == TransitCapability::Optional)
        settings.setValue(QStringLiteral("transit"), m_transitPreferred ? 1 : 0);
    else
        settings.remove(QStringLiteral("transit"));
}

// N44：分派改经登记表——键与实现的对应关系只有 providerCatalog() 一个出处，
// 新增提供者不必回改本函数。
// N52 §2.7 读回：列表身份与转接偏好的统一后处理收口在此——各实现的 load 函数体
// 不动（读侧四家零改动）。旧 INI 无 uuid 键按缺键处理（adoptListUuid 对空串保留
// 自铸身份）；无 transit 键按默认偏好（false）处理；不设迁移代码、不保留旧形态
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
    entry->adoptListUuid(settings.value(QStringLiteral("uuid")).toString(), /*allowBusy=*/false);
    if (const QVariant transit = settings.value(QStringLiteral("transit")); transit.isValid())
        entry->setTransitPreference(transit.toBool());
    return entry;
}

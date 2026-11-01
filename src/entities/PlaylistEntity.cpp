#include "pch.h"

#include "PlaylistEntity.h"

#include "entities/HttpEntity.h"
#include "entities/JellyfinEntity.h"
#include "entities/LocalFileEntity.h"
#include "entities/StdinEntity.h"
#include "entities/UnknownEntity.h"
#if defined(OXBOW_WITH_RCLONE_CRYPT)
#include "entities/RcloneCryptEntity.h"
#endif
// N55：playUrl() 出口经归属提供者现算——实体侧唯一 include 提供者头之处
// （实体头只前置声明，无环）
#include "Util.h"
#include "providers/PlaylistProvider.h"

// N57 §2.9：排序键取值冻结（对 N58 的契约③）——既有取值不得重编，新键只准追加末尾
static_assert(int(SortKey::ReleaseDate) == 8);
static_assert(int(SortKey::EnumerationOrder) == 9);
static_assert(int(SortKey::DisplayName) == 10);

// N29：排序键 → 预读元数据类别的唯一映射（各判定点共用，防散写漂移）。
// N57 §2.9：随键搬到实体侧，视图的菜单灰态判据仍调它。
// None 档收三个不依赖预读的键：名称类三档与枚举序，加 N71 的 DisplayName
// （显示名枚举期即成型，与元数据无关）
MetaCategory metaCategoryOf(SortKey key) {
    switch (key) {
    case SortKey::Created:
    case SortKey::Modified:
    case SortKey::FileSize:
        return MetaCategory::File;
    case SortKey::Duration:
    case SortKey::ReleaseDate:
        return MetaCategory::Media;
    case SortKey::None:
    case SortKey::FileName:
    case SortKey::FileNameNumeric:
    case SortKey::Extension:
    case SortKey::EnumerationOrder:
    case SortKey::DisplayName:
        return MetaCategory::None;
    }
    return MetaCategory::None;
}

namespace {
// 能力位为假时元数据入口的静态空回落值（函数内 static 惰性构造）
const FileMeta &s_emptyFileMeta() {
    static const FileMeta empty;
    return empty;
}
const PlaylistEntity::MediaMeta &s_emptyMediaMeta() {
    static const PlaylistEntity::MediaMeta empty;
    return empty;
}
} // namespace

PlaylistEntity::PlaylistEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
    : m_path(std::move(path)), m_registry(registry) {
    deriveNames();
}

PlaylistEntity::Ptr PlaylistEntity::fromPath(const QString &path,
                                             const std::shared_ptr<ServiceRegistry> &registry) {
    // N56 §2.6：定形（oxbow:// 折叠、相对转绝对、file:// 转本地）上移到造实体的
    // 一方，工厂只按前缀分派——它可能不属于任何已知协议，只是"提供者当初拿来
    // 标识这条内容的那个串"，工厂对着读不懂的串加工等于猜。
    // Debug 断言：既非绝对路径、也非含 "://" 的形态即调用方违约——撤掉兜底后，
    // 未定形的串会静默落成 LocalFileEntity（exists() 按磁盘判、键按本地收敛），
    // 必须显影（release 不拦，行为与违约后契约一致）。判据走 Util::Url::isDefiniteForm
    // （与整形出口、fromMpvEcho 的兜底分派同一条，不各写一遍）
    Q_ASSERT(Util::Url::isDefiniteForm(path));
    if (Util::Url::isNetworkStream(path)) {
        // N35 前缀分派：http(s) → HttpEntity（需非空 registry）；其余网络 / 流
        // 路径仍落 UnknownEntity 兜底。N48：判据改用 Util::Url::isHttp（唯一持有者，
        // 串前缀 + scheme 归小写，天然免疫 QUrl::scheme() 误判 Windows 盘符）——
        // 原内联前缀判定是同一判据的第三份拷贝。
        // N69：jellyfin:// → JellyfinEntity（URN 身份，N35 预留的追加点；
        // 串由提供者铸造、形态恒小写，前缀判定精确匹配 kJellyfinScheme）
        if (path.startsWith(QLatin1String(kJellyfinScheme)))
            return std::make_shared<JellyfinEntity>(path, registry);
        // N98：rclonecrypt:// → RcloneCryptEntity（URN 身份，编码相对路径；
        // scheme 常量唯一持有者是 RcloneCryptEntity.h，提供者铸造与这里共用）。
        // 特性关掉的构建里没有这条分派：rclonecrypt:// 走下面的网络/流兜底落
        // UnknownEntity（含 "://" 恒非本地形态，不判存在性、不炸断言）
#if defined(OXBOW_WITH_RCLONE_CRYPT)
        if (path.startsWith(QLatin1String(kRcloneCryptScheme)))
            return std::make_shared<RcloneCryptEntity>(path, registry);
#endif
        if (!Util::Url::isHttp(path))
            return std::make_shared<UnknownEntity>(path, registry);
        if (!registry) {
            // http(s) + 空 registry 属调用错误（registry 必填，正常调用点恒非空）；
            // Q_ASSERT 守候，release 降级 UnknownEntity 保"恒返回实体"契约
            Q_ASSERT(false);
            return std::make_shared<UnknownEntity>(path, registry);
        }
        return std::make_shared<HttpEntity>(path, registry);
    }
    return std::make_shared<LocalFileEntity>(path, registry);
}

// N61 变更记录第 1 条：回显专用入口，语义与"为什么放宽发生在这里"见声明处注释。
// 与 fromPath 同一条分派、同一条判据，只把"违约显影"换成"降级保行"——不含任何加工
PlaylistEntity::Ptr PlaylistEntity::fromMpvEcho(const QString &identity,
                                                const std::shared_ptr<ServiceRegistry> &registry) {
    if (Util::Url::isMpvStdin(identity))
        return std::make_shared<StdinEntity>(registry);
    if (!Util::Url::isDefiniteForm(identity))
        return std::make_shared<UnknownEntity>(identity, registry); // 降级：保行、不炸
    return fromPath(identity, registry);
}

QString PlaylistEntity::key() const {
    return m_key; // 构造期缓存（见 deriveNames）
}

// N56 §2.5：键级判等的唯一实现点——将来 key() 若换形（如远端改服务端 id），
// 等价规则只改这一处，外部不假设键长什么样
bool PlaylistEntity::matches(const QString &path) const { return m_key == Util::Url::key(path); }

// N55 §2.3：形态解析唯一出口的唯一定义点。锁失败即编程错误——"提供者在位"由
// 消费点封闭与来源失效清扫共同保证（N55 §2.4），Debug 断言显影；release 走不可达
// 分支返回空串（空串令 mpv 显式加载失败，绝不静默退回 path()——对全转接型提供者，
// path() 只是条目标识）。委托不是缓存：每次现问归属提供者，无陈旧问题
QString PlaylistEntity::playUrl() const {
    const auto provider = m_ownerProvider.lock();
    Q_ASSERT(provider && !m_ownerUuid.isNull());
    if (!provider || m_ownerUuid.isNull())
        return QString();
    return provider->playUrl(m_ownerUuid, m_path, m_fileName);
}

// N65：串 + 逐文件选项的整体出口——与 playUrl() 同一条归属通路、同一个锁失败
// 处置，消费者封闭于 Playlist::playTargets 一处（合并点经它取提供者级选项）
PlayTarget PlaylistEntity::playTarget() const {
    const auto provider = m_ownerProvider.lock();
    Q_ASSERT(provider && !m_ownerUuid.isNull());
    if (!provider || m_ownerUuid.isNull())
        return {};
    return provider->playTarget(m_ownerUuid, m_path, m_fileName);
}

// 构造期派生 title / fileName / suffix / key / displayUrl（纯字符串运算 + 一次规范化，
// 与身份串同生命周期；key() 被委托绘制热路径逐行调用，必须缓存）
void PlaylistEntity::deriveNames() {
    const QFileInfo fi(m_path);
    m_fileName = fi.fileName();
    m_suffix = fi.suffix().toLower();
    m_key = Util::Url::key(m_path);
    // N56 §2.4：显示形态构造期定死——displayUrl() 非虚，显示纪律不可被子类破坏
    m_displayUrl = Util::Url::stripCredentials(m_path);
    if (!m_titleSet)
        m_title = m_fileName.isEmpty() ? m_path : m_fileName;
}

// 子类修正文件名：同步重算 suffix，并在 title 未被显式设置时一并同步
void PlaylistEntity::setFilename(QString fileName) {
    m_fileName = std::move(fileName);
    m_suffix = QFileInfo(m_fileName).suffix().toLower();
    if (!m_titleSet)
        m_title = m_fileName;
}

// ——元数据（惰性求值 + 实体内缓存）——

const FileMeta &PlaylistEntity::fileMeta() const {
    // 能力位首判：不支持者直接回落静态空值，永不调用 computeFileMeta()
    if (!supportsFileMeta())
        return s_emptyFileMeta();
    if (!m_fileMeta.has_value())
        m_fileMeta = computeFileMeta();
    return *m_fileMeta;
}

const FileMeta *PlaylistEntity::cachedFileMeta() const {
    return m_fileMeta.has_value() ? &*m_fileMeta : nullptr;
}

const PlaylistEntity::MediaMeta &PlaylistEntity::mediaMeta() const {
    // 能力位首判：不支持者直接回落静态空值，永不调用 computeMediaMeta()
    if (!supportsMediaMeta())
        return s_emptyMediaMeta();
    if (!m_mediaMeta.has_value()) {
        // 解析失败返回默认 Data 并照常入缓存（has_value 即已求值），防同一坏文件反复解析
        m_mediaMeta = computeMediaMeta();
    }
    return *m_mediaMeta;
}

const PlaylistEntity::MediaMeta *PlaylistEntity::cachedMediaMeta() const {
    return m_mediaMeta.has_value() ? &*m_mediaMeta : nullptr;
}

// ——N32 求值转发：与惰性入口同口径（能力位首判 + compute*），但不落缓存——
// 任务线程只算不写；值由 GUI 续作经注入口 setFileMeta / setMediaMeta 写回。
// N35 修订 2：同步阻塞求值，无取消通道（取消 = 结果静默丢弃，见变更记录）
FileMeta PlaylistEntity::evalFileMeta() const {
    if (!supportsFileMeta())
        return {};
    return computeFileMeta();
}

PlaylistEntity::MediaMeta PlaylistEntity::evalMediaMeta() const {
    if (!supportsMediaMeta())
        return {};
    return computeMediaMeta();
}

const QIcon &PlaylistEntity::icon() const {
    if (!m_icon.has_value())
        m_icon = computeIcon();
    return *m_icon;
}

// 清空全部缓存：此后各惰性入口在下次访问时重新求值
void PlaylistEntity::refresh() {
    m_fileMeta.reset();
    m_mediaMeta.reset();
    m_icon.reset();
}

// ——N29 注入口：写入即视为已求值；不支持者忽略（缓存恒空，与惰性回落同形）——

void PlaylistEntity::setFileMeta(const FileMeta &meta) {
    if (!supportsFileMeta())
        return;
    m_fileMeta = meta;
}

void PlaylistEntity::setMediaMeta(const MediaMeta &meta) {
    if (!supportsMediaMeta())
        return;
    m_mediaMeta = meta;
}

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
// playUrl() 经归属提供者现算：实体侧唯一 include 提供者头之处（实体头仅前置声明，无环）
#include "Util.h"
#include "providers/PlaylistProvider.h"

// N57：排序键取值冻结——既有取值不得重编，新键只准追加末尾
static_assert(int(SortKey::ReleaseDate) == 8);
static_assert(int(SortKey::EnumerationOrder) == 9);
static_assert(int(SortKey::DisplayName) == 10);

// 排序键 → 预读元数据类别的唯一映射
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
// 缓存空且不可算时的静态空值
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
    // 未定形属调用方违约：Debug 断言，release 落 LocalFileEntity
    Q_ASSERT(Util::Url::isDefiniteForm(path));
    if (Util::Url::isNetworkStream(path)) {
        if (path.startsWith(QLatin1String(kJellyfinScheme)))
            return std::make_shared<JellyfinEntity>(path, registry);
        // crypt 关闭时无此分派，走 UnknownEntity 兜底
#if defined(OXBOW_WITH_RCLONE_CRYPT)
        if (path.startsWith(QLatin1String(kRcloneCryptScheme)))
            return std::make_shared<RcloneCryptEntity>(path, registry);
#endif
        if (!Util::Url::isHttp(path))
            return std::make_shared<UnknownEntity>(path, registry);
        if (!registry) {
            // registry 为空属调用错误；release 降级 UnknownEntity
            Q_ASSERT(false);
            return std::make_shared<UnknownEntity>(path, registry);
        }
        return std::make_shared<HttpEntity>(path, registry);
    }
    return std::make_shared<LocalFileEntity>(path, registry);
}

// 回显专用入口（N61）：与 fromPath 同一分派，只把违约显影换成降级保行
PlaylistEntity::Ptr PlaylistEntity::fromMpvEcho(const QString &identity,
                                                const std::shared_ptr<ServiceRegistry> &registry) {
    if (Util::Url::isMpvStdin(identity))
        return std::make_shared<StdinEntity>(registry);
    if (!Util::Url::isDefiniteForm(identity))
        return std::make_shared<UnknownEntity>(identity, registry); // 降级：保行、不炸
    return fromPath(identity, registry);
}

QString PlaylistEntity::key() const { return m_key; }

// N56：键级判等的唯一实现点
bool PlaylistEntity::matches(const QString &path) const { return m_key == Util::Url::key(path); }

// 锁失败即契约破坏：Debug 断言，release 返空串（不退回 path()）。委托不是缓存
QString PlaylistEntity::playUrl() const {
    const auto provider = m_ownerProvider.lock();
    Q_ASSERT(provider && !m_ownerUuid.isNull());
    if (!provider || m_ownerUuid.isNull())
        return QString();
    return provider->playUrl(m_ownerUuid, m_path, m_fileName);
}

PlayTarget PlaylistEntity::playTarget() const {
    const auto provider = m_ownerProvider.lock();
    Q_ASSERT(provider && !m_ownerUuid.isNull());
    if (!provider || m_ownerUuid.isNull())
        return {};
    return provider->playTarget(m_ownerUuid, m_path, m_fileName);
}

// 构造期派生名称字段；key() 在绘制热路径上逐行取用，必须缓存
void PlaylistEntity::deriveNames() {
    const QFileInfo fi(m_path);
    m_fileName = fi.fileName();
    m_suffix = fi.suffix().toLower();
    m_key = Util::Url::key(m_path);
    // N56：显示形态构造期定死——displayUrl() 非虚，显示纪律不可被子类破坏
    m_displayUrl = Util::Url::stripCredentials(m_path);
    if (!m_titleSet)
        m_title = m_fileName.isEmpty() ? m_path : m_fileName;
}

void PlaylistEntity::setFilename(QString fileName) {
    m_fileName = std::move(fileName);
    m_suffix = QFileInfo(m_fileName).suffix().toLower();
    if (!m_titleSet)
        m_title = m_fileName;
}

// ——元数据（缓存优先求值 + 实体内缓存）——

const FileMeta &PlaylistEntity::fileMeta() const {
    if (!m_fileMeta.has_value()) {
        if (!canComputeFileMeta())
            return s_emptyFileMeta();
        m_fileMeta = computeFileMeta();
    }
    return *m_fileMeta;
}

const FileMeta *PlaylistEntity::cachedFileMeta() const {
    return m_fileMeta.has_value() ? &*m_fileMeta : nullptr;
}

const PlaylistEntity::MediaMeta &PlaylistEntity::mediaMeta() const {
    if (!m_mediaMeta.has_value()) {
        if (!canComputeMediaMeta())
            return s_emptyMediaMeta();
        // 解析失败返回默认 Data，同样入缓存，防反复解析
        m_mediaMeta = computeMediaMeta();
    }
    return *m_mediaMeta;
}

const PlaylistEntity::MediaMeta *PlaylistEntity::cachedMediaMeta() const {
    return m_mediaMeta.has_value() ? &*m_mediaMeta : nullptr;
}

// N32：求值转发——只算不写；任务线程取回，GUI 续作注入。同步无取消
FileMeta PlaylistEntity::evalFileMeta() const {
    if (!canComputeFileMeta())
        return {};
    return computeFileMeta();
}

PlaylistEntity::MediaMeta PlaylistEntity::evalMediaMeta() const {
    if (!canComputeMediaMeta())
        return {};
    return computeMediaMeta();
}

const QIcon &PlaylistEntity::icon() const {
    if (!m_icon.has_value())
        m_icon = computeIcon();
    return *m_icon;
}

// 清空全部缓存，下次访问重新求值
void PlaylistEntity::refresh() {
    m_fileMeta.reset();
    m_mediaMeta.reset();
    m_icon.reset();
}

// N29：写入即视为已求值；N178：无条件落缓存

void PlaylistEntity::setFileMeta(const FileMeta &meta) { m_fileMeta = meta; }

void PlaylistEntity::setMediaMeta(const MediaMeta &meta) { m_mediaMeta = meta; }

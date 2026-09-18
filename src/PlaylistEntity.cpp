#include "pch.h"

#include "PlaylistEntity.h"

#include "HttpEntity.h"
#include "LocalFileEntity.h"
#include "Util.h"

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

PlaylistEntity::PlaylistEntity(QString playUrl, const std::shared_ptr<ServiceRegistry> &registry)
    : m_playUrl(std::move(playUrl)), m_registry(registry) {
    deriveNames();
}

PlaylistEntity::Ptr PlaylistEntity::fromPlayUrl(const QString &playUrl,
                                                const std::shared_ptr<ServiceRegistry> &registry) {
    // 本地来源唯一出口：相对 → 绝对 + 原生分隔符；网络 / 流路径原样。
    // 加工对已加工路径幂等，保证往返不变式
    const QString url = Util::resolveLocalPath(playUrl);
    if (Util::isNetworkStreamPath(url)) {
        // N35 前缀分派：http(s) → HttpEntity（需非空 registry）；其余网络 / 流
        // 路径仍落 UnknownEntity 兜底。N48：判据改用 Util::isHttpUrl（唯一持有者，
        // 串前缀 + scheme 归小写，天然免疫 QUrl::scheme() 误判 Windows 盘符）——
        // 原内联前缀判定是同一判据的第三份拷贝
        if (!Util::isHttpUrl(url))
            return std::make_shared<UnknownEntity>(url, registry);
        if (!registry) {
            // http(s) + 空 registry 属调用错误（registry 必填，正常调用点恒非空）；
            // Q_ASSERT 守候，release 降级 UnknownEntity 保"恒返回实体"契约
            Q_ASSERT(false);
            return std::make_shared<UnknownEntity>(url, registry);
        }
        return std::make_shared<HttpEntity>(url, registry);
    }
    return std::make_shared<LocalFileEntity>(url, registry);
}

QString PlaylistEntity::key() const {
    return m_key; // 构造期缓存（见 deriveNames）
}

// 构造期派生 title / fileName / suffix / key（纯字符串运算 + 一次规范化，
// 与 playUrl 同生命周期；key() 被委托绘制热路径逐行调用，必须缓存）
void PlaylistEntity::deriveNames() {
    const QFileInfo fi(m_playUrl);
    m_fileName = fi.fileName();
    m_suffix = fi.suffix().toLower();
    m_key = Util::normalizePathForMatch(m_playUrl);
    if (!m_titleSet)
        m_title = m_fileName.isEmpty() ? m_playUrl : m_fileName;
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

// ——兜底实体——

// 通用文件图标（与重构前网络路径表现一致）；仅 GUI 线程调用。
// provider 无状态，函数内 static 复用一次构造
QIcon UnknownEntity::computeIcon() const {
    static QFileIconProvider provider;
    return provider.icon(QFileIconProvider::File);
}

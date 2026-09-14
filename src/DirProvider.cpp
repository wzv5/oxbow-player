#include "pch.h"

#include "DirProvider.h"

#include "Constants.h"
#include "LocalFileEntity.h"
#include "ServiceRegistry.h"
#include "TraversalPolicy.h"

namespace {
// 非 Q_OBJECT 类的用户可见串：英文源 + QT_TRANSLATE_NOOP 标注翻译上下文
// （供 lupdate 提取），运行期经 QCoreApplication::translate 取值——无译文时即
// 英文原文（N34/HttpClient 同款约定）
QString trText(const char *source) { return QCoreApplication::translate("DirProvider", source); }

// 扩展名 → QDir 名称过滤器（*.mp4 形态）
QStringList toNameFilters(const QStringList &extensions) {
    QStringList filters;
    filters.reserve(extensions.size());
    for (const QString &ext : extensions)
        filters << QStringLiteral("*.") + ext;
    return filters;
}
// junction 链解析上限：病态链 / 自指链不至于把枚举拖死
constexpr int kJunctionChainGuard = 32;

// 目录真实路径（去重键唯一来源）：先解开 NTFS junction 链，再取 canonical
// （解析符号链接 / . / ..），解析失败退回原路径——使"同一目录经多条路径可达"
// 收敛为同一个键。
// 实测（Qt 6.11.2 / Windows 11）：canonicalFilePath() 不解析 junction——链接的
// canonical 仍是链接自身路径，直接拿它当键等于没去重，junction 环会被当成普通
// 目录逐层展开；故此处显式经 junctionTarget() 解到真实目录再取 canonical
QString canonicalDirPath(const QString &path) {
    QFileInfo info(path);
    for (int i = 0; i < kJunctionChainGuard && info.isJunction(); ++i) {
        QString target = info.junctionTarget();
        if (target.isEmpty())
            break;
        // reparse 目标可能带 \??\ / \\?\ 前缀，统一转成 Qt 路径形态再续解
        if (target.startsWith(QStringLiteral("\\\\?\\")) ||
            target.startsWith(QStringLiteral("\\??\\")))
            target = target.mid(4);
        info = QFileInfo(QDir::fromNativeSeparators(target));
    }
    const QString canon = info.canonicalFilePath();
    return canon.isEmpty() ? info.absoluteFilePath() : canon;
}
} // namespace

DirProvider::DirProvider(const QString &root, bool recursive, const QStringList &extensions,
                         const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_root(root), m_recursive(recursive), m_extensions(extensions) {}

PlaylistProvider::Type DirProvider::type() const { return Type::Dir; }

QString DirProvider::describe() const {
    const QString suffix = m_recursive
                               ? trText(QT_TRANSLATE_NOOP("DirProvider", " (with subdirectories)"))
                               : QString();
    return trText(QT_TRANSLATE_NOOP("DirProvider", "Local directory: %1%2")).arg(m_root, suffix);
}

// 枚举并造实体：成功带实体有序表与警告，失败带错误文案；
// N29 起不预热：collect 产 QFileInfoList（枚举一次 stat 顺手取得），遍历中经
// 注入口把文件元数据零成本注入实体（口径与惰性求值共用 fileMetaFrom）。
// N37：失败面收缩为"根未绑定 / 不存在 / 不可读"三项；递归中个别子目录不可读
// 为部分成功，随 warnings 回传（不再整次失败进占位）
std::expected<PlaylistProvider::Enumeration, QString> DirProvider::enumerate() const {
    if (m_root.isEmpty())
        return std::unexpected(trText(QT_TRANSLATE_NOOP("DirProvider", "No directory bound.")));
    const QDir rootDir(m_root);
    if (!rootDir.exists())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("DirProvider", "Directory does not exist: %1")).arg(m_root));
    if (!rootDir.isReadable())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("DirProvider", "Directory is not readable: %1")).arg(m_root));
    // 扩展名空表＝不过滤；过滤器一次构造逐层复用
    // N36：本批遍历参数经注册表策略快照取用（GUI 线程装配、任务线程只读，N36 修订 1）；
    // 起始处解析一次贯穿整次遍历，设置变更只影响下一次枚举。resolve 为空按缺省兜底
    int maxDepth = App::recursiveDepthLimitDefault();
    if (const auto policy = registry()->resolve<TraversalPolicy>())
        maxDepth = policy->depthLimit;
    QFileInfoList infos;
    QStringList warnings;
    // N36 修订 3：已访问目录的 canonical 路径集——符号链接环 / 同一目录经多条路径可达
    // 的剪枝依据，根先入集
    QSet<QString> visited;
    visited.insert(canonicalDirPath(m_root));
    collect(rootDir, m_extensions.isEmpty() ? QStringList() : toNameFilters(m_extensions),
            m_recursive, 0, maxDepth, visited, infos, warnings);
    // N37：warnings 随结果回传（部分成功），不再触发整次失败
    EntityList entities;
    entities.reserve(infos.size());
    for (const QFileInfo &fi : infos) {
        // 路径加工（绝对化 + 原生分隔符，B15）统一由 fromPlayUrl 内的
        // resolveLocalPath 出口负责，此处不再预处理；registry 转交（本地路径忽略）
        auto entity = PlaylistEntity::fromPlayUrl(fi.absoluteFilePath(), registry());
        injectFileMeta(*entity, LocalFileEntity::fileMetaFrom(fi)); // 零成本注入
        entities.push_back(std::move(entity));
    }
    return Enumeration{std::move(entities), std::move(warnings)};
}

bool DirProvider::supportsModify() const { return false; }

// 目录表不可增删，但可排序（规则随参数一并持久化）
bool DirProvider::supportsSort() const { return true; }

bool DirProvider::isValid() const { return !m_root.isEmpty() && QDir(m_root).exists(); }

void DirProvider::saveSettings(QSettings &settings) const {
    settings.setValue(QStringLiteral("provider"), QStringLiteral("dir"));
    settings.setValue(QStringLiteral("dirRoot"), m_root);
    settings.setValue(QStringLiteral("dirRecursive"), m_recursive);
    settings.setValue(QStringLiteral("dirExtensions"), m_extensions);
    // 目录绑定列表持久化排序键（含 None——显式记录用户未选择规则）
    settings.setValue(QStringLiteral("sortKey"), sortKey());
    settings.setValue(QStringLiteral("sortAscending"), sortAscending());
}

std::shared_ptr<DirProvider> DirProvider::load(QSettings &settings,
                                               const std::shared_ptr<ServiceRegistry> &registry) {
    auto provider = std::make_shared<DirProvider>(
        settings.value(QStringLiteral("dirRoot")).toString(),
        settings.value(QStringLiteral("dirRecursive")).toBool(),
        settings.value(QStringLiteral("dirExtensions")).toStringList(), registry);
    provider->setSortRule(settings.value(QStringLiteral("sortKey"), 0).toInt(),
                          settings.value(QStringLiteral("sortAscending"), true).toBool());
    return provider;
}

void DirProvider::collect(const QDir &dir, const QStringList &filters, bool recursive, int depth,
                          int maxDepth, QSet<QString> &visited, QFileInfoList &out,
                          QStringList &warnings) {
    // 文件层：命中扩展名的条目信息追加（N29：整个 QFileInfo 留给注入，
    // 路径加工统一在 fromPlayUrl 的 resolveLocalPath 出口做）
    const QFileInfoList files =
        dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::NoSort);
    for (const QFileInfo &fi : files)
        out << fi;
    if (!recursive)
        return;
    // 目录层：先序深入子目录（平铺语义，次序无关紧要——视图层统一重排）；
    // 不可读子目录记 warning 后跳过（部分成功语义）
    const QFileInfoList subDirs =
        dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &fi : subDirs) {
        // N36：子目录深度超过上限不再深入（上限 n = 最多深入 n 层，0 = 不递归）；
        // 截断静默，不记 warning（超深是配置使然，不是错误）
        if (depth + 1 > maxDepth)
            continue;
        // N36 修订 3：已访问过（符号链接环，或同一目录经多条路径可达）即静默跳过，
        // 不记 warning——这是防御，不是错误；深度上限之外的第二道闸门
        const QString key = canonicalDirPath(fi.absoluteFilePath());
        if (visited.contains(key))
            continue;
        if (!QDir(fi.absoluteFilePath()).isReadable()) {
            warnings << trText(QT_TRANSLATE_NOOP("DirProvider",
                                                 "Subdirectory is not readable, skipped: %1"))
                            .arg(fi.absoluteFilePath());
            continue;
        }
        visited.insert(key);
        collect(fi.absoluteFilePath(), filters, recursive, depth + 1, maxDepth, visited, out,
                warnings);
    }
}

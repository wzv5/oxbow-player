#include "pch.h"

#include "DirProvider.h"

#include "Util.h"

namespace {
// 扩展名 → QDir 名称过滤器（*.mp4 形态）
QStringList toNameFilters(const QStringList &extensions) {
    QStringList filters;
    filters.reserve(extensions.size());
    for (const QString &ext : extensions)
        filters << QStringLiteral("*.") + ext;
    return filters;
}
} // namespace

DirProvider::DirProvider() = default;

DirProvider::DirProvider(const QString &root, bool recursive, const QStringList &extensions)
    : m_root(root), m_recursive(recursive), m_extensions(extensions) {}

PlaylistProvider::Type DirProvider::type() const { return Type::Dir; }

QString DirProvider::describe() const {
    const QString suffix = m_recursive ? QObject::tr(" (with subdirectories)") : QString();
    return QObject::tr("Local directory: %1%2").arg(m_root, suffix);
}

// 枚举并造实体：成功带实体有序表（预热 fileMeta——异步路径在工作线程、
// 同步填充在 GUI 线程执行同一惰性入口），失败带错误文案；
// root 不存在/不可读为全失败，递归中个别子目录不可读为部分成功 + warning
// （error 非空即视为有错，由调用方走失败占位处理）
std::expected<PlaylistProvider::EntityList, QString> DirProvider::enumerate() const {
    if (m_root.isEmpty())
        return std::unexpected(QObject::tr("No directory bound."));
    const QDir rootDir(m_root);
    if (!rootDir.exists())
        return std::unexpected(QObject::tr("Directory does not exist: %1").arg(m_root));
    if (!rootDir.isReadable())
        return std::unexpected(QObject::tr("Directory is not readable: %1").arg(m_root));
    // 扩展名空表＝不过滤；过滤器一次构造逐层复用
    QStringList out;
    QStringList warnings;
    collect(rootDir, m_extensions.isEmpty() ? QStringList() : toNameFilters(m_extensions),
            m_recursive, out, warnings);
    if (!warnings.isEmpty())
        return std::unexpected(warnings.join(QStringLiteral("\n")));
    EntityList entities;
    entities.reserve(out.size());
    for (const QString &path : out) {
        auto entity = PlaylistEntity::fromPlayUrl(path);
        entity->fileMeta(); // 预热（QFileInfo 一次 stat，线程安全）
        entities.push_back(std::move(entity));
    }
    return entities;
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

std::shared_ptr<DirProvider> DirProvider::load(QSettings &settings) {
    auto provider = std::make_shared<DirProvider>(
        settings.value(QStringLiteral("dirRoot")).toString(),
        settings.value(QStringLiteral("dirRecursive")).toBool(),
        settings.value(QStringLiteral("dirExtensions")).toStringList());
    provider->setSortRule(settings.value(QStringLiteral("sortKey"), 0).toInt(),
                          settings.value(QStringLiteral("sortAscending"), true).toBool());
    return provider;
}

void DirProvider::collect(const QDir &dir, const QStringList &filters, bool recursive,
                          QStringList &out, QStringList &warnings) {
    // 文件层：命中扩展名的条目规范化后追加
    const QFileInfoList files =
        dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::NoSort);
    for (const QFileInfo &fi : files)
        // Qt 来源（目录枚举）：拿到即转原生分隔符（B15），不做小写/cleanPath
        out << Util::nativePath(fi.absoluteFilePath());
    if (!recursive)
        return;
    // 目录层：先序深入子目录（平铺语义，次序无关紧要——视图层统一重排）；
    // 不可读子目录记 warning 后跳过（部分成功语义）
    const QFileInfoList subDirs =
        dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &fi : subDirs) {
        if (!QDir(fi.absoluteFilePath()).isReadable()) {
            warnings << QObject::tr("Subdirectory is not readable, skipped: %1")
                            .arg(fi.absoluteFilePath());
            continue;
        }
        collect(fi.absoluteFilePath(), filters, recursive, out, warnings);
    }
}

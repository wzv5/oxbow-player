#include "pch.h"

#include "DirProvider.h"

#include "PlaylistWidget.h"
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

QStringList DirProvider::enumerate() const {
    QStringList out;
    if (!isValid())
        return out;
    // 扩展名空表＝不过滤；过滤器一次构造逐层复用
    collect(QDir(m_root), m_extensions.isEmpty() ? QStringList() : toNameFilters(m_extensions),
            out);
    return out;
}

bool DirProvider::supportsRemove() const { return false; }

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

std::unique_ptr<DirProvider> DirProvider::load(QSettings &settings) {
    auto provider = std::make_unique<DirProvider>(
        settings.value(QStringLiteral("dirRoot")).toString(),
        settings.value(QStringLiteral("dirRecursive")).toBool(),
        settings.value(QStringLiteral("dirExtensions")).toStringList());
    provider->setSortRule(settings.value(QStringLiteral("sortKey"), 0).toInt(),
                          settings.value(QStringLiteral("sortAscending"), true).toBool());
    return provider;
}

void DirProvider::collect(const QDir &dir, const QStringList &filters, QStringList &out) const {
    // 文件层：命中扩展名的条目规范化后追加
    const QFileInfoList files =
        dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::NoSort);
    for (const QFileInfo &fi : files)
        // Qt 来源（目录枚举）：拿到即转原生分隔符（B15），不做小写/cleanPath
        out << Util::nativePath(fi.absoluteFilePath());
    if (!m_recursive)
        return;
    // 目录层：先序深入子目录（平铺语义，次序无关紧要——视图层统一重排）
    const QFileInfoList subDirs =
        dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &fi : subDirs)
        collect(fi.absoluteFilePath(), filters, out);
}

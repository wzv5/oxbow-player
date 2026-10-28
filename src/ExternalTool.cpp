#include "pch.h"

#include "ExternalTool.h"

#ifndef Q_OS_WIN
#include <dlfcn.h> // dladdr：定位核心插件自身 .so
#endif

namespace {

QString foldKey(const QString &path) {
#ifdef Q_OS_WIN
    return path.toLower(); // Windows 文件系统大小写不敏感
#else
    return path;
#endif
}

} // namespace

const ExternalToolSpec &externalToolSpec(ExternalTool tool) {
    // MediaInfo 的目标名严格取 MediaInfoDLL.h 的 MEDIAINFODLL_NAME 口径：Windows 是
    // _UNICODE 那一份（shim 按名解析，改名副本不被接受），Linux 那一份即它的 SONAME；
    // macOS 不在支持范围
    static const ExternalToolSpec mediaInfo{
        "mediaInfoPath",
        QT_TRANSLATE_NOOP("ExternalTool", "MediaInfo Library"),
#ifdef Q_OS_WIN
        QStringLiteral("MediaInfo.dll"),
#else
        QStringLiteral("libmediainfo.so.0"),
#endif
    };
    switch (tool) {
    case ExternalTool::MediaInfo:
        return mediaInfo;
    }
    // 加了枚举值却没补 case 属编程错误：Debug 撞断言，设置目录的 iniKey 交叉校验也会报
    // （拿到的键名与声明不符）；release 按 MediaInfo 兜底——没有任何定义能代表不存在的工具
    Q_ASSERT(false);
    return mediaInfo;
}

QString externalToolIniKey(ExternalTool tool) {
    return QString::fromLatin1(externalToolSpec(tool).iniKey);
}

QString externalToolPluginDir() {
#ifdef Q_OS_WIN
    HMODULE hModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&externalToolPluginDir), &hModule))
        return {};
    // 缓冲按需倍增（B4 同款）
    std::wstring buf(MAX_PATH, L'\0');
    DWORD n = 0;
    while ((n = GetModuleFileNameW(hModule, buf.data(), static_cast<DWORD>(buf.size()))) ==
           buf.size())
        buf.resize(buf.size() * 2);
    const QString modulePath = QString::fromWCharArray(buf.data(), static_cast<int>(n));
#else
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&externalToolPluginDir), &info) || !info.dli_fname)
        return {};
    const QString modulePath = QString::fromUtf8(info.dli_fname);
#endif
    if (modulePath.isEmpty())
        return {};
    return QFileInfo(modulePath).absolutePath();
}

QStringList externalToolCandidates(const ExternalToolSpec &spec, const QString &configDir) {
    QStringList dirs;
    const QString plugin = externalToolPluginDir();
    if (!plugin.isEmpty())
        dirs << plugin;
    if (!configDir.isEmpty())
        dirs << configDir;
    const QStringList pathDirs =
        qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString &dir : pathDirs)
        dirs << dir;
#ifndef Q_OS_WIN
    // MediaInfoDLL.h 回退链同款的标准库目录（其 ./ 一项相对 cwd，不取）——
    // 发行版库通常不在 PATH 里
    dirs << QStringLiteral("/usr/local/lib") << QStringLiteral("/usr/local/lib64")
         << QStringLiteral("/usr/lib") << QStringLiteral("/usr/lib64");
#endif

    QStringList out;
    QSet<QString> seen;
    for (const QString &dir : dirs) {
        if (dir.isEmpty())
            continue;
        const QFileInfo info(QDir(dir).absoluteFilePath(spec.targetFileName));
        if (!info.isFile())
            continue;
        const QString path = QDir::cleanPath(info.absoluteFilePath());
        if (!seen.contains(foldKey(path))) {
            seen.insert(foldKey(path));
            // 出表的串直接进下拉框、也按原样写进 ini：Windows 侧转原生分隔符，与资源管理器
            // 和命令行的形态一致。去重与后续判据都在上面那份归一形（正斜杠）上做，不受影响
            out << QDir::toNativeSeparators(path);
        }
    }
    return out;
}

QString externalToolBrowseFilter(const ExternalToolSpec &spec) {
    return QCoreApplication::translate("ExternalTool", spec.displayName) +
           QStringLiteral(" (%1)").arg(spec.targetFileName);
}

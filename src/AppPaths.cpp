#include "pch.h"

#include "AppPaths.h"

#ifndef Q_OS_WIN
#include <dlfcn.h> // dladdr：定位核心插件自身 .so
#endif

namespace App {
namespace {

// 配置文件名：小写基名（oxbow.ini），与翻译基名（oxbow_<locale>.qm）同族。
// 只在本文件两处落点使用，故为文件内常量
const QString kIniFileName = QStringLiteral("oxbow.ini");

// 以本函数地址反查核心插件自身模块目录（动态形态 oxbowcore.dll、静态形态部署为
// main.dll 的核心本身、Linux 的 .so，同一份代码全覆盖）
QString coreModuleDir() {
#ifdef Q_OS_WIN
    HMODULE hModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&coreModuleDir), &hModule))
        return {};
    // 缓冲按需倍增（B4）
    std::wstring buf(MAX_PATH, L'\0');
    DWORD n = 0;
    while ((n = GetModuleFileNameW(hModule, buf.data(), static_cast<DWORD>(buf.size()))) ==
           buf.size())
        buf.resize(buf.size() * 2);
    const QString modulePath = QString::fromWCharArray(buf.data(), static_cast<int>(n));
#else
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&coreModuleDir), &info) || !info.dli_fname)
        return {};
    const QString modulePath = QString::fromUtf8(info.dli_fname);
#endif
    if (modulePath.isEmpty())
        return {};
    return QFileInfo(modulePath).absolutePath();
}

} // namespace

AppPaths &AppPaths::instance() {
    static AppPaths self;
    return self;
}

const QString &AppPaths::pluginDir() const {
    Q_ASSERT(m_initialized);
    return m_pluginDir;
}

const QString &AppPaths::iniFile() const {
    Q_ASSERT(m_initialized);
    return m_iniFile;
}

const QString &AppPaths::iniDir() const {
    Q_ASSERT(m_initialized);
    return m_iniDir;
}

const AppPaths &paths() { return AppPaths::instance(); }

void initializePaths() {
    AppPaths &self = AppPaths::instance();
    if (self.m_initialized)
        return;
    self.m_initialized = true;

    // N147：pluginDir 是 ini 判定的输入，三项在入口一次算全
    self.m_pluginDir = coreModuleDir();

    // 插件目录写探针通过即落插件目录（只读目录下 QSettings 静默失败）；回落 Qt 标准
    // 位置须在 applicationName 设定之后（入口顺序保证），否则目录名缺 Oxbow 一层
    QString iniFile;
    if (!self.m_pluginDir.isEmpty()) {
        QTemporaryFile probe(self.m_pluginDir + QStringLiteral("/oxbow-XXXXXX.tmp"));
        if (probe.open())
            iniFile = QDir(self.m_pluginDir).filePath(kIniFileName);
    }
    if (iniFile.isEmpty()) {
        const QString fallback =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir().mkpath(fallback);
        iniFile = QDir(fallback).filePath(kIniFileName);
    }
    self.m_iniFile = iniFile;
    self.m_iniDir = QFileInfo(iniFile).absolutePath();
}

} // namespace App

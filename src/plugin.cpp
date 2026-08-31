// mpv C 插件本体（mpvqtcore）。
// 无论 Windows（两段式壳转发）还是 Linux / 静态链接 Qt 的单一 DLL，本模块都导出
// 标准入口 mpv_open_cplugin：
//   - Windows：mpv 只加载 scripts/<name>/main.dll（加载壳），壳 LoadLibrary 本模块
//     后调用这里的 mpv_open_cplugin；mpv 绝不会直接加载本模块（它不以 main 命名、
//     也不在顶层），所以此处导出标准名是安全的；
//   - Linux / 静态 Qt 单一 DLL：mpv 直接加载本模块并调用 mpv_open_cplugin。
// mpv 在为该客户端创建的独立线程中调用入口函数，因此可以在这里
// 创建 QApplication 并运行 Qt 事件循环（Windows 上 Qt 不要求
// GUI 必须位于进程主线程；阻塞本线程不会阻塞 mpv 核心）。

#include "pch.h"

#include <mpv/client.h>

#include "Constants.h"
#include "MainWindow.h"
#include "MpvController.h"

// 本模块通过 MPV_CPLUGIN_DYNAMIC_SYM 把 mpv_* 编译为指向 pfn_* 的指针，
// 这些指针本应由 mpv 在“直接加载的模块”里填充。但本模块在 Windows 上是由壳
// 动态加载的（mpv 不会填充这里的 pfn_*）；即便在 Linux / 静态链接场景下由 mpv
// 直接加载，统一在入口处自行解析也最稳妥、使模块自带符号。
// 仅解析代码中实际用到的 10 个符号即可；其余 pfn_* 保持 NULL 也无关紧要。
// 所列符号任一缺失说明宿主不是导出完整 client API 的 mpv，入口处快速失败，
// 比运行到一半空指针崩溃更可诊断。
// 平台差异：Windows 从 mpv.exe 取；POSIX 从全局符号表（RTLD_DEFAULT，即 mpv
// 主程序导出的符号）取。
static void *resolveMpvSymbol(const char *name) {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(nullptr), name));
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

static bool resolveMpvSymbols() {
    static bool done = false;
    if (done)
        return true;
    done = true;

    const char *missing = nullptr;
#define RESOLVE(name)                                                                              \
    do {                                                                                           \
        pfn_##name = reinterpret_cast<decltype(pfn_##name)>(resolveMpvSymbol(#name));              \
        if (!pfn_##name && !missing)                                                               \
            missing = #name;                                                                       \
    } while (0)
    RESOLVE(mpv_set_property_string);
    RESOLVE(mpv_set_property);
    RESOLVE(mpv_observe_property);
    RESOLVE(mpv_command_string);
    RESOLVE(mpv_command);
    RESOLVE(mpv_get_property_string);
    RESOLVE(mpv_get_property);
    RESOLVE(mpv_free);
    RESOLVE(mpv_free_node_contents); // B18：FILE_LOADED 主动读 NODE 属性后释放
    RESOLVE(mpv_wait_event);
#undef RESOLVE

    if (missing) {
#ifdef _WIN32
        OutputDebugStringA("[mpvqt] 无法从宿主解析 mpv 符号: ");
        OutputDebugStringA(missing);
        OutputDebugStringA("\n");
#else
        fprintf(stderr, "[mpvqt] 无法从宿主解析 mpv 符号: %s\n", missing);
#endif
        return false;
    }
    return true;
}

// 解析插件配置文件完整路径：直接放在插件 DLL 所在目录下，命名为 mpv-qt.ini。
// 取本模块（mpvqtcore.dll）所在目录，在其下直接读写配置文件，目录本身已存在。
// 无法获取模块路径时退回到 Qt 标准配置位置。
static QString resolveConfigFilePath() {
    QString modulePath;
#ifdef _WIN32
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&resolveConfigFilePath), &hModule)) {
        // 按需增长缓冲直至不被截断（B4 终版方案，不写死长度）
        std::wstring buf(MAX_PATH, L'\0');
        DWORD n = 0;
        while ((n = GetModuleFileNameW(hModule, buf.data(), static_cast<DWORD>(buf.size()))) ==
               buf.size())
            buf.resize(buf.size() * 2);
        modulePath = QString::fromWCharArray(buf.data(), static_cast<int>(n));
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&resolveConfigFilePath), &info) && info.dli_fname)
        modulePath = QString::fromUtf8(info.dli_fname);
#endif

    if (!modulePath.isEmpty() && QFileInfo::exists(modulePath)) {
        // 目录只读（如安装于 Program Files）时 QSettings 会静默失败，
        // 以写探针判定；不可写则走标准配置位置
        const QString dir = QFileInfo(modulePath).absolutePath();
        QTemporaryFile probe(dir + QStringLiteral("/mpvqt-XXXXXX.tmp"));
        if (probe.open())
            return QDir(dir).filePath(App::iniFileName());
    }

    // 兜底：写入 Qt 标准配置位置（AppConfigLocation）
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(fallback);
    return QDir(fallback).filePath(App::iniFileName());
}

// 在 QApplication 就绪、MainWindow 构造前安装内嵌翻译：
// 仅从 :/i18n 资源加载（qt_add_translations 嵌入），失败则静默回退英文原文。
// 语言来源：mpv-qt.ini 的 language 键；空/"auto" 表示跟随系统， "en" 为英文原文，
// 其余按 QLocale 对应 :/i18n/mpvqt_<locale>.qm
static void installTranslator() {
    if (!qApp)
        return;
    QString lang;
    {
        QSettings s(resolveConfigFilePath(), QSettings::IniFormat);
        lang = s.value(QStringLiteral("language")).toString().trimmed();
    }
    auto *translator = new QTranslator(qApp);
    bool loaded = false;
    if (lang.isEmpty() || lang.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        loaded = translator->load(QLocale(), QStringLiteral("mpvqt"), QStringLiteral("_"),
                                  QStringLiteral(":/i18n"));
    } else if (lang.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0) {
        loaded = false;
    } else {
        const QLocale loc(lang);
        if (translator->load(loc, QStringLiteral("mpvqt"), QStringLiteral("_"),
                             QStringLiteral(":/i18n")))
            loaded = true;
        else if (translator->load(QStringLiteral("mpvqt_") + lang, QStringLiteral(":/i18n")))
            loaded = true;
    }
    if (loaded) {
        QCoreApplication::installTranslator(translator);
    } else {
        delete translator;
    }
}

extern "C" MPV_EXPORT int mpv_open_cplugin(mpv_handle *handle) {
    if (!resolveMpvSymbols())
        return 1; // 符号缺失：让 mpv 记为脚本加载失败，而非运行期崩溃

    // 防御：同一进程内入口只执行一次
    static bool started = false;
    if (started)
        return 0;
    started = true;

    // argc/argv 的生命周期必须覆盖整个进程
    static int argc = 1;
    static char *argv[] = {App::nameAscii, nullptr};

    if (!qApp) {
        // 有意不 delete：QApplication 必须与进程同生命周期
        (void)new QApplication(argc, argv);
        QApplication::setApplicationName(App::name());
    }

    installTranslator();

    auto *controller = new MpvController(handle);
    const QString configFile = resolveConfigFilePath();
    auto *window = new MainWindow(controller, configFile);
    controller->setParent(window);

    // 作为 GUI 宿主，让 mpv 在播放列表结束后保持运行而不是直接退出
    mpv_set_property_string(handle, "idle", "yes");
    // keep-open 必须关闭：EOF 时由 mpv 自动前进到下一项，列表播完进入 idle；
    // 循环行为交给 loop-file / loop-playlist 设置
    mpv_set_property_string(handle, "keep-open", "no");
    // pseudo-gui 模式默认 force-window=yes，空闲/停止时会绘制 mpv logo 提示图；
    // 关掉后空闲时视频输出销毁，露出视频区 widget 的黑色背景
    mpv_set_property_string(handle, "force-window", "no");

    // 把视频渲染嵌入 Qt 窗口（与 libmpv 的 --wid 用法一致）
    int64_t wid = int64_t(window->videoWidget()->winId());
    mpv_set_property(handle, "wid", MPV_FORMAT_INT64, &wid);

    // 嵌入属性全部就绪后才启动事件线程。首次 mpv_wait_event 会解除 mpv
    // 核心的启动门控（客户端 fuzzy_initialized），核心随即为命令行文件
    // 初始化 vo；过早调用会让 mpv 在 wid 生效前先自建窗口再重建嵌入，
    // 表现为启动时闪现 mpv 原生窗口
    controller->startEventThread();

    // mpv 退出（如 MPV_EVENT_SHUTDOWN）时由 MainWindow::onMpvShutdown
    // 补保存会话并结束 Qt 事件循环

    window->show();
    QApplication::exec();
    return 0;
}

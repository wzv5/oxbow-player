// mpv C 插件本体（oxbowcore）。
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
#include <mpv/stream_cb.h> // N52：RESOLVE(mpv_stream_cb_add_ro) 的符号声明

#ifndef Q_OS_WIN
#include <dlfcn.h> // dladdr / Dl_info：定位插件自身 .so（Linux 头不庞大，不进预编译）
#endif

#include "Constants.h"
#include "MainWindow.h"
#include "MpvController.h"
#include "StreamRelay.h"
#include "Version.h"

// 本模块在 Windows 上通过 MPV_CPLUGIN_DYNAMIC_SYM 把 mpv_* 编译为指向 pfn_* 的
// 指针：PE 加载器没有全局符号概念，本模块又是由壳动态加载的（mpv 不会填充这里的
// pfn_*），故在入口处自行解析（client.h 注释同口径——该符号表是 targeting Windows
// 的推荐做法，多定义容忍依赖 selectany）。仅解析代码中实际用到的符号即可；其余
// pfn_* 保持 NULL 也无关紧要。所列符号任一缺失说明宿主不是导出完整 client API 的
// mpv，入口处快速失败，比运行到一半空指针崩溃更可诊断。
// N83：Linux 不定义该宏，走 cplugin 标准形态——mpv 以 -rdynamic 导出 client API，
// 插件保留 mpv_* 未定义符号，dlopen 时由 ELF 动态链接器从宿主全局作用域解析；缺失
// 符号即加载失败，由 mpv 记为脚本加载失败，同样可诊断。（宏成立即必为 Windows，
// 内部不再有平台分支。）
#ifdef MPV_CPLUGIN_DYNAMIC_SYM
static void *resolveMpvSymbol(const char *name) {
    return reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(nullptr), name));
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
    RESOLVE(mpv_command_node);       // N52：loadfile 逐文件选项的节点通道
    RESOLVE(mpv_wait_event);
    RESOLVE(mpv_stream_cb_add_ro);   // N52：oxbow:// 转接协议注册
    RESOLVE(mpv_get_property_async); // N60：推送会话的屏障读（playlist 终值自带终点）
    RESOLVE(mpv_command_async); // N87：未认领按键的 keypress 注入（异步，GUI 线程不等用户命令）
    RESOLVE(mpv_error_string);  // 命令错误码转可读文本（流添加失败反馈用）
#undef RESOLVE

    if (missing) {
        // 拼接后一次输出；missing 是 ASCII 符号名，fromLatin1 足够
        const QString msg = QStringLiteral("[oxbow] 无法从宿主解析 mpv 符号: %1\n")
                                .arg(QString::fromLatin1(missing));
        OutputDebugStringW(reinterpret_cast<const wchar_t *>(msg.utf16()));
        return false;
    }
    return true;
}
#endif // MPV_CPLUGIN_DYNAMIC_SYM

// 解析插件配置文件完整路径：直接放在插件 DLL 所在目录下，命名为 oxbow.ini。
// 取本模块（oxbowcore.dll）所在目录，在其下直接读写配置文件，目录本身已存在。
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
        QTemporaryFile probe(dir + QStringLiteral("/oxbow-XXXXXX.tmp"));
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
// 语言来源：oxbow.ini 的 language 键；空/"auto" 表示跟随系统， "en" 为英文原文，
// 其余按 QLocale 对应 :/i18n/oxbow_<locale>.qm
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
        loaded = translator->load(QLocale(), QStringLiteral("oxbow"), QStringLiteral("_"),
                                  QStringLiteral(":/i18n"));
    } else if (lang.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0) {
        loaded = false;
    } else {
        const QLocale loc(lang);
        if (translator->load(loc, QStringLiteral("oxbow"), QStringLiteral("_"),
                             QStringLiteral(":/i18n")))
            loaded = true;
        else if (translator->load(QStringLiteral("oxbow_") + lang, QStringLiteral(":/i18n")))
            loaded = true;
    }
    if (loaded) {
        QCoreApplication::installTranslator(translator);
    } else {
        delete translator;
    }
}

extern "C" MPV_EXPORT int mpv_open_cplugin(mpv_handle *handle) {
#ifdef MPV_CPLUGIN_DYNAMIC_SYM
    if (!resolveMpvSymbols())
        return 1; // 符号缺失：让 mpv 记为脚本加载失败，而非运行期崩溃
#endif

    // 防御：同一进程内入口只执行一次
    static bool started = false;
    if (started)
        return 0;
    started = true;

    // N84：启动诊断行——产物自报版本身份（版本 + 短 hash[-dirty]），逆向回答
    // "手上这份产物是哪次提交、什么时候编的"。输出面与既有诊断一致：
    // Windows 打调试器（OutputDebugStringW），Linux 打 stderr。串形为
    // "[oxbow] 0.1.0 (cfaf552-dirty)"，与 About 对话框的 Version 行同源
    {
        const QString diag = QStringLiteral("[oxbow] %1\n").arg(App::versionInfo());
#ifdef Q_OS_WIN
        OutputDebugStringW(reinterpret_cast<const wchar_t *>(diag.utf16()));
#else
        fputs(diag.toUtf8().constData(), stderr);
#endif
    }

    // argc/argv 的生命周期必须覆盖整个进程
    static int argc = 1;
    static char *argv[] = {App::nameAscii, nullptr};

    if (!qApp) {
#ifndef Q_OS_WIN
        // N83：wid 嵌入只在 X11 成立——Wayland 平台的 winId 非 XID（实测播视频即
        // X Error BadWindow），故默认强制 xcb（XWayland 提供兼容）。用户显式设置
        // 的 QT_QPA_PLATFORM 不覆盖；纯 Wayland（无 XWayland）不受支持。
        if (qEnvironmentVariable("QT_QPA_PLATFORM").isEmpty())
            qputenv("QT_QPA_PLATFORM", "xcb");
#endif
        // 有意不 delete：QApplication 必须与进程同生命周期
        (void)new QApplication(argc, argv);
        QApplication::setApplicationName(App::name());
        // 退出只由 MPV_EVENT_SHUTDOWN 收口（见 onMpvShutdown）：Qt 默认的"末窗关闭即退"会让
        // exec() 在核心还在拆 vo 时就抢先返回，入口一返回 mpv 即销毁本句柄，与仍在拆核心的
        // 主线程互等成死锁（client.h 的销毁律）
        QApplication::setQuitOnLastWindowClosed(false);
    }

    installTranslator();

    auto *controller = new MpvController(handle);

    // N52：转接协议适配层——shared_ptr 寿命覆盖 QApplication::exec() 全程
    // （ADR-1：入口本就阻塞到退出）。装配硬顺序（N52 §2.5）：
    // ① install 必须早于 startEventThread——首次 mpv_wait_event 解除 mpv 启动
    //    门控后，命令行首文件即会 open 流；
    // ② 必须早于任何提供者出生——列表在 PlaylistWidget 构造期读盘建提供者，
    //    playUrl 首次下发就要能从注册表取到 relay（经 MainWindow 构造参数传入）。
    auto relay = std::make_shared<StreamRelay>();
    if (!relay->install(handle))
        return 1; // 协议注册失败：让 mpv 记为脚本加载失败，比静默残缺更可诊断

    const QString configFile = resolveConfigFilePath();
    auto *window = new MainWindow(controller, configFile, std::move(relay));
    controller->setParent(window);

    // 作为 GUI 宿主，让 mpv 在播放列表结束后保持运行而不是直接退出
    mpv_set_property_string(handle, "idle", "yes");
    // keep-open 必须关闭：EOF 时由 mpv 自动前进到下一项，列表播完进入 idle；
    // 循环行为交给 loop-file / loop-playlist 设置
    mpv_set_property_string(handle, "keep-open", "no");
    // pseudo-gui 模式默认 force-window=yes，空闲/停止时会绘制 mpv logo 提示图；
    // 关掉后空闲时视频输出销毁，露出视频区 widget 的黑色背景
    mpv_set_property_string(handle, "force-window", "no");
    // N97：拖放归插件——mpv 前端的 --drag-and-drop 默认 auto（拖入即整表替换），而 wid
    // 嵌入下它的 vo 窗口是视频区 HWND 的子窗口、照样收得到 WM_DROPFILES；不关掉就会
    // 和插件在视频区做的分派叠成两份动作
    mpv_set_property_string(handle, "drag-and-drop", "no");

#ifndef Q_OS_WIN
    // N83：X11 嵌入的输入接管——**事件层**关到底（实测 mpv 嵌入窗口的事件掩码截走
    // 指针事件，Qt 视频区收不到任何鼠标事件）：VO 键盘与指针事件逐项关闭
    // （input-vo-keyboard 不存在的旧版 mpv 回退 input-x11-keyboard），另关 mpv 侧
    // 光标自动隐藏（N67 已在 Qt 层做，避免两层打架）。键盘因此恒由 Qt 独占。
    // N87：**绑定层不干涉**——不设 input-default-bindings，取值交给 mpv 默认与用户自己的
    // mpv.conf。未认领按键经 keypress 注入后走的是绑定查找，那张默认键位表正是要查的表，
    // 显式置 no 会挡掉它，显式置 yes 又替用户做了决定。Windows 上 mpv 本就拿不到键盘
    // （wid 嵌入下焦点窗口是 Qt 的），且自会向父窗口转发鼠标消息（插件依赖该路径）
    mpv_set_property_string(handle, "input-cursor", "no");
    mpv_set_property_string(handle, "cursor-autohide", "no");
    if (mpv_set_property_string(handle, "input-vo-keyboard", "no") != 0)
        mpv_set_property_string(handle, "input-x11-keyboard", "no");
    // N83：Wayland 会话下强制 mpv 的 GL 上下文走 x11egl（实测嵌入成功）——mpv
    // 默认优先 wayland 上下文，与 xcb 嵌入窗口不同源，vo 初始化落不到 wid 上。
    // 判别用显式 wayland：未知会话类型交给 mpv 自选更稳，X11 会话不干预
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland"))
        mpv_set_property_string(handle, "gpu-context", "x11egl");
#endif

    // N83：全平台统一先 show 再嵌入——xcb 下原生窗口链在 show 时才定型，此前取的
    // winId 可能被 Qt 随后重建/重挂而失效；Windows 统一到此时序后不再依赖「子 HWND
    // 平移保身份」的平台特性，两平台同一口径更稳健
    window->show();

    // 把视频渲染嵌入 Qt 窗口（与 libmpv 的 --wid 用法一致）。wid 不可 const：
    // mpv_set_property 的 data 形参是 void *，取 const 对象地址会丢 const 报错
    int64_t wid = int64_t(window->videoWidget()->winId());
    if (mpv_set_property(handle, "wid", MPV_FORMAT_INT64, &wid) < 0) {
        fprintf(stderr, "[oxbow] wid 属性设置失败，视频嵌入将不可用\n");
    } else {
#ifndef Q_OS_WIN
        // N83：诊断日志——与 xwininfo -root -tree 的窗口 ID 比对，确认 vo 初始化时
        // 该窗口仍在（未被 Qt 重建/重挂）且与 mpv 实际处理的 ID 一致。判据：wid
        // 生效时 mpv 绝不自建顶层窗口，出现独立窗口即 wid 未生效
        fprintf(stderr, "[oxbow] wid=0x%llx 已设置\n", static_cast<unsigned long long>(wid));
#endif
    }

    // 嵌入属性全部就绪后才启动事件线程。首次 mpv_wait_event 会解除 mpv
    // 核心的启动门控（客户端 fuzzy_initialized），核心随即为命令行文件
    // 初始化 vo；过早调用会让 mpv 在 wid 生效前先自建窗口再重建嵌入，
    // 表现为启动时闪现 mpv 原生窗口
    controller->startEventThread();

    // 事件循环只由 MPV_EVENT_SHUTDOWN 收口（MainWindow::onMpvShutdown 内 exit()）：入口
    // 抢先返回会让 mpv 在核心还没拆完时就销毁本 client 句柄，两边互等成死锁
    QApplication::exec();
    return 0;
}

// mpv C 插件本体（oxbowcore）：两平台都导出标准入口 mpv_open_cplugin。Windows 上
// mpv 只加载加载壳 main.dll，本模块由壳加载并代填 pfn_* 表（mpv 不会直接加载本模块，
// 导出标准名安全）；Linux / 静态单一 DLL 下 mpv 直接加载。mpv 在独立线程调用入口，
// 可在此建 QApplication 跑事件循环，阻塞本线程不碍 mpv 核心。

#include "pch.h"

#include <mpv/client.h>
#include <mpv/stream_cb.h> // N52：mpv_stream_cb_add_ro 声明

#ifndef Q_OS_WIN
#include <dlfcn.h> // dladdr：定位插件自身 .so
#endif

// 让本模块导入表含 QtSvg（原因见入口处的锚点调用）
#include <QtSvg/QSvgRenderer>

#include "Constants.h"
#include "ExternalTool.h"
#include "Logging.h"
#include "MainWindow.h"
#include "MediaInfoHelper.h"
#include "Version.h"
#include "mpvbridge/MpvController.h"
#include "streams/StreamRelay.h"
#ifdef Q_OS_WIN
#include "platform/TaskbarIntegration.h"
#endif

// Windows 上 pfn_* 由加载者填表：经壳即壳转发填充（mpv 只认它亲自加载的壳），mpv
// 直接加载本模块即 init_sym_table 直接填。入口只做必需项非空校验：缺失即部署或
// 版本错位，一次列出全部缺失项。N83：Linux 无此宏，走 ELF 全局作用域解析，缺符号
// 由 mpv 记为脚本加载失败，同样可诊断（宏成立即必为 Windows，内部无平台分支）。
#ifdef MPV_CPLUGIN_DYNAMIC_SYM
static bool checkMpvSymbols() {
    QString missing;
#define REQUIRE(name)                                                                              \
    do {                                                                                           \
        if (!pfn_##name) {                                                                         \
            if (!missing.isEmpty())                                                                \
                missing += QStringLiteral(", ");                                                   \
            missing += QStringLiteral(#name);                                                      \
        }                                                                                          \
    } while (0)
    REQUIRE(mpv_set_property_string);
    REQUIRE(mpv_set_property);
    REQUIRE(mpv_observe_property);
    REQUIRE(mpv_command_string);
    REQUIRE(mpv_command);
    REQUIRE(mpv_get_property_string);
    REQUIRE(mpv_get_property);
    REQUIRE(mpv_free);
    REQUIRE(mpv_free_node_contents); // B18：FILE_LOADED 主动读 NODE 属性后释放
    REQUIRE(mpv_command_node);       // N52：loadfile 逐文件选项的节点通道
    REQUIRE(mpv_wait_event);
    REQUIRE(mpv_hook_add);           // N121：on_preloaded 同步预适配
    REQUIRE(mpv_hook_continue);      // N121：hook 放行
    REQUIRE(mpv_stream_cb_add_ro);   // N52：oxbow:// 转接协议注册
    REQUIRE(mpv_get_property_async); // N60：推送会话的屏障读（playlist 终值自带终点）
    REQUIRE(mpv_command_async); // N87：未认领按键的 keypress 注入（异步，GUI 线程不等用户命令）
    REQUIRE(mpv_error_string);  // 命令错误码转可读文本（流添加失败反馈用）
    // N107：缩略图第二 core 的建核三件套与预设选项出口
    REQUIRE(mpv_create);
    REQUIRE(mpv_initialize);
    REQUIRE(mpv_terminate_destroy);
    REQUIRE(mpv_set_option_string);
#undef REQUIRE

    if (!missing.isEmpty()) {
        // 日志层尚未装配，直写调试器；一次列全，版本 skew 一眼定位
        const QString msg = QStringLiteral("[%1] missing required mpv symbols: %2\n")
                                .arg(QString::fromLatin1(App::nameAscii), missing);
        OutputDebugStringW(reinterpret_cast<const wchar_t *>(msg.utf16()));
        return false;
    }
    return true;
}
#endif // MPV_CPLUGIN_DYNAMIC_SYM

// 配置文件 = 本模块目录下的 oxbow.ini；模块路径不可得或目录不可写则退 Qt 标准位置
static QString resolveConfigFilePath() {
    QString modulePath;
#ifdef _WIN32
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&resolveConfigFilePath), &hModule)) {
        // 缓冲按需倍增（B4）
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
        // 只读目录（如 Program Files）下 QSettings 静默失败，写探针判定
        const QString dir = QFileInfo(modulePath).absolutePath();
        QTemporaryFile probe(dir + QStringLiteral("/oxbow-XXXXXX.tmp"));
        if (probe.open())
            return QDir(dir).filePath(App::iniFileName());
    }

    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(fallback);
    return QDir(fallback).filePath(App::iniFileName());
}

// 安装内嵌翻译：language 键空/auto 跟随系统，en 用原文，其余按 QLocale 加载
// :/i18n 资源，失败静默回退原文
static void installTranslator(const QString &configFile) {
    if (!qApp)
        return;
    QString lang;
    {
        QSettings s(configFile, QSettings::IniFormat);
        lang = s.value(QStringLiteral("language"), App::languageDefault()).toString().trimmed();
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
    if (!checkMpvSymbols())
        return 1; // 缺符号：让 mpv 记为脚本加载失败，而非运行期崩溃
#endif

    static bool started = false;
    if (started)
        return 0;
    started = true;

    // 让位：wid 为正数说明外部宿主（--wid 或其他插件）先占了画面归属，再接手只会
    // 双 vo 互相覆盖。-1（未配置默认，mpv 自己的窗口）与 0（root 壁纸，手册
    // options-wid）都无外部宿主；属性读不到按未配置处理
    int64_t existingWid = -1;
    if (mpv_get_property(handle, "wid", MPV_FORMAT_INT64, &existingWid) >= 0 && existingWid > 0)
        return 0; // 让位不算失败

    // 本模块不用 QSvgRenderer，构造一次只为留下 QtSvg 导入项：svg 图标依赖的
    // iconengines / imageformats 插件须先有 QtSvg 常驻才加载得上；必须真调用——
    // 裸取地址会被优化器折叠，导入项静默消失（表现：图标全空且构建无异常）
    {
        QSvgRenderer svgAnchor;
        Q_UNUSED(svgAnchor);
    }

    // argc/argv 的生命周期必须覆盖整个进程
    static int argc = 1;
    static char *argv[] = {App::nameAscii, nullptr};

    if (!qApp) {
#ifndef Q_OS_WIN
        // N83：Wayland 的 winId 非 XID（实测 BadWindow），默认强制 xcb（XWayland）；
        // 用户显式设置过 QT_QPA_PLATFORM 则不覆盖，纯 Wayland 不受支持
        if (qEnvironmentVariable("QT_QPA_PLATFORM").isEmpty())
            qputenv("QT_QPA_PLATFORM", "xcb");
#endif
        // 有意不 delete：QApplication 须与进程同生命周期
        (void)new QApplication(argc, argv);
        // N111：Qt 构造会把 LC_NUMERIC 离开 "C"，libmpv 要求恒为 "C"（client.h
        // 「C environment」），违者 mpv_create 失败——复位一次覆盖后续创建点
        setlocale(LC_NUMERIC, "C");
        QApplication::setApplicationName(App::name());
        // 退出只由 MPV_EVENT_SHUTDOWN 收口：Qt 默认末窗即退会抢先返回，入口一返回
        // mpv 即销毁句柄，与仍在拆核心的主线程互等死锁
        QApplication::setQuitOnLastWindowClosed(false);
    }

    const QString configFile = resolveConfigFilePath();

    // N126：日志出口在一切子系统出生前装配，模式只读一次
    Logging::install(configFile);

    // N84：启动诊断行，自报版本身份，与 About 对话框同源
    OXBOW_LOG_INFO().noquote() << App::versionInfo();

    installTranslator(configFile);

    // N142：MediaInfo 库在入口预载——GUI 线程、先于任何元数据扫描，此后工作线程对 shim
    // 只剩只读
    {
        QSettings s(configFile, QSettings::IniFormat);
        MediaInfoHelper::preload(s.value(externalToolIniKey(ExternalTool::MediaInfo)).toString(),
                                 QFileInfo(configFile).absolutePath());
    }

    // N143：sub-delay 追加进 --reset-on-next-file——手册载明该选项默认不重置任何项，
    // 不追加则上一个文件调过的字幕偏移会串到下一个文件；复位语义由 mpv 承担。
    // 写只走 change-list append 单条，用户既有项不经插件的手；append 不去重
    // （实测重复调用即累加），故按读回的现值判重后跳过
    {
        char *raw = mpv_get_property_string(handle, "reset-on-next-file");
        const QString current = raw ? QString::fromUtf8(raw) : QString();
        mpv_free(raw);
        const QStringList items = current.split(u',', Qt::SkipEmptyParts);
        const bool already = items.contains(QStringLiteral("all"), Qt::CaseInsensitive) ||
                             items.contains(QStringLiteral("sub-delay"));
        if (!already) {
            const char *args[] = {"change-list", "reset-on-next-file", "append", "sub-delay",
                                  nullptr};
            if (mpv_command(handle, args) < 0)
                OXBOW_LOG_WARNING() << "failed to append sub-delay to reset-on-next-file";
        }
    }

    auto *controller = new MpvController(handle);

    // N52：装配硬顺序——install 须早于 startEventThread（首次 wait_event 解除启动
    // 门控后命令行文件即 open 流）、早于任何提供者出生（构造期读盘就要取 relay）；
    // shared_ptr 寿命覆盖 exec() 全程
    auto relay = std::make_shared<StreamRelay>();
    if (!relay->install(handle))
        return 1; // 注册失败让 mpv 记为脚本加载失败

    // N116：主窗构造前按 ini 套样式与配色；入口栈对象寿命覆盖 exec()，主窗只收
    // 非拥有指针做运行期套用
    AppearanceManager appearance;
    {
        QSettings s(configFile, QSettings::IniFormat);
        appearance.apply(
            s.value(QStringLiteral("style"), App::interfaceStyleDefault()).toString(),
            s.value(QStringLiteral("colorScheme"), App::colorSchemeDefault()).toString());
    }

    auto *window = new MainWindow(controller, &appearance, configFile, std::move(relay));
    controller->setParent(window);

    // GUI 宿主：列表播完进 idle 而非退出
    mpv_set_property_string(handle, "idle", "yes");
    // EOF 自动前进，循环行为交给 loop-* 设置
    mpv_set_property_string(handle, "keep-open", "no");
    // 空闲期保有视频输出：空闲画面是拖放入口，OSD 与 console 键也依赖 VO 存在；
    // 显式置 yes——删行会让这几项随启动方式（pseudo-gui）漂移
    mpv_set_property_string(handle, "force-window", "yes");
    // N97：拖放归插件；wid 嵌入下 mpv 的 vo 子窗照样收得到拖放，不关会叠成两份动作
    mpv_set_property_string(handle, "drag-and-drop", "no");
    // N119：输入与光标显式接管，不搭 mpv 实现细节（wid 嵌入下 VO 子窗
    // EnableWindow(0) 的巧合、光标预推）；光标行为单源于 Qt 层（N67）
    mpv_set_property_string(handle, "input-cursor", "no");
    mpv_set_property_string(handle, "cursor-autohide", "no");

#ifndef Q_OS_WIN
    // N83：X11 嵌入的输入在事件层关到底（mpv 事件掩码截走指针事件，Qt 收不到
    // 鼠标），旧版 mpv 回退 input-x11-keyboard；Windows 无对应物（焦点在 Qt）。
    // N87：绑定层不干涉——不设 input-default-bindings，未认领按键回落查的正是
    // 那张默认表
    if (mpv_set_property_string(handle, "input-vo-keyboard", "no") != 0)
        mpv_set_property_string(handle, "input-x11-keyboard", "no");
    // N83：Wayland 会话强制 x11egl（mpv 默认 wayland 上下文与 xcb 嵌入窗口不同源，
    // vo 落不到 wid 上，实测）；X11 与未知会话类型不干预
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland"))
        mpv_set_property_string(handle, "gpu-context", "x11egl");
#endif

    // N83：全平台先装伴生组件再 show——xcb 下原生窗口链 show 时才定型，过早取
    // winId 可能被 Qt 重建而失效。N127：任务栏集成必须早于 show()，
    // TaskbarButtonCreated 的原生事件过滤器要先就位；父对象设为主窗，与进程同寿命
#ifdef Q_OS_WIN
    new TaskbarIntegration(controller, window);
#endif

    window->show();

    // 视频渲染嵌入 Qt 窗口；wid 不可 const：mpv_set_property 的 data 形参是 void *
    int64_t wid = int64_t(window->videoWidget()->winId());
    if (mpv_set_property(handle, "wid", MPV_FORMAT_INT64, &wid) < 0) {
        OXBOW_LOG_WARNING() << "wid 属性设置失败，视频嵌入将不可用";
    } else {
#ifndef Q_OS_WIN
        // N83：与 xwininfo -root -tree 比对——wid 生效时 mpv 绝不自建顶层窗口
        OXBOW_LOG_DEBUG().noquote()
            << QStringLiteral("wid=0x%1 已设置").arg(static_cast<qulonglong>(wid), 0, 16);
#endif
    }

    // 嵌入就绪后才启动事件线程：首次 mpv_wait_event 解除核心启动门控，过早调用
    // 会让 mpv 在 wid 生效前先自建窗口再重建，启动闪现原生窗口
    controller->startEventThread();

    // 只由 SHUTDOWN 收口（onMpvShutdown 内 exit()）：抢先返回会与拆核心互等死锁
    QApplication::exec();
    return 0;
}

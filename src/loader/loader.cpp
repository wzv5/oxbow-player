// 加载壳（loader shell）。
//
// mpv 加载本 DLL（scripts/oxbow/main.dll）并调用 mpv_open_cplugin。
// 本文件刻意保持为纯 Win32、零 Qt 依赖：这样编译产物没有任何 Qt 导入，
// 也就不会在加载时要求 Qt 运行库能被搜索到（避免之前“Qt 必须放在 mpv.exe 旁”
// 的限制，以及 MSVC 延迟加载对 Qt moc 数据符号 staticMetaObject 的 LNK1194 报错）。
//
// 壳的工作：
//   1. 取得自身所在目录（即 scripts/oxbow/）；
//   2. 设置 Qt 插件搜索环境变量，指向共置的 Qt 插件子目录；
//   3. 以 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 精确加载真正带 Qt 的插件
//      oxbowcore.dll（同目录），其依赖的 Qt 运行库从该目录解析；
//      不改动进程级默认搜索行为，避免影响 mpv 自身的延迟 DLL 加载；
//   4. 把 mpv 的入口调用转发给 oxbowcore.dll 导出的 mpv_open_cplugin。
//
// 这样 Qt 运行库随插件一起放在 scripts/oxbow/ 下即可，无需污染 mpv.exe 目录。

#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include <string>

typedef int (*MpvOpenCpluginFunc)(void *handle);

static HMODULE g_core = nullptr;
static MpvOpenCpluginFunc g_entry = nullptr;
static bool g_tried = false;

// 通过本函数自身地址取得当前模块句柄（不依赖模块名）。
static HMODULE ThisModule() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &h);
    return h;
}

// 取模块完整路径：返回值等于缓冲大小时视为截断，倍增重试直至容纳（B4）。
static std::wstring ModuleFilePath(HMODULE h) {
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = 0;
    while ((len = GetModuleFileNameW(h, buf.data(), static_cast<DWORD>(buf.size()))) == buf.size())
        buf.resize(buf.size() * 2);
    buf.resize(len);
    return buf;
}

// 取模块所在目录（含末尾反斜杠）。
static std::wstring ModuleDir(HMODULE h) {
    const std::wstring path = ModuleFilePath(h);
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos + 1);
}

static bool LoadCore() {
    if (g_tried)
        return g_core != nullptr;
    g_tried = true;

    const std::wstring pluginDir = ModuleDir(ThisModule());
    if (pluginDir.empty())
        return false;

#ifndef OXBOW_STATIC
    // 让 Qt 在运行时能定位到共置的插件（平台插件/样式/图像格式等）。
    // windeployqt 默认把这些插件类型目录直接部署在插件目录下
    // （platforms/qwindows.dll、styles/、imageformats/ …），因此：
    //   - QT_PLUGIN_PATH 指向插件根目录（Qt 会去 <path>/platforms、<path>/styles 等下找）；
    //   - QT_QPA_PLATFORM_PLUGIN_PATH 精确指向 platforms 目录。
    SetEnvironmentVariableW(L"QT_PLUGIN_PATH", pluginDir.c_str());

    const std::wstring platformPath = pluginDir + L"platforms";
    SetEnvironmentVariableW(L"QT_QPA_PLATFORM_PLUGIN_PATH", platformPath.c_str());
#endif

    const std::wstring corePath = pluginDir + L"oxbowcore.dll";

    // DLL_LOAD_DIR：加载期间把 oxbowcore.dll 自身目录临时置于依赖搜索序之首，
    // 共置的 Qt 运行库借此解析；DEFAULT_DIRS 保留系统目录与应用程序目录。
    // 相比改写进程级默认搜索（SetDefaultDllDirectories），此处作用域仅限本次
    // 加载，不影响 mpv 之后按 PATH/CWD 的延迟 DLL 加载。
    g_core = LoadLibraryExW(corePath.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_core) {
        OutputDebugStringW(L"[oxbow] 加载 oxbowcore.dll 失败\n");
        return false;
    }

    // N18：预先显式加载共置的 MediaInfo.dll（DLL_LOAD_DIR + DEFAULT_DIRS），
    // 不关心返回值。MediaInfoHelper 经 shim 内 MediaInfoDLL_Load() 调用的
    // LoadLibrary("MediaInfo.dll") 会命中此已加载模块，复用同一句柄——从而
    // 绕过 shim 的 GetProcAddress 在进程默认搜索序下找不到同目录 dll 的问题。
    // 失败时由 MediaInfoHelper::available() 返回 false、UI 优雅降级
    LoadLibraryExW((pluginDir + L"MediaInfo.dll").c_str(), nullptr,
                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    g_entry = reinterpret_cast<MpvOpenCpluginFunc>(GetProcAddress(g_core, "mpv_open_cplugin"));
    if (!g_entry) {
        FreeLibrary(g_core);
        g_core = nullptr;
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) int mpv_open_cplugin(void *handle) {
    if (!LoadCore() || !g_entry)
        return 1; // 非 0 表示加载失败
    return g_entry(handle);
}

// 加载壳（loader shell）。
//
// mpv 加载本 DLL（scripts/mpvqt/main.dll）并调用 mpv_open_cplugin。
// 本文件刻意保持为纯 Win32、零 Qt 依赖：这样编译产物没有任何 Qt 导入，
// 也就不会在加载时要求 Qt 运行库能被搜索到（避免之前“Qt 必须放在 mpv.exe 旁”
// 的限制，以及 MSVC 延迟加载对 Qt moc 数据符号 staticMetaObject 的 LNK1194 报错）。
//
// 壳的工作：
//   1. 取得自身所在目录（即 scripts/mpvqt/）；
//   2. 把该目录（以及其中的 Qt 插件子目录）加入 DLL / Qt 搜索路径；
//   3. 用 LoadLibrary 加载真正带 Qt 的插件 mpvqtcore.dll（同目录），
//      由其所在目录就能解析出共置的 Qt 运行库；
//   4. 把 mpv 的入口调用转发给 mpvqtcore.dll 导出的 mpv_open_cplugin。
//
// 这样 Qt 运行库随插件一起放在 scripts/mpvqt/ 下即可，无需污染 mpv.exe 目录。

#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include <cwchar>

typedef int (*MpvOpenCpluginFunc)(void *handle);

static HMODULE g_core = nullptr;
static MpvOpenCpluginFunc g_entry = nullptr;
static bool g_tried = false;

// 通过本函数自身地址取得当前模块句柄（不依赖模块名）。
static HMODULE ThisModule()
{
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &h);
    return h;
}

// 取模块路径的目录部分（含末尾反斜杠）。
static void DirOfModule(HMODULE h, wchar_t *dir, size_t dirLen)
{
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(h, path, MAX_PATH);
    size_t len = wcsnlen(path, MAX_PATH);
    while (len > 0 && path[len - 1] != L'\\' && path[len - 1] != L'/')
        --len;
    wcsncpy_s(dir, dirLen, path, len);
    dir[len] = L'\0';
}

static bool LoadCore()
{
    if (g_tried)
        return g_core != nullptr;
    g_tried = true;

    wchar_t pluginDir[MAX_PATH] = {0};
    DirOfModule(ThisModule(), pluginDir, MAX_PATH);

    // 仅执行一次：把插件目录纳入“默认 DLL 搜索路径”，
    // 使 mpvqtcore.dll 的 Qt 运行库（与其共置）可被解析。
    // 同时保留应用程序目录（mpv.exe 所在）与系统目录，不影响 mpv 自身加载。
    static bool searchConfigured = false;
    if (!searchConfigured) {
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_USER_DIRS |
                                 LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                 LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
        AddDllDirectory(pluginDir);
        searchConfigured = true;
    }

    // 让 Qt 在运行时能定位到共置的插件（平台插件/样式/图像格式等）。
    // windeployqt 默认把这些插件类型目录直接部署在插件目录下
    // （platforms/qwindows.dll、styles/、imageformats/ …），因此：
    //   - QT_PLUGIN_PATH 指向插件根目录（Qt 会去 <path>/platforms、<path>/styles 等下找）；
    //   - QT_QPA_PLATFORM_PLUGIN_PATH 精确指向 platforms 目录。
    SetEnvironmentVariableW(L"QT_PLUGIN_PATH", pluginDir);

    wchar_t platformPath[MAX_PATH] = {0};
    wcscpy_s(platformPath, pluginDir);
    wcscat_s(platformPath, L"platforms");
    SetEnvironmentVariableW(L"QT_QPA_PLATFORM_PLUGIN_PATH", platformPath);

    wchar_t corePath[MAX_PATH] = {0};
    wcscpy_s(corePath, pluginDir);
    wcscat_s(corePath, L"mpvqtcore.dll");

    // 使用默认搜索路径（已包含插件用户目录）加载核心插件，
    // 其 Qt 依赖借此从同目录解析。
    g_core = LoadLibraryExW(corePath, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_core) {
        OutputDebugStringW(L"[mpvqt] 加载 mpvqtcore.dll 失败\n");
        return false;
    }

    g_entry = reinterpret_cast<MpvOpenCpluginFunc>(
        GetProcAddress(g_core, "mpv_open_cplugin"));
    if (!g_entry) {
        FreeLibrary(g_core);
        g_core = nullptr;
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) int mpv_open_cplugin(void *handle)
{
    if (!LoadCore() || !g_entry)
        return 1; // 非 0 表示加载失败
    return g_entry(handle);
}

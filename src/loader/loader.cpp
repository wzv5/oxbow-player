// 加载壳（构建产物 loader.dll，部署时改名为 main.dll）：mpv 加载它并调用导出的
// mpv_open_cplugin。动态 Qt 形态下本壳把插件目录指给 Qt、按 DLL_LOAD_DIR 加载核心，
// 让共置的 Qt 运行库能被精确解析。核心对 mpv 不可见，其 pfn_* 表由本壳代填（复刻
// mpv 对脚本 DLL 的填表动作），随后把入口调用转发给核心。
// 静态形态下核心不依赖同置运行库，自动部署不走壳——壳仍照常构建，供手工两件式部署
// 与后续形态复用；OXBOW_STATIC 下不设 Qt 插件环境变量。

#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include <cstring>
#include <string>

// 生成文件由构建期扫 mpv/ 目录产出（CMakeLists 的 oxbow_mpv_syms 步骤），自带
// MPV_CPLUGIN_DYNAMIC_SYM 定义。壳只持有指针，不调用任何 mpv 函数。
#include "mpv_cplugin_syms.h"

typedef int (*MpvOpenCpluginFunc)(void *handle);

static HMODULE g_core = nullptr;
static MpvOpenCpluginFunc g_entry = nullptr;
static bool g_tried = false;

// 经自身地址取当前模块句柄，不依赖模块名。
static HMODULE ThisModule() {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &h);
    return h;
}

// 完整路径，缓冲不够则倍增重试（B4）。
static std::wstring ModuleFilePath(HMODULE h) {
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = 0;
    while ((len = GetModuleFileNameW(h, buf.data(), static_cast<DWORD>(buf.size()))) == buf.size())
        buf.resize(buf.size() * 2);
    buf.resize(len);
    return buf;
}

// 模块所在目录，含末尾反斜杠。
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
    // Qt 插件目录与运行库同在插件目录下，指给 Qt
    SetEnvironmentVariableW(L"QT_PLUGIN_PATH", pluginDir.c_str());

    const std::wstring platformPath = pluginDir + L"platforms";
    SetEnvironmentVariableW(L"QT_QPA_PLATFORM_PLUGIN_PATH", platformPath.c_str());
#endif

    const std::wstring corePath = pluginDir + L"oxbowcore.dll";

    // DLL_LOAD_DIR：核心目录临时居依赖搜索序之首，共置的 Qt 运行库借此解析；
    // 不改进程级默认搜索，不影响 mpv 的延迟 DLL 加载
    g_core = LoadLibraryExW(corePath.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_core) {
        OutputDebugStringW(L"[oxbow] failed to load oxbowcore.dll\n");
        return false;
    }

    g_entry = reinterpret_cast<MpvOpenCpluginFunc>(GetProcAddress(g_core, "mpv_open_cplugin"));
    if (!g_entry) {
        OutputDebugStringW(L"[oxbow] mpv_open_cplugin not found in oxbowcore.dll\n");
        FreeLibrary(g_core);
        g_core = nullptr;
        return false;
    }
    return true;
}

// 把 mpv 填进壳槽的函数入口逐条搬进核心的同名导出变量；槽为空（宿主无此符号）
// 按 mpv 的形态跳过，必需性由核心入口校验
static void ForwardSymTable(HMODULE core) {
    for (const MpvSymSlot &e : kMpvSyms) {
        void **dst = reinterpret_cast<void **>(GetProcAddress(core, e.name));
        void *fn;
        memcpy(&fn, e.slot, sizeof fn);
        if (dst && fn)
            memcpy(dst, &fn, sizeof fn);
    }
}

extern "C" __declspec(dllexport) int mpv_open_cplugin(void *handle) {
    if (!LoadCore() || !g_entry)
        return -1; // 任何非零都被 mpv 记为脚本加载失败
    ForwardSymTable(g_core);
    if (g_entry(handle) != 0) {
        OutputDebugStringW(L"[oxbow] oxbowcore entry returned non-zero\n");
        return -1;
    }
    return 0;
}

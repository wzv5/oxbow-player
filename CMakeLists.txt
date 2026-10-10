cmake_minimum_required(VERSION 3.16)
project(oxbow-player VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(OXBOW_BUILD_TESTS "Build Qt Test unit tests for oxbow" OFF)
option(OXBOW_STATICQT "Link Qt statically (skip windeployqt at deploy time)" OFF)
# N68：强行令 Windows 也走小窗的纯 Qt 降级路径（无等比缩放/边缘吸附），便于本机验证降级形态
option(OXBOW_PUREQT "Use the pure-Qt implementation path for the mini window" OFF)

# N83：静态 Qt 依赖 MSVC 运行库口径与 windeployqt 部署语义，限 Windows（Linux 用系统
# Qt 动态链接），非 Windows 在 configure 期拦截。运行库收紧只看 OXBOW_STATICQT：
# clang（MSVC ABI、GNU 前端）下 CMake 的 MSVC 判定为假，若限定 MSVC，静态 Qt 的
# Release 插件对象（_ITERATOR_DEBUG_LEVEL=0）会与 Debug 对象（=2）在链接期 failifmismatch
# ——实测教训，不得加回 MSVC 限定
if(OXBOW_STATICQT AND NOT WIN32)
    message(FATAL_ERROR "OXBOW_STATICQT is Windows-only (Linux links the system Qt)")
endif()

if(OXBOW_STATICQT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets LinguistTools Concurrent Network)

# ---------------------------------------------------------------------------
# 目标 1：加载壳 main.dll（纯 Win32，无 Qt 依赖）
# mpv 启动时会加载 scripts/oxbow/main.dll 并调用其导出的 mpv_open_cplugin。
# 壳仅负责把插件目录加入 DLL 搜索路径，再动态加载真正带 Qt 的 oxbowcore.dll，
# 从而让 Qt 运行库能与插件共置、随插件目录分发。
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(oxbow_loader MODULE src/loader/loader.cpp)
    set_target_properties(oxbow_loader PROPERTIES PREFIX "" OUTPUT_NAME "main")
    if(MSVC)
        target_compile_options(oxbow_loader PRIVATE /utf-8)
    endif()
endif()

# ---------------------------------------------------------------------------
# 目标 2：真正的插件 oxbowcore.dll（Qt + mpv 逻辑）
# ---------------------------------------------------------------------------
add_library(oxbowcore MODULE
    src/plugin.cpp
    src/Constants.h
    src/Version.cpp
    src/Version.h
    src/StreamTransit.cpp
    src/StreamTransit.h
    src/ByteStream.h
    src/StreamEndpoint.h
    src/HttpByteStream.cpp
    src/HttpByteStream.h
    src/StreamRelay.cpp
    src/StreamRelay.h
    src/ActionManager.cpp
    src/ActionManager.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MpvController.cpp
    src/MpvController.h
    src/MpvKeyName.cpp
    src/MpvKeyName.h
    src/HttpClient.cpp
    src/HttpClient.h
    src/NavigatorPanel.cpp
    src/NavigatorPanel.h
    src/PlaylistDelegate.cpp
    src/PlaylistDelegate.h
    src/PlaylistEntity.cpp
    src/PlaylistEntity.h
    src/Playlist.cpp
    src/Playlist.h
    src/PlaylistRegistry.cpp
    src/PlaylistRegistry.h
    src/PlaylistListView.cpp
    src/PlaylistListView.h
    src/PlaylistFile.cpp
    src/PlaylistFile.h
    src/LocalFileEntity.cpp
    src/LocalFileEntity.h
    src/HttpEntity.cpp
    src/HttpEntity.h
    src/WebDavEntity.h
    src/JellyfinEntity.cpp
    src/JellyfinEntity.h
    src/ServiceRegistry.h
    src/TraversalPolicy.h
    src/PlaylistProvider.cpp
    src/PlaylistProvider.h
    src/ProviderCatalog.cpp
    src/ProviderCatalog.h
    src/ProviderSettingsPage.cpp
    src/ProviderSettingsPage.h
    src/ListSettingsDialog.cpp
    src/ListSettingsDialog.h
    src/StaticProvider.cpp
    src/StaticProvider.h
    src/DirProvider.cpp
    src/DirProvider.h
    src/WebDavProvider.cpp
    src/WebDavProvider.h
    src/JellyfinProvider.cpp
    src/JellyfinProvider.h
    src/ExternalProvider.cpp
    src/ExternalProvider.h
    src/LoadingOverlay.cpp
    src/LoadingOverlay.h
    src/PlaylistWidget.cpp
    src/PlaylistWidget.h
    src/BookmarkManager.cpp
    src/BookmarkManager.h
    src/ChapterSlider.cpp
    src/ChapterSlider.h
    src/BookmarkEditorDialog.cpp
    src/BookmarkEditorDialog.h
    src/Util.cpp
    src/Util.h
    src/MediaInfoHelper.cpp
    src/MediaInfoHelper.h
    src/MpvConf.cpp
    src/MpvConf.h
    src/InputConf.cpp
    src/InputConf.h
    src/SettingsDialog.cpp
    src/SettingsDialog.h
    src/ShortcutsDialog.cpp
    src/ShortcutsDialog.h
    src/MpvBindingDialog.cpp
    src/MpvBindingDialog.h
    src/FilterListWidget.cpp
    src/FilterListWidget.h
)

# mpv/client.h 已内置在仓库根目录的 mpv/ 下
target_include_directories(oxbowcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# N84：构建期计算版本身份（短 hash[-dirty] + UTC 构建时刻），写生成头 revision.h。
# custom target 刻意不给 OUTPUT：无输出的 custom target 每次构建都执行，hash 在
# 构建期重算正是要的语义；configure_file 内容不变则不更新文件，版本与 hash 不变
# 即不重编，构建时刻每次不同则 Version.cpp 每次必重编（已知代价，被"revision.h
# 唯一消费者是 Version.cpp"的设计框死在一个小 TU 上，MainWindow.cpp / plugin.cpp
# 一律只 include Version.h）。输入全部经 -D 传入，不得用 cache 变量承载 hash
# （二次 configure 会短路复用旧值）。脚本内所有失败路径落 unknown 继续，
# 绝不 FATAL_ERROR——源码包（无 .git）构建必须能过。
# BYPRODUCTS 声明生成头路径，让 Ninja 认得这个由该步骤产出的文件；
# add_dependencies 保证首次构建时 Version.cpp 编译前生成头已存在。
# 生成器覆盖面：Ninja（multi/single config）与 MSBuild 均支持此组合。
# ---------------------------------------------------------------------------
set(OXBOW_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
# 注意：-D 值刻意不加引号（裸路径）。Windows 下 Ninja 经 cmd.exe /C 执行本命令，
# \" 转义会被 MSVC 式参数解析吃成字面引号混进 -D 值，configure_file 随即把带引号
# 的"绝对路径"当相对路径解析而失败；裸值没有引号可损耗。代价是含空格的路径不支持
# （含空格时 CMake 会自动补引号重新踩坑）——与本项目其余路径假设一致，出现即构建
# 期显式报错、可诊断
add_custom_target(oxbow_revision
    COMMAND ${CMAKE_COMMAND}
        -DOXBOW_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOXBOW_VERSION_STRING=${PROJECT_VERSION}
        -DOXBOW_REVISION_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/revision.h.in
        -DOXBOW_REVISION_OUTPUT=${OXBOW_GENERATED_DIR}/revision.h
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/OxbowRevision.cmake
    BYPRODUCTS "${OXBOW_GENERATED_DIR}/revision.h"
    COMMENT "重算版本身份并生成 revision.h"
    VERBATIM)
add_dependencies(oxbowcore oxbow_revision)
target_include_directories(oxbowcore PRIVATE "${OXBOW_GENERATED_DIR}")

# C 插件不链接 libmpv，符号由 mpv 在加载时动态解析。
# 注意：不要定义 MPV_ENABLE_DEPRECATED=0，内置 client.h 的动态符号段
# 无条件引用已弃用的 mpv_get_wakeup_pipe，关掉弃用声明会编译失败。
# N83：动态符号表为 Windows 专属机制——client.h 注释明确该表是 targeting Windows
# 的推荐做法（PE 加载器无全局符号概念，多定义容忍依赖 selectany，ELF 无对应机制）。
# Linux 走 cplugin 标准机制：mpv 以 -rdynamic 导出 client API，插件保留 mpv_* 未定义
# 符号，dlopen 时由 ELF 动态链接器从宿主全局作用域解析；缺符号即加载失败，由 mpv
# 记为脚本加载失败，同样可诊断。plugin.cpp 的解析代码同步收进同名宏守卫。
if(WIN32)
    target_compile_definitions(oxbowcore PRIVATE MPV_CPLUGIN_DYNAMIC_SYM)
endif()
# N83：MediaInfoDLL shim 以 UNICODE/_UNICODE 宏选宽/窄字符 API（MediaInfoHelper 全按
# 宽字符口径书写）。全平台显式定义、不依赖 Qt/MSVC 的默认值（维护者裁决）；Linux 缺
# 定义时 shim 落成 char 口径，宽字面量实参的重载解析失配到 size_t 章节重载
target_compile_definitions(oxbowcore PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(oxbowcore PRIVATE /utf-8)
endif()
if(OXBOW_STATICQT)
    target_compile_definitions(oxbowcore PRIVATE OXBOW_STATICQT)
endif()
if(OXBOW_PUREQT)
    target_compile_definitions(oxbowcore PRIVATE OXBOW_PUREQT)
endif()

target_link_libraries(oxbowcore PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Network)
# A19：全屏期 DWM 圆角裁切抑制（仅 Windows；dwmapi 为 Vista 起系统组件）
if(WIN32)
    target_link_libraries(oxbowcore PRIVATE dwmapi)
endif()

# N83：X11 专用小工具集（X11Util）——EWMH 直发置顶（小窗置顶，
# 见 toggleMini）。Xlib 自建连接。Linux 本就依赖 X11（xcb 平台 + mpv X11 路径），
# 此处为编译期硬依赖；X11Util 仅非 Windows 参与构建，Windows 完全不出现该翻译单元
# （源文件不入主列表）。源内不再叠平台守卫：本段的构建条件是唯一权威——错误平台把
# 它编进去会因缺 X11 头 / 库直接失败（要的显影），源内守卫反而把实现静默掏空、
# 把编译错降级成链接期未定义符号。守卫用 NOT WIN32 而非 UNIX：UNIX 还含非目标的
# macOS，而 NOT WIN32 与代码侧调用点（MainWindow.cpp）的 Q_OS_WIN 守卫同指
# 非 Windows；X11::X11 为 imported target（CMake ≥3.14），自带 include 路径
if(NOT WIN32)
    find_package(X11 REQUIRED)
    target_link_libraries(oxbowcore PRIVATE X11::X11)
    target_sources(oxbowcore PRIVATE
        src/X11Util.cpp
        src/X11Util.h
    )
endif()

# 预编译头：Qt 与标准库集中进 src/pch.h，加快编译
target_precompile_headers(oxbowcore PRIVATE src/pch.h)
# N83：产物名分平台——Windows oxbowcore.dll（加载壳按名查找，不得改名）；其余平台
# main.so（与 Windows 的 main.dll 命名对称，mpv 对脚本 .so 文件名无要求；PREFIX ""
# 在 UNIX 下同时去掉 lib 前缀）
set_target_properties(oxbowcore PROPERTIES PREFIX "")
if(WIN32)
    set_target_properties(oxbowcore PROPERTIES OUTPUT_NAME "oxbowcore")
else()
    set_target_properties(oxbowcore PROPERTIES OUTPUT_NAME "main")
endif()

# N49：图标素材内嵌为资源（icons/<包名>/<语义键>.svg → :/icons/<包名>/<语义键>.svg）。
# 与 qt_add_translations 生成的资源并存；静态构建下随二进制内嵌，无外部文件与部署路径问题
qt_add_resources(oxbowcore "icons"
    PREFIX "/icons"
    BASE "icons"
    FILES
        icons/icons8/locate-playing.svg
        icons/icons8/search.svg
        icons/icons8/playlist.svg
        icons/icons8/playlist-on.svg
        icons/icons8/settings.svg
        icons/icons8/filter-add.svg
        icons/icons8/filter-remove.svg
)

qt_add_translations(oxbowcore MERGE_QT_TRANSLATIONS
    LUPDATE_OPTIONS -no-obsolete
    TS_FILES translations/oxbow_zh_CN.ts
)

# ---------------------------------------------------------------------------
# 部署：把插件建成独立脚本目录
#   portable_config/scripts/oxbow/main.dll        （加载壳）
#   portable_config/scripts/oxbow/oxbowcore.dll    （真插件）
#   portable_config/scripts/oxbow/*.dll + plugins/ （Qt 运行库，随插件目录分发）
# Qt 运行库用 windeployqt 部署到插件所在目录，而不是 mpv 主程序目录。
# OXBOW_STATICQT=ON 时 Qt 已静态链接进 oxbowcore.dll，跳过 windeployqt；
# 加载壳 main.dll 仍照常部署——MediaInfo.dll 依赖壳预加载才能被解析。
# ---------------------------------------------------------------------------
# N83：部署为 Windows 专属（windeployqt 与 DLL 语义；Linux 不自动部署，只编译产物）
set(MPV_BIN_DIR "" CACHE PATH "mpv.exe 所在目录（仅 Windows）；设置后构建完毕自动部署")
if(WIN32 AND MPV_BIN_DIR)
    if(NOT OXBOW_STATICQT)
        find_program(WINDEPLOYQT_EXE windeployqt HINTS "${Qt6_DIR}/../../../bin")
        if(NOT WINDEPLOYQT_EXE)
            message(FATAL_ERROR "windeployqt 未找到：请确认 CMAKE_PREFIX_PATH 指向 Qt 安装目录（其 bin 下含 windeployqt）")
        endif()
    endif()
    set(OXBOW_SCRIPT_DIR "${MPV_BIN_DIR}/portable_config/scripts/oxbow")
    add_custom_command(TARGET oxbowcore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OXBOW_SCRIPT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oxbow_loader> "${OXBOW_SCRIPT_DIR}/main.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oxbowcore> "${OXBOW_SCRIPT_DIR}/oxbowcore.dll")
    if(NOT OXBOW_STATICQT)
        add_custom_command(TARGET oxbowcore POST_BUILD
            COMMAND ${WINDEPLOYQT_EXE} --no-translations --compiler-runtime "${OXBOW_SCRIPT_DIR}/oxbowcore.dll")
    endif()
endif()

# ---------------------------------------------------------------------------
# 可选第三方运行时依赖：MediaInfo.dll
# 动态加载（不链接），存在即随插件目录部署；缺失仅跳过拷贝不影响编译。
# ---------------------------------------------------------------------------
set(MEDIAINFO_DLL "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/mediainfo/MediaInfo.dll")
if(WIN32 AND MPV_BIN_DIR AND EXISTS "${MEDIAINFO_DLL}")
    add_custom_command(TARGET oxbowcore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MEDIAINFO_DLL}" "${OXBOW_SCRIPT_DIR}/MediaInfo.dll")
endif()

# ---------------------------------------------------------------------------
# R3：单元测试（默认不构建）。不纳入插件部署路径。
# 启用方式：cmake -B build -DOXBOW_BUILD_TESTS=ON ...
# 运行：ctest --test-dir build -C Debug -R <套件短名>   （如 ctest --test-dir build -C Debug -R Util）
#       ./build/Debug/oxbow_tests_Util    （直调 exe；末尾加槽名即单用例）
# R5：一类一可执行（QTest 标准用法）+ 测试静态库，被测源码只编一份；
# 插件构建图（oxbowcore / 翻译 / 部署）不受影响。
# ---------------------------------------------------------------------------
if(OXBOW_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()

    # 被测源码进测试静态库，各测试 exe 链接复用（静态归档只拉被引用 object）
    add_library(oxbow_testlib STATIC
        src/DirProvider.cpp
        src/DirProvider.h
        src/WebDavProvider.cpp
        src/WebDavProvider.h
        src/JellyfinProvider.cpp
        src/JellyfinProvider.h
        src/ExternalProvider.cpp
        src/ExternalProvider.h
        src/PlaylistProvider.cpp
        src/PlaylistProvider.h
        src/ProviderCatalog.cpp
        src/ProviderCatalog.h
        src/ProviderSettingsPage.cpp
        src/ProviderSettingsPage.h
        src/StaticProvider.cpp
        src/StaticProvider.h
        src/MpvConf.cpp
        src/MpvConf.h
        src/InputConf.cpp
        src/InputConf.h
        src/MpvKeyName.cpp
        src/MpvKeyName.h
        src/BookmarkManager.cpp
        src/BookmarkManager.h
        src/PlaylistEntity.cpp
        src/PlaylistEntity.h
        src/Playlist.cpp
        src/Playlist.h
        src/PlaylistRegistry.cpp
        src/PlaylistRegistry.h
        src/PlaylistFile.cpp
        src/PlaylistFile.h
        src/LocalFileEntity.cpp
        src/LocalFileEntity.h
        src/HttpEntity.cpp
        src/HttpEntity.h
        src/WebDavEntity.h
        src/JellyfinEntity.cpp
        src/JellyfinEntity.h
        src/ServiceRegistry.h
        src/TraversalPolicy.h
        src/MediaInfoHelper.cpp
        src/MediaInfoHelper.h
        src/FilterListWidget.cpp
        src/FilterListWidget.h
        src/StreamTransit.cpp
        src/StreamTransit.h
        src/ByteStream.h
        src/StreamEndpoint.h
        src/HttpByteStream.cpp
        src/HttpByteStream.h
        src/Util.cpp
        src/Util.h
        src/HttpClient.cpp
        src/HttpClient.h
    )
    target_include_directories(oxbow_testlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(oxbow_testlib PUBLIC Qt6::Widgets Qt6::Concurrent Qt6::Network)
    # N83：同 oxbowcore——测试库也编 MediaInfoHelper，宽字符口径显式定义。PUBLIC：
    # 随链接传播到各测试 exe——测试 exe 以 REUSE_FROM 复用 oxbow_testlib 的 PCH，
    # 宏状态必须一致，PRIVATE 时宏不传播，gch 因 _UNICODE 未定义被弃用
    # （-Winvalid-pch，维护者 Linux 构建实测）
    target_compile_definitions(oxbow_testlib PUBLIC UNICODE _UNICODE)
    target_precompile_headers(oxbow_testlib PRIVATE src/pch.h)
    if(MSVC)
        target_compile_options(oxbow_testlib PRIVATE /utf-8)
    endif()

    # 注册一个单类测试 exe：源文件固定为 tests/tst_<name>.cpp
    # 用法：oxbow_add_test(MpvConf)
    function(oxbow_add_test name)
        add_executable(oxbow_tests_${name} tests/tst_${name}.cpp)
        target_include_directories(oxbow_tests_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_precompile_headers(oxbow_tests_${name} REUSE_FROM oxbow_testlib)
        target_link_libraries(oxbow_tests_${name} PRIVATE oxbow_testlib Qt6::Test)
        if(MSVC)
            target_compile_options(oxbow_tests_${name} PRIVATE /utf-8)
        endif()
        add_test(NAME ${name} COMMAND oxbow_tests_${name})
        # 各 exe 落同一输出目录，windeployqt 只需跑一遍：首个注册者挂载即全覆盖
        # N83：windeployqt 为 Windows 专属，非 Windows 测试直调 exe 无需部署
        if(WIN32 AND NOT OXBOW_STATICQT AND NOT _oxbow_test_deployed)
            add_custom_command(TARGET oxbow_tests_${name} POST_BUILD
                COMMAND ${WINDEPLOYQT_EXE} --no-translations --compiler-runtime $<TARGET_FILE:oxbow_tests_${name}>)
            set(_oxbow_test_deployed TRUE PARENT_SCOPE)
        endif()
    endfunction()

    oxbow_add_test(MpvConf)
    oxbow_add_test(InputConf)
    oxbow_add_test(PlaylistEntity)
    oxbow_add_test(PlaylistFile)
    oxbow_add_test(HttpClient)
    oxbow_add_test(DirProvider)
    oxbow_add_test(WebDavProvider)
    oxbow_add_test(ExternalProvider)
    oxbow_add_test(Util)
    oxbow_add_test(BookmarkManager)
    oxbow_add_test(StreamTransit)
    oxbow_add_test(PlaylistRegistry)
    oxbow_add_test(HttpByteStream)
    oxbow_add_test(FilterList)
endif()

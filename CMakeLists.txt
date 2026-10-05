cmake_minimum_required(VERSION 3.16)
project(oxbow-player VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(OXBOW_BUILD_TESTS "Build Qt Test unit tests for oxbow" OFF)
option(OXBOW_STATICQT "Link Qt statically (skip windeployqt at deploy time)" OFF)
# N68：强行令 Windows 也走小窗的纯 Qt 降级路径（无等比缩放/边缘吸附），便于本机验证降级形态
option(OXBOW_PUREQT "Use the pure-Qt implementation path for the mini window" OFF)

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
add_library(oxbow_loader MODULE src/loader/loader.cpp)
set_target_properties(oxbow_loader PROPERTIES PREFIX "" OUTPUT_NAME "main")
if(MSVC)
    target_compile_options(oxbow_loader PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# 目标 2：真正的插件 oxbowcore.dll（Qt + mpv 逻辑）
# ---------------------------------------------------------------------------
add_library(oxbowcore MODULE
    src/plugin.cpp
    src/Constants.h
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
    src/SettingsDialog.cpp
    src/SettingsDialog.h
    src/FilterListWidget.cpp
    src/FilterListWidget.h
)

# mpv/client.h 已内置在仓库根目录的 mpv/ 下
target_include_directories(oxbowcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# C 插件不链接 libmpv，符号由 mpv 在加载时动态解析。
# 注意：不要定义 MPV_ENABLE_DEPRECATED=0，内置 client.h 的动态符号段
# 无条件引用已弃用的 mpv_get_wakeup_pipe，关掉弃用声明会编译失败。
target_compile_definitions(oxbowcore PRIVATE MPV_CPLUGIN_DYNAMIC_SYM)
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

# 预编译头：Qt 与标准库集中进 src/pch.h，加快编译
target_precompile_headers(oxbowcore PRIVATE src/pch.h)
set_target_properties(oxbowcore PROPERTIES PREFIX "" OUTPUT_NAME "oxbowcore")

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
set(MPV_BIN_DIR "" CACHE PATH "mpv.exe 所在目录；设置后构建完毕自动部署")
if(MPV_BIN_DIR)
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
if(MPV_BIN_DIR AND EXISTS "${MEDIAINFO_DLL}")
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
        if(NOT OXBOW_STATICQT AND NOT _oxbow_test_deployed)
            add_custom_command(TARGET oxbow_tests_${name} POST_BUILD
                COMMAND ${WINDEPLOYQT_EXE} --no-translations --compiler-runtime $<TARGET_FILE:oxbow_tests_${name}>)
            set(_oxbow_test_deployed TRUE PARENT_SCOPE)
        endif()
    endfunction()

    oxbow_add_test(MpvConf)
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

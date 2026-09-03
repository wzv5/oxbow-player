cmake_minimum_required(VERSION 3.16)
project(mpvqt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(MPVQT_BUILD_TESTS "Build Qt Test unit tests for mpv-qt" OFF)
option(MPVQT_STATICQT "Link Qt statically (skip windeployqt at deploy time)" OFF)

if(MPVQT_STATICQT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets LinguistTools)

# ---------------------------------------------------------------------------
# 目标 1：加载壳 main.dll（纯 Win32，无 Qt 依赖）
# mpv 启动时会加载 scripts/mpvqt/main.dll 并调用其导出的 mpv_open_cplugin。
# 壳仅负责把插件目录加入 DLL 搜索路径，再动态加载真正带 Qt 的 mpvqtcore.dll，
# 从而让 Qt 运行库能与插件共置、随插件目录分发。
# ---------------------------------------------------------------------------
add_library(mpvqt_loader MODULE src/loader/loader.cpp)
set_target_properties(mpvqt_loader PROPERTIES PREFIX "" OUTPUT_NAME "main")
if(MSVC)
    target_compile_options(mpvqt_loader PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# 目标 2：真正的插件 mpvqtcore.dll（Qt + mpv 逻辑）
# ---------------------------------------------------------------------------
add_library(mpvqtcore MODULE
    src/plugin.cpp
    src/Constants.h
    src/ActionManager.cpp
    src/ActionManager.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MpvController.cpp
    src/MpvController.h
    src/NavigatorPanel.cpp
    src/NavigatorPanel.h
    src/PlaylistDelegate.cpp
    src/PlaylistDelegate.h
    src/PlaylistItem.h
    src/PlaylistProvider.cpp
    src/PlaylistProvider.h
    src/ListSettingsDialog.cpp
    src/ListSettingsDialog.h
    src/StaticProvider.cpp
    src/StaticProvider.h
    src/DirProvider.cpp
    src/DirProvider.h
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
)

# mpv/client.h 已内置在仓库根目录的 mpv/ 下
target_include_directories(mpvqtcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# C 插件不链接 libmpv，符号由 mpv 在加载时动态解析。
# 注意：不要定义 MPV_ENABLE_DEPRECATED=0，内置 client.h 的动态符号段
# 无条件引用已弃用的 mpv_get_wakeup_pipe，关掉弃用声明会编译失败。
target_compile_definitions(mpvqtcore PRIVATE MPV_CPLUGIN_DYNAMIC_SYM)
if(MSVC)
    target_compile_options(mpvqtcore PRIVATE /utf-8)
endif()
if(MPVQT_STATICQT)
    target_compile_definitions(mpvqtcore PRIVATE MPVQT_STATICQT)
endif()

target_link_libraries(mpvqtcore PRIVATE Qt6::Widgets)
# A19：全屏期 DWM 圆角裁切抑制（仅 Windows；dwmapi 为 Vista 起系统组件）
if(WIN32)
    target_link_libraries(mpvqtcore PRIVATE dwmapi)
endif()

# 预编译头：Qt 与标准库集中进 src/pch.h，加快编译
target_precompile_headers(mpvqtcore PRIVATE src/pch.h)
set_target_properties(mpvqtcore PROPERTIES PREFIX "" OUTPUT_NAME "mpvqtcore")

qt_add_translations(mpvqtcore MERGE_QT_TRANSLATIONS
    LUPDATE_OPTIONS -no-obsolete
    TS_FILES translations/mpvqt_zh_CN.ts
)

# ---------------------------------------------------------------------------
# 部署：把插件建成独立脚本目录
#   portable_config/scripts/mpvqt/main.dll        （加载壳）
#   portable_config/scripts/mpvqt/mpvqtcore.dll    （真插件）
#   portable_config/scripts/mpvqt/*.dll + plugins/ （Qt 运行库，随插件目录分发）
# Qt 运行库用 windeployqt 部署到插件所在目录，而不是 mpv 主程序目录。
# MPVQT_STATICQT=ON 时 Qt 已静态链接进 mpvqtcore.dll，跳过 windeployqt；
# 加载壳 main.dll 仍照常部署——MediaInfo.dll 依赖壳预加载才能被解析。
# ---------------------------------------------------------------------------
set(MPV_BIN_DIR "" CACHE PATH "mpv.exe 所在目录；设置后构建完毕自动部署")
if(MPV_BIN_DIR)
    if(NOT MPVQT_STATICQT)
        find_program(WINDEPLOYQT_EXE windeployqt HINTS "${Qt6_DIR}/../../../bin")
        if(NOT WINDEPLOYQT_EXE)
            message(FATAL_ERROR "windeployqt 未找到：请确认 CMAKE_PREFIX_PATH 指向 Qt 安装目录（其 bin 下含 windeployqt）")
        endif()
    endif()
    set(MPVQT_SCRIPT_DIR "${MPV_BIN_DIR}/portable_config/scripts/mpvqt")
    add_custom_command(TARGET mpvqtcore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MPVQT_SCRIPT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mpvqt_loader> "${MPVQT_SCRIPT_DIR}/main.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mpvqtcore> "${MPVQT_SCRIPT_DIR}/mpvqtcore.dll")
    if(NOT MPVQT_STATICQT)
        add_custom_command(TARGET mpvqtcore POST_BUILD
            COMMAND ${WINDEPLOYQT_EXE} --no-translations --compiler-runtime "${MPVQT_SCRIPT_DIR}/mpvqtcore.dll")
    endif()
endif()

# ---------------------------------------------------------------------------
# 可选第三方运行时依赖：MediaInfo.dll
# 动态加载（不链接），存在即随插件目录部署；缺失仅跳过拷贝不影响编译。
# ---------------------------------------------------------------------------
set(MEDIAINFO_DLL "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/mediainfo/MediaInfo.dll")
if(MPV_BIN_DIR AND EXISTS "${MEDIAINFO_DLL}")
    add_custom_command(TARGET mpvqtcore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MEDIAINFO_DLL}" "${MPVQT_SCRIPT_DIR}/MediaInfo.dll")
endif()

# ---------------------------------------------------------------------------
# R3：单元测试目标（默认不构建）。不纳入插件部署路径。
# 启用方式：cmake -B build -DMPVQT_BUILD_TESTS=ON ...
# 运行：./build/Release/mpvqt_tests.exe   （或 -platform offscreen）
# ---------------------------------------------------------------------------
if(MPVQT_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    add_executable(mpvqt_tests
        tests/tst_MpvConf.cpp
        src/MpvConf.cpp
        src/MpvConf.h
    )
    target_include_directories(mpvqt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mpvqt_tests PRIVATE Qt6::Widgets Qt6::Test)
    if(MSVC)
        target_compile_options(mpvqt_tests PRIVATE /utf-8)
    endif()
    if(NOT MPVQT_STATICQT)
        add_custom_command(TARGET mpvqt_tests POST_BUILD
            COMMAND ${WINDEPLOYQT_EXE} --no-translations --compiler-runtime $<TARGET_FILE:mpvqt_tests>)
    endif()
    add_test(NAME mpvqt_tests COMMAND mpvqt_tests)
endif()

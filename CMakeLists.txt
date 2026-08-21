cmake_minimum_required(VERSION 3.16)
project(mpvqt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

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
    src/MainWindow.cpp
    src/MainWindow.h
    src/MpvController.cpp
    src/MpvController.h
    src/PlaylistWidget.cpp
    src/PlaylistWidget.h
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

target_link_libraries(mpvqtcore PRIVATE Qt6::Widgets)
set_target_properties(mpvqtcore PROPERTIES PREFIX "" OUTPUT_NAME "mpvqtcore")

# ---------------------------------------------------------------------------
# 部署：把插件建成独立脚本目录
#   portable_config/scripts/mpvqt/main.dll        （加载壳）
#   portable_config/scripts/mpvqt/mpvqtcore.dll    （真插件）
#   portable_config/scripts/mpvqt/*.dll + plugins/ （Qt 运行库，随插件目录分发）
# Qt 运行库用 windeployqt 部署到插件所在目录，而不是 mpv 主程序目录。
# ---------------------------------------------------------------------------
set(MPV_BIN_DIR "" CACHE PATH "mpv.exe 所在目录；设置后构建完毕自动部署")
if(MPV_BIN_DIR)
    set(MPVQT_SCRIPT_DIR "${MPV_BIN_DIR}/portable_config/scripts/mpvqt")
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(NOT WINDEPLOYQT_EXE)
        message(FATAL_ERROR "windeployqt 未找到：请确认 CMAKE_PREFIX_PATH 指向 Qt 安装目录（其 bin 下含 windeployqt）")
    endif()
    add_custom_command(TARGET mpvqtcore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MPVQT_SCRIPT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mpvqt_loader> "${MPVQT_SCRIPT_DIR}/main.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mpvqtcore> "${MPVQT_SCRIPT_DIR}/mpvqtcore.dll"
        COMMAND ${WINDEPLOYQT_EXE} --no-translations --compiler-runtime --dir "${MPVQT_SCRIPT_DIR}" $<TARGET_FILE:mpvqtcore>)
endif()

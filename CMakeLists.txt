cmake_minimum_required(VERSION 3.16)
project(oxbow-player VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(OXBOW_BUILD_TESTS "Build Qt Test unit tests for oxbow" OFF)
option(OXBOW_STATIC "Link Qt and crypto dependencies statically (skip windeployqt at deploy time)" OFF)
# N68：强行令 Windows 也走小窗的纯 Qt 降级路径（无等比缩放/边缘吸附），便于本机验证降级形态
option(OXBOW_PUREQT "Use the pure-Qt implementation path for the mini window" OFF)
# N98：rclone crypt 只读后端（rclone crypt 加密的本地目录）整条特性可关。关掉即不找
# libsodium / OpenSSL、不链 bcrypt、src/rclonecrypt 与解密流与提供者一概不入列；
# 存量 rclonecrypt:// 列表在关掉的构建里按"键未知"回落登记表第 0 项（静态表）
option(OXBOW_WITH_RCLONE_CRYPT "Build the rclone crypt backend (read-only encrypted local folder)" ON)

# N83：静态 Qt 依赖 MSVC 运行库口径与 windeployqt 部署语义，限 Windows（Linux 用系统
# Qt 动态链接），非 Windows 在 configure 期拦截。运行库收紧只看 OXBOW_STATIC：
# clang（MSVC ABI、GNU 前端）下 CMake 的 MSVC 判定为假，若限定 MSVC，静态 Qt 的
# Release 插件对象（_ITERATOR_DEBUG_LEVEL=0）会与 Debug 对象（=2）在链接期 failifmismatch
# ——实测教训，不得加回 MSVC 限定
if(OXBOW_STATIC AND NOT WIN32)
    message(FATAL_ERROR "OXBOW_STATIC is Windows-only (Linux links the system Qt)")
endif()

if(OXBOW_STATIC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
endif()

# ---------------------------------------------------------------------------
# N98 §8.3 §1：rclone crypt 后端的整条接线，受 OXBOW_WITH_RCLONE_CRYPT 单点门禁。
# libsodium 的查找收进 cmake/FindSodium.cmake（形态由 OXBOW_STATIC 单旋钮驱动：
# ON→static、OFF→dynamic，Linux 恒 dynamic），主配置只做接线；头路径与
# SODIUM_STATIC 宏都在 Sodium::Sodium 的 INTERFACE 面上，调用侧既不拼路径也不手工
# 定义宏。关掉本特性则 Sodium / OpenSSL 都不 find_package，Windows 也不再需要
# -DLIBSODIUM_ROOT=<预编译包根目录>
# ---------------------------------------------------------------------------
set(OXBOW_RCLONE_CRYPT_DEPLOY_DLL OFF)
if(OXBOW_WITH_RCLONE_CRYPT)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    if(OXBOW_STATIC)
        set(SODIUM_LINKAGE "static" CACHE STRING "" FORCE)
    else()
        set(SODIUM_LINKAGE "dynamic" CACHE STRING "" FORCE)
    endif()
    find_package(Sodium REQUIRED)

    # 纯算法层（不依赖 Qt / mpv）单独住 src/rclonecrypt/：目录内互相 include 用裸文件
    # 名，跨目录的消费者写 "rclonecrypt/xxx.h"（全仓口径：跨目录一律写相对 src/ 的路径
    # 限定名；本族之上的接入层分住 providers/ 与 streams/，见 N101 §1）。源清单只在
    # 这里写一遍，oxbowcore 与 oxbow_testlib 两个消费点共用（AGENTS 八"两处源列表"即
    # 这两个 target）
    set(OXBOW_RCLONE_CRYPT_SOURCES
        src/rclonecrypt/RcloneCryptAes.cpp
        src/rclonecrypt/RcloneCryptAes.h
        src/rclonecrypt/RcloneCrypt.cpp
        src/rclonecrypt/RcloneCrypt.h
        src/streams/RcloneCryptByteStream.cpp
        src/streams/RcloneCryptByteStream.h
        src/providers/RcloneCryptProvider.cpp
        src/providers/RcloneCryptProvider.h
        src/providers/RcloneCryptEntity.h
    )
    # AES 后端按平台分文件，以构建条件为唯一权威（X11Util 纪律）：错平台编进去就在
    # 缺 CNG / OpenSSL 处编译失败，源内叠二次守卫反而把编译错降级成链接期未定义符号
    if(WIN32)
        set(OXBOW_RCLONE_CRYPT_PLATFORM_SOURCES src/rclonecrypt/rclonecrypt_aes_cng.cpp)
        set(OXBOW_RCLONE_CRYPT_PLATFORM_LIBS bcrypt)
    else()
        find_package(OpenSSL REQUIRED)
        set(OXBOW_RCLONE_CRYPT_PLATFORM_SOURCES src/rclonecrypt/rclonecrypt_aes_openssl.cpp)
        set(OXBOW_RCLONE_CRYPT_PLATFORM_LIBS OpenSSL::Crypto)
    endif()
    # 动态形态才随产物分发 libsodium.dll：判读用 FindSodium 回报的实际形态
    # （Sodium_LINKAGE），不在部署段二次反推 OXBOW_STATIC——单旋钮的下游结果只有一个出处
    if(WIN32 AND Sodium_LINKAGE STREQUAL "dynamic")
        set(OXBOW_RCLONE_CRYPT_DEPLOY_DLL ON)
    endif()
endif()

# Svg：把 QtSvg 模块写进 oxbowcore 的导入表（用法见下面 oxbowcore 的链接处）
find_package(Qt6 REQUIRED COMPONENTS Widgets LinguistTools Concurrent Network Svg)

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
    src/streams/StreamTransit.cpp
    src/streams/StreamTransit.h
    src/streams/ByteStream.h
    src/streams/StreamEndpoint.h
    src/streams/HttpByteStream.cpp
    src/streams/HttpByteStream.h
    src/streams/FileByteStream.cpp
    src/streams/FileByteStream.h
    src/streams/StreamRelay.cpp
    src/streams/StreamRelay.h
    src/ActionManager.cpp
    src/ActionManager.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/mpvbridge/MpvController.cpp
    src/mpvbridge/MpvController.h
    src/mpvbridge/MpvKeyName.cpp
    src/mpvbridge/MpvKeyName.h
    src/HttpClient.cpp
    src/HttpClient.h
    src/ui/NavigatorPanel.cpp
    src/ui/NavigatorPanel.h
    src/playlist/PlaylistDelegate.cpp
    src/playlist/PlaylistDelegate.h
    src/playlist/PlaylistEntity.cpp
    src/playlist/PlaylistEntity.h
    src/playlist/Playlist.cpp
    src/playlist/Playlist.h
    src/playlist/PlaylistRegistry.cpp
    src/playlist/PlaylistRegistry.h
    src/playlist/PlaylistListView.cpp
    src/playlist/PlaylistListView.h
    src/playlist/PlaylistFile.cpp
    src/playlist/PlaylistFile.h
    src/providers/LocalFileEntity.cpp
    src/providers/LocalFileEntity.h
    src/providers/HttpEntity.cpp
    src/providers/HttpEntity.h
    src/providers/WebDavEntity.h
    src/providers/JellyfinEntity.cpp
    src/providers/JellyfinEntity.h
    src/ServiceRegistry.h
    src/playbackreport/PlaybackReporter.h
    src/playbackreport/PlaybackReporterRegistry.cpp
    src/playbackreport/PlaybackReporterRegistry.h
    src/playbackreport/PlaybackReportDriver.cpp
    src/playbackreport/PlaybackReportDriver.h
    src/playbackreport/PlaybackSnapshot.cpp
    src/playbackreport/PlaybackSnapshot.h
    src/playbackreport/ReporterSwitchCatalog.cpp
    src/playbackreport/ReporterSwitchCatalog.h
    src/TraversalPolicy.h
    src/providers/PlaylistProvider.cpp
    src/providers/PlaylistProvider.h
    src/providers/ProviderCatalog.cpp
    src/providers/ProviderCatalog.h
    src/providers/ProviderSettingsPage.cpp
    src/providers/ProviderSettingsPage.h
    src/ui/ListSettingsDialog.cpp
    src/ui/ListSettingsDialog.h
    src/providers/StaticProvider.cpp
    src/providers/StaticProvider.h
    src/providers/DirProvider.cpp
    src/providers/DirProvider.h
    src/providers/WebDavProvider.cpp
    src/providers/WebDavProvider.h
    src/providers/JellyfinProvider.cpp
    src/providers/JellyfinProvider.h
    src/providers/ExternalProvider.cpp
    src/providers/ExternalProvider.h
    src/ui/LoadingOverlay.cpp
    src/ui/LoadingOverlay.h
    src/playlist/PlaylistWidget.cpp
    src/playlist/PlaylistWidget.h
    src/BookmarkManager.cpp
    src/BookmarkManager.h
    src/ui/ChapterSlider.cpp
    src/ui/ChapterSlider.h
    src/ui/BookmarkEditorDialog.cpp
    src/ui/BookmarkEditorDialog.h
    src/Util.cpp
    src/Util.h
    src/MediaInfoHelper.cpp
    src/MediaInfoHelper.h
    src/mpvbridge/MpvConf.cpp
    src/mpvbridge/MpvConf.h
    src/mpvbridge/InputConf.cpp
    src/mpvbridge/InputConf.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/AboutDialog.cpp
    src/ui/AboutDialog.h
    src/ui/ShortcutsDialog.cpp
    src/ui/ShortcutsDialog.h
    src/ui/MpvBindingDialog.cpp
    src/ui/MpvBindingDialog.h
    src/ui/FilterListWidget.cpp
    src/ui/FilterListWidget.h
)

# mpv/client.h 已内置在仓库根目录的 mpv/ 下；src 一并入包含路径只为子目录源单元解析
# pch.h 与它依赖的根头，跨目录 include 的拼写口径：一律写相对 src/ 的路径限定名
# （如 "playbackreport/xxx.h"、"rclonecrypt/xxx.h"、"providers/xxx.h"），同目录保持裸名
target_include_directories(oxbowcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                              ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
if(OXBOW_STATIC)
    target_compile_definitions(oxbowcore PRIVATE OXBOW_STATIC)
endif()
if(OXBOW_PUREQT)
    target_compile_definitions(oxbowcore PRIVATE OXBOW_PUREQT)
endif()

# Svg：svg 图标的两个插件（iconengines / imageformats）静态导入 QtSvg，而插件自身的
# 依赖由 OS 按标准搜索序解析、不含插件目录——模块不先常驻，那两个插件就加载不上、图标
# 全空。让它进本模块的导入表，即可在壳载入本模块时一并按 DLL_LOAD_DIR 解析（plugin.cpp
# 入口的那次 QSvgRenderer 构造是同一件事的另一半：只链接不引用留不下导入项）
target_link_libraries(oxbowcore PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Svg)
# N98：rclone crypt 后端整块受 OXBOW_WITH_RCLONE_CRYPT 门禁——scrypt 与 secretbox 交
# libsodium（形态与 SODIUM_STATIC 宏随 Sodium::Sodium 的导入面决定，OXBOW_STATIC 单
# 旋钮），AES 后端按平台取 bcrypt / OpenSSL::Crypto；宏同时是 ProviderCatalog 与
# PlaylistEntity 两处接入点的 #if 判据（关掉的构建里那两处一并不编）
if(OXBOW_WITH_RCLONE_CRYPT)
    target_link_libraries(oxbowcore PRIVATE Sodium::Sodium ${OXBOW_RCLONE_CRYPT_PLATFORM_LIBS})
    target_sources(oxbowcore PRIVATE ${OXBOW_RCLONE_CRYPT_SOURCES}
                                 ${OXBOW_RCLONE_CRYPT_PLATFORM_SOURCES})
    target_compile_definitions(oxbowcore PRIVATE OXBOW_WITH_RCLONE_CRYPT)
endif()
# A19：全屏期 DWM 圆角裁切抑制（仅 Windows；dwmapi 为 Vista 起系统组件）
if(WIN32)
    target_link_libraries(oxbowcore PRIVATE dwmapi)
endif()

# N83：X11 专用小工具集（X11Util）——EWMH 直发置顶（小窗置顶，
# 见 toggleMini）。Xlib 自建连接。Linux 本就依赖 X11（xcb 平台 + mpv X11 路径），
# 此处为编译期硬依赖；X11Util 仅非 Windows 参与构建，Windows 完全不出现该翻译单元
# （源文件不入主列表，住 src/platform/——平台专属助手的既定去处，N101 §1）。源内不再叠平台守卫：本段的构建条件是唯一权威——错误平台把
# 它编进去会因缺 X11 头 / 库直接失败（要的显影），源内守卫反而把实现静默掏空、
# 把编译错降级成链接期未定义符号。守卫用 NOT WIN32 而非 UNIX：UNIX 还含非目标的
# macOS，而 NOT WIN32 与代码侧调用点（MainWindow.cpp）的 Q_OS_WIN 守卫同指
# 非 Windows；X11::X11 为 imported target（CMake ≥3.14），自带 include 路径
if(NOT WIN32)
    find_package(X11 REQUIRED)
    target_link_libraries(oxbowcore PRIVATE X11::X11)
    target_sources(oxbowcore PRIVATE
        src/platform/X11Util.cpp
        src/platform/X11Util.h
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
# OXBOW_STATIC=ON 时 Qt 已静态链接进 oxbowcore.dll，跳过 windeployqt；
# 加载壳 main.dll 仍照常部署——MediaInfo.dll 依赖壳预加载才能被解析。
# ---------------------------------------------------------------------------
# N83：部署为 Windows 专属（windeployqt 与 DLL 语义；Linux 不自动部署，只编译产物）
set(MPV_BIN_DIR "" CACHE PATH "mpv.exe 所在目录（仅 Windows）；设置后构建完毕自动部署")
# windeployqt 发现提到部署段之外：测试目标的部署守卫同样引用它，不能依赖
# MPV_BIN_DIR 段的副作用（无部署目录的构建此前会拿到空命令串而链失败）
if(WIN32 AND NOT OXBOW_STATIC)
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${Qt6_DIR}/../../../bin")
endif()
if(WIN32 AND MPV_BIN_DIR)
    if(NOT OXBOW_STATIC AND NOT WINDEPLOYQT_EXE)
        message(FATAL_ERROR "windeployqt 未找到：请确认 CMAKE_PREFIX_PATH 指向 Qt 安装目录（其 bin 下含 windeployqt）")
    endif()
    set(OXBOW_SCRIPT_DIR "${MPV_BIN_DIR}/portable_config/scripts/oxbow")
    add_custom_command(TARGET oxbowcore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OXBOW_SCRIPT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oxbow_loader> "${OXBOW_SCRIPT_DIR}/main.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oxbowcore> "${OXBOW_SCRIPT_DIR}/oxbowcore.dll")
    if(NOT OXBOW_STATIC)
        add_custom_command(TARGET oxbowcore POST_BUILD
            COMMAND ${WINDEPLOYQT_EXE} --no-translations --compiler-runtime "${OXBOW_SCRIPT_DIR}/oxbowcore.dll")
    endif()
    # N98：动态形态随插件目录分发 libsodium.dll（$<TARGET_FILE:> 对导入目标给出
    # DLL 本体，按当次构建配置解析）；静态形态无额外文件。形态判读走 FindSodium
    # 回报的 Sodium_LINKAGE（见顶部 OXBOW_RCLONE_CRYPT_DEPLOY_DLL），不在这里反推 OXBOW_STATIC
    if(OXBOW_RCLONE_CRYPT_DEPLOY_DLL)
        add_custom_command(TARGET oxbowcore POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Sodium::Sodium>" "${OXBOW_SCRIPT_DIR}")
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
        src/providers/DirProvider.cpp
        src/providers/DirProvider.h
        src/providers/WebDavProvider.cpp
        src/providers/WebDavProvider.h
        src/providers/JellyfinProvider.cpp
        src/providers/JellyfinProvider.h
        src/providers/ExternalProvider.cpp
        src/providers/ExternalProvider.h
        src/providers/PlaylistProvider.cpp
        src/providers/PlaylistProvider.h
        src/providers/ProviderCatalog.cpp
        src/providers/ProviderCatalog.h
        src/providers/ProviderSettingsPage.cpp
        src/providers/ProviderSettingsPage.h
        src/providers/StaticProvider.cpp
        src/providers/StaticProvider.h
        src/mpvbridge/MpvConf.cpp
        src/mpvbridge/MpvConf.h
        src/mpvbridge/InputConf.cpp
        src/mpvbridge/InputConf.h
        src/mpvbridge/MpvKeyName.cpp
        src/mpvbridge/MpvKeyName.h
        src/BookmarkManager.cpp
        src/BookmarkManager.h
        src/playlist/PlaylistEntity.cpp
        src/playlist/PlaylistEntity.h
        src/playlist/Playlist.cpp
        src/playlist/Playlist.h
        src/playlist/PlaylistRegistry.cpp
        src/playlist/PlaylistRegistry.h
        src/playlist/PlaylistFile.cpp
        src/playlist/PlaylistFile.h
        src/providers/LocalFileEntity.cpp
        src/providers/LocalFileEntity.h
        src/providers/HttpEntity.cpp
        src/providers/HttpEntity.h
        src/providers/WebDavEntity.h
        src/providers/JellyfinEntity.cpp
        src/providers/JellyfinEntity.h
        src/ServiceRegistry.h
        # 上报框架四源单元（驱动器不在此列：其现读四项引用 MpvController，进测试库
        # 链接不可能成立）
        src/playbackreport/PlaybackReporter.h
        src/playbackreport/PlaybackReporterRegistry.cpp
        src/playbackreport/PlaybackReporterRegistry.h
        src/playbackreport/PlaybackSnapshot.cpp
        src/playbackreport/PlaybackSnapshot.h
        src/playbackreport/ReporterSwitchCatalog.cpp
        src/playbackreport/ReporterSwitchCatalog.h
        src/TraversalPolicy.h
        src/MediaInfoHelper.cpp
        src/MediaInfoHelper.h
        src/ui/FilterListWidget.cpp
        src/ui/FilterListWidget.h
        src/streams/StreamTransit.cpp
        src/streams/StreamTransit.h
        src/streams/ByteStream.h
        src/streams/StreamEndpoint.h
        src/streams/HttpByteStream.cpp
        src/streams/HttpByteStream.h
        src/streams/FileByteStream.cpp
        src/streams/FileByteStream.h
        src/Util.cpp
        src/Util.h
        src/HttpClient.cpp
        src/HttpClient.h
        src/Version.cpp
        src/Version.h
    )
    target_include_directories(oxbow_testlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                                    ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # N100：JellyfinProvider 引 App::versionInfo 后测试库同编 Version.cpp——但以
    # OXBOW_BUILDING_TESTS 形态编：不吃每轮必变的 revision.h（否则测试库逐轮重编、
    # 全部测试 exe 逐轮重链），三宏回落内置值，因此无需生成头包含路径与
    # oxbow_revision 依赖（口径与 oxbowcore 的 N84 接线有意分岔）
    target_compile_definitions(oxbow_testlib PRIVATE OXBOW_BUILDING_TESTS)
    target_link_libraries(oxbow_testlib PUBLIC Qt6::Widgets Qt6::Concurrent Qt6::Network)
    # N98：rclone crypt 一族同 oxbowcore 口径受同一门禁。链接必须 PUBLIC——
    # SODIUM_STATIC 与本特性的 INTERFACE 宏都要传到本库自己的编译单元与各测试 exe
    # （PRIVATE 只传链接不传宏）；平台库同理（bcrypt / OpenSSL::Crypto 随链接传播）
    if(OXBOW_WITH_RCLONE_CRYPT)
        target_link_libraries(oxbow_testlib PUBLIC Sodium::Sodium ${OXBOW_RCLONE_CRYPT_PLATFORM_LIBS})
        target_sources(oxbow_testlib PRIVATE ${OXBOW_RCLONE_CRYPT_SOURCES}
                                             ${OXBOW_RCLONE_CRYPT_PLATFORM_SOURCES})
        target_compile_definitions(oxbow_testlib PUBLIC OXBOW_WITH_RCLONE_CRYPT)
    endif()
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
        # 记下 target 名：各 exe 的 Qt 导入集互不相同（测试静态归档只拉被引用的 object），
        # 部署面要按它们的并集算，注册完毕后统一挂一次 windeployqt——见本节末尾
        set(_oxbow_test_targets ${_oxbow_test_targets} oxbow_tests_${name} PARENT_SCOPE)
        # N98：动态形态下测试 exe 也需要 libsodium.dll（照 Qt 的测试部署同款，
        # 每 exe 挂一次 copy_if_different，输出目录同址故幂等）
        if(OXBOW_RCLONE_CRYPT_DEPLOY_DLL)
            add_custom_command(TARGET oxbow_tests_${name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:Sodium::Sodium>" "$<TARGET_FILE_DIR:oxbow_tests_${name}>")
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
    oxbow_add_test(FileByteStream)
    oxbow_add_test(FilterList)
    oxbow_add_test(PlaybackReporterRegistry)
    oxbow_add_test(ReporterSwitchCatalog)
    oxbow_add_test(PlaybackSnapshot)
    # rclone crypt 的两套件与特性同进退（关掉就没有被测对象）；FileByteStream 是通用
    # L3 传输，不受本门禁影响
    if(OXBOW_WITH_RCLONE_CRYPT)
        oxbow_add_test(RcloneCrypt)
        oxbow_add_test(RcloneCryptProvider)
    endif()

    # 一次 windeployqt 吃全部测试 exe：部署面按入参二进制的依赖并集决定，逐个挂载会漏
    # （首个注册者恰是依赖集最小的那个）。挂在最后一个 exe 的 POST_BUILD，并对其余 exe
    # 加 add_dependencies，保证取参时各文件已链接。不取"每 exe 挂一次"：Ninja 并行下多个
    # windeployqt 会同时往同一目录拷同一批 dll，撞车且难复现
    # N83：windeployqt 为 Windows 专属，非 Windows 测试直调 exe 无需部署
    if(WIN32 AND NOT OXBOW_STATIC AND _oxbow_test_targets)
        list(POP_BACK _oxbow_test_targets _oxbow_test_last)
        set(_oxbow_test_files "$<TARGET_FILE:${_oxbow_test_last}>")
        foreach(_exe IN LISTS _oxbow_test_targets)
            list(APPEND _oxbow_test_files "$<TARGET_FILE:${_exe}>")
        endforeach()
        add_custom_command(TARGET ${_oxbow_test_last} POST_BUILD
            COMMAND ${WINDEPLOYQT_EXE} --no-translations --compiler-runtime ${_oxbow_test_files})
        if(_oxbow_test_targets)
            add_dependencies(${_oxbow_test_last} ${_oxbow_test_targets})
        endif()
    endif()
endif()

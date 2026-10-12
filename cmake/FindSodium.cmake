# FindSodium.cmake —— libsodium 的查找收进单点（N98 §8.3）
#
# 官方预编译包不带 CMake 配置文件，Linux 发行版只带 .pc；两条路都汇到同一个导入
# 目标 Sodium::Sodium，调用侧只见 find_package(Sodium REQUIRED)。
#
# 产出：
#   Sodium::Sodium    导入目标（头路径 + 宏 + 库 / DLL 一体带走）
#   Sodium_FOUND      命中与否
#   Sodium_LINKAGE    static | dynamic（缓存，供部署段与日志判读）
#   动态形态的 DLL 位置由 $<TARGET_FILE:Sodium::Sodium> 取（配置感知，不另设变量——
#   多配置生成器下没有一个"配置中性的 DLL 路径"可暴露，给一个反而会误导）
#
# 输入（CACHE）：
#   LIBSODIUM_ROOT    预编译包根目录（仅 Windows 需要）
#   SODIUM_TOOLSET    工具集目录名，默认 v143
#   SODIUM_LINKAGE    期望形态；主配置按 OXBOW_STATIC 单旋钮接线（ON→static、
#                     OFF→dynamic），Linux 恒 dynamic

if(TARGET Sodium::Sodium)
    return()
endif()

set(LIBSODIUM_ROOT "" CACHE PATH "libsodium 预编译包根目录（仅 Windows 使用）")
set(SODIUM_TOOLSET "v143" CACHE STRING "预编译包内的工具集目录名（v142 / v143）")

if(NOT DEFINED SODIUM_LINKAGE OR SODIUM_LINKAGE STREQUAL "")
    if(WIN32)
        set(SODIUM_LINKAGE "static")
    else()
        set(SODIUM_LINKAGE "dynamic")
    endif()
endif()

find_path(Sodium_INCLUDE_DIR NAMES sodium.h HINTS "${LIBSODIUM_ROOT}/include")

# 统一的失败出口：缺依赖要显影，不许静默退化成"crypt 功能不可用"
function(_sodium_fail msg)
    set(Sodium_FOUND FALSE PARENT_SCOPE)
    if(Sodium_FIND_REQUIRED)
        message(FATAL_ERROR "FindSodium: ${msg}")
    elseif(NOT Sodium_FIND_QUIETLY)
        message(STATUS "FindSodium: ${msg}")
    endif()
endfunction()

# Windows 的头只可能来自 LIBSODIUM_ROOT，在这里就能判死；Linux 留到 pkg-config 之后
# 再判——非系统前缀的安装只有 .pc 认得，find_path 的默认路径可能全空
if(NOT Sodium_INCLUDE_DIR AND WIN32)
    _sodium_fail("找不到 sodium.h（Windows 需 -DLIBSODIUM_ROOT=<预编译包根目录>）")
    return()
endif()

if(WIN32)
    if(NOT LIBSODIUM_ROOT)
        _sodium_fail("Windows 的 libsodium 没有 CMake 配置文件，必须给 LIBSODIUM_ROOT；期望其下有 include/sodium.h 与 x64/<配置>/${SODIUM_TOOLSET}/{static,dynamic}/libsodium.lib")
        return()
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_arch x64)
    else()
        set(_arch Win32)
    endif()

    if(SODIUM_LINKAGE STREQUAL "static")
        # 静态形态：全配置一律 Release 目录。预编译静态 Qt 只有 Release(/MT) 一份，
        # 插件的 Debug 配置也链它——库自带 /DEFAULTLIB:"LIBCMT"，与运行库
        # MultiThreaded 同源；不存在"按配置挑 Debug 静态库"的余地。
        set(_lib "${LIBSODIUM_ROOT}/${_arch}/Release/${SODIUM_TOOLSET}/static/libsodium.lib")
        if(NOT EXISTS "${_lib}")
            _sodium_fail("找不到静态库 ${_lib}（检查 LIBSODIUM_ROOT 与 SODIUM_TOOLSET）")
            return()
        endif()
        add_library(Sodium::Sodium STATIC IMPORTED GLOBAL)
        set_target_properties(Sodium::Sodium PROPERTIES
            IMPORTED_LOCATION "${_lib}"
            # 宏随选中的文件一起给出：调用侧没有写歪的机会（纪律①结构性成立）
            INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC
            INTERFACE_INCLUDE_DIRECTORIES "${Sodium_INCLUDE_DIR}")
    else()
        set(_lib_dbg "${LIBSODIUM_ROOT}/${_arch}/Debug/${SODIUM_TOOLSET}/dynamic/libsodium.lib")
        set(_lib_rel "${LIBSODIUM_ROOT}/${_arch}/Release/${SODIUM_TOOLSET}/dynamic/libsodium.lib")
        set(_dll_dbg "${LIBSODIUM_ROOT}/${_arch}/Debug/${SODIUM_TOOLSET}/dynamic/libsodium.dll")
        set(_dll_rel "${LIBSODIUM_ROOT}/${_arch}/Release/${SODIUM_TOOLSET}/dynamic/libsodium.dll")
        foreach(_f IN ITEMS _lib_dbg _lib_rel _dll_dbg _dll_rel)
            if(NOT EXISTS "${${_f}}")
                _sodium_fail("动态形态缺少 ${_f} → ${${_f}}")
                return()
            endif()
        endforeach()
        add_library(Sodium::Sodium SHARED IMPORTED GLOBAL)
        set_target_properties(Sodium::Sodium PROPERTIES
            IMPORTED_IMPLIB_DEBUG "${_lib_dbg}"
            IMPORTED_IMPLIB_RELEASE "${_lib_rel}"
            IMPORTED_LOCATION_DEBUG "${_dll_dbg}"
            IMPORTED_LOCATION_RELEASE "${_dll_rel}"
            INTERFACE_INCLUDE_DIRECTORIES "${Sodium_INCLUDE_DIR}")
        # 动态形态绝不定义 SODIUM_STATIC：定义了头会按静态展开，链接期反而找不到符号
    endif()
else()
    # Linux：先试自带 CMake 配置的安装形态（vcpkg 等），否则走 PkgConfig——
    # 发行版的 libsodium 只提供 .pc，不能指望 find_package(libsodium CONFIG)
    find_package(sodium CONFIG QUIET)
    if(TARGET sodium::sodium)
        add_library(Sodium::Sodium INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Sodium::Sodium INTERFACE sodium::sodium)
    elseif(TARGET sodium)
        add_library(Sodium::Sodium INTERFACE IMPORTED GLOBAL)
        target_link_libraries(Sodium::Sodium INTERFACE sodium)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(PC_SODIUM REQUIRED libsodium)
        find_library(Sodium_LIBRARY NAMES sodium HINTS ${PC_SODIUM_LIBRARY_DIRS})
        if(NOT Sodium_LIBRARY)
            _sodium_fail("pkg-config 报出了 libsodium 却没找到 libsodium.so（需装 libsodium-dev 之类）")
            return()
        endif()
        # 头路径两路并取：find_path 的默认搜索结果 + pkg-config 报出的目录
        set(_sodium_includes "${Sodium_INCLUDE_DIR}")
        if(PC_SODIUM_INCLUDE_DIRS)
            list(APPEND _sodium_includes ${PC_SODIUM_INCLUDE_DIRS})
        endif()
        list(REMOVE_DUPLICATES _sodium_includes)
        if(NOT _sodium_includes)
            _sodium_fail("找到 libsodium.so 却没有 sodium.h 的落点（需装 libsodium-dev 之类）")
            return()
        endif()
        add_library(Sodium::Sodium SHARED IMPORTED GLOBAL)
        set_target_properties(Sodium::Sodium PROPERTIES
            IMPORTED_LOCATION "${Sodium_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_sodium_includes}")
        if(PC_SODIUM_LDFLAGS_OTHER)
            set_target_properties(Sodium::Sodium PROPERTIES
                INTERFACE_LINK_OPTIONS ${PC_SODIUM_LDFLAGS_OTHER})
        endif()
    endif()
    set(SODIUM_LINKAGE "dynamic")
endif()

set(Sodium_FOUND TRUE)
set(Sodium_LINKAGE "${SODIUM_LINKAGE}" CACHE INTERNAL "FindSodium 解析出的链接形态")

if(NOT Sodium_FIND_QUIETLY)
    message(STATUS "FindSodium: linkage=${SODIUM_LINKAGE} include=${Sodium_INCLUDE_DIR}")
endif()

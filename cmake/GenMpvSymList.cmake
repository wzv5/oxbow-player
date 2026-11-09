# N140：扫 mpv/ 头目录，生成加载壳专用的 cplugin 符号转发表。
# 纯 CMake 脚本（cmake -P 运行），不依赖 Python / Perl 等外部解释器。
# 入参均为裸路径（不得加引号——N84 的教训：cmd.exe 会把 \" 转义吃成字面引号）：
#   -DMPV_HEADER_DIR=<mpv 头目录>       提取与 include 段的来源
#   -DMPV_SYM_LIST_OUTPUT=<输出文件>    唯一生成物路径
# 产出 generated/mpv_cplugin_syms.h：前半 include 段（壳由此声明全目录的
# pfn_* 导出变量，mpv 会把函数入口填进这些槽），后半转发表（每个符号一行
# MPV_FORWARD_SYM，表项是壳槽的地址，注入时取槽里的值搬给核心）。
# 提取对象是行首 MPV_DEFINE_SYM_PTR(<name>) 实例：它正好等于宏会生成的
# pfn_* 变量；宏自身的 #define 行以 # 开头、天然不在行首，不会被误提。

if(NOT DEFINED MPV_HEADER_DIR OR NOT DEFINED MPV_SYM_LIST_OUTPUT)
    message(FATAL_ERROR "GenMpvSymList.cmake requires -DMPV_HEADER_DIR=<mpv header dir> and -DMPV_SYM_LIST_OUTPUT=<output file>")
endif()

file(GLOB _mpv_headers "${MPV_HEADER_DIR}/*.h")
list(SORT _mpv_headers)
if(NOT _mpv_headers)
    message(FATAL_ERROR "No .h files found in mpv header dir: ${MPV_HEADER_DIR}")
endif()

set(_mpv_syms "")
set(_mpv_sym_count 0)
foreach(_header ${_mpv_headers})
    file(STRINGS "${_header}" _hits REGEX "^MPV_DEFINE_SYM_PTR\\([A-Za-z0-9_]+\\)")
    foreach(_line ${_hits})
        # .* 吃掉行尾剩余部分（万一上游在实例后加了行尾注释），保证取出的是纯名字
        string(REGEX REPLACE "^MPV_DEFINE_SYM_PTR\\(([A-Za-z0-9_]+)\\).*" "\\1" _sym "${_line}")
        list(APPEND _mpv_syms "${_sym}")
        math(EXPR _mpv_sym_count "${_mpv_sym_count} + 1")
    endforeach()
endforeach()

# 提取为 0 即 FATAL_ERROR：空表会变成"壳照常编过、跑到核心入口才报缺符号"的
# 运行期失败，必须拦在构建期。不设数值下限——上游增删符号属正常演进，唯一要
# 拦的是"正则整体失效"（上游改版式）导致的 0 条。
if(_mpv_sym_count EQUAL 0)
    message(FATAL_ERROR "Extracted zero MPV_DEFINE_SYM_PTR instances from mpv headers (upstream header format changed?): ${MPV_HEADER_DIR}")
endif()

set(_content "// 构建期由 cmake/GenMpvSymList.cmake 生成，勿手改。\n")
string(APPEND _content
    "#ifndef MPV_CPLUGIN_DYNAMIC_SYM\n#define MPV_CPLUGIN_DYNAMIC_SYM\n#endif\n")
foreach(_header ${_mpv_headers})
    get_filename_component(_header_name "${_header}" NAME)
    string(APPEND _content "#include <mpv/${_header_name}>\n")
endforeach()
string(APPEND _content
    "\nstruct MpvSymSlot {\n    const char *name; // 核心与壳两侧同名的导出变量\n"
    "    void **slot;      // 壳里这个变量的地址（mpv 已把函数入口写进去）\n};\n\n")
string(APPEND _content
    "#define MPV_FORWARD_SYM(name) {\"pfn_\" #name, reinterpret_cast<void **>(&pfn_##name)},\n")
string(APPEND _content "static const MpvSymSlot kMpvSyms[] = {\n")
foreach(_sym ${_mpv_syms})
    string(APPEND _content "    MPV_FORWARD_SYM(${_sym})\n")
endforeach()
string(APPEND _content "};\n#undef MPV_FORWARD_SYM\n")

file(WRITE "${MPV_SYM_LIST_OUTPUT}" "${_content}")
message(STATUS "GenMpvSymList: extracted ${_mpv_sym_count} symbols -> ${MPV_SYM_LIST_OUTPUT}")

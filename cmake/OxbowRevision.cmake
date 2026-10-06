# N84：构建期计算版本身份（短 hash[-dirty] + UTC 构建时刻），写生成头 revision.h。
# 由 add_custom_target 经 `cmake -P` 调用（script mode）；无 OUTPUT 的 custom target
# 每次构建都执行——hash 在构建期重算正是本方案的全部意义，configure 期快照会在
# "提交后不重新 configure 就构建"时产出带旧 hash 的产物。
# 输入一律经 -D 传入，不得用 cache 变量承载 hash：cache 会在二次 configure 时
# 短路复用旧值，比 configure 期快照更隐蔽地 stale。
# 兜底：git 不存在、无 .git、从 zip / tarball 构建等情形一律写 unknown 并继续，
# 绝不 FATAL_ERROR——源码包构建必须能过。
# 构建时刻：string(TIMESTAMP) 在环境变量 SOURCE_DATE_EPOCH 存在时改用它取值
# （可复现构建的既有约定），本方案的构建时刻随之而定，无需额外处理。
# UTC 形态用 %Y-%m-%dT%H:%M:%SZ + UTC 选项即得，全 CMake 版本可用（不依赖 %z，
# 那是 CMake 3.26 起才有、且旧版本会静默拷出字面 %z 的格式符，见 N84 背景 3）。

set(OXBOW_REVISION_STRING "unknown")

find_program(OXBOW_GIT_EXE git)
if(OXBOW_GIT_EXE)
    execute_process(
        COMMAND "${OXBOW_GIT_EXE}" -C "${OXBOW_SOURCE_DIR}" rev-parse --short HEAD
        RESULT_VARIABLE _rev_result
        OUTPUT_VARIABLE _rev_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_rev_result EQUAL 0 AND _rev_output)
        set(OXBOW_REVISION_STRING "${_rev_output}")
        # 脏判：工作区有任何改动（含未跟踪文件）即附 -dirty
        execute_process(
            COMMAND "${OXBOW_GIT_EXE}" -C "${OXBOW_SOURCE_DIR}" status --porcelain
            RESULT_VARIABLE _status_result
            OUTPUT_VARIABLE _status_output
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_status_result EQUAL 0 AND _status_output)
            string(APPEND OXBOW_REVISION_STRING "-dirty")
        endif()
    endif()
endif()

string(TIMESTAMP OXBOW_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)

# configure_file 的既有行为：内容不变则不更新目标文件、时间戳随之不变、依赖它的
# TU 不重编。版本与 hash 两项靠它拿到"每次构建都重算、却不每次构建都重编"；
# 构建时刻每次构建必然不同，生成头随之每次重写、Version.cpp 每次必重编——这是
# 已知代价，被"revision.h 唯一消费者是 Version.cpp"的设计框死在一个小 TU 上。
configure_file("${OXBOW_REVISION_TEMPLATE}" "${OXBOW_REVISION_OUTPUT}" @ONLY)

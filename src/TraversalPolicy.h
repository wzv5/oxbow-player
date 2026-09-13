#pragma once

#include "pch.h"

#include "Constants.h"

// 遍历策略（N36）：目录型提供者遍历共享的一组全局参数的不可变快照。
// 装配：GUI 线程读 ini 构成后 provide 进 ServiceRegistry（N35 先例）——构造即定型，
// 实例内不再写入；设置变更经 pluginIniKeysChanged 链路整体覆盖为新实例。
// 取用：DirProvider::enumerate 在起始处 resolve 一次，副本随本次遍历存活——
// "下一次枚举生效、在途不受影响"由共享计数天然成立，既不需要同步原语，也不必
// 把参数下发成提供者成员（详见 N36 修订 1）。任务线程自始至终不读 ini。
// resolve 为空属装配错误（如测试自建的空注册表）：调用方按 Constants.h 缺省值
// 兜底即可，不放断言——本策略只读且缺省值立即可用，强行断言会挡住轻量测试。
struct TraversalPolicy {
    // 0 = 不递归（只收根内文件），n = 最多深入 n 层；根为 0（N36 §2.2 修订 2）
    int depthLimit = App::recursiveDepthLimitDefault();
};

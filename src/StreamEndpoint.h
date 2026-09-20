#pragma once

#include "ByteStream.h"
#include "pch.h"

// N52 L2：来源解析点——一个列表一个实例。本篇唯一的扩展点：新增一个需要转接
// 的来源 = 实现本接口 + 提供者覆写基类的两个钩子，不碰协议层、传输层、适配层
// 与 UI，不新增列表类型。纯头。
//
// 契约（逐条即接口的一部分）：
//
// 1. identity 是协议层 unwrap 的产出——来源自己定义的条目身份原串，协议层、
//    接口层与适配层一概不解释它。同一个接口同时容纳"身份即地址"的来源与
//    "身份是服务端条目 id、播放地址要现取"的来源。
// 2. openStream 允许阻塞、允许发网络请求；每条流恰好在消费者线程的起流期被调
//    一次，返回即代表"能不能起这条流"已有结论（含首播取数通道的建立与能力
//    申报）。它绝不在 GUI 线程被调，也绝不随整表逐条被调。
// 3. 产出的 ByteStream 实例一条流一个，寿命归调用方（适配层的 cookie）。
// 4. 失败信息为英文可读文案（与提供者枚举的 expected<..., QString> 同形），
//    最终成为起流失败的可诊断线索。
// 5. 实现类必须：构造后不可变、不含 QObject、任意线程可析构、只持有不可变
//    参数与服务注册表的 shared_ptr（供现取网络客户端与可轮换令牌）；不得持有
//    提供者 / 实体 / 视图指针，不得回调 GUI 线程，不得 include 任何 mpv 头。
class StreamEndpoint {
  public:
    virtual ~StreamEndpoint() = default;

    virtual std::expected<std::shared_ptr<ByteStream>, QString>
    openStream(const QString &identity) const = 0;
};

#pragma once

#include "StreamEndpoint.h"
#include "pch.h"

typedef struct mpv_handle mpv_handle; // 与 client.h 的 typedef 重复声明合法

// N52 L4：mpv 适配——协议注册、端点表、回调到 ByteStream 的逐条映射、观测
// 日志与 cookie 寿命。include 闭包：mpv/stream_cb.h 只落在本层 .cpp；不碰
// HttpClient、不碰 ServiceRegistry、不碰任何提供者头文件。
// 回调映射没有任何策略——open 之外全是直转，一条 if 都算多（本层合格与否的尺子）。
//
// 线程口径（N51 §2.5，写死于回调注册处）：open / read / seek / size / close 在
// mpv 的 demux 线程按序调用；cancel 在另一条线程；回调内严禁反向调用 libmpv
// （stream_cb.h 明示死锁风险）。
//
// 版本依赖（D5：不做运行态判定）：mpv_stream_cb_info::cancel_fn 需 client API
// >= 1.106；本机宿主 v0.41.0-dev（API 2.5）满足。更旧宿主不属支持范围——向其
// 写该字段是越界写，症状是内存损坏而非优雅失败。

// ——取用侧接缝（实施勘误，随 N52 变更记录留档）——
// PlaylistProvider::playUrl 只需要 publish 一个动作，而它编进 mpvqt_testlib
// （L4 实现体不入 testlib，且其 .cpp 含 mpv 符号解析不可链入测试）。故取用侧面向
// 本抽象基编程：解析与发布经虚函数完成，测试链接不携带 L4。行为等价于规格 §2.8
// 的 resolve<StreamRelay>() 直取——同一注册表、同一实例、同一动作，纯头无实现体。
class StreamRelayIf {
  public:
    virtual ~StreamRelayIf() = default;
    virtual void publish(const QUuid &listUuid,
                         const std::shared_ptr<StreamEndpoint> &endpoint) = 0;
};

class StreamRelay final : public StreamRelayIf {
  public:
    StreamRelay() = default;
    ~StreamRelay() = default;
    StreamRelay(const StreamRelay &) = delete;
    StreamRelay &operator=(const StreamRelay &) = delete;

    // 注册 mpvqt 协议（注册名取 StreamTransit::kProtocol）并置 cancel_fn。
    // 硬顺序①：必须早于 MpvController::startEventThread——首次 mpv_wait_event
    // 解除 mpv 启动门控后，命令行首文件即会 open 流。
    bool install(mpv_handle *handle);

    // 端点表发布（§2.6 发布即下发）：playUrl 每铸一条 URI 顺手 publish 一条，
    // 同 key 幂等覆盖，成本可忽略——谁最后产出这批 URI，谁就是这批 URI 的解析者。
    // 注销 = 端点寿命终点：表存弱引用，提供者析构后表项自然 expired，取用时
    // 顺手 erase；不提供 unregister API，因而没有"谁该调、误删别人表项、
    // 指针比对"的问题。GUI 线程调用。
    void publish(const QUuid &listUuid, const std::shared_ptr<StreamEndpoint> &endpoint) override;

    // —— 以下仅 .cpp 内的 mpv 回调桩使用，勿在适配层之外调用 ——
    // 端点表取用（demux 线程）：键为 QUuid，判等全在代码侧（N57 §2.4 删去两侧 toLower
    // 归一——大小写与花括号自本版起归一命中）。命中已失效表项（提供者已析构）顺手
    // erase 并返空。
    std::shared_ptr<StreamEndpoint> takeEndpoint(const QUuid &listUuid);

  private:
    mpv_handle *m_handle = nullptr;
    mutable QMutex m_mutex;                                  // 只保护端点表这一瞬间
    QHash<QUuid, std::weak_ptr<StreamEndpoint>> m_endpoints; // 键即身份，无字面形态
};

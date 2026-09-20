#pragma once

#include "pch.h"

// 服务注册表（N35）：简易 IoC——类型键注册实例，调用方按类型解析，后续依赖同理注册。
// 只接受实例不接受工厂：HttpClient 构造只是值对象拷贝 + 一次性传输，启动 /
// 设置变更时饿汉提供足够；真有昂贵依赖那天再加 provideFactory 重载。
// 不定为进程单例——显式注入可测试（测试自建注册表，将来要 mock 时
// provide<IHttpClient>(mock) 同样成立，扩展点已留好）。
//
// 线程契约：provide 仅 GUI 线程（启动 / 设置变更时）；resolve 任意线程
// （WebDAV 枚举在枚举任务线程现查现用）——互斥锁内快照，
// 实例寿命由 shared_ptr 共享计数保证，resolve 到的副本在任务线程内恒有效。
// 设置变更即整体 provide 覆盖：在途单请求不受影响（旧 client 副本随任务存活），
// 条目粒度切换，下一次求值即用新实例。
class ServiceRegistry {
  public:
    // 注册 / 覆盖（同类型再次 provide 即替换实例）。GUI 线程调用。
    template <typename T> void provide(std::shared_ptr<T> service) {
        Q_ASSERT(service != nullptr);
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_services[std::type_index(typeid(T))] = std::move(service);
    }

    // 解析：未注册返回 nullptr（调用方按装配错误处理）。任意线程可调。
    template <typename T> std::shared_ptr<T> resolve() const {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_services.find(std::type_index(typeid(T)));
        return it == m_services.end() ? nullptr : std::static_pointer_cast<T>(it->second);
    }

  private:
    mutable std::mutex m_mutex;
    // shared_ptr<void> 仍持有原删除器，销毁安全；static_pointer_cast 依赖
    // "注册与解析用同一类型键"这一约定（调用方纪律）
    std::unordered_map<std::type_index, std::shared_ptr<void>> m_services;
};

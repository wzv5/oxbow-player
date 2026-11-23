#pragma once

#include "pch.h"

#include "playlist/ScriptListSpec.h"

class ServiceRegistry;

// 脚本应答的有界等待上限（生产值，测试可注短值）
inline constexpr int kScriptListReplyTimeoutMs = 10000;
// 首载等数据源注册的上限（仅列表首次取数使用）；与应答超时合计即首激活上限
inline constexpr int kScriptListIdWaitMs = 3000;

// 脚本列表的运行期数据源表与逐层取数通道。纯 C++、线程安全，全进程一个实例。
// 命令出口经服务注册表取 MpvApiPort（跨线程 API 面，枚举任务线程直调）；状态全部受
// 同一把互斥锁保护；id → owner 只增不清，脚本退出后列表以占位表达。
class ScriptListChannel {
  public:
    enum class Error { NotRegistered, SendFailed, Timeout, Closed };

    // 生产超时值；测试可注短值（replyMs 等应答，idMs 等注册）。
    // 注册表取弱引用：本通道自身就注册在表里，强引用会成环
    ScriptListChannel(std::weak_ptr<ServiceRegistry> registry, int replyMs,
                      int idMs = kScriptListIdWaitMs);

    // GUI 线程：id 空闲则写入，同 owner 视为幂等，被别的 owner 占用返回 false
    bool registerList(const QString &id, const QString &owner);

    // 工作线程：等 id 出现，至多 timeoutMs；关停立刻返回 false
    bool waitRegistered(const QString &id, int timeoutMs);
    int idWaitTimeout() const { return m_idWaitMs; }

    // GUI 线程：已注册 id 升序
    QStringList registeredIds() const;
    bool isRegistered(const QString &id) const;

    // 工作线程：查表、分配 reqid、登记挂起项与发送在同一把锁内完成，随后有界等待。
    // 发送失败当场撤销挂起项返回 SendFailed；超时撤销挂起项并回投一次 cancel-layer；
    // close 唤醒返回 Closed。
    std::expected<QString, Error> requestLayer(const QString &id, const QString &parentPath);

    // GUI 线程：按 reqid 配对并校验 owner，命中唤醒，未命中丢弃
    void deliverLayer(const QString &owner, const QString &reqid, const QString &json);

    // GUI 线程：置关闭旗、清表并唤醒全部等待者
    void close();

  private:
    struct Pending {
        QString owner;
        QString id;
    };

    // 工作线程：经注册表取端口并拼 script-message-to 下发；端口缺装即失败
    int send(const QString &owner, const QString &name, const QStringList &args);

    std::weak_ptr<ServiceRegistry> m_registry;
    int m_timeoutMs;
    int m_idWaitMs;
    // mutable：const 读接口也要加锁
    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_closed = false;
    quint64 m_nextReqid = 0;
    QHash<QString, QString> m_owners; // id → owner，只增不清（close 清空）
    QHash<QString, Pending> m_pending;
    QHash<QString, QString> m_delivered;
};

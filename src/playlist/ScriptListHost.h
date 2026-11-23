#pragma once

#include "pch.h"

class MpvController;
class ScriptListChannel;
class ScriptMessageBus;

// N179：script-playlist 协议宿主（GUI 线程）。按 args[1] 分派 create / layer：
// 前者只做校验与通道注册，后者只做配对转交；mpvShutdown 后早退并关停通道。
// 宿主不认识列表，也不碰注册表。
class ScriptListHost : public QObject {
    Q_OBJECT
  public:
    ScriptListHost(ScriptMessageBus *bus, MpvController *controller,
                   const std::shared_ptr<ScriptListChannel> &channel, QObject *parent = nullptr);

  private:
    void dispatch(const QStringList &args);
    void handleCreate(const QStringList &args);
    void handleLayer(const QStringList &args);
    void shutdown();
    // 回执尽力而为：失败只记日志
    void replyCreated(const QString &owner, const QString &id);
    void replyRejected(const QString &owner, const QString &id, const char *reason);

    ScriptMessageBus *m_bus = nullptr;
    std::shared_ptr<ScriptListChannel> m_channel;
    bool m_stopped = false;
};

#pragma once

#include "pch.h"

class MpvController;

// N174：脚本客户端消息总线
class ScriptMessageBus : public QObject {
    Q_OBJECT
  public:
    using Handler = std::function<void(const QStringList &args)>;

    explicit ScriptMessageBus(MpvController *controller, QObject *parent = nullptr);

    // 同名重复注册覆盖旧处理器；关停后不再受理
    void registerHandler(const QString &inbound, QObject *context, Handler handler);

    // 返回 mpv 命令错误码（0 成功）
    int reply(const QString &owner, const QString &name, const QStringList &args = {});
    int broadcast(const QString &name, const QStringList &args = {});

  public slots:
    void dispatch(const QStringList &args);

  signals:
    void messageArrived(const QString &inbound, const QStringList &args);

  private:
    // 断读取点接线与全部分派连接并停发；自连 mpvShutdown 触发
    void shutdown();

    // 失败日志只含 owner、消息名与错误码，不含命令原文（载荷可能带凭据）
    int send(const QStringList &cmd, const QString &owner, const QString &name);

    MpvController *m_controller = nullptr;
    QMetaObject::Connection m_clientMessageConnection;
    // 只服务"同名重复注册覆盖旧处理器"，不参与分派
    QHash<QString, QMetaObject::Connection> m_subscriptions;
    bool m_stopped = false;
};

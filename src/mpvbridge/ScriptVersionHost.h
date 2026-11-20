#pragma once

#include "pch.h"

class ScriptMessageBus;
class MpvController;

// N174：版本协议宿主——就绪广播、get-version 应答与 user-data 镜像守卫
class ScriptVersionHost : public QObject {
    Q_OBJECT
  public:
    ScriptVersionHost(ScriptMessageBus *bus, MpvController *controller, QObject *parent = nullptr);

    // 先写镜像再广播：广播收方立读属性即得正确值。须在事件线程启动后、exec() 前调
    void announce();

  private:
    // 断开镜像 echo 连接；自连 mpvShutdown 触发
    void shutdown();

    void handleGetVersion(const QStringList &args);
    void onVersionEcho(const QString &value, bool available);

    ScriptMessageBus *m_bus = nullptr;
    MpvController *m_controller = nullptr;
    QMetaObject::Connection m_echoConnection;
    bool m_announced = false;
};

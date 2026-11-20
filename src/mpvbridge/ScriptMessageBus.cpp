#include "ScriptMessageBus.h"

#include "Logging.h"
#include "mpvbridge/MpvController.h"

#include <mpv/client.h>

ScriptMessageBus::ScriptMessageBus(MpvController *controller, QObject *parent)
    : QObject(parent), m_controller(controller) {
    if (m_controller) {
        m_clientMessageConnection =
            connect(m_controller, &MpvController::clientMessage, this, &ScriptMessageBus::dispatch);
        connect(m_controller, &MpvController::mpvShutdown, this, &ScriptMessageBus::shutdown);
    }
}

QString ScriptMessageBus::eventName(const QString &protocol, const QString &event) {
    return QStringLiteral("oxbow-") + protocol + u':' + event;
}

void ScriptMessageBus::registerHandler(const QString &inbound, QObject *context, Handler handler) {
    if (m_stopped)
        return;
    const auto it = m_subscriptions.find(inbound);
    if (it != m_subscriptions.end())
        disconnect(*it);
    m_subscriptions.insert(inbound, connect(this, &ScriptMessageBus::messageArrived, context,
                                            [inbound, handler = std::move(handler)](
                                                const QString &name, const QStringList &args) {
                                                if (name == inbound)
                                                    handler(args);
                                            }));
}

int ScriptMessageBus::reply(const QString &owner, const QString &name, const QStringList &args) {
    QStringList cmd;
    cmd.reserve(args.size() + 3);
    cmd << QStringLiteral("script-message-to") << owner << name << args;
    return send(cmd, owner, name);
}

int ScriptMessageBus::broadcast(const QString &name, const QStringList &args) {
    QStringList cmd;
    cmd.reserve(args.size() + 2);
    cmd << QStringLiteral("script-message") << name << args;
    return send(cmd, QString(), name);
}

int ScriptMessageBus::send(const QStringList &cmd, const QString &owner, const QString &name) {
    if (m_stopped || !m_controller)
        return MPV_ERROR_GENERIC;
    const int rc = m_controller->command(cmd);
    if (rc != 0)
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("script-message send failed: owner=%1 name=%2: %3")
                   .arg(owner, name, QString::number(rc));
    return rc;
}

void ScriptMessageBus::dispatch(const QStringList &args) {
    if (m_stopped || args.isEmpty())
        return;
    emit messageArrived(args.first(), args);
}

void ScriptMessageBus::shutdown() {
    if (m_stopped)
        return;
    m_stopped = true;
    if (m_clientMessageConnection)
        disconnect(m_clientMessageConnection);
    disconnect(this, &ScriptMessageBus::messageArrived, nullptr, nullptr);
    m_subscriptions.clear();
}

#include "ScriptVersionHost.h"

#include "Constants.h"
#include "Logging.h"
#include "Version.h"
#include "mpvbridge/MpvController.h"
#include "mpvbridge/ScriptMessageBus.h"

constexpr char kVersionMessage[] = "oxbow-version";
constexpr char kGetVersionMessage[] = "get-version";

ScriptVersionHost::ScriptVersionHost(ScriptMessageBus *bus, MpvController *controller,
                                     QObject *parent)
    : QObject(parent), m_bus(bus), m_controller(controller) {
    m_bus->registerHandler(QString::fromLatin1(kGetVersionMessage), this,
                           [this](const QStringList &args) { handleGetVersion(args); });
    m_echoConnection =
        connect(m_controller, &MpvController::versionEcho, this, &ScriptVersionHost::onVersionEcho);
    connect(m_controller, &MpvController::mpvShutdown, this, &ScriptVersionHost::shutdown);
}

void ScriptVersionHost::announce() {
    if (m_announced)
        return;
    m_announced = true;
    // 先镜像后广播：收方立读属性即得正确值；就绪前的外部预写在此覆盖
    m_controller->setProperty(QString::fromLatin1(App::versionProperty), App::version());
    m_bus->broadcast(QString::fromLatin1(kVersionMessage), {App::version()});
}

void ScriptVersionHost::shutdown() {
    if (m_echoConnection)
        disconnect(m_echoConnection);
}

void ScriptVersionHost::handleGetVersion(const QStringList &args) {
    // args[1] 为请求者自报 client-name：非空即发、不校验，缺失只记日志（协议无回执）
    if (args.size() < 2 || args[1].isEmpty()) {
        OXBOW_LOG_WARNING() << "get-version without client-name, ignored";
        return;
    }
    m_bus->reply(args[1], QString::fromLatin1(kVersionMessage), {App::version()});
}

void ScriptVersionHost::onVersionEcho(const QString &value, bool available) {
    if (!m_announced)
        return; // 就绪前镜像还没有权威值可守
    if (!available || value != App::version()) {
        // 异型与缺失都算篡改；自身回写不重推事件（mpv 等值不推送），无忙等
        if (!m_controller->setProperty(QString::fromLatin1(App::versionProperty), App::version()))
            OXBOW_LOG_WARNING() << "version mirror write-back failed";
    }
}

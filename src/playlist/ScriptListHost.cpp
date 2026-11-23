#include "pch.h"

#include "playlist/ScriptListHost.h"

#include "Logging.h"
#include "mpvbridge/MpvController.h"
#include "mpvbridge/ScriptMessageBus.h"
#include "playlist/ScriptListChannel.h"
#include "playlist/ScriptListSpec.h"

ScriptListHost::ScriptListHost(ScriptMessageBus *bus, MpvController *controller,
                               const std::shared_ptr<ScriptListChannel> &channel, QObject *parent)
    : QObject(parent), m_bus(bus), m_channel(channel) {
    m_bus->registerHandler(QString::fromLatin1(ScriptListSpec::kInboundName), this,
                           [this](const QStringList &args) { dispatch(args); });
    connect(controller, &MpvController::mpvShutdown, this, &ScriptListHost::shutdown);
}

void ScriptListHost::dispatch(const QStringList &args) {
    if (m_stopped)
        return;
    if (args.size() < 2) {
        OXBOW_LOG_WARNING() << "script-playlist: message without a verb, ignored";
        return;
    }
    const QString &verb = args.at(1);
    if (verb == QLatin1String(ScriptListSpec::kVerbCreate))
        handleCreate(args);
    else if (verb == QLatin1String(ScriptListSpec::kVerbLayer))
        handleLayer(args);
    else
        OXBOW_LOG_WARNING() << "script-playlist: unknown verb, ignored";
}

void ScriptListHost::handleCreate(const QStringList &args) {
    if (args.size() < 3) {
        OXBOW_LOG_WARNING() << "script-playlist: create without owner, ignored";
        return;
    }
    const QString &owner = args.at(2);
    if (owner.isEmpty()) {
        // owner 自报缺失即无目标可回，只记日志
        OXBOW_LOG_WARNING() << "script-playlist: create with empty owner, ignored";
        return;
    }
    if (args.size() < 4) {
        replyRejected(owner, QString(), ScriptListSpec::kReasonBadArgs); // id 段缺席
        return;
    }
    const QString &id = args.at(3);
    if (!ScriptListSpec::isValidListId(id)) {
        // 空串与非法字符同档：给了这一段就不是参数不足，而是 id 非法
        replyRejected(owner, id, ScriptListSpec::kReasonBadId);
        return;
    }
    if (m_channel->registerList(id, owner))
        replyCreated(owner, id); // 注册成功即回执；幂等重注册同样回 created
    else
        replyRejected(owner, id, ScriptListSpec::kReasonExists);
}

void ScriptListHost::handleLayer(const QStringList &args) {
    if (args.size() < 5) {
        OXBOW_LOG_WARNING() << "script-playlist: layer with missing segments, dropped";
        return;
    }
    // 只做配对与转交
    m_channel->deliverLayer(args.at(2), args.at(3), args.at(4));
}

void ScriptListHost::shutdown() {
    m_stopped = true;
    m_channel->close();
}

void ScriptListHost::replyCreated(const QString &owner, const QString &id) {
    m_bus->reply(owner, ScriptListSpec::eventName(ScriptListSpec::kEventCreated), {id});
}

void ScriptListHost::replyRejected(const QString &owner, const QString &id, const char *reason) {
    m_bus->reply(owner, ScriptListSpec::eventName(ScriptListSpec::kEventRejected),
                 {id, QLatin1String(reason)});
}

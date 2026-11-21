#include "pch.h"

#include "ScriptActionHost.h"

#include "Logging.h"
#include "mpvbridge/MpvController.h"
#include "mpvbridge/ScriptMessageBus.h"

namespace {

// N173 协议 token，只在本模块消费
constexpr char kInbound[] = "script-action";
constexpr char kVerbCreate[] = "create";
constexpr char kVerbSet[] = "set";
constexpr char kEventCreated[] = "created";
constexpr char kEventRejected[] = "rejected";
constexpr char kReasonBadArgs[] = "bad-args";
constexpr char kReasonBadId[] = "bad-id";
constexpr char kReasonBadJson[] = "bad-json";
constexpr char kReasonExists[] = "exists";
constexpr char kReasonNotFound[] = "not-found";
constexpr char kReasonOwnerMismatch[] = "owner-mismatch";

} // namespace

ScriptActionHost::ScriptActionHost(ScriptMessageBus *bus, MpvController *controller,
                                   QObject *parent)
    : QObject(parent), m_bus(bus) {
    // context 挂本宿主，析构即自动断开
    m_bus->registerHandler(QLatin1String(kInbound), this,
                           [this](const QStringList &args) { handleScriptAction(args); });
    if (controller)
        connect(controller, &MpvController::mpvShutdown, this, &ScriptActionHost::shutdown);
}

std::optional<ScriptActionSpec::ScriptAction> ScriptActionHost::find(const QString &id) const {
    const auto it = m_actions.constFind(id);
    if (it == m_actions.constEnd())
        return std::nullopt;
    return it.value();
}

// args 布局：[script-action, verb, owner, id, json]
void ScriptActionHost::handleScriptAction(const QStringList &args) {
    const QString verb = args.value(1);
    if (verb == QLatin1String(kVerbCreate))
        handleCreate(args);
    else if (verb == QLatin1String(kVerbSet))
        handleSet(args);
    else
        OXBOW_LOG_DEBUG() << "script-action: unknown verb ignored:" << verb;
}

bool ScriptActionHost::precheck(const QStringList &args, QString &owner, QString &id,
                                ScriptActionSpec::ScriptActionPatch &patch) {
    owner = args.value(2);
    id = args.value(3);
    if (args.size() < 5) {
        if (args.size() > 2)
            reject(owner, id, QLatin1String(kReasonBadArgs));
        return false;
    }
    if (!ScriptActionSpec::isValidId(id)) {
        reject(owner, id, QLatin1String(kReasonBadId));
        return false;
    }
    const ScriptActionSpec::ParseResult parsed = ScriptActionSpec::parse(args.value(4).toUtf8());
    if (parsed.error != ScriptActionSpec::ParseError::None) {
        // 解析成因是脚本作者的第一个线索，reject 那行只有 token
        OXBOW_LOG_DEBUG() << "script-action: bad json id =" << id << "—" << parsed.detail;
        reject(owner, id, QLatin1String(kReasonBadJson));
        return false;
    }
    patch = parsed.patch;
    return true;
}

void ScriptActionHost::handleCreate(const QStringList &args) {
    QString owner;
    QString id;
    ScriptActionSpec::ScriptActionPatch patch;
    if (!precheck(args, owner, id, patch))
        return;
    if (m_actions.contains(id)) {
        // create 无覆盖语义，撞 id 幂等吸收（脚本重复 init 靠它）
        reject(owner, id, QLatin1String(kReasonExists));
        return;
    }
    ScriptActionSpec::ScriptAction action =
        ScriptActionSpec::apply(ScriptActionSpec::ScriptAction{}, patch);
    action.id = id;
    action.owner = owner;
    m_actions.insert(id, action);
    scheduleRebuild();
    OXBOW_LOG_DEBUG() << "script-action: created" << id << "owner =" << owner;
    m_bus->reply(owner, ScriptActionSpec::eventName(QLatin1String(kEventCreated)), {id});
}

// set 成功无回包（N170 先例）
void ScriptActionHost::handleSet(const QStringList &args) {
    QString owner;
    QString id;
    ScriptActionSpec::ScriptActionPatch patch;
    if (!precheck(args, owner, id, patch))
        return;
    const auto it = m_actions.find(id);
    if (it == m_actions.end()) {
        reject(owner, id, QLatin1String(kReasonNotFound));
        return;
    }
    if (it->owner != owner) {
        reject(owner, id, QLatin1String(kReasonOwnerMismatch));
        return;
    }
    it.value() = ScriptActionSpec::apply(it.value(), patch);
    scheduleRebuild();
}

void ScriptActionHost::reject(const QString &owner, const QString &id, QLatin1String reason) {
    OXBOW_LOG_DEBUG() << "script-action: rejected" << id << "reason =" << reason
                      << "owner =" << owner;
    m_bus->reply(owner, ScriptActionSpec::eventName(QLatin1String(kEventRejected)), {id, reason});
}

// 同一事件循环只投一次零时器
void ScriptActionHost::scheduleRebuild() {
    if (m_rebuildPending || m_stopped)
        return;
    m_rebuildPending = true;
    QTimer::singleShot(0, this, [this] {
        m_rebuildPending = false;
        if (m_stopped)
            return;
        emit actionsChanged();
    });
}

void ScriptActionHost::shutdown() {
    m_stopped = true;
    m_rebuildPending = false;
    m_actions.clear();
}

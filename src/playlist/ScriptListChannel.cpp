#include "pch.h"

#include "playlist/ScriptListChannel.h"

#include "Logging.h"
#include "ServiceRegistry.h"
#include "mpvbridge/MpvApiPort.h"

ScriptListChannel::ScriptListChannel(std::weak_ptr<ServiceRegistry> registry, int replyMs, int idMs)
    : m_registry(std::move(registry)), m_timeoutMs(replyMs), m_idWaitMs(idMs) {}

int ScriptListChannel::send(const QString &owner, const QString &name, const QStringList &args) {
    const auto registry = m_registry.lock();
    const auto port = registry ? registry->resolve<MpvApiPort>() : nullptr;
    if (!port) {
        OXBOW_LOG_WARNING() << "ScriptListChannel: MpvApiPort is not assembled";
        return -1;
    }
    return port->command(QStringList{QStringLiteral("script-message-to"), owner, name} + args);
}

bool ScriptListChannel::registerList(const QString &id, const QString &owner) {
    {
        QMutexLocker lock(&m_mutex);
        if (m_closed)
            return false;
        const auto it = m_owners.constFind(id);
        if (it != m_owners.constEnd())
            return it.value() == owner;
        m_owners.insert(id, owner);
    }
    m_wake.wakeAll(); // 首载可能在等这个 id 出现
    return true;
}

QStringList ScriptListChannel::registeredIds() const {
    QMutexLocker lock(&m_mutex);
    QStringList ids = m_owners.keys();
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool ScriptListChannel::isRegistered(const QString &id) const {
    QMutexLocker lock(&m_mutex);
    return m_owners.contains(id);
}

bool ScriptListChannel::waitRegistered(const QString &id, int timeoutMs) {
    QDeadlineTimer deadline(timeoutMs);
    QMutexLocker lock(&m_mutex);
    forever {
        if (m_closed)
            return false;
        if (m_owners.contains(id))
            return true;
        if (!m_wake.wait(&m_mutex, deadline))
            return false;
    }
}

std::expected<QString, ScriptListChannel::Error>
ScriptListChannel::requestLayer(const QString &id, const QString &parentPath) {
    QString owner;
    QString reqid;
    {
        QMutexLocker lock(&m_mutex);
        if (m_closed)
            return std::unexpected(Error::Closed);
        const auto it = m_owners.constFind(id);
        if (it == m_owners.constEnd())
            return std::unexpected(Error::NotRegistered);
        owner = it.value();
        reqid = QString::number(++m_nextReqid);
        m_pending.insert(reqid, Pending{owner, id});
        // 发送在锁内：挂起项必须先于脚本应答可见
        if (const int rc = send(owner, ScriptListSpec::eventName(ScriptListSpec::kEventNeedLayer),
                                {id, reqid, parentPath});
            rc != 0) {
            m_pending.remove(reqid);
            return std::unexpected(Error::SendFailed);
        }
    }

    // 有界等待：锁已释放
    QString payload;
    bool timedOut = false;
    {
        QMutexLocker lock(&m_mutex);
        const QDeadlineTimer deadline(m_timeoutMs);
        forever {
            if (m_delivered.contains(reqid)) {
                payload = m_delivered.take(reqid);
                break;
            }
            if (m_closed)
                return std::unexpected(Error::Closed);
            if (!m_wake.wait(&m_mutex, deadline)) {
                timedOut = true;
                break;
            }
        }
        if (timedOut)
            m_pending.remove(reqid);
    }

    if (!timedOut)
        return payload;

    // 超时回投一次 cancel-layer；发送失败只随错误码
    send(owner, ScriptListSpec::eventName(ScriptListSpec::kEventCancelLayer), {id, reqid});
    return std::unexpected(Error::Timeout);
}

void ScriptListChannel::deliverLayer(const QString &owner, const QString &reqid,
                                     const QString &json) {
    {
        QMutexLocker lock(&m_mutex);
        if (m_closed)
            return;
        const auto it = m_pending.constFind(reqid);
        if (it == m_pending.constEnd() || it.value().owner != owner)
            return; // 无此 reqid / 已超时 / owner 不符：丢弃
        m_delivered.insert(reqid, json);
        m_pending.remove(it.key());
    }
    m_wake.wakeAll();
}

void ScriptListChannel::close() {
    {
        QMutexLocker lock(&m_mutex);
        if (m_closed)
            return;
        m_closed = true;
        m_owners.clear();
        m_pending.clear();
        m_delivered.clear();
    }
    m_wake.wakeAll(); // 在途等待者立刻返回 Closed
}

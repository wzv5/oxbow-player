#pragma once

#include "pch.h"

#include "ScriptActionSpec.h"

class MpvController;
class ScriptMessageBus;

// N173：script-action 协议宿主——动作表、create / set 校验、回执与合并重渲染通知；
// 传输全归 N174 总线，本宿主不自连客户端消息
class ScriptActionHost : public QObject {
    Q_OBJECT
  public:
    // controller 只为自连 mpvShutdown 收口；收发全经 bus
    explicit ScriptActionHost(ScriptMessageBus *bus, MpvController *controller,
                              QObject *parent = nullptr);

    // 值返回——QHash 再散列会使引用失效
    std::optional<ScriptActionSpec::ScriptAction> find(const QString &id) const;

  signals:
    // create / set 成功后的合并通知，同一事件循环只发一次
    void actionsChanged();

  private:
    void handleScriptAction(const QStringList &args);
    void handleCreate(const QStringList &args);
    void handleSet(const QStringList &args);
    // create / set 共用的前三步校验；false 表已回执，调用方直接返回
    bool precheck(const QStringList &args, QString &owner, QString &id,
                  ScriptActionSpec::ScriptActionPatch &patch);
    void reject(const QString &owner, const QString &id, QLatin1String reason);
    void scheduleRebuild();
    void shutdown();

    ScriptMessageBus *m_bus = nullptr;
    QHash<QString, ScriptActionSpec::ScriptAction> m_actions; // 键为 id
    bool m_rebuildPending = false;
    bool m_stopped = false;
};

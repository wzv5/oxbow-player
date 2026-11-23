#pragma once

#include "pch.h"

#include "PlaylistProvider.h"

class ProviderSettingsPage;
class ScriptListChannel;

// 类型键与显示名源文案
inline constexpr char kScriptListProviderKey[] = "script";
inline constexpr char kScriptListProviderName[] = QT_TRANSLATE_NOOP("ScriptListProvider", "Script");

// N179：脚本数据源驱动的普通列表提供者。列表只存绑定 id，内容按 need-layer 逐层
// 向脚本取；内容更新只经用户手动刷新。
class ScriptListProvider : public PlaylistProvider {
  public:
    // 空 id = 未绑定（缺失 / 空 / 非法同此）：列表仍存在，显示占位
    ScriptListProvider(const QString &scriptId, const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    bool supportsSort() const override;
    SortRule defaultSortRule() const override;

    void saveSettings(QSettings &settings) const override;

    // 恒返回非空：工厂返空会按未知类型回落内建静态表
    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

    static QString displayName();

    QString scriptId() const { return m_scriptId; }

  private:
    QString m_scriptId;
    // 首载等注册一次即止：脚本可能晚于插件就绪，之后的切回与刷新不再等。
    // 枚举在任务线程进行，旗由原子交换一次落定
    mutable std::atomic_bool m_firstLoad = true;
};

// seed = 同类型存量提供者（新建传 nullptr）
ProviderSettingsPage *createScriptListSettingsPage(const PlaylistProvider *seed,
                                                   const std::shared_ptr<ServiceRegistry> &registry,
                                                   QWidget *parent);

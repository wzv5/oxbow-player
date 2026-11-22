#pragma once

#include "PlaylistProvider.h"

// 类型键与显示名源文案（显示名经 translate 取值）
inline constexpr char kStaticProviderKey[] = "static";
inline constexpr char kStaticProviderName[] = QT_TRANSLATE_NOOP("StaticProvider", "Static");

class ProviderSettingsPage;

// 静态列表提供者：条目由用户手工添加，内容以身份串有序表持有，enumerate 现场
// 造实体。序列化写类型键与全部条目，不持久化排序键。默认 tab 与缺键回落经
// ProviderCatalog 的静态出口出生。
class StaticProvider : public PlaylistProvider {
  public:
    // enumerate 造实体时把 registry 转交 fromPath
    explicit StaticProvider(const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    void setItems(const EntityList &entities) override;
    bool supportsModify() const override;
    // 不覆写：沿用基类默认（可修改即允许排序）

    static QString displayName();

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

  private:
    QStringList m_urls; // 持久化形态的身份串有序表
};

// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createStaticSettingsPage(const PlaylistProvider *seed,
                                               const std::shared_ptr<ServiceRegistry> &registry,
                                               QWidget *parent);

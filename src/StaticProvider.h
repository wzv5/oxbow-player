#pragma once

#include "PlaylistProvider.h"

// N44：提供者的身份常量（类型键 + 显示名源文案），登记表与 writeProviderKey 共用
// 同一点定义——键只有一处，写错即无处可错
inline constexpr char kStaticProviderKey[] = "static";
inline constexpr char kStaticProviderName[] = QT_TRANSLATE_NOOP("StaticProvider", "Static");

class ProviderSettingsPage;

// 静态列表提供者（N9）：条目由用户手工添加，内容持有于本实现内部。
// N26：仅保留持久化所需的最小表示（playUrl 有序表，不存实体）——
// enumerate 现场造实体返回；N29 起不预热元数据（只有 URL 列表、无可用元数据，
// 实体保持未求值，按需经视图侧预读骨架供给），setItems 从实体提取 playUrl 覆盖；
// 序列化写类型键 + 全部条目（不持久化排序键）；默认 tab 同样使用本实现，
// 仅持久化侧跳过。
// N44：本实现是登记表第 0 项（默认提供者），默认 tab 经 createDefault 出生
class StaticProvider : public PlaylistProvider {
  public:
    // registry 经基类持有，必填无缺省，enumerate 造实体时转交 fromPlayUrl
    explicit StaticProvider(const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate() const override;
    void setItems(const EntityList &entities) override;
    bool supportsModify() const override;
    // supportsSort 不覆写：沿用基类默认（可修改即允许排序）

    // 当前语言下的显示名（无译文时即英文源）；登记表以函数指针引用（N44）
    static QString displayName();

    void saveSettings(QSettings &settings) const override;

    // 无参出生（N44）：默认 tab 的提供者没有参数可给，登记表第 0 项经此构造
    static std::shared_ptr<PlaylistProvider>
    createDefault(const std::shared_ptr<ServiceRegistry> &registry);

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

  private:
    QStringList m_urls; // 持久化形态的 playUrl 有序表
};

// 参数页工厂（N44）：页面类定义在 StaticProvider.cpp，本处只声明工厂。
// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createStaticSettingsPage(const PlaylistProvider *seed,
                                               const std::shared_ptr<ServiceRegistry> &registry,
                                               QWidget *parent);

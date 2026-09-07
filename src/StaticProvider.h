#pragma once

#include "PlaylistProvider.h"

// 静态列表提供者（N9）：条目由用户手工添加，内容持有于本实现内部。
// N26：仅保留持久化所需的最小表示（playUrl 有序表，不存实体）——
// enumerate 现场造实体（并预热文件元数据）返回，setItems 从实体提取 playUrl 覆盖；
// 序列化写类型键 + 全部条目（不持久化排序键）；默认 tab 同样使用本实现，
// 仅持久化侧跳过
class StaticProvider : public PlaylistProvider {
  public:
    PlaylistProvider::Type type() const override;
    QString describe() const override;
    std::expected<EntityList, QString> enumerate() const override;
    void setItems(const EntityList &entities) override;
    bool supportsModify() const override;
    // supportsSort 不覆写：沿用基类默认（可修改即允许排序）

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<StaticProvider> load(QSettings &settings);

  private:
    QStringList m_urls; // 持久化形态的 playUrl 有序表
};

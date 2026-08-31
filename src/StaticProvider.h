#pragma once

#include "PlaylistProvider.h"

// 静态列表提供者（N9）：条目由用户手工添加，内容持有于本实现内部。
// enumerate 返回所持条目——与目录实现同构，视图统一从提供者取内容；
// 序列化写类型键 + 全部条目（不持久化排序键）；默认 tab 同样使用本实现，
// 仅持久化侧跳过
class StaticProvider : public PlaylistProvider {
  public:
    // 视图 → 提供者内容回写（移交所有权）
    void setItems(QStringList &&items);

    PlaylistProvider::Type type() const override;
    QString describe() const override;
    QStringList enumerate() const override;
    bool supportsRemove() const override;
    bool supportsSort() const override;

    void saveSettings(QSettings &settings) const override;

    static std::unique_ptr<StaticProvider> load(QSettings &settings);

  private:
    QStringList m_items; // 规范化路径有序表
};

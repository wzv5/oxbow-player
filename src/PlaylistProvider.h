#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

class QSettings;

// 播放列表提供者抽象接口（N9）：条目来源策略与列表视图解耦。
// N26 起以播放项实体为交换媒介：enumerate 返回实体有序表（所有权一次性移交视图，
// 实体在提供者侧现场构造并预热文件元数据）；视图层据此建行并施加排序规则；
// 序列化（存什么、怎么存）完全由具体提供者决定——含各自的排序键读写
class PlaylistProvider {
  public:
    enum class Type { Static, Dir };

    using EntityList = PlaylistEntity::List;

    virtual ~PlaylistProvider();

    virtual Type type() const = 0;
    // UI 展示描述（列表设置对话框回显）
    virtual QString describe() const = 0;
    // 枚举条目（同步）：内容唯一来源。返回 expected——成功带实体有序表，
    // 失败带错误文案（空目录成功返回空表，与失败区分）
    virtual std::expected<EntityList, QString> enumerate() const = 0;
    // 视图 → 提供者内容回传（只读引用）。基类默认实现忽略。
    // 本篇 savePlaylists 仍按 Type::Static 分支回传；
    // 回传条件收口（supportsRemove() || supportsSort() → supportsModify()）归 N27。
    // 不变式：实现方只许从实体提取标量（本轮即 playUrl），不得保存实体指针——
    // 实体归视图层，视图 refresh() / 重建后提供者持有的实例即成陈旧副本
    virtual void setItems(const EntityList &entities) { Q_UNUSED(entities); }
    // 能力位：动态类内容以枚举为准，移除类操作整组禁用（失效清理走刷新）
    virtual bool supportsRemove() const = 0;
    // 能力位：m3u 类列表不支持排序（预留）
    virtual bool supportsSort() const = 0;

    // ——INI 序列化：完全委托实现——写 provider 类型键、自身参数与排序键
    virtual void saveSettings(QSettings &settings) const = 0;

    // 工厂：按 group 内 provider 键分派到对应实现的 load；
    // static 或缺失键 → 静态提供者
    static std::unique_ptr<PlaylistProvider> load(QSettings &settings);

    // ——排序规则（非虚，统一持有；取值域见 PlaylistWidget::SortKey，
    // 提供者仅作不透明值存储并自行持久化）——
    int sortKey() const { return m_sortKey; }
    bool sortAscending() const { return m_sortAscending; }
    void setSortRule(int key, bool ascending);

  private:
    int m_sortKey = 0; // 0 = None
    bool m_sortAscending = true;
};

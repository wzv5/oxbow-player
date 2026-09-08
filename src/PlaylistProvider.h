#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

class QSettings;

// 播放列表提供者抽象接口（N9）：条目来源策略与列表视图解耦。
// N26 起以播放项实体为交换媒介：enumerate 返回实体有序表（所有权一次性移交视图，
// 实体在提供者侧现场构造，遍历中顺手已持有的元数据可经注入口主动注入）；
// 视图层据此建行并施加排序规则；
// 序列化（存什么、怎么存）完全由具体提供者决定——含各自的排序键读写
class PlaylistProvider {
  public:
    // N27：仅用于列表设置对话框的回显 / 构造与 INI 序列化，不再参与任何行为分支
    // （行为一律由 supportsModify / supportsSort 能力位决定）
    enum class Type { Static, Dir };

    using EntityList = PlaylistEntity::List;

    virtual ~PlaylistProvider();

    virtual Type type() const = 0;
    // UI 展示描述（列表设置对话框回显）
    virtual QString describe() const = 0;
    // 枚举条目（同步）：内容唯一来源。返回 expected——成功带实体有序表，
    // 失败带错误文案（空目录成功返回空表，与失败区分）
    virtual std::expected<EntityList, QString> enumerate() const = 0;
    // 视图 → 提供者内容回传（只读引用）。基类默认实现忽略——不可修改的提供者
    // 不参与回传。回传是内容落盘的唯一入口：savePlaylists 对可修改提供者逐个回传
    // 后随即序列化，回传与落盘不得拆开（N27）。
    // N28：回传另有守卫——未加载（含首载在途）的表实体集合为空，回传会清空提供者
    // 自持内容造成永久丢数据，故整段跳过；落盘侧照走 saveSettings
    // 不变式：实现方只许从实体提取标量（本轮即 playUrl），不得保存实体指针——
    // 实体归视图层，视图 refresh() / 重建后提供者持有的实例即成陈旧副本
    virtual void setItems(const EntityList &entities) { Q_UNUSED(entities); }
    // 能力位：是否支持修改（添加与删除）。不可修改的提供者内容以枚举为准，
    // 视图侧整组禁用增删（失效清理走刷新）
    virtual bool supportsModify() const = 0;
    // 能力位：是否支持排序。默认随 supportsModify——允许修改即允许排序，反之不成立
    // （目录表可排序而不可增删，须显式覆写 true；可修改而不可排序的 m3u 类
    // 预留实现须显式覆写 false）
    virtual bool supportsSort() const { return supportsModify(); }

    // ——INI 序列化：完全委托实现——写 provider 类型键、自身参数与排序键
    virtual void saveSettings(QSettings &settings) const = 0;

    // 工厂：按 group 内 provider 键分派到对应实现的 load；
    // static 或缺失键 → 静态提供者。
    // N28：直接产出 shared_ptr——提供者所有权模型即共享（视图容器 + 枚举线程），
    // 调用点无需再经 unique_ptr 中间形态转换
    static std::shared_ptr<PlaylistProvider> load(QSettings &settings);

    // ——排序规则（非虚，统一持有；取值域见 PlaylistWidget::SortKey，
    // 提供者仅作不透明值存储并自行持久化）——
    int sortKey() const { return m_sortKey; }
    bool sortAscending() const { return m_sortAscending; }
    void setSortRule(int key, bool ascending);

  private:
    int m_sortKey = 0; // 0 = None
    bool m_sortAscending = true;

  protected:
    // ——N29 元数据注入口（唯一授权通道）——
    // 实体的 setFileMeta / setMediaMeta 为 protected 且仅友元（本类）可及；
    // 友元不继承，具体提供者经此静态转发写入——访问发生在本类成员函数体内，
    // 友元在此生效。新增提供者无需改实体头文件，只调转发函数
    static void injectFileMeta(PlaylistEntity &e, const FileMeta &m) { e.setFileMeta(m); }
    static void injectMediaMeta(PlaylistEntity &e, const PlaylistEntity::MediaMeta &m) {
        e.setMediaMeta(m);
    }
};

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
    using EntityList = PlaylistEntity::List;

    // ——N37：枚举结果——条目 + 非致命警告。warnings 为"部分来源失败 / 被跳过"的可读
    // 文案（条目已产出，不判整次失败）；unexpected 语义不变：来源整体不可用，
    // 视图进占位行。
    // N45：增容器数组。容器组恒在实体组之前是模型不变式（不是显示偏好），
    // 两数组顺序完备：显示序 = containers 序接 entities 序
    struct Container {
        QString id; // 提供者内稳定，作为 parent 路径段
        QString title;
        std::optional<FileMeta> meta; // 复用 FileMeta，供容器参与排序
        // N47：该层完整路径（纯展示，供属性对话框 / 复制 / 打开；空 = 未知）。
        // 本地为绝对路径，WebDAV 为无凭据完整 URL；不参与排序、寻址与持久化
        QString path;
    };
    struct Enumeration {
        QList<Container> containers;
        EntityList entities;
        QStringList warnings;
    };

    // ——N35 服务注册表（基类统一持有，供子类 enumerate 造实体时注入）——
    // 构造注入，必填无缺省：所有提供者诞生点必须显式传入可用实例。
    // Static 经此向 fromPlayUrl 转交，使持久化的 http 条目重载后仍得同类型实体
    explicit PlaylistProvider(const std::shared_ptr<ServiceRegistry> &registry);

    virtual ~PlaylistProvider();

    // 类型键（N44）：提供者类型的唯一身份，取值即各提供者头文件的键常量
    // （kXxxProviderKey）。INI 的 provider 键写侧经 writeProviderKey 取它、
    // 读侧经登记表条目比对它，两侧共用同一点定义。
    // 取代原 Type 枚举 + type()：类型身份不再需要一处集中维护的枚举，
    // 新增提供者不必回改基类
    virtual QString typeKey() const = 0;
    // 枚举条目（同步）：内容唯一来源。返回 expected——成功带实体有序表与警告
    // （空目录成功返回空表，与失败区分；N37 起部分来源失败随 warnings 回传），
    // 失败带错误文案。
    // N45：parent 为相对根的路径段序列（不含根本身），空即根层——决定枚举哪一层。
    // 契约：视图只对提供者自己返回过的容器 id 发起非空 parent 调用；平铺形态的
    // 提供者收到非空 parent 属调用方违约——Q_ASSERT 守候，release 降级返回空结果
    // （照 fromPlayUrl 对"http 加空 registry"的既有先例），不得静默忽略。
    // 提供者不产出"上一级"项，该行由视图合成
    virtual std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const = 0;
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

    // 工厂：按 group 内 provider 键经登记表（ProviderCatalog）分派到对应实现的
    // load；键缺失 / 未知 → 回落登记表第 0 项（默认提供者，现状即静态）。
    // registry 转交对应实现（缺省空）
    // N28：直接产出 shared_ptr——提供者所有权模型即共享（视图容器 + 枚举线程），
    // 调用点无需再经 unique_ptr 中间形态转换
    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

    // ——排序规则（非虚，统一持有；取值域见 PlaylistWidget::SortKey，
    // 提供者仅作不透明值存储并自行持久化）——
    int sortKey() const { return m_sortKey; }
    bool sortAscending() const { return m_sortAscending; }
    void setSortRule(int key, bool ascending);

  private:
    int m_sortKey = 0; // 0 = None
    bool m_sortAscending = true;
    std::shared_ptr<ServiceRegistry> m_registry; // N35：服务注册表（基类统一持有）

  protected:
    // 写 INI 的 provider 键（N44）：取值即 typeKey()。各提供者 saveSettings 首行
    // 调用它——写侧（typeKey()）与读侧（登记表条目）共用同一点定义的键常量，
    // 杜绝"键写错导致重启后退化为默认提供者"的静默失败
    void writeProviderKey(QSettings &settings) const;

    // 子类经此取构造期注入的注册表（enumerate 造实体时转交 fromPlayUrl）
    const std::shared_ptr<ServiceRegistry> &registry() const { return m_registry; }

    // ——N29 元数据注入口（唯一授权通道）——
    // 实体的 setFileMeta / setMediaMeta 为 protected 且仅友元（本类）可及；
    // 友元不继承，具体提供者经此静态转发写入——访问发生在本类成员函数体内，
    // 友元在此生效。新增提供者无需改实体头文件，只调转发函数
    static void injectFileMeta(PlaylistEntity &e, const FileMeta &m) { e.setFileMeta(m); }
    static void injectMediaMeta(PlaylistEntity &e, const PlaylistEntity::MediaMeta &m) {
        e.setMediaMeta(m);
    }
};

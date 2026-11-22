#pragma once

#include "pch.h"

#include "entities/PlaylistEntity.h"

class StreamEndpoint;
class StreamRelayIf;
class PlaybackReporter;

// 播放列表提供者抽象接口：条目来源策略与列表视图解耦。交换媒介是播放项实体：
// enumerate 现场构造实体并一次性移交所有权；序列化由具体提供者决定。
class PlaylistProvider {
  public:
    using EntityList = PlaylistEntity::List;

    // 容器条目：id 在提供者内稳定，作为 parent 路径段
    struct Container {
        QString id;
        QString title;
        std::optional<FileMeta> meta; // 供容器参与排序
        // 纯展示（空 = 未知），不参与排序、寻址与持久化；WebDAV 为无凭据 URL
        QString path;
    };
    // 根层与子孙层共用的内容底座。containers 恒在 entities 之前；不含 warnings /
    // 错误字段：子层缺席即失败，解释性文案归根层 warnings
    struct LevelData {
        QList<Container> containers;
        EntityList entities;
    };
    // 根层内容 + 非致命警告。warnings 表示部分来源失败 / 被跳过，条目仍已产出；
    // unexpected 表示来源整体不可用。整树形态的 descendants 键为相对表根的路径段
    // 序列，值为该层内容；不存在部分交付
    struct Enumeration : LevelData {
        QStringList warnings;
        QMap<QStringList, LevelData> descendants;
    };

    // 构造注入、必填
    explicit PlaylistProvider(const std::shared_ptr<ServiceRegistry> &registry);

    virtual ~PlaylistProvider();

    // 类型键（kXxxProviderKey），INI 的 provider 键写读两侧共用这一点定义
    virtual QString typeKey() const = 0;
    // 同步枚举，空目录是成功而非失败。parent 为相对根的路径段序列，空即根层；
    // 视图只对提供者返回过的容器 id 发起调用，平铺提供者收到非空 parent 属调用方
    // 违约（Debug 断言，Release 返回空结果）。产物必须非空且身份已定形
    // （Util::Url::isDefiniteForm）；"上一级"项由视图合成，提供者不产出
    virtual std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const = 0;
    // 视图 → 提供者内容回传，内容落盘的唯一入口，回传后随即序列化。未加载（含首载
    // 在途）时实体集合为空，回传会清空内容，故整段跳过。只许提取标量，不得保存
    // 实体指针：实体归视图，refresh 后即成陈旧副本
    virtual void setItems(const EntityList &entities) { Q_UNUSED(entities); }
    virtual bool supportsModify() const = 0;
    // 默认随 supportsModify；可修改但不可排序的实现须显式覆写 false
    virtual bool supportsSort() const { return supportsModify(); }

    // true = 一次交付整棵树，视图只发空 parent 并按权威内容整树重建（缺席即删）；
    // 默认逐层。须显式声明而非由结果推断：空树与逐层空结果同形。
    // 整树契约（违约 Debug 断言，Release 降级不崩溃）：
    // 1. 逐层提供者不得带 descendants；
    // 2. 整树提供者不应答非空 parent；
    // 3. descendants 祖先链须由本次交付构成：首段 ∈ 根层 containers，逐段落在
    //    交付层 containers；调用层与根层不出现。非法键 Release 逐项忽略；同层容器
    //    id 重复即寻址歧义，Debug 断言，Release 按先到者寻址；
    // 4. 错误与警告只住根层，解析失败的组以缺席表达。深度不设限
    virtual bool enumeratesWholeTree() const { return false; }

    // 转接不是"能力位 + 开关"：Required × 未勾选是语义上不存在的状态，故能力
    // 三态，有效决策收成 usesTransit()：
    //   Unsupported（默认）| 任意偏好     | false
    //   Optional（WebDAV） | true / false | 跟随偏好
    //   Required（Emby 等）| 任意偏好     | true（关不掉）
    // usesTransit() 为真时 directPlayUrl() 永不被调用，Required 不必覆写它
    enum class TransitCapability { Unsupported, Optional, Required };
    virtual TransitCapability transitCapability() const { return TransitCapability::Unsupported; }
    // Unsupported 必空；Optional/Required 返空 = 编程错误。端点无 QObject，任意线程可析构
    virtual std::shared_ptr<StreamEndpoint> createStreamEndpoint() const;

    // 唯一读持久化偏好并给出有效决策的地方
    bool usesTransit() const;

    // 模板方法：直发 → directPlayUrl()，转接 → 铸 oxbow:// URI 并发布端点。
    // 实现必须轻（playTargets 整表逐条调用）、零网络、仅 GUI 线程。列表身份按值
    // 传入；串与逐文件选项在同一句 return 里出生
    PlayTarget playTarget(const QUuid &listUuid, const QString &path,
                          const QString &fileName) const;
    QString playUrl(const QUuid &listUuid, const QString &path, const QString &fileName) const {
        return playTarget(listUuid, path, fileName).url;
    }

    std::optional<bool> transitPreference() const;
    void setTransitPreference(bool preferred);

    // 提供者造报告者但不注册：注册需要列表身份，而提供者不持有
    const std::shared_ptr<PlaybackReporter> &reporter() const { return m_reporter; }

    virtual void saveSettings(QSettings &settings) const = 0;

    // 经 saveSettings / load 唯一序列化器往返；静态表不持久化排序键，故把源当下
    // 的排序规则原样补入
    std::shared_ptr<PlaylistProvider> duplicated(QSettings &staging) const;

    // 经 ProviderCatalog::loadProvider 按 provider 键分派；键缺失 / 未知回落内建
    // 静态列表（写死，与条目顺序无关）
    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

    // 排序规则值归提供者持久化，键定义归实体侧（SortKey）；默认规则由提供者自述
    SortRule sortRule() const { return m_sortRule; }
    void setSortRule(SortRule rule) { m_sortRule = rule; }
    virtual SortRule defaultSortRule() const { return {}; }

  private:
    SortRule m_sortRule;
    std::shared_ptr<ServiceRegistry> m_registry;
    std::shared_ptr<PlaybackReporter> m_reporter;
    // 端点惰性建一次并缓存；仅 GUI 线程调用，无需加锁
    bool m_transitPreferred = false;
    mutable std::shared_ptr<StreamEndpoint> m_transitEndpoint;

  protected:
    // 默认恒等。WebDavProvider 覆写为注入当前凭据——凭据只出现在 PROPFIND 与喂
    // mpv 两点；产出不进实体、不落盘
    virtual QString directPlayUrl(const QString &path) const { return path; }

    // 写 provider 键（取值即 typeKey()），避免键写错导致重启后静默退化。
    // transit 只对 Optional 写出，Unsupported / Required 不写并清掉存量键
    void writeProviderKey(QSettings &settings) const;

    const std::shared_ptr<ServiceRegistry> &registry() const { return m_registry; }

    // 注册由列表侧完成
    void setReporter(std::shared_ptr<PlaybackReporter> reporter) {
        m_reporter = std::move(reporter);
    }

    // 实体写点为 protected，友元不继承，具体提供者经此转发
    static void injectFileMeta(PlaylistEntity &e, const FileMeta &m) { e.setFileMeta(m); }
    static void injectMediaMeta(PlaylistEntity &e, const PlaylistEntity::MediaMeta &m) {
        e.setMediaMeta(m);
    }
    static void injectDisplayName(PlaylistEntity &e, const QString &name) { e.setTitle(name); }
    // title 未显式设置时同步
    static void injectFileName(PlaylistEntity &e, const QString &name) { e.setFilename(name); }
    static void injectPlayOptions(PlaylistEntity &e, QVariantMap options) {
        e.setPlayOptions(std::move(options));
    }
};

#pragma once

#include "pch.h"

#include "MediaInfoHelper.h"

// 文件级元数据；字段 nullopt 即"未知"
struct FileMeta {
    std::optional<qint64> size;
    std::optional<QDateTime> created;
    std::optional<QDateTime> modified;
};

// 排序键。取值随提供者持久化，既有值冻结、新键只准追加末尾（N57）；
// None = 保持层内当前序，EnumerationOrder = 枚举返回序（N58）。
enum class SortKey {
    None,
    FileName,
    FileNameNumeric,
    Extension,
    Created,
    Modified,
    FileSize,
    Duration,
    ReleaseDate,
    EnumerationOrder,
    DisplayName,
};

struct SortRule {
    SortKey key = SortKey::None;
    bool ascending = true;
};

// N65：loadfile 串与逐文件选项；options 的键为 mpv 选项名，值按字符串下发
struct PlayTarget {
    QString url;
    QVariantMap options;
};

// None = 该排序键不依赖元数据（N29）
enum class MetaCategory {
    None,
    File,
    Media,
};

MetaCategory metaCategoryOf(SortKey key);

// 播放项实体（N26）：提供者与视图层交换数据的媒介，也是元数据的唯一载体。
// 缓存由基类持有，子类只实现 compute* 与 canCompute*。
class PlaylistProvider;
class PlaylistWidget;
class ServiceRegistry;

class PlaylistEntity {
    // 注入口授权对象；友元不继承，派生提供者经 PlaylistProvider 静态转发
    friend class PlaylistProvider;
    // 预读结果的视图侧写点（N32）
    friend class PlaylistWidget;

  public:
    using Ptr = std::shared_ptr<PlaylistEntity>;
    using List = std::vector<Ptr>;

    // 媒体元数据：复用 MediaInfoHelper 的定义，避免字段漂移
    using MediaStream = MediaInfoHelper::Stream;
    using MediaChapter = MediaInfoHelper::Chapter;
    using MediaMeta = MediaInfoHelper::Data;

    explicit PlaylistEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry);
    virtual ~PlaylistEntity() = default;

    // 入参必须是已定形身份串（N56）；未定形属调用方违约，Debug 断言，release 落 LocalFileEntity。
    // 前缀分派不用 QUrl::scheme()，避免误判 Windows 盘符；不变式：path 往返相等。
    static Ptr fromPath(const QString &path, const std::shared_ptr<ServiceRegistry> &registry);

    // 回显专用入口（N61）：与 fromPath 同一分派，但放宽定形检查——补不出形态者落
    // UnknownEntity，stdin 先分派。入参须先整形并 unwrap；调用方是系统边界，故降级不终止。
    static Ptr fromMpvEcho(const QString &identity,
                           const std::shared_ptr<ServiceRegistry> &registry);

    // 深拷贝（含已解析缓存），副本与原实体互不影响（N31）
    virtual Ptr clone() const = 0;

    const QString &path() const { return m_path; }
    // 列表归属；未盖章为 nil（N74）
    QUuid ownerUuid() const { return m_ownerUuid; }
    // 默认 Util::Url::key(path())，构造期缓存；远端可重写
    virtual QString key() const;
    // 显示用身份，剥除凭据；构造期定死，非虚（N56）
    const QString &displayUrl() const { return m_displayUrl; }
    // 键级判等，容忍形态漂移（N30）；回刷判等另走逐字节比对（N56）
    bool matches(const QString &path) const;
    // 形态解析：现问归属提供者，不缓存。锁失败即契约破坏——Debug 断言，
    // release 返空串，不退回 path()。仅 GUI 线程调用（提供者端点缓存无锁）。
    QString playUrl() const;
    // 串与选项的整体出口；与 playUrl 同一条归属通路（N65）
    PlayTarget playTarget() const;
    // 条目级 mpv 选项；只读，不参与 key / 回显比对（N65）
    const QVariantMap &playOptions() const { return m_playOptions; }
    // 显示名；fileName / suffix 另供名称类排序（N70），title 也是 DisplayName 的
    // 取值来源（N71）
    const QString &title() const { return m_title; }       // 恒为有效值，未注入时回退文件名
    const QString &fileName() const { return m_fileName; } // 含扩展名，供名称类排序
    const QString &suffix() const { return m_suffix; }     // 无点小写，供 Extension 排序

    // 缓存优先；可算才惰性求值；任意线程
    const FileMeta &fileMeta() const;
    // 只读；空返回 nullptr
    const FileMeta *cachedFileMeta() const;
    // 缓存优先；可能阻塞
    const MediaMeta &mediaMeta() const;
    // 只读；绘制路径不触发求值
    const MediaMeta *cachedMediaMeta() const;
    const QIcon &icon() const; // 惰性；仅 GUI 线程
    void refresh();            // 清空全部缓存

    // 求值转发：只算不写，供任务线程取回、GUI 续作注入；同步无取消（N32）
    FileMeta evalFileMeta() const;
    MediaMeta evalMediaMeta() const;

    // ——类型事实（取代字符串前缀猜测）——
    virtual bool isLocalFile() const = 0;
    virtual bool exists() const = 0; // 不可判定者恒 true
    // 自主求值能力（N178）：只表示能否自己求值，与能否承载注入值无关；进程期恒定
    virtual bool canComputeFileMeta() const = 0;
    virtual bool canComputeMediaMeta() const = 0;
    // 是否允许播放之外另起解码（缩略图 / 字幕，N107）。默认拒绝，避免触发服务器
    // 连接数 / 令牌限制；放行者须保证流可寻位且总长已知
    virtual bool supportsSecondaryDecode() const { return false; }

    // 元数据派生出口（N178）：hasMeta = 可算或已有值；needsMeta = 可算且无值
    bool canComputeMeta(MetaCategory cat) const {
        return cat == MetaCategory::File    ? canComputeFileMeta()
               : cat == MetaCategory::Media ? canComputeMediaMeta()
                                            : false;
    }
    bool hasCachedMeta(MetaCategory cat) const {
        return cat == MetaCategory::File    ? cachedFileMeta() != nullptr
               : cat == MetaCategory::Media ? cachedMediaMeta() != nullptr
                                            : false;
    }
    bool hasMeta(MetaCategory cat) const { return canComputeMeta(cat) || hasCachedMeta(cat); }
    bool needsMeta(MetaCategory cat) const { return canComputeMeta(cat) && !hasCachedMeta(cat); }

  protected:
    PlaylistEntity(const PlaylistEntity &) = default;
    PlaylistEntity &operator=(const PlaylistEntity &) = delete;

    // 修正文件名；同步重算 suffix，未设 title 时同步 title（N70）
    void setFilename(QString fileName);
    // 修正显示名；置旗后 setFilename 不再联动 title
    void setTitle(QString title) {
        m_title = std::move(title);
        m_titleSet = true;
    }
    // 覆写显示形态；仅构造期调用
    void setDisplayUrl(QString displayUrl) { m_displayUrl = std::move(displayUrl); }
    // 选项写点；仅 PlaylistProvider 可及，入列后不改（N65）
    void setPlayOptions(QVariantMap options) { m_playOptions = std::move(options); }

    // 服务注册表；http(s) 子类构造时断言非空
    const std::shared_ptr<ServiceRegistry> &registry() const { return m_registry; }

    // 元数据注入口：写缓存并视同已求值；refresh() 清掉，重新枚举重注（N29/N178）。
    // 仅 PlaylistProvider / PlaylistWidget 可及
    void setFileMeta(const FileMeta &meta);
    void setMediaMeta(const MediaMeta &meta);

    // 归属写点：显式赋盖；收编 clone 的原归属靠"不再盖"保住（N55）
    void setOwnership(QUuid listUuid, std::weak_ptr<const PlaylistProvider> provider) {
        m_ownerUuid = std::move(listUuid);
        m_ownerProvider = std::move(provider);
    }

    // 同步求值，可能阻塞 / 超时，无取消；仅任务线程调用，不可算者恒不被调
    virtual FileMeta computeFileMeta() const = 0;
    virtual MediaMeta computeMediaMeta() const = 0;
    virtual QIcon computeIcon() const = 0;

  private:
    // 构造期派生名称字段
    void deriveNames();

    QString m_path;
    std::shared_ptr<ServiceRegistry> m_registry;
    // 归属：列表身份 + 提供者弱引用（不延长其寿命）。入列时一次盖章，clone 携带；
    // 不参与判等 / 持久化，未盖章时消费形态属调用方违约。
    QUuid m_ownerUuid;
    std::weak_ptr<const PlaylistProvider> m_ownerProvider;
    QString m_title;
    QString m_fileName;
    QString m_suffix;
    bool m_titleSet = false;
    QString m_key;        // 构造期缓存，绘制热路径逐行取用
    QString m_displayUrl; // 构造期缓存
    QVariantMap m_playOptions;
    mutable std::optional<FileMeta> m_fileMeta;
    mutable std::optional<MediaMeta> m_mediaMeta;
    mutable std::optional<QIcon> m_icon;
};

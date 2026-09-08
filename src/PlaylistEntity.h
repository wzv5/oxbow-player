#pragma once

#include "pch.h"

#include "MediaInfoHelper.h"

// 文件级元数据（纯数据，任意线程可求值；字段为 nullopt 即"未知"）
struct FileMeta {
    std::optional<qint64> size;
    std::optional<QDateTime> created;
    std::optional<QDateTime> modified;
};

// 播放项实体（N26）：提供者 ↔ 视图层交换数据的唯一媒介，也是元数据的唯一载体。
// 缓存由基类统一持有（模板方法），子类只实现三个求值函数与能力位。
// 能力位与静态空回落（两类元数据同形）：fileMeta() / mediaMeta() 首行各判自身能力位，
// 为假则直接返回静态空值，永不调用对应 compute*()；
// cachedFileMeta() / cachedMediaMeta() 对此类实体恒返回 nullptr。
class PlaylistProvider; // 前向声明：实体头不依赖提供者头

class PlaylistEntity {
    // N29 注入口的唯一授权对象。友元不继承：派生提供者不可直接访问，
    // 经 PlaylistProvider 的 protected 静态转发（访问发生在基类成员函数体内）写入
    friend class PlaylistProvider;

  public:
    using Ptr = std::shared_ptr<PlaylistEntity>;
    using List = std::vector<Ptr>;

    // 媒体元数据：复用 MediaInfoHelper 的定义（不 1:1 镜像，避免字段漂移）
    using MediaStream = MediaInfoHelper::Stream;
    using MediaChapter = MediaInfoHelper::Chapter;
    using MediaMeta = MediaInfoHelper::Data;

    explicit PlaylistEntity(QString playUrl);
    virtual ~PlaylistEntity() = default;

    // ——工厂：playUrl → 实体（唯一入口；后续协议在此分派）——
    // 入参先经 Util::resolveLocalPath 加工（本地来源唯一出口；网络 / 流路径原样），
    // 随后按 Util::isNetworkStreamPath 分派：本地路径 → LocalFileEntity，
    // 网络 / 流路径 → UnknownEntity。
    // 不变式：fromPlayUrl(e->playUrl())->playUrl() == e->playUrl()
    static Ptr fromPlayUrl(const QString &playUrl);

    // ——按值复制（深拷贝，含已解析缓存）——
    // 与 Ptr 共享计数相对：副本与原实体此后互不影响（一方 refresh 不清另一方）。
    // 用途：回刷收编时复用播放表已有实体，避免整表元数据重读（N30）。
    virtual Ptr clone() const = 0;

    // ——身份与显示——
    const QString &playUrl() const { return m_playUrl; } // 喂 mpv / 持久化 / 回刷比对
    // 默认 normalizePathForMatch(playUrl)（构造期缓存）；远端可重写为服务端 id
    virtual QString key() const;
    const QString &title() const { return m_title; } // 列表显示名，恒为有效值（本地回退文件名）
    const QString &fileName() const {
        return m_fileName;
    } // 含扩展名：FileName / FileNameNumeric 排序用
    const QString &suffix() const { return m_suffix; } // 无点小写：Extension 排序用

    // ——元数据（惰性求值 + 实体内缓存，全部 const）——
    const FileMeta &fileMeta() const; // 惰性 computeFileMeta()；任意线程；单次 stat，无绘制风险
    const FileMeta *cachedFileMeta() const; // 只读：未求值（或不支持）返回 nullptr
    const MediaMeta &
    mediaMeta() const; // 惰性 computeMediaMeta()；可能阻塞，仅供排序 / 对话框 / 预读
    const MediaMeta *
    cachedMediaMeta() const;   // 只读：未解析（或不支持）返回 nullptr；绘制路径专用，绝不触发求值
    const QIcon &icon() const; // 惰性 computeIcon()；仅 GUI 线程
    void refresh();            // 清空全部缓存（fileMeta / mediaMeta / icon）

    // ——能力位（取代字符串前缀猜测）——
    virtual bool isLocalFile() const = 0;
    virtual bool exists() const = 0;            // 未知实体恒 true（不可判定）
    virtual bool supportsFileMeta() const = 0;  // 是否具备文件级属性（大小 / 创建 / 修改）
    virtual bool supportsMediaMeta() const = 0; // 是否具备媒体属性

  protected:
    // 拷贝构造仅对子类开放：clone() 经子类的拷贝构造实现，对外仍不可拷贝
    PlaylistEntity(const PlaylistEntity &) = default;
    PlaylistEntity &operator=(const PlaylistEntity &) = delete;

    // 子类修正文件名：网络 / 远端地址无法从 playUrl 中准确提取文件名时调用。
    // 基类随之重算 suffix()，并在 title() 未被显式设置时同步 title()
    void setFilename(QString fileName);
    // 子类构造后修正显示名（远端取服务端标题）
    void setTitle(QString title) {
        m_title = std::move(title);
        m_titleSet = true;
    }

    // ——N29 元数据注入口（写入外部已取得的元数据，不触发求值）——
    // 写缓存 + 视同已求值：随后 fileMeta() / mediaMeta() 返回注入值，
    // cachedFileMeta() / cachedMediaMeta() 非空，预读骨架对该行命中跳过。
    // supports*Meta() 为假时忽略（缓存不落值，与惰性求值的静态空回落同形）。
    // 仅 PlaylistProvider 经友元可及；视图层与具体提供者直接调用编译失败
    void setFileMeta(const FileMeta &meta);
    void setMediaMeta(const MediaMeta &meta);

    virtual FileMeta computeFileMeta() const = 0;
    virtual MediaMeta computeMediaMeta() const = 0;
    virtual QIcon computeIcon() const = 0;

  private:
    // 构造期派生 title / fileName / suffix（纯字符串运算，与 playUrl 同生命周期）
    void deriveNames();

    QString m_playUrl;
    QString m_title;         // 构造期派生
    QString m_fileName;      // setFilename 可改，改则同步 suffix
    QString m_suffix;        // 同上
    bool m_titleSet = false; // setTitle 显式设置后置位，此后 setFilename 不再联动 title
    QString m_key;           // 构造期缓存：normalizePathForMatch(playUrl)——key()
                             // 在委托绘制热路径上，避免逐行逐帧重算
    mutable std::optional<FileMeta> m_fileMeta;
    mutable std::optional<MediaMeta> m_mediaMeta;
    mutable std::optional<QIcon> m_icon;
};

// 兜底实体（N26）：工厂对网络 / 流路径产出。插件不做任何额外加工，
// playUrl() 原样喂 mpv / 持久化 / 回显比对。完整远端实体（服务端标题、延迟解析、
// 远端元数据）归后续篇；未知实体是其最小替身，保证网络项有实体可落
// （回刷收编的 mpv 列表即现实引用者）。
class UnknownEntity : public PlaylistEntity {
  public:
    explicit UnknownEntity(QString playUrl) : PlaylistEntity(std::move(playUrl)) {}

    Ptr clone() const override { return std::make_shared<UnknownEntity>(*this); }

    bool isLocalFile() const override { return false; }
    bool exists() const override { return true; } // 不可判定，不参与失效清理
    bool supportsFileMeta() const override { return false; }
    bool supportsMediaMeta() const override { return false; }

  protected:
    FileMeta computeFileMeta() const override { return {}; }   // 恒不被调（supportsFileMeta 为假）
    MediaMeta computeMediaMeta() const override { return {}; } // 恒不被调（supportsMediaMeta 为假）
    QIcon computeIcon() const override;
};

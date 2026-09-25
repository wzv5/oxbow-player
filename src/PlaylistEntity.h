#pragma once

#include "pch.h"

#include "MediaInfoHelper.h"

// 文件级元数据（纯数据，任意线程可求值；字段为 nullopt 即"未知"）
struct FileMeta {
    std::optional<qint64> size;
    std::optional<QDateTime> created;
    std::optional<QDateTime> modified;
};

// ——N57 §2.9：排序键与默认规则归位——键的定义与"键 → 元数据类别"映射属实体侧
// （它们描述"按实体的哪个字段排"，文件派生名、文件元数据、媒体元数据本就是实体
// 属性层面的概念）；规则**值**与默认规则归提供者，执行面归视图。

// 排序规则枚举（N4/N9/N18）：None = 添加顺序。取值随提供者持久化（int 落盘）。
// N18：枚举序与菜单显示顺序解耦——菜单 action 通过 setData(int(SortKey)) 显式携带
// 键值，派发时按 data 取键，不依赖数组下标 +1 的隐式映射。
// N57：既有取值冻结在 0..8，新键只准追加末尾（实现侧 static_assert 守候）
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
};

// 排序规则值对象（N57 §2.9）：取代提供者侧的两个散字段与视图里的散参数
struct SortRule {
    SortKey key = SortKey::None;
    bool ascending = true;
};

// N29：预读元数据类别（排序键经 metaCategoryOf 映射；None = 该键不依赖元数据）
enum class MetaCategory {
    None,
    File,
    Media,
};

// N29 的唯一映射（各判定点共用，防散写漂移；N57 §2.9 随键搬到实体侧）：
// Created/Modified/FileSize → File；Duration/ReleaseDate → Media；其余 → None
MetaCategory metaCategoryOf(SortKey key);

// 播放项实体（N26）：提供者 ↔ 视图层交换数据的唯一媒介，也是元数据的唯一载体。
// 缓存由基类统一持有（模板方法），子类只实现三个求值函数与能力位。
// 能力位与静态空回落（两类元数据同形）：fileMeta() / mediaMeta() 首行各判自身能力位，
// 为假则直接返回静态空值，永不调用对应 compute*()；
// cachedFileMeta() / cachedMediaMeta() 对此类实体恒返回 nullptr。
class PlaylistProvider; // 前向声明：实体头不依赖提供者头
class PlaylistWidget;   // N32：预读结果注入的第二个授权对象（见 setFileMeta 注）
class ServiceRegistry;  // N35：服务注册表（基类统一持有，供未来远端实体取用）

class PlaylistEntity {
    // N29 注入口的唯一授权对象。友元不继承：派生提供者不可直接访问，
    // 经 PlaylistProvider 的 protected 静态转发（访问发生在基类成员函数体内）写入
    friend class PlaylistProvider;
    // N32：预读改异步后，经求值转发取回的值由 GUI 续作写回（唯一的写点），
    // 故视图层同样需要注入口；仅此二类可写，其余仍不可及
    friend class PlaylistWidget;

  public:
    using Ptr = std::shared_ptr<PlaylistEntity>;
    using List = std::vector<Ptr>;

    // 媒体元数据：复用 MediaInfoHelper 的定义（不 1:1 镜像，避免字段漂移）
    using MediaStream = MediaInfoHelper::Stream;
    using MediaChapter = MediaInfoHelper::Chapter;
    using MediaMeta = MediaInfoHelper::Data;

    // 构造注入 registry（必填，无缺省）：所有调用点必须显式传入可用实例，
    // 缺 registry 在编译期暴露。本地 / 兜底实体同样经形参接收后存基类（忽略不用）。
    explicit PlaylistEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry);
    virtual ~PlaylistEntity() = default;

    // ——工厂：身份串 → 实体（类型分派的唯一落点；后续协议在此追加）——
    // 入参契约（N56 §2.6）：入参必须是**已定形的身份串**（绝对路径或含 "://" 的 URL）。
    // 定形（mpvqt:// 折叠、相对转绝对、file:// 转本地）是造实体一方的责任，工厂
    // 不再代做——只有提供者知道该怎么拆自己拿来的串，工厂对着读不懂的串加工等于猜。
    // Debug 断言：既非绝对路径、也非含 "://" 的形态即调用方违约，显影（与"http(s)
    // + 空 registry"同款守候；release 不拦，未定形串会静默落成 LocalFileEntity）。
    // 随后按前缀分派（N35）：http:// / https://（大小写不敏感）→ HttpEntity（需
    // 非空 registry，http(s) + 空 registry 属调用错误：Q_ASSERT 守候，release
    // 降级 UnknownEntity 保"恒返回实体"契约）；其余网络 / 流路径（ftp / rtsp 等）
    // → UnknownEntity 兜底；本地路径 → LocalFileEntity（忽略 registry）。
    // 前缀判断天然免疫 QUrl::scheme() 误判 Windows 盘符的陷阱，不引入 QUrl 解析。
    // 不变式：入参已定形时 fromPath(e->path(), registry)->path() == e->path()
    // 调用方纪律：registry 必填（无缺省），一律传视图层注册表（本地路径同样传，
    // 工厂内忽略）；同一身份串在遵守前提下，无论经提供者、工厂还是收编路径，
    // 都得到同类型实体。
    static Ptr fromPath(const QString &path, const std::shared_ptr<ServiceRegistry> &registry);

    // ——工厂的回显专用入口（N61 变更记录第 1 条）——
    // 与 fromPath 的**唯一**区别：不做严格检查——补不出形态的串（空 filename、裸
    // "file:"）不炸，原样落 UnknownEntity。除此之外分派与判据同一条，且**不代做加工**：
    // 定形与折叠仍是造实体一方的责任（N56 §2.6），入参 = 已过
    // Util::Url::identityFromMpvEcho 整形、且已 StreamTransit::unwrap 折回身份的串。
    // 为什么放宽发生在工厂而不是调用方：调用方是**系统边界**（外部 IPC / 命令行喂进
    // mpv 的串再回显出来），边界上正确行为是降级不是终止进程；而"按串形态选实体类型"
    // 本是工厂的职责，视图层做它才是越权。
    // 为什么仍保留 fromPath 的断言：它守的是**插件自身四个出口**的契约，与外部来路无关
    // ——放宽只走具名的这一条路，不给 bool 参数（调用点上 true / false 不表达任何东西，
    // 将来一处顺手传错即静默失去显影）。
    // 折叠漏喂按既有纪律显影：mpvqt:// 含 "://" 属已定形，本入口不拆包装 → 转接串当
    // 身份落 UnknownEntity，故调用点那一句 unwrap 不得省
    // 不变式：入参已定形时 fromMpvEcho(s, r)->path() == s，与 fromPath 同类型同身份；
    // 不可定形时落 UnknownEntity，其 isLocalFile 为假 ⇒ StaticProvider 恒等下发 ⇒
    // 下一轮回显逐字节相等 ⇒ 静默，不落入"重复收编永续"
    static Ptr fromMpvEcho(const QString &identity,
                           const std::shared_ptr<ServiceRegistry> &registry);

    // ——按值复制（深拷贝，含已解析缓存）——
    // 与 Ptr 共享计数相对：副本与原实体此后互不影响（一方 refresh 不清另一方）。
    // 用途：回刷收编时复用播放表已有实体，避免整表元数据重读（N31）。
    virtual Ptr clone() const = 0;

    // ——身份与显示——
    const QString &path() const { return m_path; } // 身份：喂 mpv / 持久化 / 回刷比对
    // 默认 Util::Url::key(path())（构造期缓存）；远端可重写为服务端 id
    virtual QString key() const;
    // N56 §2.4：显示出口——path() 剥除凭据后的形态（构造期缓存，非虚）。
    // 非虚是刻意的：显示纪律不得被派生类破坏（与 m_key 同款）。取用点收敛在
    // 列表 tooltip 与属性对话框 File Path 两处；右键复制路径 / 打开位置给原样
    // （工具性动作，用户要一份能直接用的地址），书签层完全豁免（原样展示）
    const QString &displayUrl() const { return m_displayUrl; }
    // N56 §2.5：键级判等——实体自答"这个串是不是我"（内部 m_key == key(串)，
    // 与 key() 同一实现不会漂移）。供定位 / 记忆使用，故意宽松：形态漂移
    // （大小写 / 分隔符）仍认得出同一条内容（N30）。回刷判等不走它——那是
    // 形态级逐字节比对（R，N56 §2.5），两套语义不得合一
    bool matches(const QString &path) const;
    // ——N55 形态解析唯一出口——
    // 锁归属提供者 → provider->playUrl(归属 uuid, path())；"无来源 → 所在列表代答"
    // 的分岔整体删除（N55 §2.3）。委托不是缓存：实体不存形态串、不算形态，每次现问
    // 提供者 ⇒ 无陈旧问题、无"枚举即全表铸 URI"的前置成本。锁失败即编程错误
    // （契约见 m_ownerProvider 注），Debug Q_ASSERT 显影；release 不可达分支返回
    // 空串——空串令 mpv 显式加载失败，绝不静默退回 path()（对全转接型提供者，
    // path() 只是条目标识，推它等于静默喂垃圾）。线程契约：仅 GUI 线程调用（理由见
    // m_ownerProvider 注第 3 条）。头文件方向：本头只前置声明提供者，定义放
    // PlaylistEntity.cpp——实体侧唯一 include 提供者头之处（PlaylistProvider.h 已
    // include 本头，无环）。
    QString playUrl() const;
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

    // ——N32 求值转发（public，纯计算不落缓存，可在任意线程调用）——
    // 语义与惰性入口一致（能力位首判 → 静态空回落 / compute*），唯一差别是不写缓存：
    // 预读任务线程经此取回值，由 GUI 续作经注入口写回——实体缓存因此只在 GUI 线程写。
    // N35 修订 2：同步阻塞求值，无取消通道——取消语义为结果静默丢弃（调用方
    // 不等在途项，求值返回后不采用），见规格变更记录
    FileMeta evalFileMeta() const;
    MediaMeta evalMediaMeta() const;

    // ——能力位（取代字符串前缀猜测）——
    virtual bool isLocalFile() const = 0;
    virtual bool exists() const = 0;            // 未知实体恒 true（不可判定）
    virtual bool supportsFileMeta() const = 0;  // 是否具备文件级属性（大小 / 创建 / 修改）
    virtual bool supportsMediaMeta() const = 0; // 是否具备媒体属性

  protected:
    // 拷贝构造仅对子类开放：clone() 经子类的拷贝构造实现，对外仍不可拷贝
    PlaylistEntity(const PlaylistEntity &) = default;
    PlaylistEntity &operator=(const PlaylistEntity &) = delete;

    // 子类修正文件名：网络 / 远端地址无法从 path() 中准确提取文件名时调用。
    // 基类随之重算 suffix()，并在 title() 未被显式设置时同步 title()
    void setFilename(QString fileName);
    // 子类构造后修正显示名（远端取服务端标题）
    void setTitle(QString title) {
        m_title = std::move(title);
        m_titleSet = true;
    }

    // ——N35 服务注册表（基类统一持有，供子类按需取用）——
    // 构造注入：http(s) 实体必传非空（构造内断言）；本地 / 兜底实体沿用缺省空。
    // clone 经拷贝构造携带同一实例；注册表身份恒定，重装配只覆盖其内部实例
    const std::shared_ptr<ServiceRegistry> &registry() const { return m_registry; }

    // ——N29 元数据注入口（写入外部已取得的元数据，不触发求值）——
    // 写缓存 + 视同已求值：随后 fileMeta() / mediaMeta() 返回注入值，
    // cachedFileMeta() / cachedMediaMeta() 非空，预读骨架对该行命中跳过。
    // supports*Meta() 为假时忽略（缓存不落值，与惰性求值的静态空回落同形）。
    // 仅 PlaylistProvider 经友元可及；视图层与具体提供者直接调用编译失败
    void setFileMeta(const FileMeta &meta);
    void setMediaMeta(const MediaMeta &meta);

    // ——N55 归属注入口（显式赋盖，不判空兜底）——
    // 仅 PlaylistProvider / PlaylistWidget 经友元可及（与 setFileMeta / setMediaMeta
    // 同一条授权通道）；构造签名一字不动，归属不走构造参数（N55 §2.2）。每个入列
    // 落点各自决定盖与不盖：收编 clone() 携带来的原归属靠"不再盖"保住，故是显式
    // 赋盖而非"空则补盖"。
    void setOwnership(QUuid listUuid, std::weak_ptr<const PlaylistProvider> provider) {
        m_ownerUuid = std::move(listUuid);
        m_ownerProvider = std::move(provider);
    }

    // N35 修订 2：同步求值（可能阻塞至完成 / 超时，无取消通道，取消为
    // 结果静默丢弃——见变更记录）；仅预读任务与对话框任务线程调用。
    // N39：实体是否具备求值能力由能力位表达，无能力者恒走静态空回落
    virtual FileMeta computeFileMeta() const = 0;
    virtual MediaMeta computeMediaMeta() const = 0;
    virtual QIcon computeIcon() const = 0;

  private:
    // 构造期派生 title / fileName / suffix（纯字符串运算，与身份串同生命周期）
    void deriveNames();

    QString m_path;
    std::shared_ptr<ServiceRegistry> m_registry; // N35：服务注册表（基类统一持有）
    // ——N55 归属两件套（出生属性）——
    // 在列表中显示的每个实体都携带归属：列表身份 + 该列表当前提供者的弱引用。
    // 载体是 weak_ptr 而非裸指针 / shared_ptr（N55 §2.1）：保留非所有权语义，
    // 提供者寿命不因实体外溢，锁失败可检测可断言。
    // 1. 归属是出生属性：实体入列（进入层缓存或行）时由视图侧一次显式赋盖，此后
    //    不随层重建 / 排序 / 逐出改变；拷贝构造默认携带 ⇒ clone() 自动带走。
    //    归属不进 key()、不参与任何判等、不进 removeInvalid 判据、列表侧不落 INI。
    // 2. 枚举产物入列前无归属：异步任务只产未盖章实体表，归属由 GUI 线程在入列时盖；
    //    未盖章（两成员皆空）时消费形态属调用方违约，走 playUrl() 的断言路径显影。
    // 3. playUrl() 只许 GUI 线程调用：提供者侧端点缓存与 publish 无锁，依赖
    //    "只在 GUI 线程发生"的既有线程口径（PlaylistProvider::playUrl 的断言同款）。
    // 4. playUrl() 消费者封闭于视图侧四处：Playlist::playUrls 整表映射、
    //    PlaylistWidget::setPlaylist 第 4 步比对与第 5 步 reuse 索引键、
    //    MainWindow::onAddBookmark——异步对话框持有的实体副本只读元数据缓存，
    //    不消费形态。"提供者在位"由消费点封闭与来源失效清扫共同保证（N55 §2.4），
    //    lock() 失败即契约破坏。
    QUuid m_ownerUuid;                                     // 归属列表身份；未盖章为 null
    std::weak_ptr<const PlaylistProvider> m_ownerProvider; // 归属提供者弱引用
    QString m_title;                                       // 构造期派生
    QString m_fileName;                                    // setFilename 可改，改则同步 suffix
    QString m_suffix;                                      // 同上
    bool m_titleSet = false; // setTitle 显式设置后置位，此后 setFilename 不再联动 title
    QString m_key;           // 构造期缓存：Util::Url::key(path())——key()
                             // 在委托绘制热路径上，避免逐行逐帧重算
    QString m_displayUrl;    // N56：构造期缓存 stripCredentials(path())——显示出口，
                             // 与 m_key 同款（构造期定死、不可被子类改动）
    mutable std::optional<FileMeta> m_fileMeta;
    mutable std::optional<MediaMeta> m_mediaMeta;
    mutable std::optional<QIcon> m_icon;
};

// 兜底实体（N26）：工厂对网络 / 流路径产出。插件不做任何额外加工，
// path() 原样喂 mpv / 持久化 / 回显比对。完整远端实体（服务端标题、延迟解析、
// 远端元数据）归后续篇；未知实体是其最小替身，保证网络项有实体可落
// （回刷收编的 mpv 列表即现实引用者）。
class UnknownEntity : public PlaylistEntity {
  public:
    // registry 必填（经基类持有，兜底实体不用；无缺省，调用点显式传）
    explicit UnknownEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<UnknownEntity>(*this); }

    bool isLocalFile() const override { return false; }
    bool exists() const override { return true; } // 不可判定，不参与失效清理
    bool supportsFileMeta() const override { return false; }
    bool supportsMediaMeta() const override { return false; }

  protected:
    FileMeta computeFileMeta() const override {
        return {}; // 恒不被调（supportsFileMeta 为假）
    }
    MediaMeta computeMediaMeta() const override {
        return {}; // 恒不被调（supportsMediaMeta 为假）
    }
    QIcon computeIcon() const override;
};

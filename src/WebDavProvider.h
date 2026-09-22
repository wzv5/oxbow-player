#pragma once

#include "pch.h"

#include "HttpClient.h"
#include "PlaylistProvider.h"

// N44：身份常量（类型键 + 显示名源文案），登记表与 writeProviderKey 共用同一点定义
inline constexpr char kWebDavProviderKey[] = "webdav";
inline constexpr char kWebDavProviderName[] =
    QT_TRANSLATE_NOOP("WebDavProvider", "WebDAV Directory");

class ProviderSettingsPage;

// WebDAV 目录提供者（N37）：tab 绑定一个 WebDAV 目录 URL，PROPFIND（Depth: 1）
// 枚举其中媒体条目，条目以 http(s) URL 直接喂 mpv 串流播放。
// N46：浏览模式三选一——分层浏览（单层 PROPFIND，子目录产容器，容器 id 取该
// collection 条目的无凭据完整编码 URL，本层请求基 URL 即 id 本身）/ 仅文件
// （平铺，现状 recursive = false 的逐字保留）/ 仅文件（含子目录）（递归 DFS
// 拉平，现状 recursive = true 的逐字保留）。提供者返回前按文件名数字化排好
// 初始序（N45 §2.7）。
// 枚举、刷新、排序、续播记忆全部复用动态表既有机制——能力位与本地目录表同形
// （不可增删、可排序），排序规则随提供者持久化。
//
// 全局运行时参数一律经注册表快照取用（N36 修订 1 的路子）：enumerate 起始处
// resolve<TraversalPolicy>() 取深度上限、resolve<HttpClient>() 取网络 client；
// 不持 client、不读 INI——HttpClient 由装配顶点（MainWindow）的 provideHttpServices()
// 按 INI 装配，身份恒定、重装配即覆盖，超时与证书改动下一次枚举即生效。
//
// 路径纪律（承 B15 / N37 §3.1，N48 修订）：条目 URL 的构造点是本提供者的枚举体
// （解析纯函数）——以 rootUrl 为基 QUrl::resolved() 展开 href、**剥除 userinfo**，
// 再经 Util::Url::toNative 收口取编码形态输出；此后身份串全链路只读转发，任何环节
// 不得再改（匹配经 Util::Url::key 落键，setPlaylist 收编判定原串）。
// 凭据只在两个使用点注入：PROPFIND 请求 URL（enumerate 内逐请求注入）与喂 mpv
// 播放（directPlayUrl，经基类 playUrl 模板方法在直发形态下调入；开启转接时
// 凭据经 WebDavStreamEndpoint 在 open 期注入，播放 URL 不携带）——二者都不进
// 实体、不落盘。
// N52：转接能力取 Optional——直链与转接都合法，列表级偏好决定下发形态。
class WebDavProvider : public PlaylistProvider {
  public:
    // N46 §2.1：浏览模式（三选一）。枚举值序即参数页下拉框的选项序，亦即 INI 的
    // 持久化取值（int 不透明存储）。与 DirProvider::BrowseMode 取值同形但各自
    // 持有、互不共用（§2.1 不抽共用代码）
    enum class BrowseMode {
        Layered,        // 分层浏览：单层 PROPFIND，子目录 → 容器（新建列表默认）
        FilesOnly,      // 仅文件：只列举根内文件，平铺
        FilesRecursive, // 仅文件（含子目录）：递归 DFS 拉平
    };

    // rootUrl 为参数页来源处定形的 URL（trim → Util::Url::toNative → 补尾斜杠，见
    // normalizeRootUrl）；user / password 可空，皆空则请求与播放都不注入凭据。
    // registry 必填（经基类持有）
    WebDavProvider(const QString &rootUrl, const QString &user, const QString &password,
                   BrowseMode mode, const QStringList &extensions,
                   const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    // 显式 true：内容以枚举为准不可增删，但可排序并持久化规则（与目录表同形）
    bool supportsSort() const override;
    // N52：直链与转接都合法（首个 Optional 来源）；端点在 createStreamEndpoint 现造
    TransitCapability transitCapability() const override;
    std::shared_ptr<StreamEndpoint> createStreamEndpoint() const override;

    // 当前语言下的显示名（无译文时即英文源）；登记表以函数指针引用（N44）
    static QString displayName();

    // ——以下三个静态纯函数（N44）：参数设置页的取值 / 校验与单元测试共用同一出口——
    // URL 定形（原列表设置对话框的 davUrl 整体迁入）：trim → Util::Url::toNative →
    // 尾斜杠补在 **path** 上（在 ? / # 之前插入），query / fragment 原样保留在后。
    // 尾斜杠是 WebDAV 的基目录语义（resolved 展开相对 href 的前提），属本来源
    // 自身的规则，不进通用函数。**幂等是硬要求**——已定形的 URL 再定形结果必须
    // 不变，否则回显未改动也会被 hasChanges 误判为有改动。
    // 枚举侧"给 collection 补尾斜杠"同用此函数（N44 §2.5.3：两支对已规范化的
    // URL 输出逐字相同，故合并后枚举行为不变）
    static QString normalizeRootUrl(const QString &text);
    // 参数校验：入参是控件**原始文本**（内部先 normalizeRootUrl）。返回可直接展示
    // 的错误文案；通过返回 nullopt。不做连通性探测（N37 口径不变）
    static std::optional<QString> validateInput(const QString &rawUrl);
    // 扩展名编辑区文本 → 规范清单：按空白拆分、去前导点、转小写、去重保序；
    // 空文本 ⇒ 空表（不过滤）
    static QStringList parseExtensions(const QString &text);

    // ——以下两个静态纯函数（N46）：浏览模式的取值与容器 id 的构造——
    // 单元测试与 enumerate / INI 读取共用同一出口（N44 先例）
    // INI 的 int 取值 → 浏览模式：取值域外（损坏 / 未知值）降级 Layered
    // （新建列表默认，§2.1），不做旧键兼容。与 DirProvider 同名函数各自持有
    static BrowseMode browseModeFromInt(int value);
    // collection 条目的身份串 → 容器 id：剥除 userinfo 的无凭据形态，编码形态与
    // 尾斜杠原样保留。N48 起条目身份已恒无 userinfo（resolveEntryUrl 恒剥），
    // 故对现产出即恒等变换；剥离动作保留（id 是持久化进 queueRoot 与请求基 URL 的
    // 身份，形态恒定是硬要求）。id 恒无凭据：持久化进 queueRoot 的 INI 不携带凭据，
    // 请求侧由 enumerate 逐请求重写当前凭据
    static QString containerId(const QString &entryPath);

    QString rootUrl() const { return m_rootUrl; }
    QString user() const { return m_user; }
    QString password() const { return m_password; }
    BrowseMode browseMode() const { return m_browseMode; }
    QStringList extensions() const { return m_extensions; }

  protected:
    // N48：喂 mpv 的播放形态——在 URL 的 authority 起点插入 `编码后user:编码后pass@`
    // （凭据皆空恒等返回）。条目恒无 userinfo（resolveEntryUrl 恒剥），故这是凭据进入
    // 播放链的唯一入口；产出只喂 mpv，不进实体、不落盘、不参与比对。
    // N52：降为 protected（访问段与签名位置移动，函数体一行不动）——公开出口只有
    // 基类非虚的 playUrl，usesTransit() 为假才会调到这里
    QString directPlayUrl(const QString &path) const override;

  public:
    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

    // ——multistatus 解析纯函数（N37 §3.2，本篇唯一"自己写"的部分）——
    // 单个条目的解析产出：path 为定形后的完整 URL（resolved → 剥 userinfo →
    // Util::Url::toNative，构造确定性——同一 href 两次解析得同一串；N48 起恒无凭据）
    struct DavEntry {
        QString path;
        std::optional<qint64> size;        // getcontentlength（缺失 = 值未知）
        std::optional<QDateTime> modified; // getlastmodified（缺失 / 解析失败 = 值未知）
    };
    // 一次 PROPFIND 响应的解析产出：文件条目与子目录条目分列（DFS 队列只收后者）
    struct DavListing {
        QList<DavEntry> files;
        QList<DavEntry> collections;
        // N48：非 http(s) href 的跳过告警（每次解析至多一条、合并条数）。条目恒为
        // http(s) 是本提供者的不变式（凭据的注入与剥离都只对 http(s) 生效），故
        // 跨协议 href 不产出条目，向用户回传可读说明
        QStringList warnings;
    };
    // 入参：XML 体、基 URL（请求 URL，已含当前凭据与尾斜杠——基目录语义与相对 href
    // 展开的前提）。N48：不再收凭据形参——条目恒剥 userinfo，凭据只用于请求与播放。
    // 解析失败（非法 XML）返回 unexpected；目录自条目（与基 URL 等价，尾斜杠不计，
    // 比较在无凭据形态下进行）在此跳过；非 http(s) 的 href 跳过并记 warnings。
    // 只解释 RFC 4918 的一个子集：response 下的 href / resourcetype（是否含
    // collection）/ getcontentlength / getlastmodified（属性经 propstat / prop
    // 包裹层下探取得），元素按命名空间 + local name 匹配（DAV: 前缀不可依赖），
    // 未识别元素一律忽略。XML 解析交 QXmlStreamReader（QtCore 内建，无新增链接依赖）
    static std::expected<DavListing, QString> parseMultistatus(const QByteArray &body,
                                                               const QUrl &baseUrl);

  private:
    // 条目 URL 构造（解析与枚举共用）：href 相对基解析（绝对 href 直接采用）→ 恒剥
    // 解析结果的 userinfo（相对 href 的 userinfo 继承自基 URL、绝对 href 的来自 href
    // 自身，两者一并剥）→ Util::Url::toNative 收口取编码形态。§3.1 第 1 条的"URL 在入口处
    // 构造一次"，N48 起产出恒无凭据
    static QString resolveEntryUrl(const QUrl &baseUrl, const QString &href);

    // HTTP 状态码与网络失败 → 可读文案（英文源 + tr()，不透传 HttpClient 的 error）
    static QString davRequestError(const HttpClient::Response &resp, const QUrl &url);

    // ——提供者初始排序（N45 §2.7，N46 §2.2）——文件名数字化（Util::segmentName +
    // naturalCompare），稳定排序保等键的服务器枚举序；实体按 fileName、容器按
    // title。与视图 SortKey::None 显示序逐项一致由规格与验收约束，不共用代码
    // （DirProvider 的同名私有辅助各自持有）
    static void sortEntitiesByNumericName(EntityList &entities);
    static void sortContainersByNumericTitle(QList<Container> &containers);

    QString m_rootUrl;                             // 基目录 URL（含尾斜杠，入口处定形）
    QString m_user;                                // 可空
    QString m_password;                            // 可空；INI 中明文，与既有配置同款
    BrowseMode m_browseMode = BrowseMode::Layered; // 浏览模式（新建默认分层）
    QStringList m_extensions;                      // 无点小写扩展名；空表表示不过滤
};

// 参数页工厂（N44）：页面类定义在 WebDavProvider.cpp，本处只声明工厂。
// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createWebDavSettingsPage(const PlaylistProvider *seed,
                                               const std::shared_ptr<ServiceRegistry> &registry,
                                               QWidget *parent);

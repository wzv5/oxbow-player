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
// 逐层 DFS 枚举其中媒体条目，条目以 http(s) URL 直接喂 mpv 串流播放。
// 枚举、刷新、排序、续播记忆全部复用动态表既有机制——能力位与本地目录表同形
// （不可增删、可排序），排序规则随提供者持久化。
//
// 全局运行时参数一律经注册表快照取用（N36 修订 1 的路子）：enumerate 起始处
// resolve<TraversalPolicy>() 取深度上限、resolve<HttpClient>() 取网络 client；
// 不持 client、不读 INI——HttpClient 由 N35 的 provideHttpServices() 按 INI 装配，
// 身份恒定、重装配即覆盖，超时与证书改动下一次枚举即生效。
//
// 路径纪律（承 B15 / N37 §3.1）：条目 URL 的构造点是本提供者的枚举体（解析纯函数）——
// 以 rootUrl 为基 QUrl::resolved() 展开 href、setUserName / setPassword 写入凭据，
// 再经 Util::nativePath 收口取编码形态输出；此后 playUrl 全链路只读转发，任何环节
// 不得再改（匹配经 normalizePathForMatch 落键，setPlaylist 收编判定原串）。
class WebDavProvider : public PlaylistProvider {
  public:
    // rootUrl 为参数页来源处定形的 URL（trim → nativePath → 补尾斜杠，见
    // normalizeRootUrl）；user / password 可空，皆空则凭据不随 URL 走。
    // registry 必填（经基类持有）
    WebDavProvider(const QString &rootUrl, const QString &user, const QString &password,
                   bool recursive, const QStringList &extensions,
                   const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    // 显式 true：内容以枚举为准不可增删，但可排序并持久化规则（与目录表同形）
    bool supportsSort() const override;

    // 当前语言下的显示名（无译文时即英文源）；登记表以函数指针引用（N44）
    static QString displayName();

    // ——以下三个静态纯函数（N44）：参数设置页的取值 / 校验与单元测试共用同一出口——
    // URL 定形（原列表设置对话框的 davUrl 整体迁入）：trim → Util::nativePath →
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

    QString rootUrl() const { return m_rootUrl; }
    QString user() const { return m_user; }
    QString password() const { return m_password; }
    bool recursive() const { return m_recursive; }
    QStringList extensions() const { return m_extensions; }

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

    // ——multistatus 解析纯函数（N37 §3.2，本篇唯一"自己写"的部分）——
    // 单个条目的解析产出：playUrl 为定形后的完整 URL（resolved + 凭据 +
    // Util::nativePath，构造确定性——同一 href 两次解析得同一串）
    struct DavEntry {
        QString playUrl;
        std::optional<qint64> size;        // getcontentlength（缺失 = 值未知）
        std::optional<QDateTime> modified; // getlastmodified（缺失 / 解析失败 = 值未知）
    };
    // 一次 PROPFIND 响应的解析产出：文件条目与子目录条目分列（DFS 队列只收后者）
    struct DavListing {
        QList<DavEntry> files;
        QList<DavEntry> collections;
    };
    // 入参：XML 体、基 URL（请求 URL，已含凭据与尾斜杠——基目录语义的前提）、
    // 原始凭据（绝对 href 不继承基 userinfo，须重写）。解析失败（非法 XML）返回
    // unexpected；目录自条目（与基 URL 等价，尾斜杠不计）在此跳过。
    // 只解释 RFC 4918 的一个子集：response 下的 href / resourcetype（是否含
    // collection）/ getcontentlength / getlastmodified（属性经 propstat / prop
    // 包裹层下探取得），元素按命名空间 + local name 匹配（DAV: 前缀不可依赖），
    // 未识别元素一律忽略。XML 解析交 QXmlStreamReader（QtCore 内建，无新增链接依赖）
    static std::expected<DavListing, QString> parseMultistatus(const QByteArray &body,
                                                               const QUrl &baseUrl,
                                                               const QString &user,
                                                               const QString &password);

  private:
    // 条目 URL 构造（解析与枚举共用）：href 相对基解析（绝对 href 直接采用）→
    // 凭据无条件重写（皆空即剥除 href 自带的 userinfo）→ Util::nativePath 收口
    // 取编码形态。§3.1 第 1 条的"URL 在入口处构造一次"
    static QString resolveEntryUrl(const QUrl &baseUrl, const QString &href, const QString &user,
                                   const QString &password);

    // HTTP 状态码与网络失败 → 可读文案（英文源 + tr()，不透传 HttpClient 的 error）
    static QString davRequestError(const HttpClient::Response &resp, const QUrl &url);

    QString m_rootUrl;        // 基目录 URL（含尾斜杠，入口处定形）
    QString m_user;           // 可空
    QString m_password;       // 可空；INI 中明文，与既有配置同款
    bool m_recursive = false; // 是否含子目录（逐层 DFS 平铺）
    QStringList m_extensions; // 无点小写扩展名；空表表示不过滤
};

// 参数页工厂（N44）：页面类定义在 WebDavProvider.cpp，本处只声明工厂。
// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createWebDavSettingsPage(const PlaylistProvider *seed,
                                               const std::shared_ptr<ServiceRegistry> &registry,
                                               QWidget *parent);

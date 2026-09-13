#pragma once

#include "pch.h"

#include "HttpClient.h"
#include "PlaylistProvider.h"

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
    // rootUrl 为列表设置对话框来源处定形的 URL（trim → nativePath → 补尾斜杠）；
    // user / password 可空，皆空则凭据不随 URL 走。registry 必填（经基类持有）
    WebDavProvider(const QString &rootUrl, const QString &user, const QString &password,
                   bool recursive, const QStringList &extensions,
                   const std::shared_ptr<ServiceRegistry> &registry);

    Type type() const override;
    QString describe() const override;
    std::expected<Enumeration, QString> enumerate() const override;
    bool supportsModify() const override;
    // 显式 true：内容以枚举为准不可增删，但可排序并持久化规则（与目录表同形）
    bool supportsSort() const override;

    QString rootUrl() const { return m_rootUrl; }
    QString user() const { return m_user; }
    QString password() const { return m_password; }
    bool recursive() const { return m_recursive; }
    QStringList extensions() const { return m_extensions; }

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<WebDavProvider> load(QSettings &settings,
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

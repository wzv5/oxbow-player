#pragma once

#include "pch.h"

#include "HttpClient.h"
#include "PlaylistProvider.h"

// 类型键与显示名源文案（显示名经 translate 取值）
inline constexpr char kWebDavProviderKey[] = "webdav";
inline constexpr char kWebDavProviderName[] = QT_TRANSLATE_NOOP("WebDavProvider", "WebDAV");

class ProviderSettingsPage;

// WebDAV 目录提供者：tab 绑定一个 WebDAV 目录 URL，PROPFIND（Depth: 1）枚举
// 媒体条目，条目以 http(s) URL 喂 mpv 串流播放。三种浏览形态：分层（子目录产
// 容器，容器 id 取无凭据完整编码 URL）/ 仅文件 / 仅文件含子目录。
// 运行时参数经注册表快照取用：enumerate 起始处 resolve<TraversalPolicy>() 与
// resolve<HttpClient>()，不持 client、不读 INI。
// 路径纪律：条目 URL 只在枚举体构造一次（resolved() → 剥 userinfo → toNative），
// 此后全链路只读转发。凭据只出现在 PROPFIND 与喂 mpv 两点（转接时由端点在 open
// 期注入），不进实体、不落盘。转接能力 Optional。
class WebDavProvider : public PlaylistProvider {
  public:
    // 枚举值序即参数页下拉框选项序，亦即 INI 持久化取值
    enum class BrowseMode {
        Layered,        // 单层 PROPFIND，子目录 → 容器（新建列表默认）
        FilesOnly,      // 只列举根内文件，平铺
        FilesRecursive, // 递归 DFS 拉平
    };

    // rootUrl 为参数页定形后的 URL；user / password 可空，皆空则不注入凭据
    WebDavProvider(const QString &rootUrl, const QString &user, const QString &password,
                   BrowseMode mode, const QStringList &extensions,
                   const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    bool supportsSort() const override;
    SortRule defaultSortRule() const override { return {SortKey::Modified, true}; }
    TransitCapability transitCapability() const override;
    std::shared_ptr<StreamEndpoint> createStreamEndpoint() const override;

    static QString displayName();

    // trim → toNative → 尾斜杠补在 path 上（? / # 之前），query / fragment 原样保留。
    // 幂等是硬要求，否则回显未改动会被 hasChanges 误判
    static QString normalizeRootUrl(const QString &text);
    // 不做连通性探测
    static std::optional<QString> validateInput(const QString &rawUrl);

    static BrowseMode browseModeFromInt(int value);
    // 剥除 userinfo 的无凭据形态，编码形态与尾斜杠原样保留。id 要持久化并作请求
    // 基 URL，形态恒定是硬要求
    static QString containerId(const QString &entryPath);

    QString rootUrl() const { return m_rootUrl; }
    QString user() const { return m_user; }
    QString password() const { return m_password; }
    BrowseMode browseMode() const { return m_browseMode; }
    QStringList extensions() const { return m_extensions; }

  protected:
    // 在 URL 的 authority 起点插入 `编码后user:编码后pass@`，凭据皆空恒等返回。
    // 这是凭据进入播放链的唯一入口；产出只喂 mpv，不进实体、不落盘、不参与比对
    QString directPlayUrl(const QString &path) const override;

  public:
    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

    // 单个条目：path 为定形后的完整 URL（同一 href 两次解析得同一串）
    struct DavEntry {
        QString path;
        std::optional<qint64> size;        // 缺失 = 值未知
        std::optional<QDateTime> modified; // 缺失 / 解析失败 = 值未知
    };
    // 文件与子目录分列（DFS 队列只收后者）。warnings 为非 http(s) href 的跳过告警，
    // 合并计数
    struct DavListing {
        QList<DavEntry> files;
        QList<DavEntry> collections;
        QStringList warnings;
    };
    // baseUrl 已含当前凭据与尾斜杠。非法 XML 返回 unexpected；目录自条目（与基 URL
    // 等价，尾斜杠不计）跳过。只解释 RFC 4918 的一个子集：href / resourcetype /
    // getcontentlength / getlastmodified，元素按命名空间 + local name 匹配
    static std::expected<DavListing, QString> parseMultistatus(const QByteArray &body,
                                                               const QUrl &baseUrl);

  private:
    // href 相对基解析（绝对 href 直接采用），随后恒剥 userinfo，再 toNative 收口
    static QString resolveEntryUrl(const QUrl &baseUrl, const QString &href);

    // 不透传 HttpClient 的 error
    static QString davRequestError(const HttpClient::Response &resp, const QUrl &url);

    static void sortEntitiesByNumericName(EntityList &entities);
    static void sortContainersByNumericTitle(QList<Container> &containers);

    QString m_rootUrl; // 含尾斜杠
    QString m_user;
    QString m_password; // INI 中明文，与既有配置同款
    BrowseMode m_browseMode = BrowseMode::Layered;
    QStringList m_extensions; // 无点小写扩展名；空表表示不过滤
};

// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createWebDavSettingsPage(const PlaylistProvider *seed,
                                               const std::shared_ptr<ServiceRegistry> &registry,
                                               QWidget *parent);

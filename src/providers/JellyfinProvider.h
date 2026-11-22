#pragma once

#include "pch.h"

#include "HttpClient.h"
#include "PlaylistProvider.h"

class ProviderSettingsPage;

// 类型键与显示名源文案（显示名经 translate 取值；列表类型文案即 "Jellyfin"）
inline constexpr char kJellyfinProviderKey[] = "jellyfin";
inline constexpr char kJellyfinProviderName[] = QT_TRANSLATE_NOOP("JellyfinProvider", "Jellyfin");

// Jellyfin 服务器提供者：一个实例绑定一个服务器连接，浏览根为 UserViews 全部媒体库，
// 库内经 /Items?parentId= 逐层钻取，一层恰一次请求、整层全量返回。
// 容器 / 叶子判据是 IsFolder：文件夹一律产容器，非文件夹一律产叶子——可播与否
// 交给用户可编辑的扩展名清单。
// 全局传输经注册表现查 HttpClient；会话参数（serverUrl、serverId、token、userId）
// 为构造注入的不可变快照，重新授权 = 参数页 build 新实例。
// 条目身份是 URN 形态 jellyfin://<serverId>/<type>/<itemId>——两个持久标识 + 小写
// 类型段；取流 URL 从身份一步现拼，身份串铸造后全链路只读转发。token 只进 HTTP
// 请求头，不进实体、不落播放串。
// 转接能力 Required：mpv 侧串不得携带凭据，直推裸 URL 也折不回 URN 身份，故转接
// 是唯一传输形态，直发支永不可达。
class JellyfinProvider : public PlaylistProvider {
  public:
    // serverUrl 为定形后的基 URL；token 空 = 未登录（枚举按未登录失败处理）；
    // extensions 为空表 = 不过滤
    JellyfinProvider(const QString &serverUrl, const QString &serverId, const QString &token,
                     const QString &userId, const QStringList &extensions,
                     const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    bool supportsSort() const override;

    TransitCapability transitCapability() const override;
    std::shared_ptr<StreamEndpoint> createStreamEndpoint() const override;

    static QString displayName();

    // trim → 缺 scheme 补 http://（Jellyfin 局域网常态）→ 去尾斜杠。幂等是硬要求。
    // 保留 path 段（反向代理子路径），拼接 API 路径时原样作前缀
    static QString normalizeServerUrl(const QString &text);
    // 不做连通性探测
    static std::optional<QString> validateInput(const QString &rawUrl);
    static QStringList parseExtensions(const QString &text);

    QString serverUrl() const { return m_serverUrl; }
    QString serverId() const { return m_serverId; }
    QString token() const { return m_token; }
    QString userId() const { return m_userId; }
    QStringList extensions() const { return m_extensions; }

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

  private:
    // 基 URL + API 路径拼请求 URL（path 以 '/' 起始），统一注入 Authorization
    // （token 非空才携带）与 Accept CamelCase
    HttpClient::Request makeApiRequest(const QString &path) const;

    QString m_serverUrl; // 无尾斜杠；可含反向代理子路径
    QString m_serverId;  // 身份 URN 组成部分
    QString m_token;     // 空 = 未登录
    QString m_userId;
    QStringList m_extensions; // 无点小写扩展名；空表表示不过滤
};

// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createJellyfinSettingsPage(const PlaylistProvider *seed,
                                                 const std::shared_ptr<ServiceRegistry> &registry,
                                                 QWidget *parent);

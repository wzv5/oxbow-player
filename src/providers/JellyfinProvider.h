#pragma once

#include "pch.h"

#include "HttpClient.h"
#include "PlaylistProvider.h"

class ProviderSettingsPage;

// N69：身份常量（类型键 + 显示名源文案），登记表与 writeProviderKey 共用同一点定义。
// 显示名即列表设置对话框单选项的"列表类型"文案（维护者指示：Jellyfin）
inline constexpr char kJellyfinProviderKey[] = "jellyfin";
inline constexpr char kJellyfinProviderName[] = QT_TRANSLATE_NOOP("JellyfinProvider", "Jellyfin");

// Jellyfin 服务器提供者（N69）：一个实例绑定一个服务器连接，浏览根为 UserViews
// 全部媒体库，库内经 /Items?parentId= 逐层钻取，一层恰一次请求、整层全量返回
// （枚举无分页，实测口径）。容器 / 叶子判据是 IsFolder（变更记录 4）：文件夹一律
// 产容器，非文件夹一律产叶子——"可播与否"不再由类型白名单裁决，交给用户可编辑的
// 扩展名清单（变更记录 5，与本地目录表同款语义）。枚举、刷新、排序、续播记忆全部
// 复用动态表既有机制——能力位与 WebDAV 表同形（不可增删、可排序），排序规则随
// 提供者持久化。
//
// 全局传输经注册表现查 HttpClient（N36 修订 1 路子，WebDAV 先例）；会话参数
// （服务器 URL、serverId、token、userId）为构造注入的不可变快照，重新授权 =
// 参数页 build 新实例替换。请求头由提供者内请求装配统一注入（Authorization +
// Accept CamelCase，N69 定案：不引入 HttpClient 会话级注入）。
//
// 身份纪律：条目身份是 URN 形态 jellyfin://<serverId>/<type>/<itemId>——serverId
// 与 itemId 两个持久标识 + 条目类型段（小写；类型随身份携带，取流 URL 从身份
// 一步现拼，变更记录 7），与服务器地址、凭据、取流形态完全解耦；身份串在本类
// 铸造后全链路只读转发，任何环节不得再改。凭据（token）只进 HTTP 请求头，
// 不进实体、不落播放串。
//
// N52：转接能力 Required——取流 URL 由条目 id 与 token 组成且 mpv 侧串恒不得
// 携带凭据，直推裸 URL 在回显整形管线下无法折回 URN 身份（N69 设计），故转接
// 是唯一传输形态，直发支永不可达（基类恒等 directPlayUrl 即"该分支永不走"）。
class JellyfinProvider : public PlaylistProvider {
  public:
    // serverUrl 为参数页来源处定形的基 URL（trim → 补 scheme → 去尾斜杠，见
    // normalizeServerUrl）；token / userId / serverId 由快速连接流程取得，
    // token 为空即未登录（枚举按未登录失败处理，UI 得明确文案）；extensions
    // 为无点小写扩展名清单（空表 = 不过滤，与本地目录表同语义）
    JellyfinProvider(const QString &serverUrl, const QString &serverId, const QString &token,
                     const QString &userId, const QStringList &extensions,
                     const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    // 显式 true：内容以枚举为准不可增删，但可排序并持久化规则（与 WebDAV 表同形）。
    // 默认规则不设（基类 None = 保持服务器枚举序）
    bool supportsSort() const override;

    // N52：转接是唯一传输形态（Required）——静态取流 URL 由条目 id 与 token
    // 构成，token 恒在 Authorization 头，mpv 侧串不得携带；且裸 URL 直推在回显
    // 整形管线下折不回 URN 身份（N69 设计）。偏好不可关，直发支永不可达——
    // 基类恒等 directPlayUrl 即"该分支永不走"，本类不覆写
    TransitCapability transitCapability() const override;
    std::shared_ptr<StreamEndpoint> createStreamEndpoint() const override;

    // 当前语言下的显示名（无译文时即英文源）；登记表以函数指针引用（N44）
    static QString displayName();

    // ——静态纯函数：参数设置页的取值 / 校验与调用点共用同一出口（N44 先例）——
    // 服务器地址定形：trim → 缺 scheme 补 http://（Jellyfin 局域网常态）→ 去尾
    // 斜杠。**幂等是硬要求**（回显未改动不得被 hasChanges 误判）。
    // 保留 path 段（反向代理子路径场景），拼接 API 路径时原样作前缀
    static QString normalizeServerUrl(const QString &text);
    // 参数校验：入参是控件**原始文本**（内部先 normalizeServerUrl）。返回可直接
    // 展示的错误文案；通过返回 nullopt。不做连通性探测（N37 口径）
    static std::optional<QString> validateInput(const QString &rawUrl);
    // 扩展名编辑区文本 → 规范清单：按空白拆分、去前导点、转小写、去重保序；
    // 空文本 ⇒ 空表（不过滤）。与 DirProvider / WebDavProvider 的同名函数同口径
    // （各提供者各自持有，N44 先例）
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
    // API 请求装配（唯一入口）：基 URL + API 路径拼请求 URL，统一注入
    // Authorization（MediaBrowser 形态，token 非空才携带）与 Accept CamelCase
    // 两头。path 以 '/' 起始（如 "/UserViews"）
    HttpClient::Request makeApiRequest(const QString &path) const;

    QString m_serverUrl;      // 基 URL（入口处定形，无尾斜杠；可含反向代理子路径）
    QString m_serverId;       // /System/Info/Public 的 Id（持久标识，身份 URN 组成部分）
    QString m_token;          // AccessToken；空 = 未登录
    QString m_userId;         // 授权用户 id
    QStringList m_extensions; // 无点小写扩展名；空表表示不过滤
};

// 参数页工厂（N69）：页面类定义在 JellyfinProvider.cpp，本处只声明工厂。
// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createJellyfinSettingsPage(const PlaylistProvider *seed,
                                                 const std::shared_ptr<ServiceRegistry> &registry,
                                                 QWidget *parent);

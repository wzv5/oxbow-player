#pragma once

#include "pch.h"

#include "PlaylistProvider.h"
#include "rclonecrypt/RcloneCrypt.h"

// 类型键与显示名源文案（显示名经 translate 取值）
inline constexpr char kRcloneCryptProviderKey[] = "rclonecrypt";
inline constexpr char kRcloneCryptProviderName[] =
    QT_TRANSLATE_NOOP("RcloneCryptProvider", "Rclone Crypt");

class ProviderSettingsPage;

// rclone crypt 只读提供者：绑定一个 rclone crypt 加密的本地目录，枚举期逐名解密
// （列表显示明文名），播放经 oxbow:// 转接由 RcloneCryptByteStream 边读边解密——
// mpv 不吃 crypt 协议，转接能力恒为 Required（直发分支不可达，directPlayUrl 不覆写）。
// 身份为 URN：rclonecrypt://<percent-encoded 相对路径>，根目录归实例持有、不进身份串
// ——换根目录 / 整仓搬迁不影响已存身份与续播记忆；密文路径一律现算，不持久化。
// 密钥派生（scrypt，实测 100–200ms）不得在 GUI 线程：落点在首次 enumerate（工作
// 线程）内惰性派生一次以不可变 shared_ptr 共享，createStreamEndpoint（GUI 线程）只读
class RcloneCryptProvider : public PlaylistProvider {
  public:
    // 枚举值序即参数页下拉框选项序与 INI 持久化取值
    enum class BrowseMode { Layered, FilesOnly, FilesRecursive };

    RcloneCryptProvider(const QString &root, const QString &password, const QString &salt,
                        RcloneCrypt::NameMode nameMode, RcloneCrypt::NameEncoding encoding,
                        bool dirNameEncrypt, const QString &suffix, bool strictNames,
                        bool passBadBlocks, BrowseMode browseMode, const QStringList &extensions,
                        const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    // 显式 true：内容不可增删，但可排序并持久化规则
    bool supportsSort() const override;
    SortRule defaultSortRule() const override { return {SortKey::Modified, true}; }
    TransitCapability transitCapability() const override;
    std::shared_ptr<StreamEndpoint> createStreamEndpoint() const override;

    static QString displayName();

    // 根目录定形：trim → 空即空串 → Util::Url::toIdentity（相对转绝对、转原生
    // 分隔符）；幂等是硬要求
    static QString normalizeRoot(const QString &text);
    // 入参是控件原始文本；返回可直接展示的错误文案，通过返回 nullopt
    static std::optional<QString> validateInput(const QString &rawRoot);
    // 三个 int 不透明存储的取值换算（域外降级默认值）
    static RcloneCrypt::NameMode nameModeFromInt(int value);
    static RcloneCrypt::NameEncoding nameEncodingFromInt(int value);
    static BrowseMode browseModeFromInt(int value);
    // 与 nameEncodingFromInt 分工不同：下拉框恒只有 base32 / base64 两项、取值必在
    // 域内；INI 是用户能手改的数据，域外取值的现实来源是把 rclone 的 base32768（值
    // 2）塞进来——静默降成 base32 会解出一整层错名，故不降级，由调用方按配置数据
    // 异常显影
    static std::optional<RcloneCrypt::NameEncoding> nameEncodingFromIni(int value);
    // 明文相对路径 ↔ 段内 percent-encoded 形态：'/' 原样保留、段内 unreserved 之外
    // 全编码（单次编码无二次转义）；往返逐字节互逆是单测向量
    static QString encodeIdentity(const QString &plainRelativePath);
    static QString decodeIdentity(const QString &encodedRelativePath);

    QString rootPath() const { return m_root; }
    QString password() const { return m_password; }
    QString salt() const { return m_salt; }
    RcloneCrypt::NameMode nameMode() const { return m_nameMode; }
    RcloneCrypt::NameEncoding nameEncoding() const { return m_nameEncoding; }
    bool dirNameEncrypt() const { return m_dirNameEncrypt; }
    QString suffix() const { return m_suffix; }
    bool strictNames() const { return m_strictNames; }
    bool passBadBlocks() const { return m_passBadBlocks; }
    BrowseMode browseMode() const { return m_browseMode; }
    QStringList extensions() const { return m_extensions; }

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

  private:
    RcloneCrypt::Params cipherParams() const;
    // 记下编码的 INI 原值——仅域外取值时与 m_nameEncoding 的换算值不同
    void setEncodingRaw(int value) { m_encodingRaw = value; }
    // 惰性派生一次（enumerate 的工作线程内），失败缓存错误串
    std::expected<std::shared_ptr<const RcloneCrypt>, QString> cipherForEnum() const;
    // 不派生的只读出口：createStreamEndpoint 不得触发派生
    std::shared_ptr<const RcloneCrypt> cipherIfReady() const;

    // 一个加密目录层的收集：目录产容器（Layered）或递归（FilesRecursive）、文件解密
    // 后产实体。strict / 跳过策略在此收口；返回非空即整次失败。版本串密文名（旧版本
    // 文件）按命名冲突跳过并记警告
    std::optional<QString> collectLayer(const std::shared_ptr<const RcloneCrypt> &cipher,
                                        const QString &cipherDir, const QString &plainRel,
                                        bool makeContainers, bool recurse, int depth,
                                        int depthLimit, QSet<QString> &visited, Enumeration &out,
                                        QStringList &warnings) const;

    QString m_root;
    QString m_password; // 明文；INI 不加密
    QString m_salt;
    RcloneCrypt::NameMode m_nameMode = RcloneCrypt::NameMode::Standard;
    RcloneCrypt::NameEncoding m_nameEncoding = RcloneCrypt::NameEncoding::Base32;
    // 编码的 INI 原值：域外取值原样持有并照原值写回，m_nameEncoding 只服务算法层
    // ——"读不懂的用户数据不许被洗成读得懂的"，否则一次落盘就把配置异常静默降级
    int m_encodingRaw = 0;
    bool m_dirNameEncrypt = true;
    QString m_suffix; // 仅 Off 生效；空 = 无后缀
    bool m_strictNames = false;
    bool m_passBadBlocks = false;
    BrowseMode m_browseMode = BrowseMode::Layered;
    QStringList m_extensions; // 无点小写；空表表示不过滤

    // 非空即本提供者不可用：enumerate 作为错误上抛，视图按"来源不可用"落占位行
    QString m_configError;

    // enumerate 在工作线程调用，惰性派生以互斥锁保护（同一提供者至多派生一次）
    mutable QMutex m_cipherMutex;
    mutable std::shared_ptr<const RcloneCrypt> m_cipher;
    mutable QString m_cipherError;
};

// 参数页工厂；seed 为同类型存量提供者（新建 / 换类型传 nullptr），页面类定义在 .cpp
ProviderSettingsPage *
createRcloneCryptSettingsPage(const PlaylistProvider *seed,
                              const std::shared_ptr<ServiceRegistry> &registry, QWidget *parent);

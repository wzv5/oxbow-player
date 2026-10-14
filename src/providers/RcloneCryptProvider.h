#pragma once

#include "pch.h"

#include "PlaylistProvider.h"
#include "rclonecrypt/RcloneCrypt.h"

// N44：身份常量（类型键 + 显示名源文案），登记表与 writeProviderKey 共用同一点定义
inline constexpr char kRcloneCryptProviderKey[] = "rclonecrypt";
inline constexpr char kRcloneCryptProviderName[] =
    QT_TRANSLATE_NOOP("RcloneCryptProvider", "Rclone Crypt");

class ProviderSettingsPage;

// rclone crypt 只读提供者（N98）：绑定一个 rclone crypt 加密的本地目录，枚举期
// 逐名解密（列表显示明文名），播放经 oxbow:// 转接由 RcloneCryptByteStream 边读
// 边解密——mpv 不吃 crypt 协议，转接能力恒为 Required（N52 三态，直发分支编译期
// 不可达，directPlayUrl 不覆写）。
// 身份为 URN：rclonecrypt://<percent-encoded 相对路径>（N98 §6），根目录归提供者
// 实例持有不进身份串——换根目录 / 整仓搬迁不影响已存身份与续播记忆；密文路径一律
// 现算（明文身份 → 逐段加密），不持久化。
// N44 口径：取值 / 校验做成静态纯函数供参数页与单测共用；load ↔ saveSettings 的
// 幂等回显是硬要求。密钥派生（scrypt，实测 100–200ms）不得在 GUI 线程：落点在首次
// enumerate（工作线程）内惰性派生一次并以不可变 shared_ptr 共享（N98 §3），
// createStreamEndpoint（GUI 线程惰性调用）只读不派生。
class RcloneCryptProvider : public PlaylistProvider {
  public:
    // N98 §6：浏览模式三选一，与 DirProvider::BrowseMode 同形（分层 / 仅文件 /
    // 仅文件含子目录递归）；枚举值序即参数页下拉框选项序与 INI 持久化取值
    enum class BrowseMode { Layered, FilesOnly, FilesRecursive };

    // root 为参数页来源处定形的目录（normalizeRoot：绝对路径、原生分隔符）；
    // password / salt 明文持有（与 WebDAV 凭据同口径，INI 不加密）；
    // nameMode / encoding 沿用算法层枚举（取值即 rclone 的两选项）；suffix 仅
    // Off 模式生效（"none" 归一为空在算法层 create 内做）。registry 必填
    RcloneCryptProvider(const QString &root, const QString &password, const QString &salt,
                        RcloneCrypt::NameMode nameMode, RcloneCrypt::NameEncoding encoding,
                        bool dirNameEncrypt, const QString &suffix, bool strictNames,
                        bool passBadBlocks, BrowseMode browseMode, const QStringList &extensions,
                        const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    // 显式 true：内容以枚举为准不可增删，但可排序并持久化规则（与目录表同形）
    bool supportsSort() const override;
    // N57 §2.9：按修改时间升序（与目录表、WebDAV 表同形）
    SortRule defaultSortRule() const override { return {SortKey::Modified, true}; }
    // N98 §6：恒为 Required——mpv 不吃 crypt 协议，直发密文路径必然放不出来
    TransitCapability transitCapability() const override;
    std::shared_ptr<StreamEndpoint> createStreamEndpoint() const override;

    // 当前语言下的显示名（无译文时即英文源）；登记表以函数指针引用（N44）
    static QString displayName();

    // ——静态纯函数（N44 / N98 §2 §7）：参数页取值 / 校验与单元测试共用同一出口——
    // 根目录定形：trim → 空即空串 → Util::Url::toIdentity（相对转绝对、转原生
    // 分隔符）。幂等是硬要求（DirProvider::normalizeRoot 同款）
    static QString normalizeRoot(const QString &text);
    // 参数校验：入参是控件原始文本。返回可直接展示的错误文案；通过返回 nullopt
    static std::optional<QString> validateInput(const QString &rawRoot);
    // 扩展名编辑区文本 → 规范清单（DirProvider 同形：空白拆分、去点、小写、去重）
    static QStringList parseExtensions(const QString &text);
    // 三个 int 不透明存储的取值换算（域外降级默认值，DirProvider 先例）
    static RcloneCrypt::NameMode nameModeFromInt(int value);
    static RcloneCrypt::NameEncoding nameEncodingFromInt(int value);
    static BrowseMode browseModeFromInt(int value);
    // INI 侧的编码取值，与参数页用的 nameEncodingFromInt 分工不同：下拉框恒只有
    // base32 / base64 两项、取值必在域内，降级够用；INI 是用户能手改的数据，而域外
    // 取值的唯一现实来源是把 rclone 的 base32768（取值 2）塞进来——静默降成 base32
    // 会解出一整层错名，故这里不降级，由调用方按配置数据异常显影
    static std::optional<RcloneCrypt::NameEncoding> nameEncodingFromIni(int value);
    // ——身份编解码（N98 §6 要点 4）：明文相对路径 ↔ 段内 percent-encoded 形态。
    // '/' 分隔符保留原样、段内 unreserved 之外全编码（QUrl::toPercentEncoding 口径，
    // 单次编码无二次转义）；往返逐字节互逆是单测向量
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
    // 算法层参数装配（INI 成员 → RcloneCrypt::Params）
    RcloneCrypt::Params cipherParams() const;
    // 载入期专用：记下编码的 INI 原值——仅域外取值时与 m_nameEncoding 的换算值不同
    void setEncodingRaw(int value) { m_encodingRaw = value; }
    // 密钥就绪出口：惰性派生一次（enumerate 的工作线程内），失败缓存错误串
    std::expected<std::shared_ptr<const RcloneCrypt>, QString> cipherForEnum() const;
    // 不派生的只读出口（createStreamEndpoint 用，N98 §3：端点创建不得触发派生）
    std::shared_ptr<const RcloneCrypt> cipherIfReady() const;
    // 解密后的名字是否命中扩展名清单（空表不过滤；按解密后的名字过滤）
    bool extensionMatches(const QString &plainName) const;

    // 一个加密目录层的收集：目录产容器（Layered）或递归（FilesRecursive）、
    // 文件按形态解密后产实体。混存与解不开名的 strict / 跳过策略在此收口；
    // 返回非空即整次失败（strict_names 或根缺失）。版本串密文名（旧版本文件）
    // 按命名冲突跳过并记警告
    std::optional<QString> collectLayer(const std::shared_ptr<const RcloneCrypt> &cipher,
                                        const QString &cipherDir, const QString &plainRel,
                                        bool makeContainers, bool recurse, int depth,
                                        int depthLimit, QSet<QString> &visited, Enumeration &out,
                                        QStringList &warnings) const;

    // ——提供者初始排序在 .cpp 文件内完成（N45 §2.7 同形：文件名数字化，稳定
    // 排序保等键原序）——

    QString m_root;     // 绑定的加密目录（绝对路径、原生分隔符）
    QString m_password; // 明文；INI 中明文（N98 §2 定案）
    QString m_salt;     // 明文；空 ⇒ rclone 内置 salt
    RcloneCrypt::NameMode m_nameMode = RcloneCrypt::NameMode::Standard;
    RcloneCrypt::NameEncoding m_nameEncoding = RcloneCrypt::NameEncoding::Base32;
    // 编码的 INI 原值：域外取值（如手改进去的 base32768=2）原样持有并照原值写回，
    // 解释用的 m_nameEncoding 只服务算法层——"读不懂的用户数据不许被洗成读得懂的"，
    // 否则一次落盘就把配置异常静默降级成 base32
    int m_encodingRaw = 0;
    bool m_dirNameEncrypt = true;
    QString m_suffix; // 仅 Off 生效；空 = 无后缀（INI 字面 "none" 也归一为空）
    bool m_strictNames = false;
    bool m_passBadBlocks = false;
    BrowseMode m_browseMode = BrowseMode::Layered;
    QStringList m_extensions; // 无点小写扩展名；空表表示不过滤

    // 载入期发现的配置缺陷（目前唯一来源：编码取值域外，见 nameEncodingFromIni）。
    // 非空即本提供者不可用——enumerate 把它作为错误上抛，视图按"来源不可用"落占位行，
    // 与目录 / WebDAV 的枚举失败同一条通道
    QString m_configError;

    // 派生结果（不可变）与失败缓存。enumerate 在工作线程调用，惰性派生以互斥锁
    // 保护（同一提供者至多派生一次；N98 §3）
    mutable QMutex m_cipherMutex;
    mutable std::shared_ptr<const RcloneCrypt> m_cipher;
    mutable QString m_cipherError;
};

// 参数页工厂（N44）：页面类定义在 RcloneCryptProvider.cpp，本处只声明工厂。
// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *
createRcloneCryptSettingsPage(const PlaylistProvider *seed,
                              const std::shared_ptr<ServiceRegistry> &registry, QWidget *parent);

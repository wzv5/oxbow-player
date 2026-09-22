#include "pch.h"

#include "WebDavProvider.h"

#include "Constants.h"
#include "HttpByteStream.h"
#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "StreamEndpoint.h"
#include "TraversalPolicy.h"
#include "Util.h"
#include "WebDavEntity.h"

namespace {
// PROPFIND 请求体：只取本篇解释的四个属性（RFC 4918 子集）；前缀用 d: 惯例，
// 解析侧按命名空间匹配，前缀无约定意义
const QByteArray kPropfindBody =
    QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
                      "<d:getcontentlength/><d:getlastmodified/><d:resourcetype/>"
                      "</d:prop></d:propfind>");

// DAV: 命名空间 URI（元素按命名空间 + local name 匹配，前缀本身不可依赖）
const QString kDavNs = QStringLiteral("DAV:");

// 非 Q_OBJECT 类的用户可见串：英文源 + QT_TRANSLATE_NOOP 标注翻译上下文
// （供 lupdate 提取），运行期经 QCoreApplication::translate 取值——无译文时即
// 英文原文（N34/HttpClient 同款约定）
QString trText(const char *source) { return QCoreApplication::translate("WebDavProvider", source); }

// N46 §2.1：三档浏览模式的源文案（英文可翻译字符串，翻译上下文归属本提供者）。
// 与 DirProvider（N45 §2.11）的选项文案逐字相同是交叉验收项，两处不得各自措辞
constexpr char kBrowseLayered[] = QT_TRANSLATE_NOOP("WebDavProvider", "Layered Browsing");
constexpr char kBrowseFilesOnly[] = QT_TRANSLATE_NOOP("WebDavProvider", "Files Only");
constexpr char kBrowseFilesRecursive[] =
    QT_TRANSLATE_NOOP("WebDavProvider", "Files Only (Include Subdirectories)");

bool isDavElement(QXmlStreamReader &xml, const QString &localName) {
    // 命名空间为 DAV:（前缀不可依赖，大小写不敏感）；命名空间缺省（服务端
    // 省略 xmlns 的畸形报文）回退纯 localName 匹配——不同命名空间仍不误匹配，
    // 省略 xmlns 也不会被整树 skip 吞成"空成功"
    const QString ns = xml.namespaceUri().toString();
    if (!ns.isEmpty() && ns.compare(kDavNs, Qt::CaseInsensitive) != 0)
        return false;
    return xml.name() == localName;
}

// 条目 URL 的尾斜杠剥离：比较两侧同形用（服务端对 collection 的 href 可带可不带尾斜杠，
// 尾斜杠不计才等价）与 title 取名用。入参是条目身份串 / 请求 URL 串，故名字带 Url——
// 尾斜杠在该语境下只对 URL 有意义（本地路径由 QDir 处理）
QString stripUrlTrailingSlash(QString s) {
    while (s.endsWith(QLatin1Char('/')))
        s.chop(1);
    return s;
}

} // namespace

WebDavProvider::WebDavProvider(const QString &rootUrl, const QString &user, const QString &password,
                               BrowseMode mode, const QStringList &extensions,
                               const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_rootUrl(rootUrl), m_user(user), m_password(password),
      m_browseMode(mode), m_extensions(extensions) {}

// ——N52：WebDAV 的来源解析点（来源私有的东西与参数页同级定义，不外露）——
// openStream 就一行实质工作：identity（恒无 userinfo）+ 构造期收到的 user/password
// → Util::Url::insertCredentials → 交 HttpByteStream::open。凭据的编码形态与
// directPlayUrl 同款（QUrl userInfo(FullyEncoded) 产出，"与既有路径逐字相同"）。
namespace {
class WebDavStreamEndpoint final : public StreamEndpoint {
  public:
    WebDavStreamEndpoint(const QString &user, const QString &password,
                         std::shared_ptr<ServiceRegistry> registry)
        : m_user(user), m_password(password), m_registry(std::move(registry)) {}

    std::expected<std::shared_ptr<ByteStream>, QString>
    openStream(const QString &identity) const override {
        // 现取 client（resolve 的副本随流存活，N35 既有线程契约）；缺注册表/缺
        // client 属装配错误，起流失败并记因，不静默
        const auto client = m_registry ? m_registry->resolve<HttpClient>() : nullptr;
        if (!client)
            return std::unexpected(QStringLiteral("http client unavailable"));
        HttpStreamTarget target;
        if (!m_user.isEmpty() || !m_password.isEmpty()) {
            QUrl cred;
            cred.setUserName(m_user);
            cred.setPassword(m_password);
            const QString userinfo = cred.userInfo(QUrl::ComponentFormattingOption::FullyEncoded);
            if (!userinfo.isEmpty())
                target.url = Util::Url::insertCredentials(identity, userinfo);
        }
        if (target.url.isEmpty())
            target.url = identity; // 无凭据可注入（皆空 / 编码为空）
        // 背压水位取 HttpClient 内置默认（N51 实测口径维持）
        auto opened = HttpByteStream::open(HttpByteStream::makeFeed(client), target);
        if (!opened)
            return std::unexpected(opened.error());
        return opened.value(); // shared_ptr<HttpByteStream> → shared_ptr<ByteStream>
    }

  private:
    QString m_user;                              // 可空；构造后不可变（N52 §2.3 契约 5）
    QString m_password;                          // 可空
    std::shared_ptr<ServiceRegistry> m_registry; // 现取 HttpClient 与可轮换令牌的唯一通道
};
} // namespace

// N52：直链与转接都合法（首个 Optional 来源）；列表级偏好决定下发形态
PlaylistProvider::TransitCapability WebDavProvider::transitCapability() const {
    return TransitCapability::Optional;
}

std::shared_ptr<StreamEndpoint> WebDavProvider::createStreamEndpoint() const {
    return std::make_shared<WebDavStreamEndpoint>(m_user, m_password, registry());
}

// WebDAV 目录的参数页（N44 §2.5.3）：URL 行 + 用户名行 + 密码行 + 浏览模式 +
// 扩展名行。N46：原"包含子目录"复选框换为三选一的浏览模式下拉框（单行，与
// URL / Password 行同构），三档文案与 DirProvider 逐字相同（交叉验收项）。
// 页面类只在本文件可见，经 createWebDavSettingsPage 产出
class WebDavSettingsPage : public ProviderSettingsPage {
    Q_OBJECT
  public:
    WebDavSettingsPage(const WebDavProvider *seed, const std::shared_ptr<ServiceRegistry> &registry,
                       QWidget *parent)
        : ProviderSettingsPage(parent), m_seed(seed), m_registry(registry) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);

        m_urlEdit = new QLineEdit;
        form->addRow(tr("URL"), m_urlEdit);
        m_userEdit = new QLineEdit;
        form->addRow(tr("Username"), m_userEdit);
        m_passwordEdit = new QLineEdit;
        m_passwordEdit->setEchoMode(QLineEdit::EchoMode::Password);
        form->addRow(tr("Password"), m_passwordEdit);

        m_modeCombo = new QComboBox;
        // 选项序即 BrowseMode 枚举值序（0/1/2），setCurrentIndex 按值直接回填；
        // 三档文案与 DirProvider 逐字相同（N46 §2.1 交叉验收项）
        m_modeCombo->addItem(trText(kBrowseLayered));
        m_modeCombo->addItem(trText(kBrowseFilesOnly));
        m_modeCombo->addItem(trText(kBrowseFilesRecursive));
        form->addRow(tr("Browse Mode"), m_modeCombo);

        m_extensionsEdit = new QLineEdit;
        auto *extRow = new QHBoxLayout;
        extRow->addWidget(m_extensionsEdit, 1);
        auto *extResetBtn = new QPushButton(QStringLiteral("↺"));
        extResetBtn->setToolTip(tr("Reset to default extension list"));
        extResetBtn->setFixedWidth(28);
        extRow->addWidget(extResetBtn);
        // 说明文字移到编辑框下方单独一行（原在标题的括号里），灰色显示：
        // 标题只写 Filter，扩展名这一层含义由本行说明承担
        auto *extNote =
            new QLabel(tr("Space-separated file extensions; leave empty for no filter"));
        // 不开 wordWrap：备注按单行计入宽度，由它把表单区（进而对话框）撑宽。
        // 开换行会让 QLabel 变成"用高度换宽度"，布局反而不为它撑宽、把它压到折行
        extNote->setEnabled(false); // 禁用态即取调色板的灰，跟随主题不硬编码颜色
        // 备注右侧留 12px：它是本列最宽的一项，不留就会直接贴住窗口右边
        auto *noteRow = new QHBoxLayout;
        noteRow->addWidget(extNote);
        noteRow->addSpacing(12);
        auto *extField = new QVBoxLayout;
        extField->addLayout(extRow);
        extField->addLayout(noteRow);
        form->addRow(tr("Filter"), extField);

        layout->addLayout(form);
        // N52：转接行（Optional 类型 = 可勾选项）——基类一次实现，本页只此一行；
        // 回显取 seed 的持久化偏好，新建列表 false 起。放在 stretch 之前即视觉底行
        enableTransitOption(m_seed ? m_seed->transitPreference() : std::optional<bool>(false));
        // 同目录页：把被拉伸出来的余量压到内容之下，避免行间距被均摊撑大
        layout->addStretch(1);

        // 回填：rootUrl 已含尾斜杠，URL 框原样回显；无回显来源（新建 / 换类型）
        // 时扩展名框预填全局默认清单、模式落默认分层浏览（下拉框当前项 0 即
        // 默认，显式置位表达意图）
        if (m_seed) {
            m_urlEdit->setText(m_seed->rootUrl());
            m_userEdit->setText(m_seed->user());
            m_passwordEdit->setText(m_seed->password());
            m_modeCombo->setCurrentIndex(int(m_seed->browseMode()));
            m_extensionsEdit->setText(m_seed->extensions().join(QLatin1Char(' ')));
        } else {
            m_modeCombo->setCurrentIndex(int(WebDavProvider::BrowseMode::Layered));
            m_extensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        }

        // 复位：预填默认清单（不校验、可继续编辑）
        connect(extResetBtn, &QPushButton::clicked, this, [this] {
            m_extensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        });
    }

    std::optional<QString> validate() const override {
        return WebDavProvider::validateInput(m_urlEdit->text());
    }

    bool hasChanges() const override {
        return !m_seed || urlValue() != m_seed->rootUrl() || userValue() != m_seed->user() ||
               passwordValue() != m_seed->password() || modeValue() != m_seed->browseMode() ||
               extensionsValue() != m_seed->extensions() ||
               transitEnabled() != m_seed->transitPreference().value_or(false);
    }

    std::shared_ptr<PlaylistProvider> build() const override {
        return std::make_shared<WebDavProvider>(urlValue(), userValue(), passwordValue(),
                                                modeValue(), extensionsValue(), m_registry);
    }

  private:
    // 取规范化参数值（hasChanges 与 build 共用同一出口）
    QString urlValue() const { return WebDavProvider::normalizeRootUrl(m_urlEdit->text()); }
    QString userValue() const { return m_userEdit->text().trimmed(); }
    // 密码不 trim——首尾空格可能是有效字符
    QString passwordValue() const { return m_passwordEdit->text(); }
    WebDavProvider::BrowseMode modeValue() const {
        return WebDavProvider::browseModeFromInt(m_modeCombo->currentIndex());
    }
    QStringList extensionsValue() const {
        return WebDavProvider::parseExtensions(m_extensionsEdit->text());
    }

    const WebDavProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QLineEdit *m_urlEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QLineEdit *m_extensionsEdit = nullptr;
};

ProviderSettingsPage *createWebDavSettingsPage(const PlaylistProvider *seed,
                                               const std::shared_ptr<ServiceRegistry> &registry,
                                               QWidget *parent) {
    // 同类型由调用方（对话框）保证：seed 非空即本类型
    return new WebDavSettingsPage(seed ? static_cast<const WebDavProvider *>(seed) : nullptr,
                                  registry, parent);
}

QString WebDavProvider::typeKey() const { return QLatin1String(kWebDavProviderKey); }

QString WebDavProvider::displayName() {
    return QCoreApplication::translate("WebDavProvider", kWebDavProviderName);
}

// N48：喂 mpv 的播放形态——把当前凭据注入 URL 的 userinfo 段。凭据的编码形态取自
// QUrl：今天凭据形态本就是 setUserName / setPassword 的产出，取其 userInfo(FullyEncoded)
// 即"与既有路径逐字相同"，不必复刻 Qt 的编码表；插入动作交 Util::Url::insertCredentials 的纯
// 字符串拼接（authority 定位与插入规则在 Util 内单点，本函数不再碰任何偏移），其余字节
// 原样保留，故往返不变式 strip(embed(u)) == u 构造性成立。凭据皆空、解析失败、非 http(s)、
// 无 authority 一律恒等返回（绝不注入，避免产出 http://u:p@/path 之类垃圾串）
QString WebDavProvider::directPlayUrl(const QString &path) const {
    if (m_user.isEmpty() && m_password.isEmpty())
        return path;
    // 解析走剥过的串（入参恒无 userinfo，§2.3；此处兜住将来形态变化），凭据写入后取编码形态
    QUrl url(Util::Url::stripCredentials(path), QUrl::ParsingMode::TolerantMode);
    if (!url.isValid())
        return path;
    url.setUserName(m_user);
    url.setPassword(m_password);
    const QString userinfo = url.userInfo(QUrl::ComponentFormattingOption::FullyEncoded);
    if (userinfo.isEmpty())
        return path;
    return Util::Url::insertCredentials(path, userinfo);
}

QString WebDavProvider::normalizeRootUrl(const QString &text) {
    // URL 的来源处（N37 §3.1 第 1 条）：trim → 经 Util::Url::toNative 规范化（网络分支）
    // → 补尾斜杠
    QString normalized = text.trimmed();
    if (normalized.isEmpty())
        return normalized;
    normalized = Util::Url::toNative(normalized);
    // 尾斜杠补在 path 上：基目录语义只对路径成立，直接补在串尾会把分隔符塞进
    // query（"…/dav?token=1/"）。query / fragment 原样保留在后。
    // N48：query / fragment 的分界走 Util::Url::pathEnd（与显示名取末段同一口径）
    const qsizetype cut = Util::Url::pathEnd(normalized);
    QString head = normalized.left(cut);
    if (!head.endsWith(QLatin1Char('/')))
        head += QLatin1Char('/');
    return head + normalized.mid(cut);
}

std::optional<QString> WebDavProvider::validateInput(const QString &rawUrl) {
    // 入参是控件原始文本，内部先定形——不存在"必须先定形"的前置条件
    const QString url = normalizeRootUrl(rawUrl);
    if (url.isEmpty())
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Please enter a directory URL."));
    // 只要求可解析且 scheme ∈ {http, https}；不得内嵌 userinfo（提示改用用户名 /
    // 密码栏，避免凭据出现两个来源）。不做连通性探测——交由异步枚举，失败即
    // 占位行 + 右键刷新重试（N37 口径不变）
    const QUrl parsed(url, QUrl::ParsingMode::TolerantMode);
    if (!parsed.isValid() || (parsed.scheme().toLower() != QLatin1String("http") &&
                              parsed.scheme().toLower() != QLatin1String("https")))
        return trText(
            QT_TRANSLATE_NOOP("WebDavProvider", "Directory URL must be a valid http(s) URL."));
    if (!parsed.userName(QUrl::ComponentFormattingOption::FullyDecoded).isEmpty() ||
        !parsed.password(QUrl::ComponentFormattingOption::FullyDecoded).isEmpty())
        return trText(QT_TRANSLATE_NOOP("WebDavProvider",
                                        "Directory URL must not embed credentials; "
                                        "use the username and password fields instead."));
    return std::nullopt;
}

QStringList WebDavProvider::parseExtensions(const QString &text) {
    // 与 DirProvider::parseExtensions 同口径（N44 §2.3：两份实现为已知并接受的代价）
    QStringList out;
    for (const QString &token :
         text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        QString ext = token;
        while (ext.startsWith(QLatin1Char('.')))
            ext.remove(0, 1);
        ext = ext.toLower();
        if (!ext.isEmpty() && !out.contains(ext))
            out << ext;
    }
    return out;
}

// N46 §2.1：INI 的 int 取值 → 浏览模式；取值域外（损坏 / 未知值）降级 Layered
// （新建列表默认），不做旧键兼容。与 DirProvider 同名函数各自持有（§2.1）
WebDavProvider::BrowseMode WebDavProvider::browseModeFromInt(int value) {
    switch (value) {
    case int(BrowseMode::Layered):
        return BrowseMode::Layered;
    case int(BrowseMode::FilesOnly):
        return BrowseMode::FilesOnly;
    case int(BrowseMode::FilesRecursive):
        return BrowseMode::FilesRecursive;
    }
    return BrowseMode::Layered;
}

// N46 §2.4：collection 条目身份串 → 容器 id。N48 起条目身份已恒无 userinfo
// （resolveEntryUrl 恒剥），故剥离动作对现产出即恒等变换，仍予保留——id 是持久化进
// queueRoot 与作请求基 URL 的身份，形态恒定是硬要求；对万一带 userinfo 的入参，
// 空串赋值是清空、不留空段（既有用例 absoluteHrefCredentialsStrippedWhenNoCreds
// 实证的 Qt 行为）。编码形态与尾斜杠原样保留；id 恒无凭据——持久化进 queueRoot 的
// INI 不携带凭据，请求侧由 enumerate 逐请求注入当前凭据
QString WebDavProvider::containerId(const QString &entryPath) {
    QUrl url(entryPath, QUrl::ParsingMode::TolerantMode);
    if (!url.userInfo().isEmpty()) {
        url.setUserName(QString());
        url.setPassword(QString());
    }
    return Util::Url::toNative(url.toString(QUrl::ComponentFormattingOption::FullyEncoded));
}

// ——提供者初始排序（N45 §2.7，N46 §2.2）：文件名数字化，稳定排序保等键的
// 服务器枚举序——预切分数字段后按下标排序，避免比较器里 O(n log n) 次重复
// segmentName。与视图 SortKey::None 显示序逐项一致由规格与验收约束（N45 §2.7），
// 不共用代码（DirProvider 的同名私有辅助各自持有）
void WebDavProvider::sortEntitiesByNumericName(EntityList &entities) {
    const int n = int(entities.size());
    if (n < 2)
        return;
    std::vector<Util::SegmentedName> segs;
    segs.reserve(n);
    for (const auto &e : entities)
        segs.push_back(Util::segmentName(e->fileName()));
    std::vector<int> order;
    order.reserve(n);
    for (int i = 0; i < n; ++i)
        order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&segs](int a, int b) { return Util::naturalCompare(segs[a], segs[b]) < 0; });
    EntityList sorted;
    sorted.reserve(n);
    for (int i : order)
        sorted.push_back(std::move(entities[i]));
    entities = std::move(sorted);
}

void WebDavProvider::sortContainersByNumericTitle(QList<Container> &containers) {
    const int n = int(containers.size());
    if (n < 2)
        return;
    std::vector<Util::SegmentedName> segs;
    segs.reserve(n);
    for (const Container &c : containers)
        segs.push_back(Util::segmentName(c.title));
    std::vector<int> order;
    order.reserve(n);
    for (int i = 0; i < n; ++i)
        order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&segs](int a, int b) { return Util::naturalCompare(segs[a], segs[b]) < 0; });
    QList<Container> sorted;
    sorted.reserve(n);
    for (int i : order)
        sorted.push_back(std::move(containers[i]));
    containers = std::move(sorted);
}

// 枚举：N32 统一后的 QtConcurrent 任务内同步推进（在途至多 1 条请求，
// 顺序推进不设取消通道——孤儿任务跑完整轮，结果按代际身份静默丢弃）。
// N46 §2.2：按浏览模式分派——分层浏览与仅文件为单次 PROPFIND（每层即本次枚举
// 的根请求：请求 / 解析失败 → 整次 unexpected 进占位，体超限随之整层失败）；
// 仅文件（含子目录）保留逐层 DFS（N37 §3.5 失败语义不变：根失败整次 unexpected，
// 子目录记 warning 跳过）。两档平铺收到非空 parent 属调用方违约，Q_ASSERT 守候，
// release 降级返回空结果。提供者初始排序见 §2.2（N45 §2.7）
std::expected<PlaylistProvider::Enumeration, QString>
WebDavProvider::enumerate(const QStringList &parent) const {
    // 装配错误：HttpClient 启动期必已 provide（N35 §2.3），永不命中；
    // 断言守候 + fail-safe（枚举不发请求即整次失败）
    const std::shared_ptr<HttpClient> client =
        registry() ? registry()->resolve<HttpClient>() : nullptr;
    if (!client) {
        Q_ASSERT(false);
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("WebDavProvider", "HTTP client is not available.")));
    }
    if (!parent.isEmpty() && m_browseMode != BrowseMode::Layered) {
        // 平铺形态不产容器，视图永不应以非空 parent 调用本提供者
        Q_ASSERT(false);
        return Enumeration{};
    }

    // 请求基 URL：分层模式非空 parent 时取末段容器 id（N46 §2.4：id 即本层基
    // URL，前序段只构成祖先链语义），否则 rootUrl。id 与 rootUrl 同一信任来源
    // （同属应用持久化数据），不做归属校验——解析失败或非网络 URL 按该层失败
    const QString baseText = parent.isEmpty() ? m_rootUrl : parent.last();
    QUrl rootUrl = QUrl(baseText, QUrl::ParsingMode::TolerantMode);
    if (!rootUrl.isValid() || !Util::Url::isNetworkStream(baseText))
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("WebDavProvider", "Invalid WebDAV directory URL: %1"))
                .arg(baseText));
    // N48：请求 URL 的凭据逐请求注入——任务 URL（rootUrl / 容器 id）恒为无凭据形态，
    // 注入只作用于请求副本，不进实体、不落盘。凭据随 URL 走，经 N34 §2.5 的"URL
    // userinfo 优先"生效（Basic 预置头省一次 401 往返，Digest / NTLM 由
    // authenticationRequired 兜底）。根请求与 DFS 子目录请求同一条规则：漏注入即该次
    // 请求 401（DFS 子项原先靠条目身份自带凭据，条目转干净后该通路不再成立）
    auto requestUrlOf = [this](const QUrl &taskUrl) {
        QUrl req = taskUrl;
        if (!m_user.isEmpty() || !m_password.isEmpty()) {
            req.setUserName(m_user);
            req.setPassword(m_password);
        }
        return req;
    };

    // 文件条目 → 实体（两分支共用）：扩展名过滤（取显示名的后缀，大小写不敏感；
    // 空清单 = 不过滤）→ 造 WebDavEntity（具备文件级元数据能力）→ 已取得的
    // size / modified 经注入口零成本写入（零额外请求）。两者皆缺不注入，即值未知
    auto appendFileEntities = [this](const QList<DavEntry> &files, EntityList &entities) {
        for (const DavEntry &entry : files) {
            if (!m_extensions.isEmpty()) {
                // 经 Util::Url::fileName 单次解码（剥 query / fragment → 末段 →
                // decode）。不得走 QUrl::path()——它默认返回已解码形态，再解一次
                // 会让字面 "%252F" 误解为分隔符
                const QString name = Util::Url::fileName(entry.path);
                const int dot = name.lastIndexOf(QLatin1Char('.'));
                if (dot < 0)
                    continue;
                if (!m_extensions.contains(name.mid(dot + 1).toLower()))
                    continue;
            }
            // N39：直接构造 WebDavEntity，不经工厂——工厂对 http(s) 只产无元数据
            // 能力的 HttpEntity（收编 / 静态表重载的落点）。二者对网络路径的
            // 加工等价（Util::Url::toIdentity 对网络路径原样返回），故绕过工厂无损
            auto entity = std::make_shared<WebDavEntity>(entry.path, registry());
            if (entry.size || entry.modified) {
                FileMeta fm;
                fm.size = entry.size;
                fm.modified = entry.modified;
                injectFileMeta(*entity, fm);
            }
            entities.push_back(std::move(entity));
        }
    };

    if (m_browseMode == BrowseMode::FilesRecursive) {
        // ——仅文件（含子目录）：现状 DFS 逐字保留——
        // 深度上限：注册表策略快照取用（GUI 线程装配、任务线程只读，N36 修订 1）；
        // resolve 为空按缺省兜底（同 DirProvider，策略只读且缺省值立即可用）
        int maxDepth = App::recursiveDepthLimitDefault();
        if (const auto policy = registry()->resolve<TraversalPolicy>())
            maxDepth = policy->depthLimit;

        // DFS 栈（先序）：每项为待枚举目录的请求 URL 与自身深度（根为 0，与
        // DirProvider 的 N36 口径一致）。请求 URL 即基 URL：尾斜杠（davUrl 来源处
        // 已补、collection 条目在解析侧补齐）是"基目录"语义的前提，否则 resolved
        // 会把末段当文件名替换掉
        struct DavTask {
            QUrl url;
            int depth;
        };
        QList<DavTask> stack;
        stack.append({rootUrl, 0});
        // 已访问目录的匹配键集（N37 实施修订，与 DirProvider 的 N36 修订 3 同口径）：
        // 服务端以符号链接 / 外部挂载让同一目录经多条路径可达（乃至成环）时，无剪枝会
        // 把同一批文件重复枚举多次——重复行要用户手动清理，且多打一整轮请求。命中即
        // 静默跳过、不记 warning（防御，不是错误）；根先入集
        QSet<QString> visited;
        visited.insert(
            Util::Url::key(rootUrl.toString(QUrl::ComponentFormattingOption::FullyEncoded)));

        EntityList entities;
        QStringList warnings;
        while (!stack.isEmpty()) {
            const DavTask task = stack.takeLast();
            const QUrl reqUrl = requestUrlOf(task.url);
            HttpClient::Request req;
            req.url = reqUrl; // 绝对 URL：与注册表 client 的缺省 baseUrl 解绑
            req.method = QByteArrayLiteral("PROPFIND");
            req.body = kPropfindBody;
            req.contentType = QByteArrayLiteral("application/xml");
            req.headers.append({QByteArrayLiteral("Depth"), QByteArrayLiteral("1")});
            // 同步阻塞（exec）：枚举任务体不可取消（遮罩 Loading 模式无取消按钮），
            // 仅任务线程调用——符合 N34 的 exec 使用前提
            const HttpClient::Response resp = client->exec(req);

            // 成功判定：207 Multi-Status 为 WebDAV 的正常应答，个别服务端回 200；
            // 其余状态码按失败处理。error 非空亦判失败——HttpClient 的体超限
            // （maxBodyBytes）语义是"status 为真实状态码（可能是 207）、body 丢弃为空、
            // error 记文案"，只看状态码会把超限目录当成空目录静默吞掉（N37 §3.2 步骤 5
            // 要求超限按该目录失败处理）。不透传 error 文案，只作判据
            const bool ok = (resp.status == 207 || resp.status == 200) && !resp.canceled &&
                            resp.error.isEmpty();
            if (!ok) {
                const QString message = davRequestError(resp, task.url);
                if (task.depth == 0)
                    return std::unexpected(message); // 根失败：整次 unexpected 进占位
                warnings << trText(
                                QT_TRANSLATE_NOOP("WebDavProvider",
                                                  "Subdirectory enumeration failed, skipped: %1"))
                                .arg(message);
                continue;
            }

            const auto listing = parseMultistatus(resp.body, reqUrl);
            if (!listing) {
                // 解析失败与请求失败同语义：根即整次失败，子目录记 warning 跳过
                if (task.depth == 0)
                    return std::unexpected(listing.error());
                warnings << trText(
                                QT_TRANSLATE_NOOP("WebDavProvider",
                                                  "Subdirectory enumeration failed, skipped: %1"))
                                .arg(listing.error());
                continue;
            }
            // N48：本层的跳过告警（非 http(s) href）随该层产出向上汇总
            warnings << listing->warnings;

            // 子目录：depth + 1 > maxDepth 即静默截断不再深入（截断是配置使然，
            // 不是错误，不记 warning——与 N36 §2.2 同口径）；已访问过的目录静默跳过
            for (const DavEntry &dir : listing->collections) {
                if (task.depth + 1 > maxDepth)
                    continue;
                const QString key = Util::Url::key(dir.path);
                if (visited.contains(key))
                    continue;
                visited.insert(key);
                stack.append({QUrl(dir.path), task.depth + 1});
            }

            appendFileEntities(listing->files, entities);
        }
        sortEntitiesByNumericName(entities);
        return Enumeration{{}, std::move(entities), std::move(warnings)};
    }

    // ——分层浏览 / 仅文件：单次 PROPFIND，逐层懒加载（N46 §2.2）。
    // 层缓存与"返回上一层不重枚举"由 N45 §2.5 的视图机制承担
    const QUrl reqUrl = requestUrlOf(rootUrl);
    HttpClient::Request req;
    req.url = reqUrl; // 绝对 URL + 当前凭据：与注册表 client 的缺省 baseUrl 解绑
    req.method = QByteArrayLiteral("PROPFIND");
    req.body = kPropfindBody;
    req.contentType = QByteArrayLiteral("application/xml");
    req.headers.append({QByteArrayLiteral("Depth"), QByteArrayLiteral("1")});
    // 同步阻塞（exec）：枚举任务体不可取消（遮罩 Loading 模式无取消按钮），
    // 仅任务线程调用——符合 N34 的 exec 使用前提
    const HttpClient::Response resp = client->exec(req);

    // 成功判定与 DFS 分支同口径：207 正常应答（个别服务端回 200），error 非空
    // 亦判失败（体超限——本层即根请求，整层失败进占位，§2.5）
    const bool ok =
        (resp.status == 207 || resp.status == 200) && !resp.canceled && resp.error.isEmpty();
    if (!ok)
        return std::unexpected(davRequestError(resp, reqUrl));

    const auto listing = parseMultistatus(resp.body, reqUrl);
    if (!listing)
        return std::unexpected(listing.error());

    Enumeration out;
    // N48：本层的跳过告警（非 http(s) href）
    out.warnings = listing->warnings;
    if (m_browseMode == BrowseMode::Layered) {
        // 目录层 → 容器（视图侧模型不变式：容器组恒在实体组之前，两数组天然有序）。
        // id 为无凭据完整编码 URL（N46 §2.4）；title 取解码可读名；meta 只填
        // modified——getcontentlength 对 collection 语义未定义（服务端普遍缺失 /
        // 返 0 / 不可信聚合值），size 恒未知，视图按大小排序时容器组组内退化为
        // 保持初始序（N45 §2.6 未知值末尾规则）
        out.containers.reserve(listing->collections.size());
        for (const DavEntry &dir : listing->collections) {
            Container c;
            c.id = containerId(dir.path);
            // N47：该层完整 URL（无凭据，纯展示，供属性对话框 / 复制；打开仅本地）——
            // 与 id 同串，语义正交（身份 vs 展示）
            c.path = c.id;
            // title 取解码可读名：collection 的身份串恒以尾斜杠结尾
            // （normalizeRootUrl 补齐），Util::Url::fileName 对以 / 结尾的 URL 返回
            // 空串（末段为空），须先剥尾斜杠再取解码名
            c.title = Util::Url::fileName(stripUrlTrailingSlash(dir.path));
            if (dir.modified) {
                FileMeta fm;
                fm.modified = dir.modified;
                c.meta = fm;
            }
            out.containers.push_back(std::move(c));
        }
        sortContainersByNumericTitle(out.containers);
    }
    appendFileEntities(listing->files, out.entities);
    sortEntitiesByNumericName(out.entities);
    return out;
}

bool WebDavProvider::supportsModify() const { return false; }

bool WebDavProvider::supportsSort() const { return true; }

void WebDavProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // N44：类型键经基类取 typeKey()，不再写字面量
    settings.setValue(QStringLiteral("davUrl"), m_rootUrl);
    settings.setValue(QStringLiteral("davUser"), m_user);
    settings.setValue(QStringLiteral("davPassword"), m_password);
    // N46：浏览模式一值替代原 davRecursive（不做旧键兼容与迁移，§2.1）
    settings.setValue(QStringLiteral("davBrowseMode"), int(m_browseMode));
    settings.setValue(QStringLiteral("davExtensions"), m_extensions);
    // 排序规则随提供者持久化（与目录表同形，含 None——显式记录用户未选择规则）
    settings.setValue(QStringLiteral("sortKey"), sortKey());
    settings.setValue(QStringLiteral("sortAscending"), sortAscending());
}

std::shared_ptr<PlaylistProvider>
WebDavProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    auto provider = std::make_shared<WebDavProvider>(
        settings.value(QStringLiteral("davUrl")).toString(),
        settings.value(QStringLiteral("davUser")).toString(),
        settings.value(QStringLiteral("davPassword")).toString(),
        // 新键缺失落默认分层浏览（存量 recursive = true 的 tab 升级后改为逐层
        // 钻取，破坏性变更已由维护者确认接受，§2.1）
        browseModeFromInt(settings.value(QStringLiteral("davBrowseMode"), 0).toInt()),
        settings.value(QStringLiteral("davExtensions")).toStringList(), registry);
    provider->setSortRule(settings.value(QStringLiteral("sortKey"), 0).toInt(),
                          settings.value(QStringLiteral("sortAscending"), true).toBool());
    return provider;
}

QString WebDavProvider::resolveEntryUrl(const QUrl &baseUrl, const QString &href) {
    // href → 条目 URL：绝对 href 直接采用，相对 href 按基解析（基的尾斜杠让
    // "基目录"语义成立）。URL 的解析与形态交 QUrl 与 Util::Url::toNative，不自行拼串
    QUrl resolved = baseUrl.resolved(QUrl(href, QUrl::ParsingMode::TolerantMode));
    // N48：凭据不进条目——恒剥解析结果的 userinfo。相对 href 的 userinfo 继承自基
    // 请求 URL、绝对 href 的来自 href 自身（恶意 / 畸形报文夹带），两者一并剥。
    // 注意不得无条件 setUserName("") / setPassword("")——若本无 userinfo，空串赋值
    // 会把该段标记为 present 且空，序列化产出 "http://:@host/…" 这种既非标准也不
    // 简洁的形态；故仅在确有 userinfo 时以空串赋值清空（不留空段，既有用例
    // absoluteHrefCredentialsStrippedWhenNoCreds 已实证该 Qt 行为）
    if (!resolved.userInfo().isEmpty()) {
        resolved.setUserName(QString());
        resolved.setPassword(QString());
    }
    // 经 Util::Url::toNative 收口取编码形态输出——即"入口处构造一次"（§3.1 第 1 条）；
    // 此后身份串全链路只读转发，凭据只在请求与播放时注入
    return Util::Url::toNative(resolved.toString(QUrl::ComponentFormattingOption::FullyEncoded));
}

std::expected<WebDavProvider::DavListing, QString>
WebDavProvider::parseMultistatus(const QByteArray &body, const QUrl &baseUrl) {
    DavListing out;
    QXmlStreamReader xml(body);
    // 层级化解析（readNextStartElement 的语义是"读到任一结束元素即返回 false"，
    // 故每层 while 只消费自己这一层的子元素，包裹层交下一层的 while 递归消化；
    // 绝不跨层 continue——跨层会让循环在子元素的 </...> 处提前退出）。
    // 前置约定：各辅助 lambda 被调用时，reader 停在对应元素的起始标签上；
    // 返回时其结束标签已被消费。
    // N48：非 http(s) href 的跳过计数与首个示例（收尾时合并为一条 warning）
    int skippedNonHttp = 0;
    QString skippedSample;
    auto parseProp = [&](bool &isCollection, std::optional<qint64> &size,
                         std::optional<QDateTime> &modified) {
        while (xml.readNextStartElement()) {
            if (isDavElement(xml, QStringLiteral("resourcetype"))) {
                // 深入一层找 <collection/>（可为空——普通文件）；其余子元素跳过
                while (xml.readNextStartElement()) {
                    if (isDavElement(xml, QStringLiteral("collection")))
                        isCollection = true;
                    xml.skipCurrentElement();
                }
            } else if (isDavElement(xml, QStringLiteral("getcontentlength"))) {
                bool ok = false;
                const qint64 v = xml.readElementText().toLongLong(&ok);
                if (ok && v >= 0)
                    size = v;
            } else if (isDavElement(xml, QStringLiteral("getlastmodified"))) {
                // 解析失败返回 invalid：不置 optional（等价于属性缺失，按"值未知"处理）
                const QDateTime dt = Util::parseHttpDate(xml.readElementText().toUtf8());
                if (dt.isValid())
                    modified = dt;
            } else {
                xml.skipCurrentElement();
            }
        }
    };
    auto parsePropstat = [&](bool &isCollection, std::optional<qint64> &size,
                             std::optional<QDateTime> &modified) {
        while (xml.readNextStartElement()) {
            if (isDavElement(xml, QStringLiteral("prop")))
                parseProp(isCollection, size, modified);
            else
                xml.skipCurrentElement(); // <status> 等
        }
    };
    auto parseResponse = [&] {
        QString href;
        bool isCollection = false;
        std::optional<qint64> size;
        std::optional<QDateTime> modified;
        while (xml.readNextStartElement()) {
            if (isDavElement(xml, QStringLiteral("href"))) {
                href = xml.readElementText();
            } else if (isDavElement(xml, QStringLiteral("propstat"))) {
                parsePropstat(isCollection, size, modified);
            } else {
                xml.skipCurrentElement();
            }
        }

        // href 缺失 / 为空：服务端畸形 response，跳过不产出（部分失败语义不适用于
        // 单条目级瑕疵——忽略即可，不记 warning）
        if (href.isEmpty())
            return;

        DavEntry entry;
        entry.path = resolveEntryUrl(baseUrl, href);
        entry.size = size;
        entry.modified = modified;

        // N48：非 http(s) 的 href 不产出条目——条目恒为 http(s) 是凭据注入与剥离
        // 对称成立的前提（今天的绝对 href 被直接采用，跨协议 href 会以条目身份进入
        // 播放与浏览链）。文件与 collection 一律跳过；示例 URL 取剥离后的形态
        // （不含凭据），同层同类只记一条并合并条数，避免畸形报文把警告刷爆
        if (!Util::Url::isHttp(entry.path)) {
            ++skippedNonHttp;
            if (skippedSample.isEmpty())
                skippedSample = entry.path;
            return;
        }

        // 目录自条目跳过：与请求 URL 等价（尾斜杠不计）者不产出。N48：比较在无凭据
        // 形态下进行——条目串已无凭据，请求基 URL 一侧在此剥除后方可比
        const QString baseClean = Util::Url::stripCredentials(
            Util::Url::toNative(baseUrl.toString(QUrl::ComponentFormattingOption::FullyEncoded)));
        if (stripUrlTrailingSlash(entry.path) == stripUrlTrailingSlash(baseClean))
            return;

        // 目录条目补尾斜杠（服务端常省略）：它只作下一跳 PROPFIND 的基 URL，
        // 尾斜杠是"基目录"语义的前提（自条目判定用 stripUrlTrailingSlash，补齐与否
        // 对跳过判定等价）。N44：同语义合并进 normalizeRootUrl——本处入参已是
        // resolveEntryUrl 产出的规范串，结果与原实现逐字相同
        if (isCollection)
            out.collections.push_back(
                {WebDavProvider::normalizeRootUrl(entry.path), entry.size, entry.modified});
        else
            out.files.push_back(std::move(entry));
    };

    while (xml.readNextStartElement()) {
        // 包裹层 multistatus：内层 while 消费其 response 子元素（实测 rclone 等
        // 服务端的标准报文形状）；整树 skip 会连 response 一并吞掉，产出恒为空
        if (isDavElement(xml, QStringLiteral("multistatus"))) {
            while (xml.readNextStartElement()) {
                if (isDavElement(xml, QStringLiteral("response")))
                    parseResponse();
                else
                    xml.skipCurrentElement();
            }
        } else if (isDavElement(xml, QStringLiteral("response"))) {
            // 个别服务端省略包裹层：response 直接出现在文档层
            parseResponse();
        } else {
            xml.skipCurrentElement();
        }
    }
    if (xml.hasError())
        return std::unexpected(trText(
            QT_TRANSLATE_NOOP("WebDavProvider", "Malformed WebDAV response (invalid XML).")));
    // N48：同层同类的跳过合并为一条（示例 URL 取首个，形态已无凭据）——畸形报文
    // 可能带成百上千条跨协议 href，逐条回传会把视图侧的警告弹框刷爆。
    // 文案避开复数语法（无 %n 复数机制，单个 / 多个都读得通）
    if (skippedNonHttp > 0)
        out.warnings << trText(QT_TRANSLATE_NOOP("WebDavProvider",
                                                 "Unsupported URL scheme, skipped: %1 (total %2)"))
                            .arg(skippedSample)
                            .arg(skippedNonHttp);
    return out;
}

QString WebDavProvider::davRequestError(const HttpClient::Response &resp, const QUrl &url) {
    // 可读文案（英文源 + tr()）：HTTP 状态码为主脉络，不透传 HttpClient 的 error。
    // 拼接 URL 供定位——N48：先剥凭据，请求 URL 是带凭据形态（含 401 这一最常见的
    // 失败路径），文案进占位行与警告消息框，凭据不得出现在用户可见文案里
    const QString target = Util::Url::stripCredentials(url.toString(QUrl::FullyEncoded));
    if (resp.canceled)
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Request canceled: %1")).arg(target);
    // 体超限（HttpClient 的 maxBodyBytes）：status 仍是真实状态码（常为 207），
    // 只有 error 非空可判——不透传该文案，另给可读说明
    if (!resp.error.isEmpty() && (resp.status == 200 || resp.status == 207))
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Response too large, skipped: %1"))
            .arg(target);
    if (resp.status == 401)
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Authentication failed (401): %1"))
            .arg(target);
    if (resp.status == 403)
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Access denied (403): %1")).arg(target);
    if (resp.status == 404)
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Not found (404): %1")).arg(target);
    if (resp.status >= 400)
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Server error (HTTP %1): %2"))
            .arg(resp.status)
            .arg(target);
    // 其余带状态码的非预期应答（如重定向未收敛的 3xx）
    if (resp.status > 0)
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Unexpected response (HTTP %1): %2"))
            .arg(resp.status)
            .arg(target);
    // 其余为网络层失败 / 超时 / 体超限未识别：status 恒 0，统一以网络错误表述
    return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Request failed (network error): %1"))
        .arg(target);
}

// N44：本文件定义了 Q_OBJECT 类（WebDavSettingsPage），CMake AUTOMOC 要求显式包含
// 同名 .moc（否则 configure 期直接报错），按源文件名而非类名
#include "WebDavProvider.moc"

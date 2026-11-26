#include "pch.h"

#include "WebDavProvider.h"

#include "Constants.h"
#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "TraversalPolicy.h"
#include "Util.h"
#include "entities/HttpEntity.h"
#include "streams/HttpByteStream.h"
#include "streams/StreamEndpoint.h"
#include "ui/ExtensionFilterEdit.h"

namespace {
// PROPFIND 请求体：RFC 4918 子集，前缀 d: 只是惯例，解析按命名空间匹配
const QByteArray kPropfindBody =
    QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
                      "<d:getcontentlength/><d:getlastmodified/><d:resourcetype/>"
                      "</d:prop></d:propfind>");

const QString kDavNs = QStringLiteral("DAV:");

QString trText(const char *source) { return QCoreApplication::translate("WebDavProvider", source); }

// 三档浏览模式的源文案；与 DirProvider 的选项文案逐字相同，两处不得各自措辞
constexpr char kBrowseLayered[] = QT_TRANSLATE_NOOP("WebDavProvider", "Layered Browsing");
constexpr char kBrowseFilesOnly[] = QT_TRANSLATE_NOOP("WebDavProvider", "Files Only");
constexpr char kBrowseFilesRecursive[] =
    QT_TRANSLATE_NOOP("WebDavProvider", "Files Only (Include Subdirectories)");

bool isDavElement(QXmlStreamReader &xml, const QString &localName) {
    // 命名空间为 DAV:（前缀不可依赖，大小写不敏感）；命名空间缺省（服务端
    // 省略 xmlns 的畸形报文）回退纯 localName 匹配——省略 xmlns 不会被整树
    // skip 吞成"空成功"
    const QString ns = xml.namespaceUri().toString();
    if (!ns.isEmpty() && ns.compare(kDavNs, Qt::CaseInsensitive) != 0)
        return false;
    return xml.name() == localName;
}

// 比较两侧同形（服务端对 collection 的 href 可带可不带尾斜杠）与 title 取名用
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

// openStream：identity（恒无 userinfo）+ 构造期凭据 → insertCredentials。
// 凭据编码与 directPlayUrl 同款（QUrl userInfo(FullyEncoded)）
namespace {
class WebDavStreamEndpoint final : public StreamEndpoint {
  public:
    WebDavStreamEndpoint(const QString &user, const QString &password,
                         std::shared_ptr<ServiceRegistry> registry)
        : m_user(user), m_password(password), m_registry(std::move(registry)) {}

    std::expected<std::shared_ptr<ByteStream>, QString>
    openStream(const QString &identity) const override {
        // 缺注册表 / client 属装配错误，起流失败并记因
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
            target.url = identity;
        // 背压水位取 HttpClient 内置默认
        auto opened = HttpByteStream::open(HttpByteStream::makeFeed(client), target);
        if (!opened)
            return std::unexpected(opened.error());
        return opened.value();
    }

  private:
    QString m_user;
    QString m_password;
    std::shared_ptr<ServiceRegistry> m_registry; // 现取 HttpClient
};
} // namespace

PlaylistProvider::TransitCapability WebDavProvider::transitCapability() const {
    return TransitCapability::Optional;
}

std::shared_ptr<StreamEndpoint> WebDavProvider::createStreamEndpoint() const {
    return std::make_shared<WebDavStreamEndpoint>(m_user, m_password, registry());
}

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
        m_modeCombo->addItem(trText(kBrowseLayered));
        m_modeCombo->addItem(trText(kBrowseFilesOnly));
        m_modeCombo->addItem(trText(kBrowseFilesRecursive));
        form->addRow(tr("Browse Mode"), m_modeCombo);

        m_extFilter = new ExtensionFilterEdit;
        form->addRow(tr("Filter"), m_extFilter);
        auto *extNote =
            new QLabel(tr("Space-separated file extensions; leave empty for no filter"));
        extNote->setEnabled(false);
        form->addRow(extNote);

        layout->addLayout(form);
        enableTransitOption(m_seed ? m_seed->transitPreference() : std::optional<bool>(false));
        // 余量压到内容之下，避免行间距被均摊撑大
        layout->addStretch(1);

        if (m_seed) {
            m_urlEdit->setText(m_seed->rootUrl());
            m_userEdit->setText(m_seed->user());
            m_passwordEdit->setText(m_seed->password());
            m_modeCombo->setCurrentIndex(int(m_seed->browseMode()));
            m_extFilter->setText(m_seed->extensions().join(QLatin1Char(' ')));
        } else {
            m_modeCombo->setCurrentIndex(int(WebDavProvider::BrowseMode::Layered));
            m_extFilter->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        }
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
    QString urlValue() const { return WebDavProvider::normalizeRootUrl(m_urlEdit->text()); }
    QString userValue() const { return m_userEdit->text().trimmed(); }
    // 密码不 trim——首尾空格可能是有效字符
    QString passwordValue() const { return m_passwordEdit->text(); }
    WebDavProvider::BrowseMode modeValue() const {
        return WebDavProvider::browseModeFromInt(m_modeCombo->currentIndex());
    }
    QStringList extensionsValue() const { return Util::parseExtensions(m_extFilter->text()); }

    const WebDavProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QLineEdit *m_urlEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QComboBox *m_modeCombo = nullptr;
    ExtensionFilterEdit *m_extFilter = nullptr;
};

ProviderSettingsPage *createWebDavSettingsPage(const PlaylistProvider *seed,
                                               const std::shared_ptr<ServiceRegistry> &registry,
                                               QWidget *parent) {
    return new WebDavSettingsPage(seed ? static_cast<const WebDavProvider *>(seed) : nullptr,
                                  registry, parent);
}

QString WebDavProvider::typeKey() const { return QLatin1String(kWebDavProviderKey); }

QString WebDavProvider::displayName() {
    return QCoreApplication::translate("WebDavProvider", kWebDavProviderName);
}

// 凭据编码走 QUrl userInfo(FullyEncoded)，插入交 Util::Url::insertCredentials。
// 凭据皆空 / 解析失败 / 非 http(s) / 无 authority 一律恒等返回
QString WebDavProvider::directPlayUrl(const QString &path) const {
    if (m_user.isEmpty() && m_password.isEmpty())
        return path;
    // 先剥凭据再写，兜住形态变化
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
    QString normalized = text.trimmed();
    if (normalized.isEmpty())
        return normalized;
    normalized = Util::Url::toNative(normalized);
    // 尾斜杠补在 path 上：补在串尾会把分隔符塞进 query（"…/dav?token=1/"）
    const qsizetype cut = Util::Url::pathEnd(normalized);
    QString head = normalized.left(cut);
    if (!head.endsWith(QLatin1Char('/')))
        head += QLatin1Char('/');
    return head + normalized.mid(cut);
}

std::optional<QString> WebDavProvider::validateInput(const QString &rawUrl) {
    const QString url = normalizeRootUrl(rawUrl);
    if (url.isEmpty())
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Please enter a directory URL."));
    // 不得内嵌 userinfo；不做连通性探测，失败由异步枚举落占位行
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

// 剥除 userinfo 的无凭据形态，编码形态与尾斜杠原样保留。id 要持久化并作请求
// 基 URL，形态恒定是硬要求；只在确有 userinfo 时清空，避免留下 "http://:@host"
QString WebDavProvider::containerId(const QString &entryPath) {
    QUrl url(entryPath, QUrl::ParsingMode::TolerantMode);
    if (!url.userInfo().isEmpty()) {
        url.setUserName(QString());
        url.setPassword(QString());
    }
    return Util::Url::toNative(url.toString(QUrl::ComponentFormattingOption::FullyEncoded));
}

// 预切分数字段后按下标排序：比较器里重复 segmentName 会把 O(n) 摊成 O(n log n)
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

// 在 QtConcurrent 任务内同步推进：不设取消通道，孤儿任务跑完整轮、结果按代际丢弃。
// 两档平铺收到非空 parent 属调用方违约（Debug 断言，Release 返回空结果）
std::expected<PlaylistProvider::Enumeration, QString>
WebDavProvider::enumerate(const QStringList &parent) const {
    // 启动期必已 provide，断言守候 + fail-safe
    const std::shared_ptr<HttpClient> client =
        registry() ? registry()->resolve<HttpClient>() : nullptr;
    if (!client) {
        Q_ASSERT(false);
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("WebDavProvider", "HTTP client is not available.")));
    }
    if (!parent.isEmpty() && m_browseMode != BrowseMode::Layered) {
        Q_ASSERT(false);
        return Enumeration{};
    }

    // parent 末段即本层基 URL；id 与 rootUrl 都是持久化数据，不做归属校验
    const QString baseText = parent.isEmpty() ? m_rootUrl : parent.last();
    QUrl rootUrl = QUrl(baseText, QUrl::ParsingMode::TolerantMode);
    if (!rootUrl.isValid() || !Util::Url::isNetworkStream(baseText))
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("WebDavProvider", "Invalid WebDAV directory URL: %1"))
                .arg(baseText));
    // 任务 URL 恒无凭据，注入只作用于请求副本；漏注入即该次请求 401
    auto requestUrlOf = [this](const QUrl &taskUrl) {
        QUrl req = taskUrl;
        if (!m_user.isEmpty() || !m_password.isEmpty()) {
            req.setUserName(m_user);
            req.setPassword(m_password);
        }
        return req;
    };

    // 扩展名按显示名后缀过滤；size / modified 皆缺则不注入。取名经 fileName 剥 query
    // 与解码——判定是纯字符串运算，喂原始 URL 会误判 %2E 的点与带 query 的条目
    auto appendFileEntities = [this](const QList<DavEntry> &files, EntityList &entities) {
        for (const DavEntry &entry : files) {
            if (!Util::passesExtensionFilter(Util::Url::fileName(entry.path), m_extensions))
                continue;
            auto entity = std::make_shared<HttpEntity>(entry.path, registry());
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
        // 深度上限取策略快照；resolve 为空按缺省兜底
        int maxDepth = App::recursiveDepthLimitDefault();
        if (const auto policy = registry()->resolve<TraversalPolicy>())
            maxDepth = policy->depthLimit;

        // 请求 URL 即基 URL：尾斜杠是"基目录"语义的前提，否则 resolved 会把末段
        // 当文件名替换掉
        struct DavTask {
            QUrl url;
            int depth;
        };
        QList<DavTask> stack;
        stack.append({rootUrl, 0});
        // 环与同目录多路径的剪枝依据；根先入集，命中静默跳过
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
            // 同步阻塞，仅任务线程调用
            const HttpClient::Response resp = client->exec(req);

            // error 非空亦判失败：体超限时 status 可能仍是 207 而 body 为空，只看
            // 状态码会把超限目录当成空目录静默吞掉
            const bool ok = (resp.status == 207 || resp.status == 200) && !resp.canceled &&
                            resp.error.isEmpty();
            if (!ok) {
                const QString message = davRequestError(resp, task.url);
                if (task.depth == 0)
                    return std::unexpected(message);
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
            // 本层跳过告警向上汇总
            warnings << listing->warnings;

            // 超深静默截断（配置使然）；已访问目录静默跳过
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
        Enumeration out;
        out.entities = std::move(entities);
        out.warnings = std::move(warnings);
        return out;
    }

    // 单次 PROPFIND，逐层懒加载；层缓存由视图承担
    const QUrl reqUrl = requestUrlOf(rootUrl);
    HttpClient::Request req;
    req.url = reqUrl;
    req.method = QByteArrayLiteral("PROPFIND");
    req.body = kPropfindBody;
    req.contentType = QByteArrayLiteral("application/xml");
    req.headers.append({QByteArrayLiteral("Depth"), QByteArrayLiteral("1")});
    // 同步阻塞，仅任务线程调用
    const HttpClient::Response resp = client->exec(req);

    const bool ok =
        (resp.status == 207 || resp.status == 200) && !resp.canceled && resp.error.isEmpty();
    if (!ok)
        return std::unexpected(davRequestError(resp, reqUrl));

    const auto listing = parseMultistatus(resp.body, reqUrl);
    if (!listing)
        return std::unexpected(listing.error());

    Enumeration out;
    out.warnings = listing->warnings;
    if (m_browseMode == BrowseMode::Layered) {
        // size 恒未知：getcontentlength 对 collection 语义未定义，按大小排序时容器
        // 组退化为保持初始序
        out.containers.reserve(listing->collections.size());
        for (const DavEntry &dir : listing->collections) {
            Container c;
            c.id = containerId(dir.path);
            c.path = c.id; // 与 id 同串：身份与展示语义正交
            // collection 身份串恒以尾斜杠结尾，Util::Url::fileName 对以 / 结尾的 URL
            // 返回空串，须先剥尾斜杠再取解码名
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
    writeProviderKey(settings); // 类型键经基类取 typeKey()
    settings.setValue(QStringLiteral("davUrl"), m_rootUrl);
    settings.setValue(QStringLiteral("davUser"), m_user);
    settings.setValue(QStringLiteral("davPassword"), m_password);
    settings.setValue(QStringLiteral("davBrowseMode"), int(m_browseMode));
    settings.setValue(QStringLiteral("davExtensions"), m_extensions);
    // 含 None：显式记录用户未选择规则
    settings.setValue(QStringLiteral("sortKey"), int(sortRule().key));
    settings.setValue(QStringLiteral("sortAscending"), sortRule().ascending);
}

std::shared_ptr<PlaylistProvider>
WebDavProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    auto provider = std::make_shared<WebDavProvider>(
        settings.value(QStringLiteral("davUrl")).toString(),
        settings.value(QStringLiteral("davUser")).toString(),
        settings.value(QStringLiteral("davPassword")).toString(),
        browseModeFromInt(settings.value(QStringLiteral("davBrowseMode"), 0).toInt()),
        settings.value(QStringLiteral("davExtensions")).toStringList(), registry);
    // 缺键落 {None, 升序}
    const SortRule rule{static_cast<SortKey>(settings.value(QStringLiteral("sortKey"), 0).toInt()),
                        settings.value(QStringLiteral("sortAscending"), true).toBool()};
    provider->setSortRule(rule);
    return provider;
}

QString WebDavProvider::resolveEntryUrl(const QUrl &baseUrl, const QString &href) {
    // 绝对 href 直接采用，相对 href 按基解析（基的尾斜杠让"基目录"语义成立）
    QUrl resolved = baseUrl.resolved(QUrl(href, QUrl::ParsingMode::TolerantMode));
    // 恒剥 userinfo：相对 href 继承自基请求，绝对 href 来自自身，两者一并剥。
    // 不得无条件 setUserName("")：本无 userinfo 时会把该段标记为 present 且空，
    // 产出 "http://:@host/…"，故仅在确有 userinfo 时清空
    if (!resolved.userInfo().isEmpty()) {
        resolved.setUserName(QString());
        resolved.setPassword(QString());
    }
    // 此后身份串全链路只读转发，凭据只在请求与播放时注入
    return Util::Url::toNative(resolved.toString(QUrl::ComponentFormattingOption::FullyEncoded));
}

std::expected<WebDavProvider::DavListing, QString>
WebDavProvider::parseMultistatus(const QByteArray &body, const QUrl &baseUrl) {
    DavListing out;
    QXmlStreamReader xml(body);
    // readNextStartElement 遇结束元素即返回 false，故每层 while 只消费本层子元素，
    // 包裹层交下一层递归消化；绝不跨层 continue
    int skippedNonHttp = 0;
    QString skippedSample;
    auto parseProp = [&](bool &isCollection, std::optional<qint64> &size,
                         std::optional<QDateTime> &modified) {
        while (xml.readNextStartElement()) {
            if (isDavElement(xml, QStringLiteral("resourcetype"))) {
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
                // invalid 不置 optional，等价于属性缺失
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
                xml.skipCurrentElement();
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

        // 畸形 response：忽略，不记 warning
        if (href.isEmpty())
            return;

        DavEntry entry;
        entry.path = resolveEntryUrl(baseUrl, href);
        entry.size = size;
        entry.modified = modified;

        // 条目恒为 http(s) 是凭据注入与剥离对称成立的前提，跨协议 href 一律跳过；
        // 同类只记一条并合并条数
        if (!Util::Url::isHttp(entry.path)) {
            ++skippedNonHttp;
            if (skippedSample.isEmpty())
                skippedSample = entry.path;
            return;
        }

        // 与请求 URL 等价（尾斜杠不计）的自条目跳过
        const QString baseClean = Util::Url::stripCredentials(
            Util::Url::toNative(baseUrl.toString(QUrl::ComponentFormattingOption::FullyEncoded)));
        if (stripUrlTrailingSlash(entry.path) == stripUrlTrailingSlash(baseClean))
            return;

        // 目录条目补尾斜杠：它只作下一跳 PROPFIND 的基 URL
        if (isCollection)
            out.collections.push_back(
                {WebDavProvider::normalizeRootUrl(entry.path), entry.size, entry.modified});
        else
            out.files.push_back(std::move(entry));
    };

    while (xml.readNextStartElement()) {
        // 内层 while 消费 response 子元素；整树 skip 会把 response 一并吞掉
        if (isDavElement(xml, QStringLiteral("multistatus"))) {
            while (xml.readNextStartElement()) {
                if (isDavElement(xml, QStringLiteral("response")))
                    parseResponse();
                else
                    xml.skipCurrentElement();
            }
        } else if (isDavElement(xml, QStringLiteral("response"))) {
            // 个别服务端省略包裹层
            parseResponse();
        } else {
            xml.skipCurrentElement();
        }
    }
    if (xml.hasError())
        return std::unexpected(trText(
            QT_TRANSLATE_NOOP("WebDavProvider", "Malformed WebDAV response (invalid XML).")));
    // 畸形报文可能带大量跨协议 href，逐条回传会刷爆警告；示例取首个，形态已无凭据
    if (skippedNonHttp > 0)
        out.warnings << trText(QT_TRANSLATE_NOOP("WebDavProvider",
                                                 "Unsupported URL scheme, skipped: %1 (total %2)"))
                            .arg(skippedSample)
                            .arg(skippedNonHttp);
    return out;
}

QString WebDavProvider::davRequestError(const HttpClient::Response &resp, const QUrl &url) {
    // 不透传 HttpClient 的 error；先剥凭据：401 是最常见失败路径，凭据不得进
    // 用户可见文案
    const QString target = Util::Url::stripCredentials(url.toString(QUrl::FullyEncoded));
    if (resp.canceled)
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Request canceled: %1")).arg(target);
    // 体超限：status 仍是真实状态码（常为 207），只有 error 非空可判
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
    if (resp.status > 0)
        return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Unexpected response (HTTP %1): %2"))
            .arg(resp.status)
            .arg(target);
    return trText(QT_TRANSLATE_NOOP("WebDavProvider", "Request failed (network error): %1"))
        .arg(target);
}

// 本文件定义 Q_OBJECT 类（WebDavSettingsPage），AUTOMOC 要求显式包含同名 .moc
#include "WebDavProvider.moc"

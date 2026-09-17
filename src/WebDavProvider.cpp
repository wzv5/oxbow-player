#include "pch.h"

#include "WebDavProvider.h"

#include "Constants.h"
#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
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

bool isDavElement(QXmlStreamReader &xml, const QString &localName) {
    // 命名空间为 DAV:（前缀不可依赖，大小写不敏感）；命名空间缺省（服务端
    // 省略 xmlns 的畸形报文）回退纯 localName 匹配——不同命名空间仍不误匹配，
    // 省略 xmlns 也不会被整树 skip 吞成"空成功"
    const QString ns = xml.namespaceUri().toString();
    if (!ns.isEmpty() && ns.compare(kDavNs, Qt::CaseInsensitive) != 0)
        return false;
    return xml.name() == localName;
}

// 目录自条目判定：与请求 URL 逐段等价（尾斜杠不计——服务端对 collection 的
// href 可带可不带尾斜杠）。比较 FullyEncoded 串形态，两侧同源同形
QString stripTrailingSlash(QString s) {
    while (s.endsWith(QLatin1Char('/')))
        s.chop(1);
    return s;
}

} // namespace

WebDavProvider::WebDavProvider(const QString &rootUrl, const QString &user, const QString &password,
                               bool recursive, const QStringList &extensions,
                               const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_rootUrl(rootUrl), m_user(user), m_password(password),
      m_recursive(recursive), m_extensions(extensions) {}

// WebDAV 目录的参数页（N44 §2.5.3）：URL 行 + 用户名行 + 密码行 + 递归 + 扩展名行，
// 内容与重构前的 WebDAV 参数区逐项一致；原 16px 左缩进取消（四行布局下参数区
// 不再挂在某个单选之下）。页面类只在本文件可见，经 createWebDavSettingsPage 产出
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

        m_recursiveCheck = new QCheckBox(tr("Include Subdirectories"));
        form->addRow(m_recursiveCheck); // 单控件行：占第 1 列起、跨两列

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
        // 同目录页：把被拉伸出来的余量压到内容之下，避免行间距被均摊撑大
        layout->addStretch(1);

        // 回填：rootUrl 已含尾斜杠，URL 框原样回显；无回显来源（新建 / 换类型）
        // 时扩展名框预填全局默认清单
        if (m_seed) {
            m_urlEdit->setText(m_seed->rootUrl());
            m_userEdit->setText(m_seed->user());
            m_passwordEdit->setText(m_seed->password());
            m_recursiveCheck->setChecked(m_seed->recursive());
            m_extensionsEdit->setText(m_seed->extensions().join(QLatin1Char(' ')));
        } else {
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
               passwordValue() != m_seed->password() ||
               m_recursiveCheck->isChecked() != m_seed->recursive() ||
               extensionsValue() != m_seed->extensions();
    }

    std::shared_ptr<PlaylistProvider> build() const override {
        return std::make_shared<WebDavProvider>(urlValue(), userValue(), passwordValue(),
                                                m_recursiveCheck->isChecked(), extensionsValue(),
                                                m_registry);
    }

  private:
    // 取规范化参数值（hasChanges 与 build 共用同一出口）
    QString urlValue() const { return WebDavProvider::normalizeRootUrl(m_urlEdit->text()); }
    QString userValue() const { return m_userEdit->text().trimmed(); }
    // 密码不 trim——首尾空格可能是有效字符
    QString passwordValue() const { return m_passwordEdit->text(); }
    QStringList extensionsValue() const {
        return WebDavProvider::parseExtensions(m_extensionsEdit->text());
    }

    const WebDavProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QLineEdit *m_urlEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QCheckBox *m_recursiveCheck = nullptr;
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

QString WebDavProvider::normalizeRootUrl(const QString &text) {
    // URL 的来源处（N37 §3.1 第 1 条）：trim → 经 Util::nativePath 规范化（网络分支）
    // → 补尾斜杠
    QString normalized = text.trimmed();
    if (normalized.isEmpty())
        return normalized;
    normalized = Util::nativePath(normalized);
    // 尾斜杠补在 path 上：基目录语义只对路径成立，直接补在串尾会把分隔符塞进
    // query（"…/dav?token=1/"）。query / fragment 原样保留在后
    const qsizetype q = normalized.indexOf(QLatin1Char('?'));
    const qsizetype f = normalized.indexOf(QLatin1Char('#'));
    qsizetype cut = normalized.size();
    if (q >= 0)
        cut = qMin(cut, q);
    if (f >= 0)
        cut = qMin(cut, f);
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

// 枚举：N32 统一后的 QtConcurrent 任务内逐层 DFS 顺序推进（在途至多 1 条请求，
// 顺序推进不设取消通道——孤儿任务跑完整轮 DFS，结果按代际身份静默丢弃）。
// 失败语义（§3.5）：根请求失败 → 整次 unexpected 进占位；子目录请求失败 →
// 记 warning 并跳过该子树，其余照常返回；单目录体超 maxBodyBytes 同走 warning
std::expected<PlaylistProvider::Enumeration, QString> WebDavProvider::enumerate() const {
    // 装配错误：HttpClient 启动期必已 provide（N35 §2.3），永不命中；
    // 断言守候 + fail-safe（枚举不发请求即整次失败）
    const std::shared_ptr<HttpClient> client =
        registry() ? registry()->resolve<HttpClient>() : nullptr;
    if (!client) {
        Q_ASSERT(false);
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("WebDavProvider", "HTTP client is not available.")));
    }
    // 深度上限：注册表策略快照取用（GUI 线程装配、任务线程只读，N36 修订 1）；
    // resolve 为空按缺省兜底（同 DirProvider，策略只读且缺省值立即可用）
    int maxDepth = App::recursiveDepthLimitDefault();
    if (const auto policy = registry()->resolve<TraversalPolicy>())
        maxDepth = policy->depthLimit;

    // 请求基 URL：rootUrl 写入凭据（两者皆空则不设）——凭据随 URL 走，经 N34 §2.5
    // 的"URL userinfo 优先"生效（Basic 预置头省一次 401 往返，Digest / NTLM 由
    // authenticationRequired 兜底）。漏写即请求无凭据，401 在根请求直接失败
    QUrl rootUrl = QUrl(m_rootUrl, QUrl::ParsingMode::TolerantMode);
    if (!rootUrl.isValid() || !Util::isNetworkStreamPath(m_rootUrl))
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("WebDavProvider", "Invalid WebDAV directory URL: %1"))
                .arg(m_rootUrl));
    if (!m_user.isEmpty() || !m_password.isEmpty()) {
        rootUrl.setUserName(m_user);
        rootUrl.setPassword(m_password);
    }

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
    visited.insert(Util::normalizePathForMatch(
        rootUrl.toString(QUrl::ComponentFormattingOption::FullyEncoded)));

    EntityList entities;
    QStringList warnings;
    while (!stack.isEmpty()) {
        const DavTask task = stack.takeLast();
        HttpClient::Request req;
        req.url = task.url; // 绝对 URL：与注册表 client 的缺省 baseUrl 解绑
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
        const bool ok =
            (resp.status == 207 || resp.status == 200) && !resp.canceled && resp.error.isEmpty();
        if (!ok) {
            const QString message = davRequestError(resp, task.url);
            if (task.depth == 0)
                return std::unexpected(message); // 根失败：整次 unexpected 进占位
            warnings << trText(QT_TRANSLATE_NOOP("WebDavProvider",
                                                 "Subdirectory enumeration failed, skipped: %1"))
                            .arg(message);
            continue;
        }

        const auto listing = parseMultistatus(resp.body, task.url, m_user, m_password);
        if (!listing) {
            // 解析失败与请求失败同语义：根即整次失败，子目录记 warning 跳过
            if (task.depth == 0)
                return std::unexpected(listing.error());
            warnings << trText(QT_TRANSLATE_NOOP("WebDavProvider",
                                                 "Subdirectory enumeration failed, skipped: %1"))
                            .arg(listing.error());
            continue;
        }

        // 子目录：depth + 1 > maxDepth 即静默截断不再深入（截断是配置使然，
        // 不是错误，不记 warning——与 N36 §2.2 同口径）；已访问过的目录静默跳过
        if (m_recursive) {
            for (const DavEntry &dir : listing->collections) {
                if (task.depth + 1 > maxDepth)
                    continue;
                const QString key = Util::normalizePathForMatch(dir.playUrl);
                if (visited.contains(key))
                    continue;
                visited.insert(key);
                stack.append({QUrl(dir.playUrl), task.depth + 1});
            }
        }

        // 文件条目：扩展名过滤（取显示名的后缀，大小写不敏感；空清单 = 不过滤）
        // → 造 WebDavEntity（具备文件级元数据能力）→ 已取得的 size / modified
        // 经注入口零成本写入（零额外请求）。两者皆缺不注入，即值未知
        for (const DavEntry &entry : listing->files) {
            if (!m_extensions.isEmpty()) {
                // 经 Util::playUrlFileName 单次解码（剥 query / fragment → 末段 →
                // decode）。不得走 QUrl::path()——它默认返回已解码形态，再解一次
                // 会让字面 "%252F" 误解为分隔符
                const QString name = Util::playUrlFileName(entry.playUrl);
                const int dot = name.lastIndexOf(QLatin1Char('.'));
                if (dot < 0)
                    continue;
                if (!m_extensions.contains(name.mid(dot + 1).toLower()))
                    continue;
            }
            // N39：直接构造 WebDavEntity，不经工厂——工厂对 http(s) 只产无元数据
            // 能力的 HttpEntity（收编 / 静态表重载的落点）。二者对网络路径的
            // 加工等价（resolveLocalPath 对网络路径原样返回），故绕过工厂无损
            auto entity = std::make_shared<WebDavEntity>(entry.playUrl, registry());
            if (entry.size || entry.modified) {
                FileMeta fm;
                fm.size = entry.size;
                fm.modified = entry.modified;
                injectFileMeta(*entity, fm);
            }
            entities.push_back(std::move(entity));
        }
    }
    return Enumeration{std::move(entities), std::move(warnings)};
}

bool WebDavProvider::supportsModify() const { return false; }

bool WebDavProvider::supportsSort() const { return true; }

void WebDavProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // N44：类型键经基类取 typeKey()，不再写字面量
    settings.setValue(QStringLiteral("davUrl"), m_rootUrl);
    settings.setValue(QStringLiteral("davUser"), m_user);
    settings.setValue(QStringLiteral("davPassword"), m_password);
    settings.setValue(QStringLiteral("davRecursive"), m_recursive);
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
        settings.value(QStringLiteral("davRecursive")).toBool(),
        settings.value(QStringLiteral("davExtensions")).toStringList(), registry);
    provider->setSortRule(settings.value(QStringLiteral("sortKey"), 0).toInt(),
                          settings.value(QStringLiteral("sortAscending"), true).toBool());
    return provider;
}

QString WebDavProvider::resolveEntryUrl(const QUrl &baseUrl, const QString &href,
                                        const QString &user, const QString &password) {
    // href → 条目 URL：绝对 href 直接采用，相对 href 按基解析（基的尾斜杠让
    // "基目录"语义成立）。URL 的解析与形态交 QUrl 与 nativePath，不自行拼串
    QUrl resolved = baseUrl.resolved(QUrl(href, QUrl::ParsingMode::TolerantMode));
    // 凭据重写：绝对 href 不继承基 userinfo，凭据来源必须唯一（皆空即无凭据，
    // 恶意 / 畸形报文夹带的凭据不进 playUrl）。
    // 注意不得无条件 setUserName("") / setPassword("")——若 href 本无 userinfo，
    // 空串赋值会把该段标记为 present 且空，序列化产出 "http://:@host/…" 这种
    // 既非标准也不简洁的形态（N37 实施修订：严格规范化只落在 key，playUrl 只求
    // 标准与简洁）。故：有凭据即写入；皆空时**仅在 href 自带 userinfo 时**才以
    // 空串赋值剥除（对已有 userinfo 的 URL，空串赋值是清空、不留空段——既有用例
    // absoluteHrefCredentialsStrippedWhenNoCreds 已实证该 Qt 行为）
    if (!user.isEmpty() || !password.isEmpty()) {
        resolved.setUserName(user);
        resolved.setPassword(password);
    } else if (!resolved.userInfo().isEmpty()) {
        resolved.setUserName(QString());
        resolved.setPassword(QString());
    }
    // 经 Util::nativePath 收口取编码形态输出——即"入口处构造一次"（§3.1 第 1 条）；
    // 此后 playUrl 全链路只读转发
    return Util::nativePath(resolved.toString(QUrl::ComponentFormattingOption::FullyEncoded));
}

std::expected<WebDavProvider::DavListing, QString>
WebDavProvider::parseMultistatus(const QByteArray &body, const QUrl &baseUrl, const QString &user,
                                 const QString &password) {
    DavListing out;
    QXmlStreamReader xml(body);
    // 层级化解析（readNextStartElement 的语义是"读到任一结束元素即返回 false"，
    // 故每层 while 只消费自己这一层的子元素，包裹层交下一层的 while 递归消化；
    // 绝不跨层 continue——跨层会让循环在子元素的 </...> 处提前退出）。
    // 前置约定：各辅助 lambda 被调用时，reader 停在对应元素的起始标签上；
    // 返回时其结束标签已被消费。
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
        entry.playUrl = resolveEntryUrl(baseUrl, href, user, password);
        entry.size = size;
        entry.modified = modified;

        // 目录自条目跳过：与请求 URL 等价（尾斜杠不计）者不产出
        if (stripTrailingSlash(entry.playUrl) ==
            stripTrailingSlash(
                Util::nativePath(baseUrl.toString(QUrl::ComponentFormattingOption::FullyEncoded))))
            return;

        // 目录条目补尾斜杠（服务端常省略）：它只作下一跳 PROPFIND 的基 URL，
        // 尾斜杠是"基目录"语义的前提（自条目判定用 stripTrailingSlash，补齐与否
        // 对跳过判定等价）。N44：同语义合并进 normalizeRootUrl——本处入参已是
        // resolveEntryUrl 产出的规范串，结果与原实现逐字相同
        if (isCollection)
            out.collections.push_back(
                {WebDavProvider::normalizeRootUrl(entry.playUrl), entry.size, entry.modified});
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
    return out;
}

QString WebDavProvider::davRequestError(const HttpClient::Response &resp, const QUrl &url) {
    // 可读文案（英文源 + tr()）：HTTP 状态码为主脉络，不透传 HttpClient 的 error。
    // 拼接 URL 供定位（含 userinfo 的完整形态，与其余 UI 呈现同款取舍）
    const QString target = url.toString(QUrl::FullyEncoded);
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

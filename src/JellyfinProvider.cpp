#include "pch.h"

#include "JellyfinProvider.h"

#include "Constants.h"
#include "HttpByteStream.h"
#include "JellyfinEntity.h"
#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "StreamEndpoint.h"
#include "Util.h"

namespace {
// 翻译取值（上下文归属本提供者，与 WebDavProvider::trText 同款）
QString trText(const char *source) {
    return QCoreApplication::translate("JellyfinProvider", source);
}

// 授权头与 UA 用的固定客户端标识（协议取值，非 UI 文案，不翻译）
constexpr char kClientName[] = "mpv-qt";
constexpr char kClientVersion[] = "1.0";

// 枚举请求显式元数据字段（N69 背景：默认不返回，须逐请求声明）。Path 为原始
// 文件路径——原始文件名与原始后缀的唯一可靠来源（N69 变更记录 5：扩展名过滤；
// N70：文件名注入）
constexpr char kItemFields[] = "MediaSources,DateCreated,PremiereDate,Path";

// 文件路径的末段 = 服务器上的原始文件名（N70：文件名注入的唯一来源）。服务器路径
// 分隔符随服务器操作系统而异，'/' 与 '\' 都作边界；路径为空返回空串
QString extractFileName(const QString &path) {
    if (path.isEmpty())
        return {};
    const qsizetype sep =
        qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return sep >= 0 ? path.mid(sep + 1) : path;
}

// 文件路径的原始后缀（无点小写；取不到为空串）。末段切分复用 extractFileName，
// 同一运算不留两份拷贝
QString extractSuffix(const QString &path) {
    const QString name = extractFileName(path);
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return {}; // 无点或点在首字符（隐藏文件形态）→ 无有效后缀
    return name.mid(dot + 1).toLower();
}

// 叶子显示名 = 集号前缀 + 服务端标题（N72）：仅 Episode 叶子生效，其余类型原样
// 返回服务端标题（音乐库的 Audio 带曲目号语义的 indexNumber，判据取 type 而非
// "有集号就加"）。前缀按"有则写、无则省"拼装——季号非空拼 S%02dE%02d - ，季号
// 空退化为 E%02d - （保留 E 标记，不与列表行首恒画的行号混淆），集号空不加任何
// 前缀；数字至少两位、不足补零、超出原样。字段缺位判据是 toVariant().isValid()
// ——直接 toLongLong() 会把 JSON null 落成 0，把"未知"显示成"第 0 季 / 第 0 集"
// （与下方 size 注入处的保护同口径）。name 为空原样返回（空则不注，落 URN 派生
// 占位名，不凑前缀造名）
QString episodeDisplayName(const QString &type, const QString &name, const QJsonObject &obj) {
    if (type != QLatin1String("episode") || name.isEmpty())
        return name;
    const QVariant season = obj.value(QLatin1String("parentIndexNumber")).toVariant();
    const QVariant episode = obj.value(QLatin1String("indexNumber")).toVariant();
    if (!episode.isValid())
        return name;
    const auto padded = [](const QVariant &v) {
        return QString::number(v.toLongLong()).rightJustified(2, QLatin1Char('0'));
    };
    if (!season.isValid())
        return QStringLiteral("E%1 - %2").arg(padded(episode), name);
    return QStringLiteral("S%1E%2 - %3").arg(padded(season), padded(episode), name);
}

// ISO 8601 → QDateTime（空 / 解析失败返回无效 QDateTime 即"缺失"，与
// MediaMeta::recordedDate 的缺位语义同形）。服务器对缺失日期序列化为 .NET
// DateTime.MinValue（0001-01-01，实测复现），解析成功但年份 ≤1 视作缺位。
// 服务器恒返回带时区偏移的 ISO 形态，Qt::ISODate 直接解析
QDateTime parseIsoDate(const QString &text) {
    if (text.isEmpty())
        return {};
    const QDateTime dt = QDateTime::fromString(text, Qt::ISODate); // 无效即缺位
    if (!dt.isValid() || dt.date().year() <= 1)
        return {};
    return dt;
}

// /Items 与 /UserViews 共用的响应形状：{"items":[…], "totalRecordCount":n}。
// 解析失败（非 JSON / 缺 items 数组）返回 unexpected。键一律 camelCase——请求
// 恒带 Accept profile="CamelCase"，服务器响应恒为 camelCase（实测口径；键大小写
// 与之不符即静默读空，N69 变更记录 1 的教训）
std::expected<QJsonArray, QString> parseItemsResponse(const QByteArray &body) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Invalid JSON response from server.")));
    const QJsonArray items = doc.object().value(QLatin1String("items")).toArray();
    if (items.isEmpty() && !doc.object().contains(QLatin1String("items")))
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Invalid JSON response from server.")));
    return items;
}

// HTTP 状态码与网络失败 → 可读文案（英文源 + tr()，不透传 HttpClient 的 error）。
// 文件内自由函数：提供者枚举与授权流（对话框 / 测试任务）共用同一出口
QString apiRequestError(const HttpClient::Response &resp) {
    if (resp.canceled)
        return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Request canceled."));
    if (!resp.error.isEmpty())
        return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Network error: %1")).arg(resp.error);
    return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Server returned HTTP %1."))
        .arg(resp.status);
}

// Authorization 头装配（枚举请求与取流请求共用同一点定义）：MediaBrowser 形态，
// token 非空才携带 Token 段。12.x 只认标准 Authorization 头（旧
// X-Emby-Authorization 已被忽略，实测口径）。token 只进请求头，不进 URL、
// 不进实体、不落盘
QByteArray authorizationHeaderValue(const QString &token) {
    QString auth = QStringLiteral("MediaBrowser Client=\"%1\", Device=\"%1\", DeviceId=\"%1\", "
                                  "Version=\"%2\"")
                       .arg(QLatin1String(kClientName), QLatin1String(kClientVersion));
    if (!token.isEmpty())
        auth += QStringLiteral(", Token=\"%1\"").arg(token);
    return auth.toUtf8();
}

// 请求装配内核（提供者成员与授权流任务共用）：基 URL + API 路径拼请求 URL，
// 统一注入 Authorization 与 Accept CamelCase 两头。path 以 '/' 起始
// （如 "/UserViews"）；body 非空时按 JSON 交代。名字与成员 makeApiRequest
// 刻意错开——类作用域名字查找会遮蔽同名自由函数
HttpClient::Request assembleApiRequest(const QString &baseUrl, const QString &token,
                                       const QString &path, const QByteArray &method,
                                       const QByteArray &body = {}) {
    HttpClient::Request req;
    req.url = QUrl(baseUrl + path, QUrl::ParsingMode::TolerantMode);
    req.method = method;
    req.body = body;
    if (!body.isEmpty())
        req.contentType = QByteArrayLiteral("application/json");
    req.headers.append({QByteArrayLiteral("Authorization"), authorizationHeaderValue(token)});
    // 固定 camelCase 响应，不做大小写兼容层（实测口径）
    req.headers.append({QByteArrayLiteral("Accept"),
                        QByteArrayLiteral("application/json; profile=\"CamelCase\"")});
    return req;
}

// ——N69 授权流（快速连接两阶段任务与页面测试任务共用的传输值）——
// error 非空即失败（可读英文文案）；canceled 语义由调用方按"结果整体丢弃"处理
// （error 与各凭据字段皆空即取消）
struct JellyfinAuthResult {
    QString serverId;    // 阶段 1：/System/Info/Public 的 Id
    QString code;        // 阶段 1：6 位授权码
    QString secret;      // 阶段 1：轮询句柄
    QString accessToken; // 阶段 2：授权完成的 token
    QString userId;      // 阶段 2：授权用户 id
    QString error;
};

// 任务线程内可取消执行：cancel 置位即 abort 在途请求。返回后调用方先查 cancel
// 再查 error——canceled 响应的字段无意义
HttpClient::Response execCancelable(const HttpClient &client, const HttpClient::Request &req,
                                    const std::atomic_bool &cancel) {
    const auto id = client.submit(req);
    while (!HttpClient::wait(id, 100)) {
        if (cancel) {
            HttpClient::abort(id);
            break;
        }
    }
    return HttpClient::take(id);
}

// 任务线程内可取消休眠：false = 期间被取消
bool sleepCancelable(int ms, const std::atomic_bool &cancel) {
    constexpr int kSliceMs = 100;
    for (int waited = 0; waited < ms; waited += kSliceMs) {
        if (cancel)
            return false;
        QThread::msleep(kSliceMs);
    }
    return true;
}

// ——N69：Jellyfin 的来源解析点（N52 L2，转接回调内的直连取流策略）——
// openStream 收到的 identity 是协议层 unwrap 的产出——
// jellyfin://<serverId>/<type>/<itemId>。取流 URL 一步现拼：全部类型统一走
// /Items/{id}/Download（原始文件端点——/Images/Original 照片通路实测打不开，
// 维护者裁决统一改道，变更记录 9）。类型段当前不参与取流分派，保留在身份中
// 作将来按类型分流的预留位（转码 PlaybackInfo、Images 形态等）。无签名参数、
// 无转码会话参数，token 经 Authorization 头携带。Range / 206 / seek 映射由
// HttpByteStream 承担。
// 契约（N52 §2.3）：构造后不可变、不含 QObject、任意线程可析构、不持实体 / 视图指针
class JellyfinStreamEndpoint final : public StreamEndpoint {
  public:
    JellyfinStreamEndpoint(QString serverUrl, QString serverId, QString token,
                           std::shared_ptr<ServiceRegistry> registry)
        : m_serverUrl(std::move(serverUrl)), m_serverId(std::move(serverId)),
          m_token(std::move(token)), m_registry(std::move(registry)) {}

    std::expected<std::shared_ptr<ByteStream>, QString>
    openStream(const QString &identity) const override {
        // 拆身份：serverId 须与本端点绑定的服务器一致（身份由本提供者铸造，
        // 不一致属编程错误——防御性失败，不静默错播另一台服务器的内容）
        const QString scheme = QString::fromLatin1(kJellyfinScheme);
        if (!identity.startsWith(scheme))
            return std::unexpected(QStringLiteral("identity is not a Jellyfin URN"));
        const QStringList segs = identity.mid(scheme.size()).split(QLatin1Char('/'));
        if (segs.size() != 3 || segs.at(0) != m_serverId || segs.at(1).isEmpty() ||
            segs.at(2).isEmpty())
            return std::unexpected(QStringLiteral("malformed Jellyfin identity"));

        // 现取 client（resolve 的副本随流存活，N35 既有线程契约）；缺注册表 / 缺
        // client 属装配错误，起流失败并记因，不静默
        const auto client = m_registry ? m_registry->resolve<HttpClient>() : nullptr;
        if (!client)
            return std::unexpected(QStringLiteral("http client unavailable"));

        HttpStreamTarget target;
        target.url = m_serverUrl + QStringLiteral("/Items/%1/Download").arg(segs.at(2));
        // Authorization 头与枚举请求同一形态（token 不进 URL）；Accept 头对
        // 原始文件流无意义，不加。HttpStreamTarget::headers 是 QString 对，
        // 装配点出口（authorizationHeaderValue）恒 ASCII，fromUtf8 无损
        target.headers.append({QStringLiteral("Authorization"),
                               QString::fromUtf8(authorizationHeaderValue(m_token))});
        // 背压水位取 HttpClient 内置默认（N51 实测口径维持）
        auto opened = HttpByteStream::open(HttpByteStream::makeFeed(client), target);
        if (!opened)
            return std::unexpected(opened.error());
        return opened.value(); // shared_ptr<HttpByteStream> → shared_ptr<ByteStream>
    }

  private:
    QString m_serverUrl;                         // 构造后不可变（N52 §2.3 契约 5）
    QString m_serverId;                          // 身份归属校验用
    QString m_token;                             // 取流凭据（只进请求头）
    std::shared_ptr<ServiceRegistry> m_registry; // 现取 HttpClient 的唯一通道
};
} // namespace

JellyfinProvider::JellyfinProvider(const QString &serverUrl, const QString &serverId,
                                   const QString &token, const QString &userId,
                                   const QStringList &extensions,
                                   const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_serverUrl(serverUrl), m_serverId(serverId), m_token(token),
      m_userId(userId), m_extensions(extensions) {}

QString JellyfinProvider::typeKey() const { return QLatin1String(kJellyfinProviderKey); }

bool JellyfinProvider::supportsModify() const { return false; }
bool JellyfinProvider::supportsSort() const { return true; }

QString JellyfinProvider::displayName() { return trText(kJellyfinProviderName); }

QString JellyfinProvider::normalizeServerUrl(const QString &text) {
    QString s = text.trimmed();
    if (s.isEmpty())
        return s;
    // 缺 scheme 补 http://（幂等：带 scheme 的输入原样通过）
    if (!s.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) &&
        !s.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        s.prepend(QLatin1String("http://"));
    while (s.endsWith(QLatin1Char('/')))
        s.chop(1);
    return s;
}

std::optional<QString> JellyfinProvider::validateInput(const QString &rawUrl) {
    const QString url = normalizeServerUrl(rawUrl);
    if (url.isEmpty())
        return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Enter the server URL."));
    const QUrl parsed = QUrl(url, QUrl::ParsingMode::TolerantMode);
    if (!parsed.isValid() || parsed.host().isEmpty())
        return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Invalid server URL: %1")).arg(url);
    return std::nullopt;
}

// 与 DirProvider / WebDavProvider 的同名函数同口径：按空白拆分、去前导点、
// 转小写、去重保序；空文本 ⇒ 空表（不过滤）
QStringList JellyfinProvider::parseExtensions(const QString &text) {
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

// 请求装配：委托文件内自由函数内核（本提供者恒 GET、无请求体；授权流的
// POST 分支同走该内核）
HttpClient::Request JellyfinProvider::makeApiRequest(const QString &path) const {
    return assembleApiRequest(m_serverUrl, m_token, path, QByteArrayLiteral("GET"));
}

// N52：转接是唯一传输形态（Required）——usesTransit() 恒真，directPlayUrl
// 永不被调（基类恒等实现即"该分支永不走"），令牌不进播放串由这条不可达路径承载
PlaylistProvider::TransitCapability JellyfinProvider::transitCapability() const {
    return TransitCapability::Required;
}

// 端点惰性建一次并缓存（基类模板方法职责，N57 §2.4）：参数不可变故无需重建
std::shared_ptr<StreamEndpoint> JellyfinProvider::createStreamEndpoint() const {
    return std::make_shared<JellyfinStreamEndpoint>(m_serverUrl, m_serverId, m_token, registry());
}

// 枚举：QtConcurrent 任务内同步推进（WebDAV 同款——在途至多 1 条请求，顺序推进
// 不设取消通道，exec 使用前提符合）。逐层态：进入一层举一层，parent 末段即本层
// parentId（视图只对提供者自己返回过的容器 id 发起非空 parent 调用——N45 契约，
// 段即持久 itemId，无需归属校验）。根层 = UserViews 媒体库容器组；子层 = /Items
// 一次全量（无翻页，实测口径），叶子类型产实体、其余类型产容器
std::expected<PlaylistProvider::Enumeration, QString>
JellyfinProvider::enumerate(const QStringList &parent) const {
    // 装配错误：HttpClient 启动期必已 provide（N35 §2.3），永不命中；断言守候
    // + fail-safe（枚举不发请求即整次失败）
    const std::shared_ptr<HttpClient> client =
        registry() ? registry()->resolve<HttpClient>() : nullptr;
    if (!client) {
        Q_ASSERT(false);
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("JellyfinProvider", "HTTP client is not available.")));
    }
    // 未登录（构造即无 token / 已退出登录）：整次失败给明确文案，不静默返回空层
    if (m_token.isEmpty())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Not signed in to the Jellyfin server.")));

    // 根层举 UserViews（媒体库列表，带授权用户 id——实测口径），子层举
    // /Items?parentId=<末段容器 id>
    const QString path =
        parent.isEmpty()
            ? (m_userId.isEmpty() ? QStringLiteral("/UserViews")
                                  : QStringLiteral("/UserViews?userId=%1").arg(m_userId))
            : QStringLiteral("/Items?parentId=%1&fields=%2")
                  .arg(parent.last(), QLatin1String(kItemFields));
    const HttpClient::Request req = makeApiRequest(path);
    const HttpClient::Response resp = client->exec(req);
    const bool ok = resp.status == 200 && !resp.canceled && resp.error.isEmpty();
    if (!ok)
        return std::unexpected(apiRequestError(resp));
    const auto items = parseItemsResponse(resp.body);
    if (!items)
        return std::unexpected(items.error());

    Enumeration out;
    for (const QJsonValue &value : *items) {
        const QJsonObject obj = value.toObject();
        const QString id = obj.value(QLatin1String("id")).toString();
        if (id.isEmpty())
            continue; // 无 id 条目不可寻址，跳过
        const QString name = obj.value(QLatin1String("name")).toString();

        // 容器 / 叶子判据（变更记录 4）：IsFolder=true 一律产容器（Series / Season /
        // Folder / CollectionFolder / …）；非文件夹且类型可播产叶子，其余（Photo 等
        // 无取流通路的内容）跳过不出行。IsFolder 是 BaseItemDto 的基础字段，/Items
        // 默认返回，无需 fields 声明
        const bool isFolder = obj.value(QLatin1String("isFolder")).toBool();
        if (parent.isEmpty() || isFolder) {
            // 容器（根层媒体库，或可进入层）。
            // id = 持久 itemId（进 queueRoot / 层键 / Origin::levelIds 的回举路径段）；
            // path 展示位留空——Jellyfin 无自然路径形态（N69：空 = 未知，合法值）
            Container container;
            container.id = id;
            container.title = name.isEmpty() ? id : name;
            out.containers.push_back(std::move(container));
            continue;
        }

        // 原始文件路径（fields=Path 显式声明；回退 mediaSources[0].path，N69 变更记录 5）。
        // 一份来源两用：末段作**文件名**（N70，排序键取值）+ 点后段作后缀（扩展名过滤
        // 判据与 Extension 排序键）
        const QJsonArray sources = obj.value(QLatin1String("mediaSources")).toArray();
        QString rawPath = obj.value(QLatin1String("path")).toString();
        if (rawPath.isEmpty() && !sources.isEmpty())
            rawPath = sources.at(0).toObject().value(QLatin1String("path")).toString();
        const QString suffix = extractSuffix(rawPath);
        const QString rawName = extractFileName(rawPath);

        // 扩展名过滤（与本地目录表同语义）：清单非空时，无后缀 / 不在清单内的
        // 条目跳过
        if (!m_extensions.isEmpty() && (suffix.isEmpty() || !m_extensions.contains(suffix)))
            continue;

        // 叶子条目：URN 身份 jellyfin://<serverId>/<type>/<itemId>（变更记录 7：
        // 类型段随身份携带，取流 URL 从身份一步现拼，不再失败回退）。类型归小写
        // ——URN 形态稳定不随服务器端大小写漂移；空类型落 unknown（起流期得明确
        // 错误，不静默）。直构 JellyfinEntity 不经工厂（WebDAV 先例：工厂分派点
        // 是收编 / 重载的落点，枚举侧绕过无损——URN 形态经 Util::Url::toIdentity
        // 原样返回）
        const QString type = obj.value(QLatin1String("type")).toString().toLower();
        const QString urn = QStringLiteral("%1%2/%3/%4")
                                .arg(QLatin1String(kJellyfinScheme), m_serverId,
                                     type.isEmpty() ? QStringLiteral("unknown") : type, id);
        auto entity = std::make_shared<JellyfinEntity>(urn, registry());
        // 名称两句注入（显示名与文件名分家）：显示名 = 剧集叶子带集号前缀的服务端
        // 标题（N72），其余类型仍是服务端标题原样（URN 尾段是 itemId 噪声，派生值
        // 不可用；不再往里追加原始后缀——那正是 N69 变更记录 5 的权宜写法）；
        // 文件名 = 原始路径末段（服务器上的真实文件名，FileName / FileNameNumeric /
        // Extension 三个排序键的取值来源）。各按"空则不注"，拿不到就不给假值，落回派生
        // 占位；两个出口调用顺序无关（显示名一经 setTitle 置旗，文件名不再联动它）
        const QString displayName = episodeDisplayName(type, name, obj);
        if (!displayName.isEmpty())
            injectDisplayName(*entity, displayName);
        if (!rawName.isEmpty())
            injectFileName(*entity, rawName);

        // 元数据注入（N29 通道，枚举期零额外请求）：
        // 大小 ← mediaSources[0].size，创建时间 ← dateCreated
        const QDateTime created = parseIsoDate(obj.value(QLatin1String("dateCreated")).toString());
        if (!sources.isEmpty() || created.isValid()) {
            FileMeta fm;
            // 大小只在取到有效值时注入：键缺失 / 为 null 时 toVariant().toLongLong()
            // 会落 0，把"未知"显示成"0 字节"——与 created 的 isValid 保护同口径
            const QVariant size =
                sources.isEmpty()
                    ? QVariant()
                    : sources.at(0).toObject().value(QLatin1String("size")).toVariant();
            if (size.isValid())
                fm.size = size.toLongLong();
            if (created.isValid())
                fm.created = created;
            injectFileMeta(*entity, fm);
        }
        // 时长 ← runTimeTicks（100ns 计数 → 秒），发行时间 ← premiereDate
        const qint64 runTimeTicks =
            obj.value(QLatin1String("runTimeTicks")).toVariant().toLongLong();
        const QDateTime premiered =
            parseIsoDate(obj.value(QLatin1String("premiereDate")).toString());
        if (runTimeTicks > 0 || premiered.isValid()) {
            PlaylistEntity::MediaMeta mm;
            if (runTimeTicks > 0)
                mm.duration = runTimeTicks / 10000000.0;
            if (premiered.isValid())
                mm.recordedDate = premiered;
            injectMediaMeta(*entity, mm);
        }
        out.entities.push_back(std::move(entity));
    }
    // 容器组恒在实体组之前（N45 模型不变式）：服务器对每层的返回本就同序产出，
    // 根层只有容器、叶子层只有实体，组内相对序即服务器枚举序
    return out;
}

void JellyfinProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // N44：类型键经基类取 typeKey()，不再写字面量
    settings.setValue(QStringLiteral("jfUrl"), m_serverUrl);
    settings.setValue(QStringLiteral("jfServerId"), m_serverId);
    // 凭据明文落 INI（N69 已定案，与既有配置同款）
    settings.setValue(QStringLiteral("jfToken"), m_token);
    settings.setValue(QStringLiteral("jfUserId"), m_userId);
    settings.setValue(QStringLiteral("jfExtensions"), m_extensions);
    // 排序规则随提供者持久化（与 WebDAV 表同形）
    settings.setValue(QStringLiteral("sortKey"), int(sortRule().key));
    settings.setValue(QStringLiteral("sortAscending"), sortRule().ascending);
}

std::shared_ptr<PlaylistProvider>
JellyfinProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    auto provider = std::make_shared<JellyfinProvider>(
        settings.value(QStringLiteral("jfUrl")).toString(),
        settings.value(QStringLiteral("jfServerId")).toString(),
        settings.value(QStringLiteral("jfToken")).toString(),
        settings.value(QStringLiteral("jfUserId")).toString(),
        settings.value(QStringLiteral("jfExtensions")).toStringList(), registry);
    const SortRule rule{static_cast<SortKey>(settings.value(QStringLiteral("sortKey"), 0).toInt()),
                        settings.value(QStringLiteral("sortAscending"), true).toBool()};
    provider->setSortRule(rule);
    return provider;
}

// ============================================================================
// N69：快速连接授权与参数页（页面类不进头文件，WebDavSettingsPage 先例）
// ============================================================================

namespace {
// 轮询节奏：间隔 3s，整体上限 5 分钟（用户批准是人工动作，不设过紧）；连续
// 网络失败 5 次即判失败（容忍服务器瞬断，不无限空转）
constexpr int kPollIntervalMs = 3000;
constexpr int kPollTimeoutMs = 5 * 60 * 1000;
constexpr int kPollFailStreakMax = 5;

QString invalidResponseText() {
    return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Invalid JSON response from server."));
}
} // namespace

// 快速连接模态对话框（N69 定案）：授权码展示 + 轮询 + 底部居中取消按钮。
// 流程分两阶段任务（QtConcurrent，任务线程内 exec/execCancelable 均符合 N34
// 前提），阶段间经 QFutureWatcher 回 GUI；取消旗为共享原子（任务生命周期
// 独立于对话框——对话框销毁后在途任务自行收尾，结果随 watcher 一并丢弃）：
//   阶段 1：GET /System/Info/Public 取 serverId → POST /QuickConnect/Initiate
//           得 6 位授权码与 secret；
//   阶段 2：循环 GET /QuickConnect/Connect?secret= 至 Authenticated=true →
//           POST /Users/AuthenticateWithQuickConnect 换 token 与 userId。
// 关窗（reject）即置取消旗，等效取消按钮。
class JellyfinQuickConnectDialog : public QDialog {
    Q_OBJECT
  public:
    JellyfinQuickConnectDialog(const QString &serverUrl,
                               const std::shared_ptr<ServiceRegistry> &registry, QWidget *parent)
        : QDialog(parent), m_serverUrl(serverUrl), m_registry(registry),
          m_cancel(std::make_shared<std::atomic_bool>(false)) {
        setWindowTitle(tr("Jellyfin Quick Connect"));
        setModal(true);
        setMinimumWidth(320);

        auto *layout = new QVBoxLayout(this);
        auto *prompt =
            new QLabel(tr("Enter this code on your Jellyfin server to approve sign-in:"));
        prompt->setWordWrap(true);
        layout->addWidget(prompt);

        m_codeLabel = new QLabel;
        m_codeLabel->setAlignment(Qt::AlignCenter);
        m_codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont codeFont = m_codeLabel->font();
        codeFont.setPointSize(codeFont.pointSize() * 2 + 4);
        codeFont.setBold(true);
        m_codeLabel->setFont(codeFont);
        layout->addWidget(m_codeLabel);

        m_statusLabel = new QLabel(tr("Contacting server…"));
        m_statusLabel->setAlignment(Qt::AlignCenter);
        m_statusLabel->setWordWrap(true);
        layout->addWidget(m_statusLabel);

        layout->addStretch(1);

        // 底部居中一个取消按钮（关窗等效取消）
        auto *buttonRow = new QHBoxLayout;
        buttonRow->addStretch(1);
        auto *cancelButton = new QPushButton(tr("Cancel"));
        buttonRow->addWidget(cancelButton);
        buttonRow->addStretch(1);
        layout->addLayout(buttonRow);
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

        startPhase1();
    }

    // 授权产物：accessToken 为空 = 未完成授权（取消或失败，失败文案已在对话框内显示）
    const JellyfinAuthResult &result() const { return m_result; }

  protected:
    void reject() override {
        *m_cancel = true; // 关窗即取消：abort 在途请求、终止轮询
        QDialog::reject();
    }

  private:
    // ——阶段 1：可达性 + serverId + 发起快速连接——
    void startPhase1() {
        auto watcher = new QFutureWatcher<JellyfinAuthResult>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
            watcher->deleteLater();
            phase1Done(watcher->result());
        });
        const QString serverUrl = m_serverUrl;
        const auto registry = m_registry;
        const auto cancel = m_cancel;
        watcher->setFuture(QtConcurrent::run([serverUrl, registry, cancel]() -> JellyfinAuthResult {
            JellyfinAuthResult out;
            const auto client = registry ? registry->resolve<HttpClient>() : nullptr;
            if (!client) {
                out.error =
                    trText(QT_TRANSLATE_NOOP("JellyfinProvider", "HTTP client is not available."));
                return out;
            }
            // ① 可达性探测与 serverId 取得（免鉴权端点）
            const auto info = execCancelable(
                *client,
                assembleApiRequest(serverUrl, {}, QStringLiteral("/System/Info/Public"),
                                   QByteArrayLiteral("GET")),
                *cancel);
            if (*cancel)
                return {};
            if (info.status != 200 || !info.error.isEmpty()) {
                out.error = apiRequestError(info);
                return out;
            }
            out.serverId =
                QJsonDocument::fromJson(info.body).object().value(QLatin1String("id")).toString();
            if (out.serverId.isEmpty()) {
                out.error = invalidResponseText();
                return out;
            }
            // ② 发起快速连接：400 / 403 = 服务器未开启快速连接，给明确文案
            const auto init = execCancelable(
                *client,
                assembleApiRequest(serverUrl, {}, QStringLiteral("/QuickConnect/Initiate"),
                                   QByteArrayLiteral("POST")),
                *cancel);
            if (*cancel)
                return {};
            if (init.status == 400 || init.status == 403) {
                out.error = trText(QT_TRANSLATE_NOOP(
                    "JellyfinProvider", "Quick Connect is not enabled on this server."));
                return out;
            }
            if (init.status != 200 || !init.error.isEmpty()) {
                out.error = apiRequestError(init);
                return out;
            }
            const QJsonObject obj = QJsonDocument::fromJson(init.body).object();
            out.code = obj.value(QLatin1String("code")).toString();
            out.secret = obj.value(QLatin1String("secret")).toString();
            if (out.code.isEmpty() || out.secret.isEmpty())
                out.error = invalidResponseText();
            return out;
        }));
    }

    void phase1Done(const JellyfinAuthResult &r) {
        if (!isVisible())
            return;              // 关窗即取消：结果丢弃
        if (!r.code.isEmpty()) { // 成功：展示授权码并进入轮询
            m_codeLabel->setText(r.code);
            m_statusLabel->setText(tr("Waiting for approval…"));
            startPhase2(r.serverId, r.secret);
            return;
        }
        if (!r.error.isEmpty())
            m_statusLabel->setText(r.error); // 失败：文案就地显示，用户可关闭重试
        // 其余（取消）：不更新界面
    }

    // ——阶段 2：轮询批准状态 → 授权完成换 token——
    void startPhase2(const QString &serverId, const QString &secret) {
        auto watcher = new QFutureWatcher<JellyfinAuthResult>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
            watcher->deleteLater();
            phase2Done(watcher->result());
        });
        const QString serverUrl = m_serverUrl;
        const auto registry = m_registry;
        const auto cancel = m_cancel;
        watcher->setFuture(QtConcurrent::run([serverUrl, registry, cancel, serverId,
                                              secret]() -> JellyfinAuthResult {
            JellyfinAuthResult out;
            out.serverId = serverId;
            const auto client = registry ? registry->resolve<HttpClient>() : nullptr;
            if (!client) {
                out.error =
                    trText(QT_TRANSLATE_NOOP("JellyfinProvider", "HTTP client is not available."));
                return out;
            }
            int netFailStreak = 0;
            for (int waited = 0; waited < kPollTimeoutMs; waited += kPollIntervalMs) {
                if (*cancel)
                    return JellyfinAuthResult{}; // 取消：整体丢弃（error 恒空）
                const QString path = QStringLiteral("/QuickConnect/Connect?secret=%1")
                                         .arg(QString::fromUtf8(QUrl::toPercentEncoding(secret)));
                const auto resp = execCancelable(
                    *client, assembleApiRequest(serverUrl, {}, path, QByteArrayLiteral("GET")),
                    *cancel);
                if (*cancel)
                    return JellyfinAuthResult{};
                if (resp.status == 200 && resp.error.isEmpty()) {
                    netFailStreak = 0;
                    if (QJsonDocument::fromJson(resp.body)
                            .object()
                            .value(QLatin1String("authenticated"))
                            .toBool()) {
                        // 用户已批准：用 secret 换 token 与 userId（请求体键
                        // 同为 camelCase，实测口径）
                        const QJsonObject bodyObj{{QLatin1String("secret"), secret}};
                        const QByteArray body =
                            QJsonDocument(bodyObj).toJson(QJsonDocument::Compact);
                        const auto auth = execCancelable(
                            *client,
                            assembleApiRequest(
                                serverUrl, {},
                                QStringLiteral("/Users/AuthenticateWithQuickConnect"),
                                QByteArrayLiteral("POST"), body),
                            *cancel);
                        if (*cancel)
                            return JellyfinAuthResult{};
                        if (auth.status != 200 || !auth.error.isEmpty()) {
                            out.error = apiRequestError(auth);
                            return out;
                        }
                        const QJsonObject obj = QJsonDocument::fromJson(auth.body).object();
                        out.accessToken = obj.value(QLatin1String("accessToken")).toString();
                        out.userId = obj.value(QLatin1String("user"))
                                         .toObject()
                                         .value(QLatin1String("id"))
                                         .toString();
                        if (out.accessToken.isEmpty())
                            out.error = invalidResponseText();
                        return out;
                    }
                } else if (++netFailStreak >= kPollFailStreakMax) {
                    out.error = apiRequestError(resp);
                    return out;
                }
                if (!sleepCancelable(kPollIntervalMs, *cancel))
                    return JellyfinAuthResult{};
            }
            out.error =
                trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Timed out waiting for approval."));
            return out;
        }));
    }

    void phase2Done(const JellyfinAuthResult &r) {
        if (!isVisible())
            return; // 关窗即取消：结果丢弃
        if (r.error.isEmpty() && r.accessToken.isEmpty())
            return; // 取消：界面不动
        if (!r.error.isEmpty()) {
            m_statusLabel->setText(r.error); // 失败：留在对话框，用户可关闭重试
            return;
        }
        m_result = r; // 授权完成：自动关闭，页面凭此更新登录状态
        accept();
    }

    QString m_serverUrl;
    std::shared_ptr<ServiceRegistry> m_registry;
    std::shared_ptr<std::atomic_bool> m_cancel; // 共享取消旗（对话框销毁后任务自行收尾）
    JellyfinAuthResult m_result;
    QLabel *m_codeLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
};

namespace {
// 页面测试任务的返回值：ok 区分消息框图标（成功 information / 失败 warning）
struct JellyfinTestOutcome {
    bool ok = false;
    QString message;
};
} // namespace

// 参数页（N69 定案布局）：URL 行 + 三行（登录状态 / 快速连接或退出登录 / 测试）。
// 登录状态非动态判定——页面持有 token 即已登录（绿色），无则未登录（灰色）；
// 快速连接走模态对话框，授权产物先落页面状态，accept 期经 build() 落盘。
// 页面类只在本文件可见，经 createJellyfinSettingsPage 产出
class JellyfinSettingsPage : public ProviderSettingsPage {
    Q_OBJECT
  public:
    JellyfinSettingsPage(const JellyfinProvider *seed,
                         const std::shared_ptr<ServiceRegistry> &registry, QWidget *parent)
        : ProviderSettingsPage(parent), m_seed(seed), m_registry(registry) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);

        m_urlEdit = new QLineEdit;
        form->addRow(tr("URL"), m_urlEdit);

        // 登录状态：无标题行，状态文本直落第二列（维护者指示）
        m_statusLabel = new QLabel;
        form->addRow(QString(), m_statusLabel);
        m_authButton = new QPushButton;
        form->addRow(QString(), m_authButton);
        m_testButton = new QPushButton(tr("Test"));
        form->addRow(QString(), m_testButton);

        // 扩展名过滤（与本地目录表同款：空 = 不过滤；复位回全局默认清单）
        m_extensionsEdit = new QLineEdit;
        auto *extRow = new QHBoxLayout;
        extRow->addWidget(m_extensionsEdit, 1);
        auto *extResetBtn = new QPushButton(QStringLiteral("↺"));
        extResetBtn->setToolTip(tr("Reset to default extension list"));
        extResetBtn->setFixedWidth(28);
        extRow->addWidget(extResetBtn);
        auto *extNote =
            new QLabel(tr("Space-separated file extensions; leave empty for no filter"));
        // 不开 wordWrap：备注按单行计入宽度，由它把表单区（进而对话框）撑宽
        // （WebDAV 页同款口径）
        extNote->setEnabled(false); // 禁用态即取调色板的灰，跟随主题不硬编码颜色
        auto *extField = new QVBoxLayout;
        extField->addLayout(extRow);
        extField->addWidget(extNote);
        form->addRow(tr("Filter"), extField);
        connect(extResetBtn, &QPushButton::clicked, this, [this] {
            m_extensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        });

        layout->addLayout(form);
        // 转接行（Required = 底部说明行，无勾选框，N52 §2.8 契约）
        enableTransitOption(std::optional<bool>());
        layout->addStretch(1);

        // 回填：URL 原样回显（定形在 urlValue 统一做），凭据取存量快照；扩展名
        // 无回显来源时预填全局默认清单
        if (m_seed) {
            m_urlEdit->setText(m_seed->serverUrl());
            m_serverId = m_seed->serverId();
            m_token = m_seed->token();
            m_userId = m_seed->userId();
            m_extensionsEdit->setText(m_seed->extensions().join(QLatin1Char(' ')));
        } else {
            m_extensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        }
        updateAuthUi();

        connect(m_authButton, &QPushButton::clicked, this, [this] {
            if (!m_token.isEmpty()) {
                // 退出登录：清空页面持有凭据（尚未落盘——accept 经 build() 才生效）
                m_serverId.clear();
                m_token.clear();
                m_userId.clear();
                updateAuthUi();
                return;
            }
            // 快速连接前先按 URL 行校验（与 accept 期同一出口）：空 / 非法地址就地
            // 弹可读文案，不进对话框——否则任务里拼出的是相对 URL，回报的只是莫名
            // 的网络错误（Test 按钮同场景给的是同一条文案）
            if (const std::optional<QString> error =
                    JellyfinProvider::validateInput(m_urlEdit->text())) {
                // 标题复用提供者显示名（同一条已注册源文案，不新增待翻译串）
                QMessageBox::warning(this, JellyfinProvider::displayName(), *error);
                return;
            }
            // 快速连接：模态对话框内完成授权码展示与轮询
            JellyfinQuickConnectDialog dialog(urlValue(), m_registry, this);
            dialog.exec();
            const JellyfinAuthResult &r = dialog.result();
            if (r.accessToken.isEmpty())
                return; // 取消或失败（失败文案已在对话框内显示）
            m_serverId = r.serverId;
            m_token = r.accessToken;
            m_userId = r.userId;
            updateAuthUi();
        });
        connect(m_testButton, &QPushButton::clicked, this, [this] {
            m_testButton->setEnabled(false);
            const QString url = urlValue();
            const QString token = m_token; // 点击时刻的凭据快照
            const bool signedIn = !token.isEmpty();
            const auto registry = m_registry;
            auto watcher = new QFutureWatcher<JellyfinTestOutcome>(this);
            connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
                watcher->deleteLater();
                const JellyfinTestOutcome outcome = watcher->result();
                if (outcome.ok)
                    QMessageBox::information(this, tr("Test Result"), outcome.message);
                else
                    QMessageBox::warning(this, tr("Test Result"), outcome.message);
                m_testButton->setEnabled(true);
            });
            watcher->setFuture(QtConcurrent::run([url, token, signedIn,
                                                  registry]() -> JellyfinTestOutcome {
                const auto client = registry ? registry->resolve<HttpClient>() : nullptr;
                if (!client) {
                    return {false, trText(QT_TRANSLATE_NOOP("JellyfinProvider",
                                                            "HTTP client is not available."))};
                }
                if (url.isEmpty())
                    return {false,
                            trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Enter the server URL."))};
                if (!signedIn) {
                    // 未登录：测服务器可达性（免鉴权端点）
                    const HttpClient::Response resp = client->exec(assembleApiRequest(
                        url, {}, QStringLiteral("/System/Info/Public"), QByteArrayLiteral("GET")));
                    if (resp.status == 200 && !resp.canceled && resp.error.isEmpty())
                        return {true, trText(QT_TRANSLATE_NOOP("JellyfinProvider",
                                                               "Server is reachable."))};
                    return {false, apiRequestError(resp)};
                }
                // 已登录：测凭据有效性（401 即失效）
                const HttpClient::Response resp = client->exec(assembleApiRequest(
                    url, token, QStringLiteral("/UserViews"), QByteArrayLiteral("GET")));
                if (resp.status == 401)
                    return {false, trText(QT_TRANSLATE_NOOP(
                                       "JellyfinProvider", "Credentials rejected by the server."))};
                if (resp.status == 200 && !resp.canceled && resp.error.isEmpty())
                    return {true, trText(QT_TRANSLATE_NOOP("JellyfinProvider",
                                                           "Credentials are valid."))};
                return {false, apiRequestError(resp)};
            }));
        });
    }

    std::optional<QString> validate() const override {
        return JellyfinProvider::validateInput(m_urlEdit->text());
    }

    bool hasChanges() const override {
        return !m_seed || urlValue() != m_seed->serverUrl() || m_serverId != m_seed->serverId() ||
               m_token != m_seed->token() || m_userId != m_seed->userId() ||
               extensionsValue() != m_seed->extensions();
    }

    std::shared_ptr<PlaylistProvider> build() const override {
        return std::make_shared<JellyfinProvider>(urlValue(), m_serverId, m_token, m_userId,
                                                  extensionsValue(), m_registry);
    }

  private:
    // 取规范化参数值（hasChanges 与 build 共用同一出口）
    QString urlValue() const { return JellyfinProvider::normalizeServerUrl(m_urlEdit->text()); }
    QStringList extensionsValue() const {
        return JellyfinProvider::parseExtensions(m_extensionsEdit->text());
    }

    // 登录状态行与按钮文案（非动态判定：持有 token 即已登录）。绿 / 灰用 CSS
    // 关键字色，亮暗两主题下均可读。登录后 URL 编辑框一并禁用（维护者指示）：
    // 地址与凭据 / serverId 成对生效，改地址须先退出登录重新授权
    void updateAuthUi() {
        const bool signedIn = !m_token.isEmpty();
        m_statusLabel->setStyleSheet(signedIn ? QStringLiteral("color: green;")
                                              : QStringLiteral("color: gray;"));
        m_statusLabel->setText(signedIn ? tr("Signed In") : tr("Not Signed In"));
        m_authButton->setText(signedIn ? tr("Sign Out") : tr("Quick Connect…"));
        m_urlEdit->setEnabled(!signedIn);
    }

    const JellyfinProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QLineEdit *m_urlEdit = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_authButton = nullptr;
    QPushButton *m_testButton = nullptr;
    QLineEdit *m_extensionsEdit = nullptr;
    // 页面持有的授权产物（对话框 accept 期经 build() 进入提供者实例）
    QString m_serverId;
    QString m_token;
    QString m_userId;
};

ProviderSettingsPage *createJellyfinSettingsPage(const PlaylistProvider *seed,
                                                 const std::shared_ptr<ServiceRegistry> &registry,
                                                 QWidget *parent) {
    // 同类型由调用方（对话框）保证：seed 非空即本类型
    return new JellyfinSettingsPage(seed ? static_cast<const JellyfinProvider *>(seed) : nullptr,
                                    registry, parent);
}

// N69：本文件定义了 Q_OBJECT 类（JellyfinQuickConnectDialog / JellyfinSettingsPage），
// CMake AUTOMOC 要求显式包含同名 .moc，按源文件名而非类名
#include "JellyfinProvider.moc"

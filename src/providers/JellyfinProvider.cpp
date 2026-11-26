#include "pch.h"

#include "JellyfinProvider.h"

#include "Constants.h"
#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "Util.h"
#include "Version.h"
#include "entities/JellyfinEntity.h"
#include "playbackreport/PlaybackReporter.h"
#include "streams/HttpByteStream.h"
#include "streams/StreamEndpoint.h"
#include "ui/ExtensionFilterEdit.h"

namespace {
QString trText(const char *source) {
    return QCoreApplication::translate("JellyfinProvider", source);
}

// Authorization 头值是带引号的逗号分隔键值，主机名不可信：为空 / 含引号 / 含反斜杠 /
// 含非 ASCII 即整串回退。Device 与 DeviceId 同值，进程内只取一次
QString deviceName() {
    static const QString name = [] {
        const QString host = QSysInfo::machineHostName();
        const bool clean =
            !host.isEmpty() && !host.contains(QLatin1Char('"')) &&
            !host.contains(QLatin1Char('\\')) &&
            std::all_of(host.cbegin(), host.cend(), [](QChar c) { return c.unicode() < 0x80; });
        if (clean)
            return host;
        const QString os = QSysInfo::prettyProductName();
        return os.isEmpty() ? App::name() : os;
    }();
    return name;
}

// Path 是原始文件名与原始后缀的唯一可靠来源
constexpr char kItemFields[] = "MediaSources,DateCreated,PremiereDate,Path";

// 服务器路径分隔符随其操作系统而异，'/' 与 '\' 都作边界
QString extractFileName(const QString &path) {
    if (path.isEmpty())
        return {};
    const qsizetype sep =
        qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return sep >= 0 ? path.mid(sep + 1) : path;
}

// 仅 Episode 生效。字段缺位用 toVariant().isValid() 判：直接 toLongLong 会把
// JSON null 落成 0，显示成"第 0 季 / 第 0 集"
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

// artists 按 "; " 拼接；空则回退 albumArtist；皆空返回空串（不注假值）
QString performerOf(const QJsonObject &obj) {
    QStringList names;
    const QJsonArray arr = obj.value(QLatin1String("artists")).toArray();
    for (const QJsonValue &value : arr) {
        const QString artist = value.toString();
        if (!artist.isEmpty())
            names << artist;
    }
    if (!names.isEmpty())
        return names.join(QLatin1String("; "));
    return obj.value(QLatin1String("albumArtist")).toString();
}

// 无效即缺位。服务器把缺失日期序列化为 .NET DateTime.MinValue（0001-01-01，
// 实测复现），解析成功但年份 ≤1 亦视作缺位
QDateTime parseIsoDate(const QString &text) {
    if (text.isEmpty())
        return {};
    const QDateTime dt = QDateTime::fromString(text, Qt::ISODate);
    if (!dt.isValid() || dt.date().year() <= 1)
        return {};
    return dt;
}

// 键一律 camelCase：请求恒带 Accept profile="CamelCase"，大小写不符即静默读空
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

// 不透传 HttpClient 的 error
QString apiRequestError(const HttpClient::Response &resp) {
    if (resp.canceled)
        return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Request canceled."));
    if (!resp.error.isEmpty())
        return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Network error: %1")).arg(resp.error);
    return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Server returned HTTP %1."))
        .arg(resp.status);
}

// Authorization 头装配（枚举请求与取流请求共用同一点定义）：MediaBrowser 形态，
// 12.x 只认标准 Authorization 头（旧 X-Emby-Authorization 已被忽略，实测）。
// token 只进请求头，不进 URL、不进实体、不落盘
QByteArray authorizationHeaderValue(const QString &token) {
    QString auth = QStringLiteral("MediaBrowser Client=\"%1\", Device=\"%2\", DeviceId=\"%2\", "
                                  "Version=\"%3\"")
                       .arg(App::name(), deviceName(), App::versionInfo());
    if (!token.isEmpty())
        auth += QStringLiteral(", Token=\"%1\"").arg(token);
    return auth.toUtf8();
}

// 名字与成员 makeApiRequest 刻意错开：类作用域名字查找会遮蔽同名自由函数
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
    req.headers.append({QByteArrayLiteral("Accept"),
                        QByteArrayLiteral("application/json; profile=\"CamelCase\"")});
    return req;
}

// error 非空即失败；error 与各凭据字段皆空即取消
struct JellyfinAuthResult {
    QString serverId;
    QString code;
    QString secret;
    QString accessToken;
    QString userId;
    QString error;
};

// 先查 cancel 再查 error：canceled 响应的字段无意义
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

// 任务线程内可取消休眠
bool sleepCancelable(int ms, const std::atomic_bool &cancel) {
    constexpr int kSliceMs = 100;
    for (int waited = 0; waited < ms; waited += kSliceMs) {
        if (cancel)
            return false;
        QThread::msleep(kSliceMs);
    }
    return true;
}

// URN 拆段单点：reporter 认领与端点取流共用。serverId 须与本实例一致；
// 返回 <type, itemId>
std::expected<std::pair<QString, QString>, QString> parseJellyfinUrn(const QString &identity,
                                                                     const QString &serverId) {
    const QString scheme = QString::fromLatin1(kJellyfinScheme);
    if (!identity.startsWith(scheme))
        return std::unexpected(QStringLiteral("identity is not a Jellyfin URN"));
    const QStringList segs = identity.mid(scheme.size()).split(QLatin1Char('/'));
    if (segs.size() != 3 || segs.at(0) != serverId || segs.at(1).isEmpty() || segs.at(2).isEmpty())
        return std::unexpected(QStringLiteral("malformed Jellyfin identity"));
    return std::make_pair(segs.at(1), segs.at(2));
}

// 取流统一走 /Items/{id}/Download（/Images/Original 照片通路实测打不开）；
// token 经请求头，Range / 206 / seek 由 HttpByteStream 承担。
// 构造后不可变、无 QObject、任意线程可析构
class JellyfinStreamEndpoint final : public StreamEndpoint {
  public:
    JellyfinStreamEndpoint(QString serverUrl, QString serverId, QString token,
                           std::shared_ptr<ServiceRegistry> registry)
        : m_serverUrl(std::move(serverUrl)), m_serverId(std::move(serverId)),
          m_token(std::move(token)), m_registry(std::move(registry)) {}

    std::expected<std::shared_ptr<ByteStream>, QString>
    openStream(const QString &identity) const override {
        // serverId 不一致属编程错误：防御性失败，不静默错播另一台服务器的内容
        const auto urn = parseJellyfinUrn(identity, m_serverId);
        if (!urn)
            return std::unexpected(urn.error());

        // 缺注册表 / client 属装配错误，起流失败并记因
        const auto client = m_registry ? m_registry->resolve<HttpClient>() : nullptr;
        if (!client)
            return std::unexpected(QStringLiteral("http client unavailable"));

        HttpStreamTarget target;
        target.url = m_serverUrl + QStringLiteral("/Items/%1/Download").arg(urn->second);
        // Accept 头对原始文件流无意义；headers 是 QString 对，授权头恒 ASCII
        target.headers.append({QStringLiteral("Authorization"),
                               QString::fromUtf8(authorizationHeaderValue(m_token))});
        auto opened = HttpByteStream::open(HttpByteStream::makeFeed(client), target);
        if (!opened)
            return std::unexpected(opened.error());
        return opened.value();
    }

  private:
    QString m_serverUrl;
    QString m_serverId;
    QString m_token;
    std::shared_ptr<ServiceRegistry> m_registry;
};

// 独立对象，寿命归提供者，注册表只持 weak；只拷构造期快照与注册表，无环。
// 节流三件（会话 token / 上次已报位置 / 上次暂停态）随 Start 与新 sessionToken 复位。
// 认领判据：URN 且 serverId 一致，token 空不认领；同服务器多列表的归属由驱动器
// 按来源列表选桶。认证类（401 / 403）连续 3 次即永久停手，解除 = 重新授权
class JellyfinReporter final : public PlaybackReporter {
  public:
    JellyfinReporter(QString serverUrl, QString serverId, QString token,
                     std::shared_ptr<ServiceRegistry> registry)
        : m_serverUrl(std::move(serverUrl)), m_serverId(std::move(serverId)),
          m_token(std::move(token)), m_registry(std::move(registry)) {}

    void playbackEvent(const PlaybackEvent &ev) override {
        if (m_fused)
            return;
        if (m_token.isEmpty())
            return;
        const auto urn = parseJellyfinUrn(ev.identity, m_serverId);
        if (!urn)
            return;

        // token 变化即新段，节流状态复位；非 Start 的换段事件属陈旧会话尾巴，忽略
        if (ev.sessionToken != m_sessionToken) {
            if (ev.phase != PlaybackEvent::Phase::Start)
                return;
            m_sessionToken = ev.sessionToken;
            m_lastReported = ev.positionSeconds;
            m_lastPaused = ev.paused;
            postPlaying(QStringLiteral("/Sessions/Playing"), ev, urn->second);
            return;
        }

        switch (ev.phase) {
        case PlaybackEvent::Phase::Start: // 同 token 重复 Start：重发幂等，基线照刷
            m_lastReported = ev.positionSeconds;
            m_lastPaused = ev.paused;
            postPlaying(QStringLiteral("/Sessions/Playing"), ev, urn->second);
            break;
        case PlaybackEvent::Phase::Progress: {
            // 按位置差而非采样次数节流：前进 ≥10s / 回跳 ≥5s / 暂停翻转才报；
            // 暂停期位置不动自然停发，服务器据此不再计时
            const bool pauseSwitched = m_lastPaused != ev.paused;
            const double diff = ev.positionSeconds - m_lastReported;
            if (!pauseSwitched && diff < 10.0 && diff > -5.0)
                return;
            m_lastReported = ev.positionSeconds;
            m_lastPaused = ev.paused;
            postPlaying(QStringLiteral("/Sessions/Playing/Progress"), ev, urn->second);
            break;
        }
        case PlaybackEvent::Phase::Stop:
            m_sessionToken.clear();
            postPlaying(QStringLiteral("/Sessions/Playing/Stopped"), ev, urn->second);
            break;
        }
    }

  private:
    // 三端点共用最小字段集。Stopped 的取值判据在驱动器侧，本报告者只搬运——
    // 服务器按"位置 ≈ duration"自行判已看
    void postPlaying(const QString &path, const PlaybackEvent &ev, const QString &itemId) {
        // 上报面取不到 client 就静默（只有枚举 / 取流的装配失败需要显影）
        const auto client = m_registry ? m_registry->resolve<HttpClient>() : nullptr;
        if (!client)
            return;
        QJsonObject body;
        body.insert(QLatin1String("ItemId"), itemId);
        body.insert(QLatin1String("PositionTicks"),
                    static_cast<qint64>(qMax(0.0, ev.positionSeconds) * 1e7));
        body.insert(QLatin1String("IsPaused"), ev.paused);
        body.insert(QLatin1String("PlaySessionId"), ev.sessionToken);
        body.insert(QLatin1String("PlayMethod"), QStringLiteral("DirectPlay"));
        const HttpClient::Request req =
            assembleApiRequest(m_serverUrl, m_token, path, QByteArrayLiteral("POST"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
        // 恒至多一条在途，wait(1000) 未结束即 abort：单条 ≤1000ms，Progress 按
        // 位置差节流 ⇒ 稳态约 0.1 槽；改动节流 / 切片须重算
        const auto id = client->submit(req);
        if (!HttpClient::wait(id, 1000))
            HttpClient::abort(id);
        const HttpClient::Response res = HttpClient::take(id);
        // 任一次 2xx 清零。只认认证类（401 / 403），连续 3 次停手；其余不计也不清：
        // 400 是字段装配错、404 是条目级事实、5xx 与 status 0 是瞬态
        if (res.status >= 200 && res.status < 300) {
            m_authFailures = 0;
            return;
        }
        if (res.status == 401 || res.status == 403) {
            if (++m_authFailures >= 3)
                m_fused = true;
        }
    }

    QString m_serverUrl;
    QString m_serverId;
    QString m_token;
    std::shared_ptr<ServiceRegistry> m_registry;
    QString m_sessionToken;
    double m_lastReported = 0;
    bool m_lastPaused = false;
    // 认证退避不随会话复位：token 失效是常驻形态，重新授权换实例才是唯一解除
    int m_authFailures = 0; // 认证类连续计数，任一次 2xx 清零
    bool m_fused = false;   // 置位后 playbackEvent 一律早退
};
} // namespace

JellyfinProvider::JellyfinProvider(const QString &serverUrl, const QString &serverId,
                                   const QString &token, const QString &userId,
                                   const QStringList &extensions,
                                   const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_serverUrl(serverUrl), m_serverId(serverId), m_token(token),
      m_userId(userId), m_extensions(extensions) {
    // 只造实例不注册：注册点在列表绑定点，投递路由需要列表身份
    setReporter(std::make_shared<JellyfinReporter>(serverUrl, serverId, token, registry));
}

QString JellyfinProvider::typeKey() const { return QLatin1String(kJellyfinProviderKey); }

bool JellyfinProvider::supportsModify() const { return false; }
bool JellyfinProvider::supportsSort() const { return true; }

QString JellyfinProvider::displayName() { return trText(kJellyfinProviderName); }

QString JellyfinProvider::normalizeServerUrl(const QString &text) {
    QString s = text.trimmed();
    if (s.isEmpty())
        return s;
    // 缺 scheme 补 http://（带 scheme 的输入原样通过）
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

HttpClient::Request JellyfinProvider::makeApiRequest(const QString &path) const {
    return assembleApiRequest(m_serverUrl, m_token, path, QByteArrayLiteral("GET"));
}

// Required：usesTransit() 恒真，directPlayUrl 永不被调，令牌不进播放串
PlaylistProvider::TransitCapability JellyfinProvider::transitCapability() const {
    return TransitCapability::Required;
}

std::shared_ptr<StreamEndpoint> JellyfinProvider::createStreamEndpoint() const {
    return std::make_shared<JellyfinStreamEndpoint>(m_serverUrl, m_serverId, m_token, registry());
}

// 在 QtConcurrent 任务内同步推进；parent 末段即本层 parentId，视图只对返回过的
// 容器 id 调用。根层 = UserViews 媒体库，子层 = /Items 一次全量（无翻页）
std::expected<PlaylistProvider::Enumeration, QString>
JellyfinProvider::enumerate(const QStringList &parent) const {
    const std::shared_ptr<HttpClient> client =
        registry() ? registry()->resolve<HttpClient>() : nullptr;
    if (!client) {
        Q_ASSERT(false);
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("JellyfinProvider", "HTTP client is not available.")));
    }
    // 未登录：整次失败给明确文案，不静默返回空层
    if (m_token.isEmpty())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Not signed in to the Jellyfin server.")));

    // UserViews 实测需带授权用户 id
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

        // IsFolder=true 产容器，其余产叶子；Photo 等无取流通路的内容由扩展名
        // 清单挡掉。IsFolder 是基础字段，默认返回
        const bool isFolder = obj.value(QLatin1String("isFolder")).toBool();
        if (parent.isEmpty() || isFolder) {
            // id 进 queueRoot / 层键；path 留空（Jellyfin 无自然路径形态）
            Container container;
            container.id = id;
            container.title = name.isEmpty() ? id : name;
            out.containers.push_back(std::move(container));
            continue;
        }

        // 后缀判定走 Util 字面比对；Extension 排序读实体的 suffix 缓存，不经这里
        const QJsonArray sources = obj.value(QLatin1String("mediaSources")).toArray();
        QString rawPath = obj.value(QLatin1String("path")).toString();
        if (rawPath.isEmpty() && !sources.isEmpty())
            rawPath = sources.at(0).toObject().value(QLatin1String("path")).toString();
        const QString rawName = extractFileName(rawPath);

        // 清单非空时，无后缀 / 不在清单内的条目跳过（点文件按 Qt 口径有后缀）
        if (!Util::passesExtensionFilter(rawPath, m_extensions))
            continue;

        // 类型归小写，空类型落 unknown（起流期得明确错误）。直构实体不经工厂：
        // 工厂分派点是收编 / 重载的落点，枚举侧绕过无损
        const QString type = obj.value(QLatin1String("type")).toString().toLower();
        const QString urn = QStringLiteral("%1%2/%3/%4")
                                .arg(QLatin1String(kJellyfinScheme), m_serverId,
                                     type.isEmpty() ? QStringLiteral("unknown") : type, id);
        auto entity = std::make_shared<JellyfinEntity>(urn, registry());
        // 显示名与文件名分家：显示名可为剧集带集号前缀，文件名取原始路径末段
        // （三个文件名排序键的取值来源）。空则不注
        const QString displayName = episodeDisplayName(type, name, obj);
        if (!displayName.isEmpty())
            injectDisplayName(*entity, displayName);
        if (!rawName.isEmpty())
            injectFileName(*entity, rawName);

        // 大小 ← mediaSources[0].size，创建时间 ← dateCreated
        const QDateTime created = parseIsoDate(obj.value(QLatin1String("dateCreated")).toString());
        if (!sources.isEmpty() || created.isValid()) {
            FileMeta fm;
            // 键缺失 / 为 null 时 toLongLong 会落 0，把"未知"显示成"0 字节"
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
        // format 空回退 mediaSources[0].container；媒体标签的 title 用服务端标题
        // 原样（不带集号前缀，那是列表显示名语义）
        const qint64 runTimeTicks =
            obj.value(QLatin1String("runTimeTicks")).toVariant().toLongLong();
        const QDateTime premiered =
            parseIsoDate(obj.value(QLatin1String("premiereDate")).toString());
        QString format = obj.value(QLatin1String("container")).toString();
        if (format.isEmpty() && !sources.isEmpty())
            format = sources.at(0).toObject().value(QLatin1String("container")).toString();
        const QString album = obj.value(QLatin1String("album")).toString();
        const QString performer = performerOf(obj);
        if (runTimeTicks > 0 || premiered.isValid() || !format.isEmpty() || !name.isEmpty() ||
            !album.isEmpty() || !performer.isEmpty()) {
            PlaylistEntity::MediaMeta mm;
            if (runTimeTicks > 0)
                mm.duration = runTimeTicks / 10000000.0;
            if (premiered.isValid())
                mm.recordedDate = premiered;
            if (!format.isEmpty())
                mm.format = format;
            if (!name.isEmpty())
                mm.title = name;
            if (!album.isEmpty())
                mm.album = album;
            if (!performer.isEmpty())
                mm.performer = performer;
            injectMediaMeta(*entity, mm);
        }
        out.entities.push_back(std::move(entity));
    }
    return out;
}

void JellyfinProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // 类型键经基类取 typeKey()
    settings.setValue(QStringLiteral("jfUrl"), m_serverUrl);
    settings.setValue(QStringLiteral("jfServerId"), m_serverId);
    settings.setValue(QStringLiteral("jfToken"), m_token); // 明文落 INI，与既有配置同款
    settings.setValue(QStringLiteral("jfUserId"), m_userId);
    settings.setValue(QStringLiteral("jfExtensions"), m_extensions);
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

namespace {
// 批准是人工动作，5 分钟上限；网络连续失败 5 次即判失败，不无限空转
constexpr int kPollIntervalMs = 3000;
constexpr int kPollTimeoutMs = 5 * 60 * 1000;
constexpr int kPollFailStreakMax = 5;

QString invalidResponseText() {
    return trText(QT_TRANSLATE_NOOP("JellyfinProvider", "Invalid JSON response from server."));
}
} // namespace

// 两阶段任务（QtConcurrent），阶段间经 QFutureWatcher 回 GUI；取消旗为共享原子，
// 对话框销毁后在途任务自行收尾。阶段 1：Public 取 serverId → Initiate 得授权码；
// 阶段 2：轮询 Connect 至已批准 → Authenticate 换 token 与 userId。关窗即置取消旗
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

        auto *buttonRow = new QHBoxLayout;
        buttonRow->addStretch(1);
        auto *cancelButton = new QPushButton(tr("Cancel"));
        buttonRow->addWidget(cancelButton);
        buttonRow->addStretch(1);
        layout->addLayout(buttonRow);
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

        startPhase1();
    }

    // accessToken 为空 = 未完成授权
    const JellyfinAuthResult &result() const { return m_result; }

  protected:
    void reject() override {
        *m_cancel = true; // 关窗即取消：abort 在途请求、终止轮询
        QDialog::reject();
    }

  private:
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
            // 400 / 403 = 服务器未开启快速连接
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
            return;
        if (!r.code.isEmpty()) {
            m_codeLabel->setText(r.code);
            m_statusLabel->setText(tr("Waiting for approval…"));
            startPhase2(r.serverId, r.secret);
            return;
        }
        if (!r.error.isEmpty())
            m_statusLabel->setText(r.error);
    }

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
                        // 请求体键同为 camelCase，实测口径
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

// 登录状态非动态判定——持有 token 即已登录；快速连接产物先落页面，accept 期经
// build 落盘。页面类只在本文件可见
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

        // 登录状态：无标题行，状态文本直落第二列
        m_statusLabel = new QLabel;
        form->addRow(QString(), m_statusLabel);
        m_authButton = new QPushButton;
        form->addRow(QString(), m_authButton);
        m_testButton = new QPushButton(tr("Test"));
        form->addRow(QString(), m_testButton);

        // 空 = 不过滤；复位回全局默认清单
        m_extFilter = new ExtensionFilterEdit;
        form->addRow(tr("Filter"), m_extFilter);
        auto *extNote =
            new QLabel(tr("Space-separated file extensions; leave empty for no filter"));
        extNote->setEnabled(false);
        form->addRow(extNote);

        layout->addLayout(form);
        enableTransitOption(std::optional<bool>());
        layout->addStretch(1);

        if (m_seed) {
            m_urlEdit->setText(m_seed->serverUrl());
            m_serverId = m_seed->serverId();
            m_token = m_seed->token();
            m_userId = m_seed->userId();
            m_extFilter->setText(m_seed->extensions().join(QLatin1Char(' ')));
        } else {
            m_extFilter->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
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
            // 空 / 非法地址就地弹可读文案，避免任务里拼出相对 URL 只报网络错误
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
    QString urlValue() const { return JellyfinProvider::normalizeServerUrl(m_urlEdit->text()); }
    QStringList extensionsValue() const { return Util::parseExtensions(m_extFilter->text()); }

    // 绿 / 灰用 CSS 关键字色，亮暗主题下均可读。登录后禁用 URL 框：地址与凭据
    // 成对生效
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
    ExtensionFilterEdit *m_extFilter = nullptr;
    // 对话框 accept 期经 build 进入提供者实例
    QString m_serverId;
    QString m_token;
    QString m_userId;
};

ProviderSettingsPage *createJellyfinSettingsPage(const PlaylistProvider *seed,
                                                 const std::shared_ptr<ServiceRegistry> &registry,
                                                 QWidget *parent) {
    return new JellyfinSettingsPage(seed ? static_cast<const JellyfinProvider *>(seed) : nullptr,
                                    registry, parent);
}

// 本文件定义 Q_OBJECT 类（JellyfinQuickConnectDialog / JellyfinSettingsPage），
// AUTOMOC 要求显式包含同名 .moc
#include "JellyfinProvider.moc"

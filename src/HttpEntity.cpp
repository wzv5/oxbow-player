#include "pch.h"

#include "HttpEntity.h"

#include "HttpClient.h"
#include "ServiceRegistry.h"

namespace {
// N35 修订 3（实施修正）：HTTP 日期（IMF-fixdate，RFC 7231）解析。Qt 的
// Qt::RFC2822Date 有两条官方确认的严格限制（探针实证，Windows/Linux 一致）：
// ① 不支持 GMT/UTC/UT 字母时区后缀（数字 +hhmm 才受支持）；② 星期与日期不一致
// 即 invalid（视为数据校验的一部分，保留——服务器给的星期不准时按"值未知"处理）。
// 另实证可依赖的行为：无时区后缀按 0 时区解释（Last-Modified 恒为 UTC 墙钟，
// 正合需求）；日可单数字；解析结果即正确时刻（toLocalTime 得本地展示）。
// 故仅需处理时区后缀：字母后缀统一替换为等价 +0000，交原生解析。
QDateTime parseHttpDate(const QByteArray &raw) {
    QString s = QString::fromLatin1(raw).trimmed();
    if (s.isEmpty())
        return {};

    // 字母时区后缀 → 等价 +0000（数字时区保留原样；无后缀即 0 时区，与
    // Last-Modified 恒为 UTC 墙钟的语义吻合）
    static const char *kZoneNames[] = {"GMT", "UTC", "UT"};
    for (const char *z : kZoneNames) {
        const QString suffix = QStringLiteral(" ") + QString::fromLatin1(z);
        if (s.endsWith(suffix, Qt::CaseInsensitive)) {
            s.chop(suffix.size());
            s += QStringLiteral(" +0000");
            break;
        }
    }

    const QDateTime dt = QDateTime::fromString(s, Qt::RFC2822Date);
    return dt.toLocalTime(); // invalid 时保持 invalid，调用方按"值未知"丢弃
}
} // namespace

HttpEntity::HttpEntity(QString playUrl, const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistEntity(std::move(playUrl), registry) {
    Q_ASSERT(registry != nullptr);

    // 显示名（N35 §2.2）：先剥 query / fragment（首个 ? 或 # 之后丢弃，避免末段
    // 混入 "?token=xx"），取末段，经 percent-decode 后 setFilename（基类 deriveNames
    // 用 QFileInfo 取末段、不解码，否则列表出现 "%20"、"%E4%B8%AD" 之类）；
    // 解后为空（如以 / 结尾、纯主机地址）回退基类行为（title 已落 playUrl）。
    // 构造参数名遮蔽基类同名访问器，取 playUrl 须显式限定
    const QString raw = PlaylistEntity::playUrl();
    const qsizetype cutA = raw.indexOf(QLatin1Char('?'));
    const qsizetype cutB = raw.indexOf(QLatin1Char('#'));
    qsizetype cut = raw.size();
    if (cutA >= 0)
        cut = qMin(cut, cutA);
    if (cutB >= 0)
        cut = qMin(cut, cutB);
    const QString spec = raw.left(cut);
    const qsizetype slash = spec.lastIndexOf(QLatin1Char('/'));
    const QString lastSeg = slash >= 0 ? spec.mid(slash + 1) : spec;
    if (!lastSeg.isEmpty()) {
        const QString decoded = QUrl::fromPercentEncoding(lastSeg.toUtf8());
        if (!decoded.isEmpty())
            setFilename(decoded);
    }
}

// 文件级元数据回退路（N35 §2.4 经修订 2）：未注入时向 registry 现查 client，
// exec 阻塞求值（N34 文件头："无取消需求的调用点用 exec"——仅任务线程调用）。
// 取消语义为结果静默丢弃：调用方不等在途项、不采用其结果，故无取消通道、
// 体内零轮询。失败或超时返回空 FileMeta 入缓存不再重试。缺陷短路由 exec
// 内部处理（N34），调用方不持缺陷表知识。
FileMeta HttpEntity::computeFileMeta() const {
    // 装配错误：启动期必已 provide（N35 §2.3），永不命中；断言守候 + fail-safe
    const std::shared_ptr<HttpClient> client =
        registry() ? registry()->resolve<HttpClient>() : nullptr;
    if (!client) {
        Q_ASSERT(false);
        return {};
    }

    HttpClient::Request req;
    req.url = QUrl(playUrl()); // 绝对 URL：与 baseUrl 解绑，注册表 client 不绑 host
    req.method = QByteArrayLiteral("HEAD");
    req.headersOnly = true;

    const HttpClient::Response resp = client->exec(req);

    FileMeta meta;
    if (!resp.canceled && resp.status >= 200 && resp.status < 300) {
        const QByteArray len = resp.headers.value(QByteArrayLiteral("content-length"));
        if (!len.isEmpty()) {
            bool ok = false;
            const qint64 sz = len.toLongLong(&ok);
            if (ok && sz >= 0)
                meta.size = sz;
        }
        const QByteArray lm = resp.headers.value(QByteArrayLiteral("last-modified"));
        if (!lm.isEmpty()) {
            // 解析失败返回 invalid：不置 optional（等价于头缺失，按"值未知"处理）
            const QDateTime modified = parseHttpDate(lm);
            if (modified.isValid())
                meta.modified = modified;
        }
    }
    return meta;
}

// 恒不被调（supportsMediaMeta 为假）
PlaylistEntity::MediaMeta HttpEntity::computeMediaMeta() const { return {}; }

// 通用文件图标（与 UnknownEntity 同形，N35 §2.2）；仅 GUI 线程调用。
// provider 无状态，函数内 static 复用一次构造
QIcon HttpEntity::computeIcon() const {
    static QFileIconProvider provider;
    return provider.icon(QFileIconProvider::File);
}

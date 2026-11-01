#include "StreamTransit.h"
#include "pch.h"

namespace {

// 前缀长度派生量：由 scheme 常量运算得出，禁止手算（N51 坑 1）。scheme 本体
// 在 StreamTransit.h（唯一持有者），本文件不复制。
constexpr qsizetype kSchemeLen = sizeof(StreamTransit::kScheme) - 1;

// 尾段转义（N148）：只取必要项，中文与空格原样——全量编码会把字幕缓存名的超长带回来。
// '%' 必须在集内：mpv 的 filename / media-title 属性取基名前先对 URL unescape，不转义
// 就被那条显示链错误解码，转义的严格可逆也随之丢掉。集内其余是 Windows 文件名非法字符
// 与 C0（crypt 明文可能来自 Linux 端 rclone）。'/' 不转义：单个路径段表达不了它，真出现
// 只会被读成段边界。
QString escapeTail(const QString &name) {
    QString out;
    out.reserve(name.size() + 8);
    const char hex[] = "0123456789ABCDEF";
    for (QChar ch : name) {
        const char16_t c = ch.unicode();
        const bool escape = c <= 0x1F || c == u'%' || c == u'#' || c == u'?' || c == u'\\' ||
                            c == u':' || c == u'*' || c == u'"' || c == u'<' || c == u'>' ||
                            c == u'|';
        if (!escape) {
            out.append(ch);
            continue;
        }
        out.append(u'%');
        out.append(QLatin1Char(hex[(c >> 4) & 0xF]));
        out.append(QLatin1Char(hex[c & 0xF]));
    }
    return out;
}

} // namespace

namespace StreamTransit {

bool isTransitUri(const QString &path) {
    return path.startsWith(QLatin1String(StreamTransit::kScheme), Qt::CaseInsensitive);
}

QString mint(const QUuid &listUuid, const QString &identity, const QString &tail) {
    Q_ASSERT(!listUuid.isNull()); // nil 身份属调用方违约；Release 按下式返空串
    if (listUuid.isNull() || identity.isEmpty())
        return {};

    // 凭据双保险：调用方（基类下发点）应已 Util::Url::stripCredentials；此处守候。
    // 检测不依赖 Util：仅当身份含 "://" 才存在 authority，取其区间查 '@'。
    const qsizetype schemeSep = identity.indexOf(QStringLiteral("://"));
    if (schemeSep > 0) {
        const qsizetype authStart = schemeSep + 3;
        const qsizetype pathStart = identity.indexOf(u'/', authStart);
        const qsizetype authEnd = pathStart < 0 ? identity.size() : pathStart;
        const qsizetype at = identity.indexOf(u'@', authStart);
        const bool hasUserinfo = at >= 0 && at < authEnd;
        Q_ASSERT(!hasUserinfo);
        if (hasUserinfo)
            return {};
    }

    // unreserved 之外全编码（含 '/' → %2F）：整段只占一个路径段。
    const QByteArray segment = identity.toUtf8().toPercentEncoding();
    QString uri = QLatin1String(StreamTransit::kScheme) +
                  listUuid.toString(QUuid::StringFormat::WithoutBraces) + u'/' +
                  QString::fromUtf8(segment);
    // N148：空尾段不追加分隔符，产出与旧形态逐字节相同
    if (!tail.isEmpty())
        uri += u'/' + escapeTail(tail);
    return uri;
}

QUuid listUuidOfUri(const QString &uri) {
    // 形态已由 isTransitUri（或调用方）判定过才走到这里；只用通用 URI 语法 "://"
    // 定位 authority，协议名与前缀长度的知识全在 L1（N51 坑 1 的纪律照旧）
    const qsizetype schemeEnd = uri.indexOf(QStringLiteral("://"));
    if (schemeEnd < 0)
        return {};
    const qsizetype start = schemeEnd + 3;
    const qsizetype slash = uri.indexOf(u'/', start);
    if (slash < 0)
        return {};
    return QUuid::fromString(uri.mid(start, slash - start)); // 解不出即 nil，nil 恒不命中
}

QString unwrap(const QString &path) {
    if (!isTransitUri(path))
        return path;

    // oxbow://<uuid>/<身份段>[/<尾段>]：slash <= 0 覆盖两栏非法——authority 空（==0）与
    // 无身份段（<0）；身份段止于下一个 '/'，尾段不参与还原；段在但解出空串同样原样返回。
    // 命中即返回身份原串，重复施加不再命中前缀，故幂等。
    const QString rest = path.mid(kSchemeLen);
    const qsizetype slash = rest.indexOf(u'/');
    if (slash <= 0)
        return path;
    const qsizetype next = rest.indexOf(u'/', slash + 1);
    const QString segment = next < 0 ? rest.mid(slash + 1) : rest.mid(slash + 1, next - slash - 1);
    if (segment.isEmpty())
        return path;
    const QString identity = QUrl::fromPercentEncoding(segment.toUtf8());
    if (identity.isEmpty())
        return path;
    return identity;
}

} // namespace StreamTransit

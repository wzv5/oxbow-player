#include "StreamTransit.h"
#include "pch.h"

namespace {

// 前缀长度派生量：由 scheme 常量运算得出，禁止手算（N51 坑 1）。scheme 本体
// 在 StreamTransit.h（唯一持有者），本文件不复制。
constexpr qsizetype kSchemeLen = sizeof(StreamTransit::kScheme) - 1;

} // namespace

namespace StreamTransit {

bool isTransitUri(const QString &path) {
    return path.startsWith(QLatin1String(StreamTransit::kScheme), Qt::CaseInsensitive);
}

QString mint(const QUuid &listUuid, const QString &identity) {
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
    return QLatin1String(StreamTransit::kScheme) +
           listUuid.toString(QUuid::StringFormat::WithoutBraces) + u'/' +
           QString::fromUtf8(segment);
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

    // oxbow://<uuid>/<segment>：slash <= 0 覆盖两栏非法——authority 空（==0）
    // 与无条目段（<0）；段在但解出空串同样原样返回。命中即返回身份原串，
    // 重复施加不再命中前缀，故幂等。
    const QString tail = path.mid(kSchemeLen);
    const qsizetype slash = tail.indexOf(u'/');
    if (slash <= 0)
        return path;
    const QString segment = tail.mid(slash + 1);
    if (segment.isEmpty())
        return path;
    const QString identity = QUrl::fromPercentEncoding(segment.toUtf8());
    if (identity.isEmpty())
        return path;
    return identity;
}

} // namespace StreamTransit

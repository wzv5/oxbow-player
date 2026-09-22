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

QString mint(const QString &uuid, const QString &identity) {
    if (uuid.isEmpty() || identity.isEmpty())
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
    Q_ASSERT(!uuid.contains(u'/'));

    // unreserved 之外全编码（含 '/' → %2F）：整段只占一个路径段。
    const QByteArray segment = identity.toUtf8().toPercentEncoding();
    return QLatin1String(StreamTransit::kScheme) + uuid + u'/' + QString::fromUtf8(segment);
}

QString unwrap(const QString &path) {
    if (!isTransitUri(path))
        return path;

    // mpvqt://<uuid>/<segment>：slash <= 0 覆盖两栏非法——authority 空（==0）
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

QVariantMap perFileOptions(const QString &path) {
    if (!isTransitUri(path))
        return {};
    return QVariantMap{{QStringLiteral("cache"), QStringLiteral("yes")}};
}

} // namespace StreamTransit

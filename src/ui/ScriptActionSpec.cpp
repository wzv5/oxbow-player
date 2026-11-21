#include "pch.h"

#include "ScriptActionSpec.h"

#include "Util.h"

namespace ScriptActionSpec {

namespace {

// boundId 与 isValidId 共用的字符集判据
bool validIdBody(QStringView body) {
    if (body.isEmpty() || body.size() > 64)
        return false;
    for (const QChar ch : body) {
        const char16_t c = ch.unicode();
        const bool ok = (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') ||
                        (c >= u'A' && c <= u'Z') || c == u'_' || c == u'.' || c == u'-';
        if (!ok)
            return false;
    }
    return true;
}

} // namespace

ParseResult parse(const QByteArray &json) {
    ParseResult result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        result.error = ParseError::BadJson;
        result.detail = parseError.error == QJsonParseError::NoError
                            ? QStringLiteral("root is not an object")
                            : parseError.errorString();
        return result;
    }
    const QJsonObject object = doc.object();
    const auto stringValue = [&object](QLatin1String key) -> std::optional<QString> {
        const auto it = object.find(key);
        if (it == object.end() || !it->isString())
            return std::nullopt;
        return it->toString();
    };
    const auto boolValue = [&object](QLatin1String key) -> std::optional<bool> {
        const auto it = object.find(key);
        if (it == object.end() || !it->isBool())
            return std::nullopt;
        return it->toBool();
    };
    result.patch.command = stringValue(QLatin1String("command"));
    result.patch.callback = stringValue(QLatin1String("callback"));
    result.patch.icon = stringValue(QLatin1String("icon"));
    result.patch.tooltip = stringValue(QLatin1String("tooltip"));
    result.patch.checked = boolValue(QLatin1String("checked"));
    result.patch.disabled = boolValue(QLatin1String("disabled"));
    result.patch.hidden = boolValue(QLatin1String("hidden"));
    return result;
}

ScriptAction apply(const ScriptAction &base, const ScriptActionPatch &patch) {
    ScriptAction merged = base;
    if (patch.command)
        merged.command = *patch.command;
    if (patch.callback)
        merged.callback = *patch.callback;
    if (patch.icon)
        merged.icon = *patch.icon;
    if (patch.tooltip)
        merged.tooltip = *patch.tooltip;
    if (patch.checked) {
        merged.checkable = true;
        merged.checked = *patch.checked;
    }
    if (patch.disabled)
        merged.disabled = *patch.disabled;
    if (patch.hidden)
        merged.hidden = *patch.hidden;
    return merged;
}

QString boundId(const QString &command) {
    if (!command.startsWith(u'@'))
        return QString();
    const QStringView body = QStringView(command).sliced(1);
    return validIdBody(body) ? body.toString() : QString();
}

bool isValidId(const QString &id) { return validIdBody(QStringView(id)); }

QString eventName(const QString &event) {
    return Util::scriptEventName(QStringLiteral("script-action"), event);
}

} // namespace ScriptActionSpec

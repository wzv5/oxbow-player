#include "pch.h"

#include "playlist/ScriptListSpec.h"

#include "Logging.h"
#include "Util.h"

namespace ScriptListSpec {
namespace {

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

// double(INT64_MAX) 恰为 2^63，等号成立时转 int64 已越界，故上界一律取严格小于
constexpr double kInt64UpperBound = static_cast<double>(std::numeric_limits<qint64>::max());

// 选项值只收标量：数字 / 布尔转文本，数组 / 对象 / null 忽略
QString scalarText(const QJsonValue &value) {
    if (value.isString())
        return value.toString();
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) {
        const double number = value.toDouble();
        // 整数值落整数文本，其余取 double 最短形态
        if (number >= static_cast<double>(std::numeric_limits<qlonglong>::min()) &&
            number < kInt64UpperBound &&
            number == static_cast<double>(static_cast<qlonglong>(number)))
            return QString::number(static_cast<qlonglong>(number));
        return QString::number(number);
    }
    return QString();
}

// 正 int64：非数 / 非正 / 越界即未给（越界不猜值，交调用方按未给处理）
std::optional<qint64> positiveInt64(const QJsonValue &value, bool integral) {
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (!(number > 0 && number < kInt64UpperBound))
        return std::nullopt;
    const qint64 asInt = static_cast<qint64>(number);
    if (integral && number != static_cast<double>(asInt))
        return std::nullopt;
    return asInt;
}

// 正整数（size）：还须无小数部分
std::optional<qint64> positiveInteger(const QJsonValue &value) {
    return positiveInt64(value, true);
}

// 正数（duration）：非数 / 非正即未给
std::optional<double> positiveNumber(const QJsonValue &value) {
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    return number > 0 ? std::optional<double>(number) : std::nullopt;
}

// 正数 unix 秒（created / modified / recordedDate）：小数截断
std::optional<qint64> positiveSeconds(const QJsonValue &value) {
    return positiveInt64(value, false);
}

ContainerData parseContainer(const QJsonObject &object) {
    ContainerData container;
    container.id = object.value(QLatin1String("id")).toString();
    container.title = object.value(QLatin1String("title")).toString();
    container.path = object.value(QLatin1String("path")).toString();
    if (container.title.isEmpty())
        container.title = container.id; // 缺省用 id
    return container;
}

ItemData parseItem(const QJsonObject &object) {
    ItemData item;
    item.url = object.value(QLatin1String("url")).toString();
    item.displayName = object.value(QLatin1String("displayName")).toString();
    item.filename = object.value(QLatin1String("filename")).toString();
    if (const auto options = object.value(QLatin1String("options")); options.isObject()) {
        const QJsonObject optionObject = options.toObject();
        for (auto it = optionObject.begin(); it != optionObject.end(); ++it) {
            const QString text = scalarText(it.value());
            if (!text.isEmpty())
                item.options.insert(it.key(), text);
        }
    }
    if (const auto file = object.value(QLatin1String("file")); file.isObject()) {
        const QJsonObject fileObject = file.toObject();
        item.fileSize = positiveInteger(fileObject.value(QLatin1String("size")));
        item.fileCreated = positiveSeconds(fileObject.value(QLatin1String("created")));
        item.fileModified = positiveSeconds(fileObject.value(QLatin1String("modified")));
    }
    if (const auto metadata = object.value(QLatin1String("metadata")); metadata.isObject()) {
        const QJsonObject metaObject = metadata.toObject();
        item.metaFormat = metaObject.value(QLatin1String("format")).toString();
        item.metaDuration = positiveNumber(metaObject.value(QLatin1String("duration")));
        if (const auto recorded = positiveSeconds(metaObject.value(QLatin1String("recordedDate"))))
            item.metaRecordedDate = QDateTime::fromSecsSinceEpoch(*recorded); // 按本地时区落
        item.metaTitle = metaObject.value(QLatin1String("title")).toString();
        item.metaAlbum = metaObject.value(QLatin1String("album")).toString();
        item.metaPerformer = metaObject.value(QLatin1String("performer")).toString();
    }
    return item;
}

} // namespace

bool isValidListId(const QString &id) { return validIdBody(QStringView(id)); }

QString eventName(const QString &event) {
    return Util::scriptEventName(QStringLiteral("script-playlist"), event);
}

LayerData parseLayer(const QByteArray &json) {
    LayerData layer;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (doc.isNull() || !doc.isObject())
        return layer; // 顶层非对象：整层非法

    const QJsonObject object = doc.object();
    // 脚本自定错误：存在即须为字符串；非空即整层作废，内容不再解析
    if (const auto error = object.value(QLatin1String("error"));
        !error.isUndefined() && !error.isNull()) {
        if (!error.isString())
            return layer; // 存在但不是字符串：整层非法
        layer.error = error.toString().trimmed();
        if (!layer.error.isEmpty()) {
            layer.valid = true;
            return layer;
        }
    }

    QSet<QString> containerIds;
    if (const auto containers = object.value(QLatin1String("containers"));
        !containers.isUndefined() && !containers.isArray())
        return layer; // 存在但不是数组：整层非法
    else if (containers.isArray())
        for (const auto &entry : containers.toArray()) {
            if (!entry.isObject())
                continue;
            ContainerData container = parseContainer(entry.toObject());
            if (!isValidListId(container.id)) {
                OXBOW_LOG_WARNING() << "script-playlist: dropped container with invalid id";
                continue;
            }
            if (containerIds.contains(container.id)) {
                // 同层重复 id 只留首个
                OXBOW_LOG_WARNING()
                    << "script-playlist: dropped duplicate container id" << container.id;
                continue;
            }
            containerIds.insert(container.id);
            layer.containers.append(std::move(container));
        }

    if (const auto items = object.value(QLatin1String("items"));
        !items.isUndefined() && !items.isArray())
        return layer; // 存在但不是数组：整层非法
    else if (items.isArray())
        for (const auto &entry : items.toArray()) {
            if (!entry.isObject()) {
                ++layer.skippedCount;
                continue;
            }
            ItemData item = parseItem(entry.toObject());
            if (item.url.isEmpty() || !Util::Url::isDefiniteForm(item.url)) {
                ++layer.skippedCount;
                OXBOW_LOG_WARNING() << "script-playlist: skipped an item without a definite url";
                continue;
            }
            layer.items.append(std::move(item));
        }

    layer.valid = true;
    return layer;
}

} // namespace ScriptListSpec

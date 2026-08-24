#include "pch.h"

#include "MpvController.h"

namespace {

const QEvent::Type WakeupEventType = QEvent::Type(QEvent::registerEventType());

enum ObservedProperty {
    PropTimePos = 1,
    PropDuration,
    PropPause,
    PropVolume,
    PropPlaylistPos,
    PropPlaylist,
    PropIdleActive,
    // 导航器面板（N1）
    PropVideoZoom,
    PropVideoPanX,
    PropVideoPanY,
    PropWidth,
    PropHeight,
    // 媒体元数据与流参数（N4）
    PropMetadata,
    PropFileFormat,      // file-format
    PropVideoFormat,     // video-format
    PropParamW,          // video-params/w
    PropParamH,          // video-params/h
    PropContainerFps,    // container-fps
    PropAudioCodecName,  // audio-codec-name
    PropChannelCount,    // audio-params/channel-count
    PropSampleRate,      // audio-params/samplerate
};

QVariantList parsePlaylist(const mpv_node *node)
{
    QVariantList items;
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY)
        return items;

    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; ++i) {
        QVariantMap item;
        mpv_node *entry = &list->values[i];
        if (entry->format != MPV_FORMAT_NODE_MAP) {
            items.append(item);
            continue;
        }
        mpv_node_list *map = entry->u.list;
        for (int j = 0; j < map->num; ++j) {
            const QString key = QString::fromUtf8(map->keys[j]);
            const mpv_node *value = &map->values[j];
            if (key == QLatin1String("filename") && value->format == MPV_FORMAT_STRING)
                item[QStringLiteral("filename")] = QString::fromUtf8(value->u.string);
            else if (key == QLatin1String("title") && value->format == MPV_FORMAT_STRING)
                item[QStringLiteral("title")] = QString::fromUtf8(value->u.string);
            else if (key == QLatin1String("current") && value->format == MPV_FORMAT_FLAG)
                item[QStringLiteral("current")] = value->u.flag != 0;
        }
        items.append(item);
    }
    return items;
}

// 浅层转换：metadata 字典（字符串标签）转 QVariantMap
QVariantMap parseNodeMap(const mpv_node *node)
{
    QVariantMap result;
    if (!node || node->format != MPV_FORMAT_NODE_MAP)
        return result;
    mpv_node_list *map = node->u.list;
    for (int i = 0; i < map->num; ++i) {
        if (map->values[i].format == MPV_FORMAT_STRING)
            result[QString::fromUtf8(map->keys[i])] = QString::fromUtf8(map->values[i].u.string);
    }
    return result;
}

} // namespace

MpvController::MpvController(mpv_handle *mpv, QObject *parent)
    : QObject(parent), m_mpv(mpv)
{
    mpv_set_wakeup_callback(m_mpv, &MpvController::wakeupCallback, this);

    // 观察属性后 mpv 会立即推送一次当前值，无需手动读取初始状态
    mpv_observe_property(m_mpv, PropTimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropVolume, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropPlaylistPos, "playlist-pos", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropPlaylist, "playlist", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, PropIdleActive, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropVideoZoom, "video-zoom", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropVideoPanX, "video-pan-x", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropVideoPanY, "video-pan-y", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropWidth, "width", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropHeight, "height", MPV_FORMAT_INT64);
    // N4：媒体元数据与流参数。观察即推送首值；流参数均为离散属性，
    // 仅在加载与换轨时变化，事件量可忽略
    mpv_observe_property(m_mpv, PropMetadata, "metadata", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, PropFileFormat, "file-format", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, PropVideoFormat, "video-format", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, PropParamW, "video-params/w", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropParamH, "video-params/h", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropContainerFps, "container-fps", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropAudioCodecName, "audio-codec-name", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, PropChannelCount, "audio-params/channel-count", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropSampleRate, "audio-params/samplerate", MPV_FORMAT_INT64);
}

MpvController::~MpvController()
{
    if (m_mpv)
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
}

void MpvController::command(const QString &cmd)
{
    mpv_command_string(m_mpv, cmd.toUtf8().constData());
}

void MpvController::command(const QStringList &args)
{
    if (args.isEmpty())
        return;
    QList<QByteArray> utf8;
    utf8.reserve(args.size());
    for (const QString &arg : args)
        utf8.append(arg.toUtf8());
    QVector<const char *> argv;
    argv.reserve(utf8.size() + 1);
    for (const QByteArray &arg : utf8)
        argv.append(arg.constData());
    argv.append(nullptr);
    mpv_command(m_mpv, argv.data());
}

void MpvController::setProperty(const QString &name, const QString &value)
{
    mpv_set_property_string(m_mpv, name.toUtf8().constData(), value.toUtf8().constData());
}

void MpvController::setProperty(const QString &name, int value)
{
    int64_t v = value;
    mpv_set_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_INT64, &v);
}

void MpvController::setProperty(const QString &name, double value)
{
    mpv_set_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_DOUBLE, &value);
}

void MpvController::setProperty(const QString &name, bool value)
{
    int v = value ? 1 : 0;
    mpv_set_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_FLAG, &v);
}

QString MpvController::propertyString(const QString &name)
{
    char *s = mpv_get_property_string(m_mpv, name.toUtf8().constData());
    QString result = s ? QString::fromUtf8(s) : QString();
    mpv_free(s);
    return result;
}

double MpvController::propertyDouble(const QString &name, double fallback)
{
    double v = fallback;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_DOUBLE, &v) < 0)
        return fallback; // 失败时不写入 v
    return v;
}

bool MpvController::propertyFlag(const QString &name, bool fallback)
{
    int v = fallback ? 1 : 0;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_FLAG, &v) < 0)
        return fallback;
    return v != 0;
}

void MpvController::wakeupCallback(void *ctx)
{
    // 该回调由 mpv 内部线程触发，禁止在其中调用 mpv 函数；
    // 仅向 GUI 线程投递事件，由 customEvent 中 drain 事件队列。
    auto *self = static_cast<MpvController *>(ctx);
    QCoreApplication::postEvent(self, new QEvent(WakeupEventType));
}

void MpvController::customEvent(QEvent *event)
{
    if (event->type() == WakeupEventType) {
        drainEvents();
        return;
    }
    QObject::customEvent(event);
}

void MpvController::drainEvents()
{
    for (;;) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE:
            handlePropertyChange(event);
            break;
        case MPV_EVENT_SHUTDOWN:
            emit mpvShutdown();
            break;
        default:
            break;
        }
    }
}

void MpvController::handlePropertyChange(const mpv_event *event)
{
    auto *prop = static_cast<mpv_event_property *>(event->data);
    if (!prop)
        return;

    switch (int(event->reply_userdata)) {
    case PropTimePos:
        // 停止/卸载后属性不可用（MPV_FORMAT_NONE），归零时间显示
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            emit timePosChanged(*static_cast<double *>(prop->data));
        else if (prop->format == MPV_FORMAT_NONE)
            emit timePosChanged(0.0);
        break;
    case PropDuration:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            emit durationChanged(*static_cast<double *>(prop->data));
        else if (prop->format == MPV_FORMAT_NONE)
            emit durationChanged(0.0);
        break;
    case PropPause:
        if (prop->format == MPV_FORMAT_FLAG && prop->data)
            emit pauseChanged(*static_cast<int *>(prop->data) != 0);
        break;
    case PropVolume:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            emit volumeChanged(*static_cast<double *>(prop->data));
        break;
    case PropPlaylistPos:
        // 停止后 playlist-pos 不可用（MPV_FORMAT_NONE），以 -1 表示无当前项
        if (prop->format == MPV_FORMAT_INT64 && prop->data)
            emit playlistPosChanged(int(*static_cast<int64_t *>(prop->data)));
        else if (prop->format == MPV_FORMAT_NONE)
            emit playlistPosChanged(-1);
        break;
    case PropPlaylist:
        if (prop->format == MPV_FORMAT_NODE && prop->data)
            emit playlistChanged(parsePlaylist(static_cast<mpv_node *>(prop->data)));
        break;
    case PropIdleActive:
        if (prop->format == MPV_FORMAT_FLAG && prop->data)
            emit idleActive(*static_cast<int *>(prop->data) != 0);
        break;
    case PropVideoZoom:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            emit videoZoomChanged(*static_cast<double *>(prop->data));
        else if (prop->format == MPV_FORMAT_NONE)
            emit videoZoomChanged(0.0);
        break;
    case PropVideoPanX:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            emit videoPanXChanged(*static_cast<double *>(prop->data));
        else if (prop->format == MPV_FORMAT_NONE)
            emit videoPanXChanged(0.0);
        break;
    case PropVideoPanY:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            emit videoPanYChanged(*static_cast<double *>(prop->data));
        else if (prop->format == MPV_FORMAT_NONE)
            emit videoPanYChanged(0.0);
        break;
    case PropWidth:
    case PropHeight: {
        // 尺寸两属性独立推送，合并缓存后成对上报；任一侧失效按 0 处理
        const int value = (prop->format == MPV_FORMAT_INT64 && prop->data)
                              ? int(*static_cast<int64_t *>(prop->data)) : 0;
        if (int(event->reply_userdata) == PropWidth)
            m_videoW = value;
        else
            m_videoH = value;
        emit videoSizeChanged(m_videoW, m_videoH);
        break;
    }
    case PropMetadata:
        if (prop->format == MPV_FORMAT_NODE && prop->data)
            emit mediaMetadataChanged(parseNodeMap(static_cast<mpv_node *>(prop->data)));
        else if (prop->format == MPV_FORMAT_NONE)
            emit mediaMetadataChanged(QVariantMap());
        break;
    // 流参数：任一属性变化即更新合并缓存并整体重发快照；
    // 属性不可用（NONE，文件卸载或该流不存在）时移除对应键
    case PropFileFormat:
    case PropVideoFormat:
    case PropAudioCodecName:
    case PropParamW:
    case PropParamH:
    case PropContainerFps:
    case PropChannelCount:
    case PropSampleRate: {
        QString key;
        switch (int(event->reply_userdata)) {
        case PropFileFormat:      key = QStringLiteral("fileFormat"); break;
        case PropVideoFormat:     key = QStringLiteral("videoCodec"); break;
        case PropAudioCodecName:  key = QStringLiteral("audioCodec"); break;
        case PropParamW:          key = QStringLiteral("videoW"); break;
        case PropParamH:          key = QStringLiteral("videoH"); break;
        case PropContainerFps:    key = QStringLiteral("fps"); break;
        case PropChannelCount:    key = QStringLiteral("channels"); break;
        case PropSampleRate:      key = QStringLiteral("sampleRate"); break;
        }
        if (prop->format == MPV_FORMAT_NONE)
            m_streamInfo.remove(key);
        else if (prop->format == MPV_FORMAT_STRING && prop->data)
            // client.h：STRING 格式的事件数据是 char**（镜像 get_property 出参），
            // 必须解一层引用；直接把指针值当字符串读会得到地址字节的"乱码"
            m_streamInfo[key] = QString::fromUtf8(*static_cast<char **>(prop->data));
        else if (prop->format == MPV_FORMAT_INT64 && prop->data)
            m_streamInfo[key] = qlonglong(*static_cast<int64_t *>(prop->data));
        else if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            m_streamInfo[key] = *static_cast<double *>(prop->data);
        emit streamInfoChanged(m_streamInfo);
        break;
    }
    default:
        break;
    }
}

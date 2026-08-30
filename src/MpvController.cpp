#include "pch.h"

#include "MpvController.h"

namespace {

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
    // 媒体元数据（N4，文件级）+ 文件格式（N14 文件属性单设）
    PropMetadata,
    PropFileFormat,      // file-format（N14 文件属性）
    PropTrackList,       // track-list（N13，N14 后为唯一流源）
    PropChapterList,     // chapter-list（N15）
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

// N13：track-list 解析
QList<MpvController::TrackInfo> parseTrackList(const mpv_node *node)
{
    QList<MpvController::TrackInfo> result;
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY)
        return result;
    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; ++i) {
        mpv_node *entry = &list->values[i];
        if (entry->format != MPV_FORMAT_NODE_MAP)
            continue;
        MpvController::TrackInfo info;
        mpv_node_list *map = entry->u.list;
        for (int j = 0; j < map->num; ++j) {
            const QString key = QString::fromUtf8(map->keys[j]);
            const mpv_node *v = &map->values[j];
            if (key == QLatin1String("id") && v->format == MPV_FORMAT_INT64)
                info.id = v->u.int64;
            else if (key == QLatin1String("type") && v->format == MPV_FORMAT_STRING)
                info.type = QString::fromUtf8(v->u.string);
            else if (key == QLatin1String("title") && v->format == MPV_FORMAT_STRING)
                info.title = QString::fromUtf8(v->u.string);
            else if (key == QLatin1String("lang") && v->format == MPV_FORMAT_STRING)
                info.lang = QString::fromUtf8(v->u.string);
            else if (key == QLatin1String("codec") && v->format == MPV_FORMAT_STRING)
                info.codec = QString::fromUtf8(v->u.string);
            else if (key == QLatin1String("external") && v->format == MPV_FORMAT_FLAG)
                info.external = v->u.flag != 0;
            else if (key == QLatin1String("external-filename") && v->format == MPV_FORMAT_STRING)
                info.externalFilename = QString::fromUtf8(v->u.string);
            else if (key == QLatin1String("default") && v->format == MPV_FORMAT_FLAG)
                info.isDefault = v->u.flag != 0;
            else if (key == QLatin1String("selected") && v->format == MPV_FORMAT_FLAG)
                info.selected = v->u.flag != 0;
            else if (key == QLatin1String("demux-w") && v->format == MPV_FORMAT_INT64)
                info.demuxW = int(v->u.int64);
            else if (key == QLatin1String("demux-h") && v->format == MPV_FORMAT_INT64)
                info.demuxH = int(v->u.int64);
            else if (key == QLatin1String("demux-fps") && v->format == MPV_FORMAT_DOUBLE)
                info.demuxFps = v->u.double_;
            else if (key == QLatin1String("demux-samplerate") && v->format == MPV_FORMAT_INT64)
                info.demuxSamplerate = int(v->u.int64);
            else if (key == QLatin1String("demux-channel-count") && v->format == MPV_FORMAT_INT64)
                info.demuxChannelCount = int(v->u.int64);
            else if (key == QLatin1String("demux-bitrate") && v->format == MPV_FORMAT_INT64)
                info.demuxBitrate = v->u.int64;
            else if (key == QLatin1String("metadata") && v->format == MPV_FORMAT_NODE_MAP) {
                mpv_node_list *mmap = v->u.list;
                for (int k = 0; k < mmap->num; ++k) {
                    if (QLatin1String(mmap->keys[k]) == QLatin1String("BPS")
                        && mmap->values[k].format == MPV_FORMAT_STRING) {
                        bool ok = false;
                        qint64 bps = QString::fromUtf8(mmap->values[k].u.string).toLongLong(&ok);
                        if (ok)
                            info.metadataBPS = bps;
                    }
                }
            }
        }
        result.append(info);
    }
    return result;
}

// N15：chapter-list 解析
QList<MpvController::ChapterInfo> parseChapterList(const mpv_node *node)
{
    QList<MpvController::ChapterInfo> result;
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY)
        return result;
    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; ++i) {
        mpv_node *entry = &list->values[i];
        if (entry->format != MPV_FORMAT_NODE_MAP)
            continue;
        MpvController::ChapterInfo info;
        mpv_node_list *map = entry->u.list;
        for (int j = 0; j < map->num; ++j) {
            const QString key = QString::fromUtf8(map->keys[j]);
            const mpv_node *v = &map->values[j];
            if (key == QLatin1String("title") && v->format == MPV_FORMAT_STRING)
                info.title = QString::fromUtf8(v->u.string);
            else if (key == QLatin1String("time") && v->format == MPV_FORMAT_DOUBLE)
                info.time = v->u.double_;
            else if (key == QLatin1String("time") && v->format == MPV_FORMAT_INT64)
                info.time = double(v->u.int64);
        }
        result.append(info);
    }
    return result;
}

} // namespace

// B17：事件专用线程——阻塞 mpv_wait_event(-1)，事件就地分派。
// 生命周期由 SHUTDOWN 收口：收到 MPV_EVENT_SHUTDOWN 后 emit 并退出，
// 随后入口函数返回、mpv 销毁 client handle，插件自身从不销毁 handle，
// 故无需 stop 标志与析构清理。线程启动时机见 startEventThread()。
class MpvController::EventThread : public QThread
{
public:
    EventThread(MpvController *ctrl, mpv_handle *mpv)
        : m_ctrl(ctrl), m_mpv(mpv) {}

    void run() override
    {
        for (;;) {
            mpv_event *e = mpv_wait_event(m_mpv, -1);
            switch (e->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE:
                m_ctrl->handlePropertyChange(e);
                break;
            case MPV_EVENT_SHUTDOWN:
                emit m_ctrl->mpvShutdown();
                return;
            case MPV_EVENT_END_FILE: {
                auto *ef = static_cast<mpv_event_end_file *>(e->data);
                emit m_ctrl->endFile(ef && ef->reason == MPV_END_FILE_REASON_EOF);
                break;
            }
            default:
                break;
            }
        }
    }

private:
    MpvController *m_ctrl;
    mpv_handle *m_mpv;
};

MpvController::MpvController(mpv_handle *mpv, QObject *parent)
    : QObject(parent), m_mpv(mpv)
{
    // 事件线程只创建不启动：启动时机由宿主在嵌入属性就绪后经
    // startEventThread() 控制（见该函数注释的启动门控说明）
    m_eventThread = new EventThread(this, m_mpv);

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
    // N4：媒体元数据（文件级标签）
    mpv_observe_property(m_mpv, PropMetadata, "metadata", MPV_FORMAT_NODE);
    // N14：文件格式（文件属性单设）
    mpv_observe_property(m_mpv, PropFileFormat, "file-format", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, PropTrackList, "track-list", MPV_FORMAT_NODE);
    // N15：章节列表
    mpv_observe_property(m_mpv, PropChapterList, "chapter-list", MPV_FORMAT_NODE);
}

void MpvController::startEventThread()
{
    m_eventThread->start();
}

QSize MpvController::videoSize() const
{
    int w = 0, h = 0;
    mpv_get_property(m_mpv, "width", MPV_FORMAT_INT64, &w);
    mpv_get_property(m_mpv, "height", MPV_FORMAT_INT64, &h);
    return QSize(w, h);
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
    case PropFileFormat:
        if (prop->format == MPV_FORMAT_STRING && prop->data)
            emit fileFormatChanged(QString::fromUtf8(*static_cast<char **>(prop->data)));
        else if (prop->format == MPV_FORMAT_NONE)
            emit fileFormatChanged(QString());
        break;
    case PropTrackList:
        if (prop->format == MPV_FORMAT_NODE && prop->data)
            emit tracksChanged(parseTrackList(static_cast<mpv_node *>(prop->data)));
        else if (prop->format == MPV_FORMAT_NONE)
            emit tracksChanged(QList<TrackInfo>());
        break;
    case PropChapterList:
        if (prop->format == MPV_FORMAT_NODE && prop->data)
            emit chaptersChanged(parseChapterList(static_cast<mpv_node *>(prop->data)));
        else if (prop->format == MPV_FORMAT_NONE)
            emit chaptersChanged(QList<ChapterInfo>());
        break;
    default:
        break;
    }
}

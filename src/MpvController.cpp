#include "MpvController.h"

#include <QCoreApplication>
#include <QEvent>

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

int64_t MpvController::propertyInt64(const QString &name) const
{
    int64_t val = -1;
    mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_INT64, &val);
    return val;
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
    default:
        break;
    }
}

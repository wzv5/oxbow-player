#include "pch.h"

#include "MpvController.h"

#include "Util.h" // N61 §2.3：回显整形（playlist 条目 filename 进判据前唯一一道加工）

#include "FilterListWidget.h"

namespace {

enum ObservedProperty {
    PropTimePos = 1,
    PropDuration, // duration（B18：双通道 observe 半程，留守）
    PropPause,
    PropVolume,
    PropMute, // N38：静音开关（紧邻音量）
    PropPlaylistPos,
    PropPlaylist,
    PropIdleActive,
    // 导航器面板（N1）
    PropVideoZoom,
    PropVideoPanX,
    PropVideoPanY,
    // N40：width / height 两属性观察已移除，视频尺寸改由 VIDEO_RECONFIG
    // 事件驱动主动读取（见 publishVideoSize），
    // R3：音量上限（mpv 自管 volume-max 属性，运行时推送驱动 MainWindow 滑条量程）
    PropVolumeMax,
    // N87：console 开关状态。新 token 一律加在末尾——token 就是下发给 mpv 的
    // reply_userdata，插到中间会移位既有观察项的取值
    PropConsoleOpen,
    // N88：fullscreen 镜像（仍居末尾纪律之内）
    PropFullscreen,
    // N93：track-list 重回观察（仍居末尾纪律之内）——运行期增删轨道只有 observe
    // 覆盖得到，FILE_LOADED 主动读只管装载时刻
    PropTrackList,
    // B18：metadata / file-format / chapter-list 三项文件级属性不 observe，
    // 改由 FILE_LOADED 主动读（见 handleFileLoaded）
};

QVariantList parsePlaylist(const mpv_node *node) {
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
            else if (key == QLatin1String("current") && value->format == MPV_FORMAT_FLAG)
                item[QStringLiteral("current")] = value->u.flag != 0;
        }
        items.append(item);
    }
    return items;
}

// 浅层转换：metadata 字典（字符串标签）转 QVariantMap
QVariantMap parseNodeMap(const mpv_node *node) {
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
QList<MpvController::TrackInfo> parseTrackList(const mpv_node *node) {
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
            else if (key == QLatin1String("main-selection") && v->format == MPV_FORMAT_INT64)
                info.mainSelection = int(v->u.int64);
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
                    if (QLatin1String(mmap->keys[k]) == QLatin1String("BPS") &&
                        mmap->values[k].format == MPV_FORMAT_STRING) {
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
QList<MpvController::ChapterInfo> parseChapterList(const mpv_node *node) {
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

// N42：af / vf node 链表 → 条目列表。结构化读彻底绕开转义串解析（F4）；
// text 序列化：[@label:] + name [+ "=" 参数串]——位置参数（键 @n）按 n 升序
// 置于最前、命名参数随后按 key 排序（QMap 有序，F13），值经引号选择
// （[...] 配平优先、不配平回退 %n%，FilterListWidget::quoteParamValue）
QList<FilterEntry> parseFilterNode(const mpv_node *node) {
    QList<FilterEntry> result;
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY)
        return result;
    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; ++i) {
        const mpv_node *entry = &list->values[i];
        if (entry->format != MPV_FORMAT_NODE_MAP)
            continue;
        QString name;
        QString label;
        QList<QPair<int, QString>> positional; // 后缀 n → 值（键 "@0"/"@1"/...）
        QMap<QString, QString> named;
        bool enabled = true;
        mpv_node_list *map = entry->u.list;
        for (int j = 0; j < map->num; ++j) {
            const QString key = QString::fromUtf8(map->keys[j]);
            const mpv_node *v = &map->values[j];
            if (key == QLatin1String("enabled") && v->format == MPV_FORMAT_FLAG)
                enabled = v->u.flag != 0;
            else if (key == QLatin1String("label") && v->format == MPV_FORMAT_STRING)
                label = QString::fromUtf8(v->u.string);
            else if (key == QLatin1String("name") && v->format == MPV_FORMAT_STRING)
                name = QString::fromUtf8(v->u.string);
            else if (key == QLatin1String("params") && v->format == MPV_FORMAT_NODE_MAP) {
                mpv_node_list *pm = v->u.list;
                for (int k = 0; k < pm->num; ++k) {
                    const QString pk = QString::fromUtf8(pm->keys[k]);
                    const QString pv = (pm->values[k].format == MPV_FORMAT_STRING)
                                           ? QString::fromUtf8(pm->values[k].u.string)
                                           : QString();
                    if (pk.startsWith(u'@')) {
                        bool ok = false;
                        const int n = pk.mid(1).toInt(&ok);
                        // max 加括号：规避 windows.h 的 min/max 宏污染
                        positional.append({ok ? n : (std::numeric_limits<int>::max)(), pv});
                    } else {
                        named.insert(pk, pv);
                    }
                }
            }
        }
        if (name.isEmpty())
            continue; // 无名残缺条目：node 读侧不该出现，防御性跳过
        std::sort(positional.begin(), positional.end(),
                  [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
                      return a.first < b.first;
                  });
        QString text;
        if (!label.isEmpty())
            text = u'@' + label + u':';
        text += name;
        if (!positional.isEmpty() || !named.isEmpty()) {
            QStringList params;
            for (const auto &p : positional)
                params.append(FilterListWidget::quoteParamValue(p.second));
            for (auto it = named.cbegin(); it != named.cend(); ++it)
                params.append(it.key() + u'=' + FilterListWidget::quoteParamValue(it.value()));
            text += u'=';
            text += params.join(u':');
        }
        FilterEntry item;
        item.enabled = enabled;
        item.text = text;
        result.append(item);
    }
    return result;
}

} // namespace

// B17：事件专用线程——阻塞 mpv_wait_event(-1)，事件就地分派。
// 生命周期由 SHUTDOWN 收口：收到 MPV_EVENT_SHUTDOWN 后 emit 并退出，
// 随后入口函数返回、mpv 销毁 client handle，插件自身从不销毁 handle，
// 故无需 stop 标志与析构清理。线程启动时机见 startEventThread()。
class MpvController::EventThread : public QThread {
  public:
    EventThread(MpvController *ctrl, mpv_handle *mpv) : m_ctrl(ctrl), m_mpv(mpv) {}

    void run() override {
        for (;;) {
            mpv_event *e = mpv_wait_event(m_mpv, -1);
            switch (e->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE:
                m_ctrl->handlePropertyChange(e);
                break;
            case MPV_EVENT_GET_PROPERTY_REPLY:
                // N60：屏障读的终点。独立分派，不并入 handlePropertyChange 的
                // observe token switch（两者共用 reply_userdata 但按 event_id 分流）
                m_ctrl->handleBarrierReply(e);
                break;
            case MPV_EVENT_START_FILE:
                m_ctrl->handleStartFile();
                break;
            case MPV_EVENT_FILE_LOADED:
                m_ctrl->handleFileLoaded();
                break;
            case MPV_EVENT_VIDEO_RECONFIG:
                // N40：视频尺寸变化的主触发源（F2：覆盖起播配置、分辨率变化、
                // 滤镜变化、视频消失）。事件到达 ≠ 尺寸变了（F3① 同一变化会
                // 触发多次、前几次仍读旧值），取值与去重收口在 publishVideoSize
                m_ctrl->publishVideoSize();
                break;
            case MPV_EVENT_SHUTDOWN:
                emit m_ctrl->mpvShutdown();
                return;
            case MPV_EVENT_END_FILE: {
                // B18：reason 原值透传——EOF 判定（B9 续播清除）与会话态
                // 重置由消费者各自处理
                auto *ef = static_cast<mpv_event_end_file *>(e->data);
                emit m_ctrl->endFile(ef ? int(ef->reason) : int(MPV_END_FILE_REASON_ERROR));
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

MpvController::MpvController(mpv_handle *mpv, QObject *parent) : QObject(parent), m_mpv(mpv) {
    // 事件线程只创建不启动：启动时机由宿主在嵌入属性就绪后经
    // startEventThread() 控制（见该函数注释的启动门控说明）
    m_eventThread = new EventThread(this, m_mpv);

    // 观察属性后 mpv 会立即推送一次当前值，无需手动读取初始状态
    mpv_observe_property(m_mpv, PropTimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropVolume, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropMute, "mute", MPV_FORMAT_FLAG); // N38
    mpv_observe_property(m_mpv, PropPlaylistPos, "playlist-pos", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropPlaylist, "playlist", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, PropIdleActive, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropVideoZoom, "video-zoom", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropVideoPanX, "video-pan-x", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropVideoPanY, "video-pan-y", MPV_FORMAT_DOUBLE);
    // N40：width / height 不再观察——属性报解码尺寸且独立推送，改事件驱动（见 publishVideoSize）
    // R3：音量上限观测，启动后立即推送当前值（mpv 默认 130）
    mpv_observe_property(m_mpv, PropVolumeMax, "volume-max", MPV_FORMAT_DOUBLE);
    // N87：console 脚本的开关位。属性在 console.lua 首次写入前不存在，观察注册成功但无
    // 初值可推（NONE 不发射），故插件侧按"未收到推送即 false"处理
    mpv_observe_property(m_mpv, PropConsoleOpen, "user-data/mpv/console/open", MPV_FORMAT_FLAG);
    // N88：fullscreen 镜像。普通属性，注册即推初值（client.h "You always get an
    // initial change notification"），与 console 位"无初值可推"的口径相反
    mpv_observe_property(m_mpv, PropFullscreen, "fullscreen", MPV_FORMAT_FLAG);
    // N93：track-list 观察——双通道的 observe 半程（主动读半程在 handleFileLoaded）。
    // 覆盖运行期轨道增删与外部切换（console 的 sub-add / audio-add / sub-remove、
    // 改 sid 等）；手册未声明该属性不支持变更通知（反例是 input-bindings：明写
    // not supported，故它只在对话框打开时读一次、不观察）
    mpv_observe_property(m_mpv, PropTrackList, "track-list", MPV_FORMAT_NODE);
    // B18：metadata / file-format / chapter-list 改 FILE_LOADED 主动读
    // （见 handleFileLoaded），不 observe；track-list 见上
}

void MpvController::startEventThread() { m_eventThread->start(); }

// B18：START_FILE 时刷新当前文件缓存，作为后续 FILE_LOADED 主动读与
// duration observe 推送的归属键（仅事件线程读写）
// N61 §2.3：归属键的来源由 path 属性换成本出口（时序实测见该节——START_FILE 当场
// playlist-pos 与 playlist/<pos>/filename 已指向本条，与随后 FILE_LOADED 同值）；
// 信号签名不动，串参无人消费
void MpvController::handleStartFile() { m_currentFile = currentPlayingPath(); }

// B18：FILE_LOADED 时以缓存为归属键，当场主动读 5 项文件级属性并发射。
// 与 observe 相比：首值即时（不等 flush）、规避"等值不重推"（连续两文件
// 值相同也会重新发送）；duration 为双通道的即时读半程（见 PropDuration）
void MpvController::handleFileLoaded() {
    const QString path = m_currentFile;

    double d = 0;
    if (mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &d) == 0 && d > 0)
        emit durationChanged(path, d);
    else
        emit durationChanged(path, 0.0);

    mpv_node node;
    if (mpv_get_property(m_mpv, "metadata", MPV_FORMAT_NODE, &node) == 0) {
        emit mediaMetadataChanged(path, parseNodeMap(&node));
        mpv_free_node_contents(&node);
    } else {
        emit mediaMetadataChanged(path, QVariantMap());
    }

    char *fmt = mpv_get_property_string(m_mpv, "file-format");
    emit fileFormatChanged(path, fmt ? QString::fromUtf8(fmt) : QString());
    mpv_free(fmt);

    if (mpv_get_property(m_mpv, "track-list", MPV_FORMAT_NODE, &node) == 0) {
        emit tracksChanged(path, parseTrackList(&node));
        mpv_free_node_contents(&node);
    } else {
        emit tracksChanged(path, QList<TrackInfo>());
    }

    if (mpv_get_property(m_mpv, "chapter-list", MPV_FORMAT_NODE, &node) == 0) {
        emit chaptersChanged(path, parseChapterList(&node));
        mpv_free_node_contents(&node);
    } else {
        emit chaptersChanged(path, QList<ChapterInfo>());
    }
}

// 安全约定：mpv_get_property 的接收缓冲必须与 MPV_FORMAT_* 的基本类型同宽
// （FLAG=int / INT64=int64_t / DOUBLE=double，见 mpv/client.h 的 mpv_node 联合体）。
// 窄类型接收宽写入会踩坏相邻栈槽——B19 即 int 接 INT64，第二次写入的高 4 字节
// 把 w 覆成 0，导航器播种恒判"无视频"。新增调用点务必按此表核对。
QSize MpvController::videoSize() const {
    // N40：公开入口与事件侧发布共用同一条取值路径（纯读，不写去重状态）
    return readVideoSize();
}

// N40：视频尺寸取值收口（F1/F3②/F5）——先读 video-out-params/w、/h（滤镜后
// 输出尺寸，与实际渲染一致）；不可读回退 width、height（覆盖"起播早期 out
// 未就绪"，两属性可用时机互补）；仍不可读或读得 0 判定无视频（0,0）。
// 只读 /w、/h 帧尺寸，绝不读 /dw、/dh 显示尺寸（F7：随显示侧参数漂移）
QSize MpvController::readVideoSize() const {
    int64_t w = 0, h = 0;
    const bool outOk = mpv_get_property(m_mpv, "video-out-params/w", MPV_FORMAT_INT64, &w) == 0 &&
                       mpv_get_property(m_mpv, "video-out-params/h", MPV_FORMAT_INT64, &h) == 0;
    if (!outOk || w <= 0 || h <= 0) {
        w = 0;
        h = 0;
        mpv_get_property(m_mpv, "width", MPV_FORMAT_INT64, &w);
        mpv_get_property(m_mpv, "height", MPV_FORMAT_INT64, &h);
        if (w <= 0 || h <= 0)
            return QSize(0, 0); // 无视频（或读值时机不完整，按无视频口径处理）
    }
    return QSize(int(w), int(h));
}

// N40：视频尺寸发布收口（事件线程）——所有触发源（VIDEO_RECONFIG 主触发、
// idle-active 假转真兜底）汇入此处：读值 → 与上次已发布值比对 → 不同才
// 写入并 emit。F3① 决定去重非做不可：一次 vf 变化会来多次 reconfig、前几次
// 读到的还是旧值，不去重会连带产生多次多余的窗口适配
void MpvController::publishVideoSize() {
    const QSize size = readVideoSize();
    if (size.width() == m_videoW && size.height() == m_videoH)
        return;
    m_videoW = size.width();
    m_videoH = size.height();
    emit videoSizeChanged(m_videoW, m_videoH);
}

// N96：返回 mpv_command_string 的错误码（0 成功）；空串不特判，mpv 自报参数错误
int MpvController::command(const QString &cmd) {
    return mpv_command_string(m_mpv, cmd.toUtf8().constData());
}

// N96：返回 mpv_command 的错误码（0 成功）；空表不特判，只剩终结符的 argv 同样自报参数错误
int MpvController::command(const QStringList &args) {
    QList<QByteArray> utf8;
    utf8.reserve(args.size());
    for (const QString &arg : args)
        utf8.append(arg.toUtf8());
    QVector<const char *> argv;
    argv.reserve(utf8.size() + 1);
    for (const QByteArray &arg : utf8)
        argv.append(arg.constData());
    argv.append(nullptr);
    return mpv_command(m_mpv, argv.data());
}

// 错误码转可读文本。新用的 mpv API 必须同批进 plugin.cpp 的符号解析清单——清单外的指针
// 恒为 NULL，编译链接都不报错，失效一律推迟到首次调用才显影
QString MpvController::errorString(int code) { return QString::fromUtf8(mpv_error_string(code)); }

// N87：与同步通道的差别只在下发方式——argv 构造逐字相同，因为 mpv 在 mpv_command_async
// 调用内就用 mp_input_parse_cmd_strv 解析并复制参数，局部缓冲区不跨调用，无须堆上保命；
// 回包由下一次 mpv_wait_event 自动释放，故事件线程不接它
void MpvController::commandAsync(const QStringList &args) {
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
    mpv_command_async(m_mpv, 0, argv.data());
}

// N92：同步命令带回返回串（口径见声明处）。参数走节点数组而非 command-string，不赌其解析；
// QByteArray 与节点向量的生命期必须覆盖整次调用（mpv_command_node 返回前不析构、不复制）
QString MpvController::commandResultString(const QStringList &args) const {
    if (args.isEmpty())
        return QString();
    std::vector<QByteArray> utf8;
    utf8.reserve(size_t(args.size()));
    for (const QString &arg : args)
        utf8.push_back(arg.toUtf8());
    std::vector<mpv_node> argNodes;
    argNodes.reserve(utf8.size());
    for (const QByteArray &arg : utf8) {
        mpv_node node;
        node.format = MPV_FORMAT_STRING;
        node.u.string = const_cast<char *>(arg.constData());
        argNodes.push_back(node);
    }
    mpv_node_list argList;
    argList.num = int(argNodes.size());
    argList.keys = nullptr; // 顶层是数组无键（与 loadFile / screenshotRaw 节点通道同口径）
    argList.values = argNodes.data();
    mpv_node argRoot;
    argRoot.format = MPV_FORMAT_NODE_ARRAY;
    argRoot.u.list = &argList;

    mpv_node result;
    const int err = mpv_command_node(m_mpv, &argRoot, &result);
    if (err != 0) {
        qDebug() << "[oxbow] commandResultString failed:" << err;
        return QString();
    }
    QString out;
    if (result.format == MPV_FORMAT_STRING && result.u.string)
        out = QString::fromUtf8(result.u.string);
    mpv_free_node_contents(&result);
    return out;
}

// N92 §2：配置目录取值的唯一收口，三道校验的清单与安全顺序见声明处。这里只留最不吃眼的
// 一条——判空必须落在目录上而非文件路径上：--no-config 下 ~~home/ 是空串，而
// expand-path ~~/input.conf 交回的是非空裸串 "input.conf"，对它判空等于没判
QString MpvController::configDir() const {
    const QString raw =
        commandResultString({QStringLiteral("expand-path"), QStringLiteral("~~home/")});
    if (raw.isEmpty())
        return QString();
    const QString absolute = commandResultString({QStringLiteral("normalize-path"), raw});
    if (absolute.isEmpty() || !QDir::isAbsolutePath(absolute))
        return QString();
    if (!QFileInfo(absolute).isDir())
        return QString();
    return absolute;
}

// 两个文件出口共用这一次目录取值，文件名词法拼接（toNativeSeparators 是纯串加工，
// 不碰文件系统——目录已由 normalize-path 交回反斜杠形态，这里只是让拼接符与之一致）
QString MpvController::mpvConfPath() const {
    const QString dir = configDir();
    return dir.isEmpty() ? QString() : QDir::toNativeSeparators(dir + QStringLiteral("/mpv.conf"));
}

// N89：在 N92 的 ~~home/ 链之前加 --input-conf 判读——重启口径下这份文件是改动
// 生效的唯一途径，判读必须严格同构 mpv（四步表见规格 §6）：
//   1. input-conf 含 "://"（memory:// 等流式形态）→ 不是可编辑的文件，空串；
//   2. 非空普通路径 → expand-path + normalize-path 绝对化（允许相对路径），再探
//      读（不存在 / 打不开 / 读失败同属 mpv 侧的"读不到"这一种失败）→ 读到才用；
//   3. 读不到 → 回落 ~~home/ 链（mpv 随即去扫配置目录，写到 --input-conf 指定的
//      位置永远不会被读）；
//   4. input-conf 未设（空）→ ~~home/ 链（configDir() 内含判空 / 绝对化 / 判目录
//      存在三道校验），拼 input.conf（文件本身不必存在）。
// 全部答案都来自 mpv 原语与探读，无一处目录推导；空串即报错，不猜、不 mkpath、
// 不降级。configDir() 与 mpvConfPath() 不动（对已归档 N92 的范围扩展仅此一处）。
QString MpvController::inputConfPath() const {
    const QString conf = propertyString(QStringLiteral("input-conf"));
    if (conf.contains(QStringLiteral("://")))
        return QString();
    if (!conf.isEmpty()) {
        const QString raw = commandResultString({QStringLiteral("expand-path"), conf});
        if (!raw.isEmpty()) {
            const QString absolute = commandResultString({QStringLiteral("normalize-path"), raw});
            if (!absolute.isEmpty() && QDir::isAbsolutePath(absolute)) {
                // 探读代替存在性检查：不存在 / 打不开 / 读失败同走回落链
                QFile probe(absolute);
                if (probe.open(QIODevice::ReadOnly))
                    return QDir::toNativeSeparators(absolute);
            }
        }
    }
    const QString dir = configDir();
    return dir.isEmpty() ? QString()
                         : QDir::toNativeSeparators(dir + QStringLiteral("/input.conf"));
}

QImage MpvController::screenshotRaw() {
    // N78：screenshot-raw 经节点通道取内存原始图像（仅 client API 可用）。
    // 命令不带参数：flags 省略 = subtitles（原始视频分辨率、带字幕），
    // format 省略 = bgr0（B8G8R8X8，B 为 LSB，X 填充未定义）
    const QByteArray cmd = QByteArrayLiteral("screenshot-raw");
    mpv_node cmdNode;
    cmdNode.format = MPV_FORMAT_STRING;
    cmdNode.u.string = const_cast<char *>(cmd.constData());
    mpv_node_list argList;
    argList.num = 1;
    argList.keys = nullptr; // 顶层是数组无键（与 loadFile 节点通道同口径）
    argList.values = &cmdNode;
    mpv_node argRoot;
    argRoot.format = MPV_FORMAT_NODE_ARRAY;
    argRoot.u.list = &argList;

    mpv_node result;
    const int err = mpv_command_node(m_mpv, &argRoot, &result);
    if (err != 0) {
        // 含空闲态无视频（命令即报错），空 QImage 交调用方出提示
        qDebug() << "[oxbow] screenshot-raw failed:" << err;
        return QImage();
    }

    QImage out;
    // 返回 map 键：w / h / stride（INT64）、format（STRING）、data（BYTE_ARRAY，
    // 随 result node 释放而失效，故必须逐行拷进自建 QImage）
    const mpv_node_list *map = result.format == MPV_FORMAT_NODE_MAP ? result.u.list : nullptr;
    qint64 w = 0, h = 0, stride = 0;
    const char *format = nullptr;
    const mpv_byte_array *ba = nullptr;
    if (map) {
        for (int i = 0; i < map->num; ++i) {
            const mpv_node &v = map->values[i];
            const QLatin1String key(map->keys[i]);
            if (key == QLatin1String("w") && v.format == MPV_FORMAT_INT64)
                w = v.u.int64;
            else if (key == QLatin1String("h") && v.format == MPV_FORMAT_INT64)
                h = v.u.int64;
            else if (key == QLatin1String("stride") && v.format == MPV_FORMAT_INT64)
                stride = v.u.int64;
            else if (key == QLatin1String("format") && v.format == MPV_FORMAT_STRING)
                format = v.u.string;
            else if (key == QLatin1String("data") && v.format == MPV_FORMAT_BYTE_ARRAY)
                ba = v.u.ba;
        }
    }

    // 只认 bgr0（下发未带 format 参数，mpv 恒给默认值）；stride 只收 ≥ w*4 的正值
    // ——本命令载荷由 mpv 侧新建缓冲区、按现行实现只给正值，取到 0/负值或行重叠
    // 的畸形载荷即上游语义变化，判异常返空图，交由调用方 OSD 报错（偏离见变更记录）
    const bool shapeOk = w > 0 && h > 0 && stride >= w * 4 && ba && format &&
                         qstrcmp(format, "bgr0") == 0 &&
                         qint64(ba->size) >= (h - 1) * stride + w * 4;
    if (shapeOk) {
        QImage img(int(w), int(h), QImage::Format_RGB32);
        const uchar *base = static_cast<const uchar *>(ba->data);
        for (int y = 0; y < int(h); ++y) // byte_index = y*stride 手册原始语义
            std::memcpy(img.scanLine(y), base + qint64(y) * stride, size_t(w) * 4);
        out = img;
    } else {
        qDebug() << "[oxbow] screenshot-raw unexpected payload:" << w << h << stride
                 << (format ? format : "(null)");
    }

    mpv_free_node_contents(&result); // 节点内存（含图像 data）按既有用法规还
    return out;
}

void MpvController::loadFile(const QString &url, const QString &appendMode,
                             const QVariantMap &options) {
    // N65：纯管道——选项调用方给什么发什么，不判 scheme、不补选项、不校验键名、
    // 不改值。空 map 走既有三参数字符串通道：本地项命令与原行为逐字相同
    if (options.isEmpty()) {
        command({QStringLiteral("loadfile"), url, appendMode});
        return;
    }

    // 节点通道（走 mpv_command_node，不赌 command-string 的 JSON 自动识别）。
    // 位序坑（N51 实证）：mpv 0.38 起 loadfile 第三参数是插入索引、选项 map 挤到
    // 第四——append 语义下索引传 -1 占位；选项值必须字符串形态；漏传 index 的
    // 报错是语焉不详的 invalid parameter(-4)。
    const QByteArray cmd = QByteArrayLiteral("loadfile");
    const QByteArray urlUtf8 = url.toUtf8();
    const QByteArray modeUtf8 = appendMode.toUtf8();

    // N65 泛化为 N 键：键数组与值数组同序。每个值一律 .toString() 转成字符串再发
    // ——实测（mpv 0.41 节点通道）string list 选项喂数组节点会让整条 loadfile 被
    // 拒（argument options has incompatible type），故不得按 QVariant 类型分派节点
    // 格式。QByteArray 与 node 向量的生命期覆盖整次调用（mpv_command_node 返回前
    // 不析构），constData 指针安全
    const QList<QString> keys = options.keys();
    std::vector<QByteArray> keyUtf8;
    std::vector<QByteArray> valueUtf8;
    keyUtf8.reserve(size_t(keys.size()));
    valueUtf8.reserve(size_t(keys.size()));
    for (const QString &key : keys) {
        keyUtf8.push_back(key.toUtf8());
        valueUtf8.push_back(options.value(key).toString().toUtf8());
    }
    std::vector<char *> keyPtrs;
    std::vector<mpv_node> valueNodes;
    keyPtrs.reserve(keyUtf8.size() + 1);
    valueNodes.reserve(valueUtf8.size());
    for (size_t i = 0; i < keyUtf8.size(); ++i) {
        keyPtrs.push_back(const_cast<char *>(keyUtf8[i].constData()));
        mpv_node optionValue;
        optionValue.format = MPV_FORMAT_STRING;
        optionValue.u.string = const_cast<char *>(valueUtf8[i].constData());
        valueNodes.push_back(optionValue);
    }
    keyPtrs.push_back(nullptr);

    // 选项 map 键值成对：keys[i] → values[i]。此前曾有实现把键与值放进同一个
    // 双槽 node 数组再整体交给 values——num=1 时 values[0] 取到的是键 "cache"，
    // mpv 实收 cache=cache，choice 校验即报
    // "Error parsing option cache (option parameter could not be parsed)"
    mpv_node_list optionMapList;
    optionMapList.num = int(valueNodes.size());
    optionMapList.keys = keyPtrs.data();
    optionMapList.values = valueNodes.data();

    mpv_node argNodes[5];
    argNodes[0].format = MPV_FORMAT_STRING;
    argNodes[0].u.string = const_cast<char *>(cmd.constData());
    argNodes[1].format = MPV_FORMAT_STRING;
    argNodes[1].u.string = const_cast<char *>(urlUtf8.constData());
    argNodes[2].format = MPV_FORMAT_STRING;
    argNodes[2].u.string = const_cast<char *>(modeUtf8.constData());
    argNodes[3].format = MPV_FORMAT_INT64;
    argNodes[3].u.int64 = -1; // append 语义下索引占位
    argNodes[4].format = MPV_FORMAT_NODE_MAP;
    argNodes[4].u.list = &optionMapList; // map 与 array 同用 mpv_node_list（keys/values 成对）

    mpv_node_list argList;
    argList.num = 5;
    argList.keys = nullptr; // 顶层是数组无键；map 与 array 同用 mpv_node_list，留空防未初始化
    argList.values = argNodes;

    mpv_node argRoot;
    argRoot.format = MPV_FORMAT_NODE_ARRAY;
    argRoot.u.list = &argList;

    mpv_node result;
    const int err = mpv_command_node(m_mpv, &argRoot, &result);
    if (err == 0)
        mpv_free_node_contents(&result); // 节点内存按既有用法规还
    else
        qDebug() << "[oxbow] loadfile(node) failed:" << err << url;
}

bool MpvController::pushBarrier(quint64 gen) {
    // N60 §2.2：只发请求不候结果——reply 由 EventThread 的 GET_PROPERTY_REPLY
    // 分派发回 pushBarrierResult，gen 作 reply_userdata 与终点配对。playlist
    // 属性恒可读（观察它拿回显的正是同一属性），失败只在 mpv 正在退出 / 参数
    // 非法时出现，故此处不重试也不排队；发射失败不产生 reply，调用方就地摘除
    const int err = mpv_get_property_async(m_mpv, gen, "playlist", MPV_FORMAT_NODE);
    if (err != 0)
        qDebug() << "[oxbow] push barrier failed:" << err << "gen" << gen;
    return err == 0;
}

bool MpvController::setProperty(const QString &name, const QString &value) {
    // N42：返回实际写入结果——失败（顶层语法错误 / 未知滤镜名等）时 mpv 保持
    // 原值不清理（F5），调用方据其决定不写 conf
    return mpv_set_property_string(m_mpv, name.toUtf8().constData(), value.toUtf8().constData()) ==
           0;
}

bool MpvController::setProperty(const QString &name, int value) {
    int64_t v = value;
    return mpv_set_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_INT64, &v) == 0;
}

bool MpvController::setProperty(const QString &name, double value) {
    return mpv_set_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_DOUBLE, &value) == 0;
}

bool MpvController::setProperty(const QString &name, bool value) {
    int v = value ? 1 : 0;
    return mpv_set_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_FLAG, &v) == 0;
}

QString MpvController::propertyString(const QString &name) const {
    char *s = mpv_get_property_string(m_mpv, name.toUtf8().constData());
    QString result = s ? QString::fromUtf8(s) : QString();
    mpv_free(s);
    return result;
}

QList<FilterEntry> MpvController::filterEntries(const QString &name) {
    QList<FilterEntry> result;
    mpv_node node;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_NODE, &node) != 0)
        return result; // 失败 / 不可读 → 空列表（调用方按空滤镜链呈现）
    result = parseFilterNode(&node);
    mpv_free_node_contents(&node);
    return result;
}

// N89：input-bindings 的 NODE 逐项解析（数组套 map；key / cmd / section / owner /
// comment 为字符串，is_weak 为 flag，owner / comment 可缺）。形状异常的条目跳过。
QList<MpvController::InputBinding> parseBindingNode(const mpv_node *node) {
    QList<MpvController::InputBinding> result;
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY)
        return result;
    for (int i = 0; i < node->u.list->num; ++i) {
        const mpv_node &entry = node->u.list->values[i];
        if (entry.format != MPV_FORMAT_NODE_MAP)
            continue;
        MpvController::InputBinding b;
        for (int j = 0; j < entry.u.list->num; ++j) {
            const char *key = entry.u.list->keys[j];
            const mpv_node &v = entry.u.list->values[j];
            if (qstrcmp(key, "key") == 0 && v.format == MPV_FORMAT_STRING)
                b.key = QString::fromUtf8(v.u.string);
            else if (qstrcmp(key, "cmd") == 0 && v.format == MPV_FORMAT_STRING)
                b.cmd = QString::fromUtf8(v.u.string);
            else if (qstrcmp(key, "is_weak") == 0 && v.format == MPV_FORMAT_FLAG)
                b.isWeak = v.u.flag != 0;
            else if (qstrcmp(key, "owner") == 0 && v.format == MPV_FORMAT_STRING)
                b.owner = QString::fromUtf8(v.u.string);
            else if (qstrcmp(key, "section") == 0 && v.format == MPV_FORMAT_STRING)
                b.section = QString::fromUtf8(v.u.string);
            else if (qstrcmp(key, "comment") == 0 && v.format == MPV_FORMAT_STRING)
                b.comment = QString::fromUtf8(v.u.string);
        }
        result.append(b);
    }
    return result;
}

MpvController::InputBindingTiers MpvController::inputBindings() const {
    InputBindingTiers tiers;
    mpv_node node;
    if (mpv_get_property(m_mpv, "input-bindings", MPV_FORMAT_NODE, &node) != 0)
        return tiers; // 失败 / 不可读 → 空档
    const QList<InputBinding> all = parseBindingNode(&node);
    mpv_free_node_contents(&node);
    // 两档筛选三判据（规格 §2）：内置集 = is_weak 且 section == "default" 且无 owner；
    // 脚本集 = owner 非空者。owner 判据在前：weak 且带 owner 的条目属脚本档
    for (const InputBinding &b : all) {
        if (!b.owner.isEmpty())
            tiers.scripts.append(b);
        else if (b.isWeak && b.section == QLatin1String("default"))
            tiers.builtins.append(b);
    }
    return tiers;
}

double MpvController::propertyDouble(const QString &name, double fallback) const {
    double v = fallback;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_DOUBLE, &v) < 0)
        return fallback; // 失败时不写入 v
    return v;
}

bool MpvController::propertyFlag(const QString &name, bool fallback) const {
    int v = fallback ? 1 : 0;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_FLAG, &v) < 0)
        return fallback;
    return v != 0;
}

qint64 MpvController::propertyInt64(const QString &name, qint64 fallback) const {
    qint64 v = fallback;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_INT64, &v) < 0)
        return fallback;
    return v;
}

// N61 §2.3：「在播哪一条」的唯一出口，取法与两条硬约束见头文件声明处注释。
// 三步都是 O(1)：读 pos（不可读 / 停止态即无正播）→ 现读该条的 filename（下标越界
// 报 property not found，propertyString 返回空串，与"无正播"同形）→ 过一次回显整形
QString MpvController::currentPlayingPath() const {
    const qint64 pos = propertyInt64(QStringLiteral("playlist-pos"), -1);
    if (pos < 0)
        return QString();
    return Util::Url::identityFromMpvEcho(
        propertyString(QStringLiteral("playlist/%1/filename").arg(pos)));
}

// N60 §2.2：屏障读的 reply——在事件线程解析并释放 node 后把终值投 GUI。
// 载荷是 mpv_event_property*，数组性判的是 **data 所指 node 的格式**：`prop->format`
// 只是请求格式（NODE），拿它判数组性永不得到 NODE_ARRAY；取不到值时它是 NONE 且
// data 为 NULL（client.h 原文），故 NONE 一律按 ok = false 且不 free
void MpvController::handleBarrierReply(const mpv_event *event) {
    const quint64 gen = event->reply_userdata;
    auto *prop = static_cast<mpv_event_property *>(event->data);
    if (!prop) {
        emit pushBarrierResult(gen, false, QVariantList());
        return;
    }

    // 属性不可读（卸载中 / 出错）时 format 为 NONE 且 data 无效，不得 free
    if (event->error != 0 || prop->format != MPV_FORMAT_NODE || !prop->data) {
        emit pushBarrierResult(gen, false, QVariantList());
        return;
    }

    auto *node = static_cast<mpv_node *>(prop->data);
    const bool ok = node->format == MPV_FORMAT_NODE_ARRAY;
    const QVariantList items = parsePlaylist(node); // 非数组时它自己返回空表
    mpv_free_node_contents(node);                   // 与 mpv_get_property 同口径
    emit pushBarrierResult(gen, ok, items);
}

void MpvController::handlePropertyChange(const mpv_event *event) {
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
        // B18：双通道的 observe 半程——payload 配当前文件缓存为归属键（方案 i）。
        // NONE（卸载路径）不发射：卸载瞬间的 0.0 会被下游按"读取失败即清空"
        // 处理，反而抹掉刚播完文件的档案；会话态清空由 END_FILE 重置负责
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            emit durationChanged(m_currentFile, *static_cast<double *>(prop->data));
        break;
    case PropPause:
        if (prop->format == MPV_FORMAT_FLAG && prop->data)
            emit pauseChanged(*static_cast<int *>(prop->data) != 0);
        break;
    case PropVolume:
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            emit volumeChanged(*static_cast<double *>(prop->data));
        break;
    case PropMute:
        if (prop->format == MPV_FORMAT_FLAG && prop->data)
            emit muteChanged(*static_cast<int *>(prop->data) != 0);
        break;
    case PropVolumeMax:
        // R3：音量上限推送；NONE（不可能）按 0 不发射，滑条量程保持现有值
        if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
            emit volumeMaxChanged(*static_cast<double *>(prop->data));
        break;
    case PropConsoleOpen:
        // NONE 是"属性尚未诞生"的常态，不发射——接收侧初值本就是 false，无须第二条兜底
        if (prop->format == MPV_FORMAT_FLAG && prop->data)
            emit consoleOpenChanged(*static_cast<int *>(prop->data) != 0);
        break;
    case PropFullscreen:
        // N88：镜像推送（含注册即推的首值）。只认 FLAG 且有载荷；NONE（属性暂不可用）
        // 不发射。初值到不到、何时到由接收侧按启动时序消化，本类不读该属性判初值
        if (prop->format == MPV_FORMAT_FLAG && prop->data)
            emit fullscreenChanged(*static_cast<int *>(prop->data) != 0);
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
        if (prop->format == MPV_FORMAT_FLAG && prop->data) {
            const bool active = *static_cast<int *>(prop->data) != 0;
            if (active)
                publishVideoSize(); // N40：假转真兜底——覆盖停止/清空/起播前空闲等不经 reconfig
                                    // 的路径
            emit idleActive(active);
        }
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
    case PropTrackList:
        // N93：运行期轨道变更（外部增删轨 / 切换选中）走本分支；装载时刻的首值仍由
        // handleFileLoaded 主动读给出，两路都在事件线程、保序，重复发射对接收方幂等。
        // 载荷为 NODE 即解析；NONE（无文件 / 属性不可用）交空列表——与 handleFileLoaded
        // 读失败分支同形，不让停止态留下陈旧轨道。事件数据随 mpv_event 由 mpv 回收，
        // 不 mpv_free_node_contents（与 PropPlaylist 同口径）
        if (prop->format == MPV_FORMAT_NODE && prop->data)
            emit tracksChanged(m_currentFile, parseTrackList(static_cast<mpv_node *>(prop->data)));
        else
            emit tracksChanged(m_currentFile, QList<TrackInfo>());
        break;
    // N40：PropWidth / PropHeight 分支已随 observe 移除，视频尺寸改由
    // publishVideoSize 统一取值发布；B18：PropMetadata / PropFileFormat /
    // PropChapterList 分支已随 observe 移除，改由 handleFileLoaded 主动读
    // （PropTrackList 自 N93 起重回观察，见上）
    default:
        break;
    }
}

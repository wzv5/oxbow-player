#include "pch.h"

#include <mpv/client.h>

#include "Logging.h"
#include "streams/StreamRelay.h"
#include "streams/StreamTransit.h"
#include "thumb/ThumbEngine.h"

namespace {

// seek 去抖：与本场会话最后请求到的时刻差小于该值（媒体秒）不发新 seek。唯一使用点
// 在本文件，故不入 Constants.h
constexpr double kSeekDebounceSecs = 1.0;
// 精确档延迟：keyframes 档先出图保证跟手，光标静默达该时长后再补一发 exact 收敛。
// 高码率长 GOP 上一次 exact 要从关键帧逐帧解完整段（可达秒级），逐请求直发会把
// 画面钉在"永远追不上光标"的状态；移动中只解一帧才是跟手的关键
constexpr int kExactSeekDelayMs = 200;

void setOptionOrIgnore(mpv_handle *handle, const char *name, const char *value) {
    mpv_set_option_string(handle, name, value);
}

} // namespace

ThumbEngine::ThumbEngine(std::shared_ptr<StreamRelay> relay, QObject *parent)
    : QObject(parent), m_relay(std::move(relay)) {
    m_drainTimer = new QTimer(this);
    m_drainTimer->setInterval(50);
    connect(m_drainTimer, &QTimer::timeout, this, [this] {
        if (!m_handle)
            return;
        for (;;) {
            const mpv_event *ev = mpv_wait_event(m_handle, 0);
            if (ev->event_id == MPV_EVENT_NONE)
                break;
            // 首帧的精确收敛等文件真正装载完再排：装载前的 seek 无 demuxer 可落。
            // 已在飞的不动——移动中的请求已经把精确档顺延，此处不得重新起表
            if (ev->event_id == MPV_EVENT_FILE_LOADED && !m_exactTimer->isActive() &&
                m_lastSeek >= 0.0)
                m_exactTimer->start();
        }
    });
    // 精确档的唯一发起点：每次被去抖接受的请求都重启它，期间又有新请求则顺延——
    // 光标移动中永远只有 keyframes 档在跑，停稳后才轮到 exact
    m_exactTimer = new QTimer(this);
    m_exactTimer->setSingleShot(true);
    m_exactTimer->setInterval(kExactSeekDelayMs);
    connect(m_exactTimer, &QTimer::timeout, this, [this] {
        if (m_handle && m_lastSeek >= 0.0)
            seekAbsolute(m_lastSeek, /*exact=*/true);
    });
}

ThumbEngine::~ThumbEngine() { teardownCore(); }

void ThumbEngine::startSession(quint64 seq, WId hostWid, const QString &path, double startTime) {
    teardownCore(); // 防御：上会话的 endSession 未到先到的新会话
    m_seq = seq;
    m_path = path;
    // 去抖基准取 loadfile 的 start——画面已由它定格在该刻，同一处再发一发是白发
    m_lastSeek = startTime;

    m_handle = mpv_create();
    if (!m_handle) {
        OXBOW_LOG_DEBUG() << "thumb mpv_create failed";
        m_seq = 0;
        emit sessionEnded(seq);
        return;
    }
    OXBOW_LOG_DEBUG() << "thumb core start path=" << m_path << "time=" << startTime
                      << "wid=" << (void *)hostWid;

    // wid 指向缩略图宿主窗（VO 直接渲染进它）
    setOptionOrIgnore(m_handle, "wid", QByteArray::number(static_cast<qint64>(hostWid)));

    // ——N107 §2 选项集，全部在 initialize 前设完——
    // load-scripts=no 是硬要求：本插件自己就是 cplugin，第二个 core 一旦扫描
    // scripts 目录就会再加载一份 oxbow，那份又建一个 core——无限递归。
    // mpv_create 默认不加载 config，脚本自动加载是独立开关，必须显式关掉。
    setOptionOrIgnore(m_handle, "load-scripts", "no");
    setOptionOrIgnore(m_handle, "config", "no");
    setOptionOrIgnore(m_handle, "osc", "no");
    setOptionOrIgnore(m_handle, "ytdl", "no");
    setOptionOrIgnore(m_handle, "drag-and-drop", "no");
    setOptionOrIgnore(m_handle, "load-console", "no");
    setOptionOrIgnore(m_handle, "load-commands", "no");
    setOptionOrIgnore(m_handle, "load-auto-profiles", "no");
    setOptionOrIgnore(m_handle, "load-select", "no");
    setOptionOrIgnore(m_handle, "load-context-menu", "no");
    setOptionOrIgnore(m_handle, "load-positioning", "no");
    setOptionOrIgnore(m_handle, "pause", "yes");
    setOptionOrIgnore(m_handle, "keep-open", "always");
    // 首帧落关键帧：loadfile 的 start 是绝对寻址，默认 hr-seek 档（absolute）会做
    // 精确 seek——高码率长 GOP 上开局就要解一整段；精确收敛交给静默后的补发
    setOptionOrIgnore(m_handle, "hr-seek", "no");
    setOptionOrIgnore(m_handle, "audio", "no");
    setOptionOrIgnore(m_handle, "sub", "no");
    setOptionOrIgnore(m_handle, "really-quiet", "yes");
    setOptionOrIgnore(m_handle, "msg-level", "all=no");
    setOptionOrIgnore(m_handle, "terminal", "no");
    // 硬解恒关：缩略图 seek 场景软解更快
    setOptionOrIgnore(m_handle, "hwdec", "no");
    setOptionOrIgnore(m_handle, "untimed", "yes");
    setOptionOrIgnore(m_handle, "scale", "bilinear");
    setOptionOrIgnore(m_handle, "cscale", "bilinear");
    setOptionOrIgnore(m_handle, "dscale", "bilinear");
    setOptionOrIgnore(m_handle, "deband", "no");
    setOptionOrIgnore(m_handle, "interpolation", "no");
    setOptionOrIgnore(m_handle, "vd-lavc-skiploopfilter", "all");
    setOptionOrIgnore(m_handle, "vd-lavc-fast", "yes");
    setOptionOrIgnore(m_handle, "vd-lavc-threads", "2");
    setOptionOrIgnore(m_handle, "demuxer-readahead-secs", "0");
    setOptionOrIgnore(m_handle, "demuxer-max-bytes", "128KiB");
#ifndef Q_OS_WIN
    setOptionOrIgnore(m_handle, "input-cursor", "no");
    setOptionOrIgnore(m_handle, "cursor-autohide", "no");
    if (mpv_set_option_string(m_handle, "input-vo-keyboard", "no") != 0)
        mpv_set_option_string(m_handle, "input-x11-keyboard", "no");
    setOptionOrIgnore(m_handle, "input-media-keys", "no");
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland"))
        setOptionOrIgnore(m_handle, "gpu-context", "x11egl");
#endif

    // 转接协议注册必须早于 initialize（StreamRelay::install 的硬顺序）；失败
    // 不阻断会话——本地文件不需要它，转接项届时自然打不开，窗口保持黑底
    if (m_relay)
        m_relay->install(m_handle);

    if (mpv_initialize(m_handle) != 0) {
        OXBOW_LOG_DEBUG() << "thumb mpv_initialize failed";
        mpv_terminate_destroy(m_handle);
        m_handle = nullptr;
        m_seq = 0;
        emit sessionEnded(seq);
        return;
    }
    if (!loadFile(startTime)) {
        teardownCore();
        m_seq = 0;
        emit sessionEnded(seq);
        return;
    }
    m_drainTimer->start();
    m_exactTimer->start(); // 首帧同样先 keyframes 后收敛（start 只到关键帧）
    OXBOW_LOG_DEBUG() << "thumb core ready";
}

bool ThumbEngine::loadFile(double startTime) {
    // 形态与 MpvController::loadFile 同款（N65）：位置数组 5 参——mpv 0.38 起
    // 选项 map 挤到第四参、第三参传 -1 占位，漏传 index 会报语焉不详的
    // invalid parameter。选项值必须字符串形态。全部容器寿命覆盖到
    // mpv_command_node 返回（mpv 不接管输入节点）
    const QByteArray cmd = QByteArrayLiteral("loadfile");
    const QByteArray urlUtf8 = m_path.toUtf8();
    const QByteArray modeUtf8 = QByteArrayLiteral("replace");
    const QByteArray startUtf8 = QByteArray::number(startTime, 'f', 3);

    // 选项固定带 start；转接支补 cache=yes（与 N52 §2.10 同口径），本地走 mpv 默认
    const bool transit = StreamTransit::isTransitUri(m_path);
    std::vector<QByteArray> optValues;
    optValues.push_back(startUtf8);
    if (transit)
        optValues.push_back(QByteArrayLiteral("yes"));
    std::vector<const char *> optKeys;
    optKeys.push_back("start");
    if (transit)
        optKeys.push_back("cache");
    std::vector<mpv_node> optNodes;
    optNodes.reserve(optValues.size());
    for (size_t i = 0; i < optValues.size(); ++i) {
        mpv_node n{};
        n.format = MPV_FORMAT_STRING;
        n.u.string = const_cast<char *>(optValues[i].constData());
        optNodes.push_back(n);
    }

    mpv_node_list optionMapList{};
    optionMapList.num = static_cast<int>(optNodes.size());
    optionMapList.keys = const_cast<char **>(optKeys.data());
    optionMapList.values = optNodes.data();

    mpv_node argNodes[5];
    argNodes[0].format = MPV_FORMAT_STRING;
    argNodes[0].u.string = const_cast<char *>(cmd.constData());
    argNodes[1].format = MPV_FORMAT_STRING;
    argNodes[1].u.string = const_cast<char *>(urlUtf8.constData());
    argNodes[2].format = MPV_FORMAT_STRING;
    argNodes[2].u.string = const_cast<char *>(modeUtf8.constData());
    argNodes[3].format = MPV_FORMAT_INT64;
    argNodes[3].u.int64 = -1; // 不用插入索引，占位
    argNodes[4].format = MPV_FORMAT_NODE_MAP;
    argNodes[4].u.list = &optionMapList;

    mpv_node_list argList{};
    argList.num = 5;
    argList.keys = nullptr; // 顶层是数组无键，留空防未初始化
    argList.values = argNodes;

    mpv_node argRoot{};
    argRoot.format = MPV_FORMAT_NODE_ARRAY;
    argRoot.u.list = &argList;

    mpv_node result;
    const int err = mpv_command_node(m_handle, &argRoot, &result);
    if (err == 0)
        mpv_free_node_contents(&result); // 节点内存按既有用法规还
    else
        OXBOW_LOG_DEBUG() << "thumb loadfile failed:" << err << mpv_error_string(err);
    return err == 0;
}

void ThumbEngine::seekTo(double time) {
    if (!m_handle)
        return;
    // 与上次请求时刻差小于 1s 不重取，停点与光标最多差 1s（两级 seek 下这只是省一次
    // 无意义的关键帧重解：同一 GOP 内 keyframes 档本来也落在同一帧上）
    if (std::abs(time - m_lastSeek) < kSeekDebounceSecs)
        return;
    m_lastSeek = time;
    // 两级 seek：先发 keyframes（只解一帧，毫秒级，画面立即跟手），并把精确档顺延；
    // 光标静默 kExactSeekDelayMs 后 m_exactTimer 才发 exact 收敛到目标帧。高码率下
    // exact 要从关键帧逐帧解完整段 GOP，逐请求直发会把画面钉在追不上的状态
    seekAbsolute(time, /*exact=*/false);
    m_exactTimer->start();
}

// 恒走 mpv_command（argv 形态，与主通路同款）——libmpv 客户端命令默认 no-osd，
// 不画 seek 进度条；exact=false 时 keyframes 档显式覆盖 hr-seek=no 的默认
void ThumbEngine::seekAbsolute(double time, bool exact) {
    if (!m_handle)
        return;
    const QList<QByteArray> utf8 = {QByteArrayLiteral("seek"), QByteArray::number(time, 'f', 3),
                                    exact ? QByteArrayLiteral("absolute+exact")
                                          : QByteArrayLiteral("absolute+keyframes")};
    std::vector<const char *> argv;
    argv.reserve(utf8.size() + 1);
    for (const QByteArray &arg : utf8)
        argv.push_back(arg.constData());
    argv.push_back(nullptr);
    mpv_command(m_handle, argv.data());
}

void ThumbEngine::endSession() {
    const bool hadCore = m_handle != nullptr;
    const quint64 seq = m_seq;
    m_seq = 0;
    teardownCore();
    if (hadCore)
        emit sessionEnded(seq);
}

void ThumbEngine::teardownCore() {
    m_drainTimer->stop();
    m_exactTimer->stop();
    if (m_handle) {
        mpv_terminate_destroy(m_handle); // 阻塞至 core 拆完；此后窗口才可销毁
        m_handle = nullptr;
    }
}

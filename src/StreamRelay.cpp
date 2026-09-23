#include "pch.h"

#include "StreamRelay.h"

#include <mpv/stream_cb.h>

#include "StreamTransit.h"

namespace {

// 每条流的 cookie：shared_ptr<ByteStream> + 日志上下文。寿命归 mpv——close_fn
// 释放最后一份引用即触发流的析构（析构内 cancel + 回收在途作业，§2.4），
// "close = 析构"由对象寿命的自然终点承载，不再手写清理。
struct StreamCookie {
    std::shared_ptr<ByteStream> stream;
    QString uuidTag; // UUID 短前缀：多列表并存时区分日志归属
    // —— N52 探针（临时，随 D7 裁决定留）：观测 mpv 经 stream_cb 的实际拉取量
    // 与节奏，回答"mpv 是否把整个文件拉完"——
    QElapsedTimer clock;     // 自 open 起的时轴
    qint64 loggedServed = 0; // 上次采样时的网络侧累计字节
};

// 探针采样：served = pos - skipBytes（跳读丢弃的字节不过网络）。每跨 8MB 一行；
// read 终态（EOF/错误）逐次记录——二者都是稀缺事件。
void probeLog(StreamCookie *cookie, qint64 n) {
    const ByteStream::Stats st = cookie->stream->stats();
    const qint64 served = st.pos - st.skipBytes;
    if (n == ByteStream::kEof || n == ByteStream::kError) {
        qDebug() << "[mpvqt]" << cookie->uuidTag << "probe read-terminal n=" << n
                 << "served=" << served << "pos=" << st.pos << "reopens=" << st.reopens
                 << "t=" << cookie->clock.elapsed() << "ms";
        cookie->loggedServed = served;
        return;
    }
    if (n > 0 && served - cookie->loggedServed >= 8 * 1024 * 1024) {
        qDebug() << "[mpvqt]" << cookie->uuidTag << "probe served=" << served << "pos=" << st.pos
                 << "reopens=" << st.reopens << "skips=" << st.skipHits
                 << "t=" << cookie->clock.elapsed() << "ms";
        cookie->loggedServed = served;
    }
}

int64_t readCb(void *cookiePtr, char *buf, uint64_t nbytes) {
    auto *cookie = static_cast<StreamCookie *>(cookiePtr);
    // 三态直转：kEof → 0（mpv 按文件尽头处理）；kError → -1
    // （kNoData 为防御返回，正常不外抛，按错误收敛）
    const qint64 n = cookie->stream->read(buf, static_cast<qint64>(nbytes));
    probeLog(cookie, n);
    if (n > 0)
        return n;
    return n == ByteStream::kEof ? 0 : -1;
}

int64_t seekCb(void *cookiePtr, int64_t offset) {
    auto *cookie = static_cast<StreamCookie *>(cookiePtr);
    const qint64 r = cookie->stream->seek(static_cast<qint64>(offset));
    if (r >= 0) {
        // N52 探针（临时）：成功 seek 也记——demuxer 的索引扫描（如 mkv cues）
        // 会触发后向重开，是拉取节奏的一部分
        qDebug() << "[mpvqt]" << cookie->uuidTag << "probe seek" << offset
                 << "ok t=" << cookie->clock.elapsed() << "ms";
        return r;
    }
    // mpv 对两档失败行为一致：seek 不成、原位续播
    qDebug() << "[mpvqt]" << cookie->uuidTag << "seek" << offset << "failed:" << r;
    return r == ByteStream::kSeekUnsupported ? int64_t(MPV_ERROR_UNSUPPORTED)
                                             : int64_t(MPV_ERROR_GENERIC);
}

int64_t sizeCb(void *cookiePtr) {
    auto *cookie = static_cast<StreamCookie *>(cookiePtr);
    const qint64 sz = cookie->stream->size();
    return sz >= 0 ? sz : int64_t(MPV_ERROR_UNSUPPORTED);
}

void cancelCb(void *cookiePtr) {
    // 任意线程、不阻塞：必须能打断等头与读阻塞中的作业（§2.4 解救通道之一）
    static_cast<StreamCookie *>(cookiePtr)->stream->cancel();
}

void closeCb(void *cookiePtr) {
    auto *cookie = static_cast<StreamCookie *>(cookiePtr);
    // 观测计数经 Stats 由 close 一行带出：HTTP 之外任何传输自动获得同等可观测性
    const ByteStream::Stats st = cookie->stream->stats();
    qDebug() << "[mpvqt]" << cookie->uuidTag << "close pos=" << st.pos << "total=" << st.total
             << "reopens=" << st.reopens << "skips=" << st.skipHits << "skipBytes=" << st.skipBytes;
    delete cookie; // 释放最后一份 shared_ptr 即触发流析构
}

// uuid 段提取归 L1（N57 §2.4）：与 mint 成对的 StreamTransit::listUuidOfUri，
// 解不出合法身份即 nil——nil 恒不命中任何端点，本文件不再自持一份 URI 语法知识
int openCb(void *user_data, char *uri, mpv_stream_cb_info *info) {
    auto *relay = static_cast<StreamRelay *>(user_data);
    const QString path = QString::fromUtf8(uri);
    const QString identity = StreamTransit::unwrap(path);
    const QUuid listUuid = StreamTransit::listUuidOfUri(path);
    auto fail = [&](const QString &why) {
        qDebug() << "[mpvqt] open" << path << "failed:" << why;
        return int(MPV_ERROR_LOADING_FAILED); // 不静默降级为无凭据直连（N51 §2.3 纪律）
    };

    if (listUuid.isNull() || identity == path)
        return fail(QStringLiteral("malformed transit uri"));
    const std::shared_ptr<StreamEndpoint> endpoint = relay->takeEndpoint(listUuid);
    if (!endpoint)
        return fail(QStringLiteral(
            "no endpoint for list (deleted, or stale uri from a previous session?)"));

    // 来源解析点：每条流恰好一次，demux 线程（§2.3 契约 2）；失败文案透传
    const auto opened = endpoint->openStream(identity);
    if (!opened)
        return fail(opened.error());

    auto *cookie = new StreamCookie{
        opened.value(),
        listUuid.toString(QUuid::StringFormat::WithoutBraces).left(8), // 日志短前缀
        QElapsedTimer(), 0};
    cookie->clock.start();
    info->cookie = cookie;
    info->read_fn = &readCb;
    info->close_fn = &closeCb;
    info->cancel_fn = &cancelCb;
    // 能力申报只有两项（§2.3 契约 4）：假可寻位不注册 seek_fn；负总长不注册 size_fn。
    // 裸 TCP / 直播类源借此完整表达"不可寻位、总长未知"，适配层无新增分支。
    info->seek_fn = cookie->stream->isSeekable() ? &seekCb : nullptr;
    info->size_fn = cookie->stream->size() >= 0 ? &sizeCb : nullptr;

    qDebug() << "[mpvqt]" << cookie->uuidTag << "open ok total=" << cookie->stream->size()
             << "seekable=" << cookie->stream->isSeekable();
    return 0;
}

} // namespace

bool StreamRelay::install(mpv_handle *handle) {
    m_handle = handle;
    const int err = mpv_stream_cb_add_ro(handle, StreamTransit::kProtocol, this, &openCb);
    if (err != 0) {
        qDebug() << "[mpvqt] stream_cb register failed:" << err;
        return false;
    }
    return true;
}

void StreamRelay::publish(const QUuid &listUuid, const std::shared_ptr<StreamEndpoint> &endpoint) {
    QMutexLocker l(&m_mutex);
    m_endpoints.insert(listUuid, endpoint);
}

std::shared_ptr<StreamEndpoint> StreamRelay::takeEndpoint(const QUuid &listUuid) {
    QMutexLocker l(&m_mutex);
    const auto it = m_endpoints.constFind(listUuid);
    if (it == m_endpoints.constEnd())
        return nullptr;
    std::shared_ptr<StreamEndpoint> ep = it->lock();
    if (!ep)
        m_endpoints.erase(it); // 命中 expired：顺手注销，表不留垃圾
    return ep;
}

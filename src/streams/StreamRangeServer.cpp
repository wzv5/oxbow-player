#include "pch.h"

#include "StreamRangeServer.h"

#include "Logging.h"

namespace {

// 事件循环空转时观察取消旗的粒度（有活干的时候每个回调都查旗，不必等这一拍）
constexpr int kCancelWatchdogMs = 100;
constexpr qint64 kMaxHeadBytes = 8192; // 请求头上限，超出直接弃这条
constexpr int kChunkBytes = 64 * 1024; // 明文搬运块
// 单条连接的待发上限：到线即停手，等 socket 排空再续。并行服务就靠这条界限成立——
// 某个对端不读，只是它那一侧自己不推进，既不占别人的执行流、也不必去猜"它是不是弃连
// 了"（那种猜测只能拟合某个客户端的行为）。于是本桥没有任何计时判据：取消旗是唯一能
// 让服务线程退出的东西，而它最迟一拍生效。
constexpr qint64 kHighWaterBytes = 512 * 1024;

const char *statusReason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 206:
        return "Partial Content";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    default:
        return "Range Not Satisfiable"; // 416
    }
}

// Range 形态由状态码唯一决定，日志按此还原，不必另存一份
const char *rangeFormText(int status) {
    if (status == 206)
        return "partial";
    return status == 416 ? "unsat" : "full";
}

bool isDigits(const QByteArray &text) {
    if (text.isEmpty())
        return false;
    for (const char c : text)
        if (c < '0' || c > '9')
            return false;
    return true;
}

// 纯数字串 → 偏移；溢出按上限处理（巨大 start 落不可满足、巨大 suffix 落整文件），
// 非数字即畸形由调用方按 None 处理
std::optional<qint64> parseOffset(const QByteArray &text) {
    if (!isDigits(text))
        return std::nullopt;
    bool ok = false;
    const qulonglong value = text.toULongLong(&ok);
    constexpr qulonglong kMax = qulonglong(std::numeric_limits<qint64>::max());
    if (!ok || value > kMax)
        return std::numeric_limits<qint64>::max();
    return qint64(value);
}

// 单段 Range 解析结论：合法单段 → Partial（end 钳到 size-1、suffix 超长
// 取整文件）；合法但不可满足（start >= size 含恰好 EOF、bytes=-0）→ Unsatisfiable；
// 多段（含 ','）与畸形（非数字、end < start、缺 '-'）→ None（按无 Range 全量）
enum class RangeKind { None, Partial, Unsatisfiable };

struct RangeSpec {
    RangeKind kind = RangeKind::None;
    qint64 start = 0;
    qint64 end = 0;
};

RangeSpec parseRange(const QByteArray &value, qint64 size) {
    RangeSpec spec;
    if (!value.startsWith("bytes="))
        return spec;
    const QByteArray specPart = value.mid(6).trimmed();
    if (specPart.contains(',')) // 多段：RFC 允许忽略，ffmpeg 不发
        return spec;
    const int dash = specPart.indexOf('-');
    if (dash < 0)
        return spec;
    const QByteArray first = specPart.left(dash).trimmed();
    const QByteArray second = specPart.mid(dash + 1).trimmed();

    if (first.isEmpty()) {
        // bytes=-suffix
        const auto suffix = parseOffset(second);
        if (!suffix)
            return spec;
        if (*suffix == 0)
            return {RangeKind::Unsatisfiable, 0, 0};
        if (*suffix >= size)
            return {RangeKind::Partial, 0, size - 1};
        return {RangeKind::Partial, size - *suffix, size - 1};
    }
    const auto start = parseOffset(first);
    if (!start)
        return spec;
    if (second.isEmpty()) {
        // bytes=start-
        if (*start >= size)
            return {RangeKind::Unsatisfiable, 0, 0};
        return {RangeKind::Partial, *start, size - 1};
    }
    const auto end = parseOffset(second);
    if (!end || *end < *start)
        return spec;
    if (*start >= size)
        return {RangeKind::Unsatisfiable, 0, 0};
    return {RangeKind::Partial, *start, qMin(*end, size - 1)};
}

// 一条连接的收场。前四档都是正常终局，后三档说明"这一问没交付足额"，是异常。
enum class Ending {
    Done,      // 承诺量交齐，本侧发 FIN
    Cancelled, // 取数收口（新请求 / 取消 / 关面板 / 进程终局）
    PeerGone,  // 对端先断：seek 换连的常态，不当异常
    HeadTooLong,
    SeekFailed,
    StreamEnded, // 流提前结束（kEof / kError）
};

const char *endingText(Ending ending) {
    switch (ending) {
    case Ending::Done:
        return "done";
    case Ending::Cancelled:
        return "cancelled";
    case Ending::PeerGone:
        return "peer-gone";
    case Ending::HeadTooLong:
        return "head-too-long";
    case Ending::SeekFailed:
        return "seek-failed";
    case Ending::StreamEnded:
        return "stream-ended";
    }
    return "unknown";
}

// 一条连接的应答侧：读头 → 定档 → 按 socket 的余量逐块喂体 → 本侧发 FIN 收尾。
// 只在服务线程内活动，且以该 socket 为父（随它一并销毁），不跨线程访问。
//
// 取字节走的是共享的那一条解密流：每次都是"seek 自己的偏移 + read"成对出现，中间不
// 处理事件，故同线程内不会与别的连接交叉——ByteStream 的"单一消费者线程按序驱动"契约
// 原样成立，也不需要锁。
class RangeConnection final : public QObject {
  public:
    RangeConnection(QTcpSocket *socket, ByteStream &stream, qint64 total, const QString &token,
                    const std::atomic<bool> &cancel);

  private:
    enum class Phase { ReadingHead, Serving, ClosingSent, Finished };

    // 已定档的应答：只供每请求一行日志（诊断抓手是形态 + 承诺跨度 + 已交付）。
    // Range 形态不必记——它由状态码唯一决定（206 段、416 不可满足、其余全量）
    struct Reply {
        bool sent = false;
        int status = 0;
        qint64 bodyStart = 0;
        qint64 promised = 0;
    };

    void onReadyRead();
    void onBytesWritten();
    void onDisconnected();
    void serve(); // 解析请求行与 Range，写响应头，进入喂体
    void pump();  // socket 有余量就取一块写一块
    void reply(int status, const QByteArray &extra, qint64 bodyStart, qint64 bodyLength);
    void finish(Ending ending); // 出声并收尾：交齐了发 FIN，异常则直接 abort

    QTcpSocket *m_socket;
    ByteStream &m_stream;
    const std::atomic<bool> &m_cancel;
    const qint64 m_total;
    const QString m_token;

    Phase m_phase = Phase::ReadingHead;
    bool m_pumping = false; // 防 write() 同拍回吐 bytesWritten 造成递归
    bool m_isHead = false;  // 请求侧事实：HEAD 定档后不喂体
    QByteArray m_head;
    QByteArray m_chunk; // 首次喂体才分配：错误应答与 HEAD 一字节都不搬
    qint64 m_bodyPos = 0;
    qint64 m_remaining = 0;
    QElapsedTimer m_clock;
    Reply m_reply;
};

RangeConnection::RangeConnection(QTcpSocket *socket, ByteStream &stream, qint64 total,
                                 const QString &token, const std::atomic<bool> &cancel)
    : QObject(socket), m_socket(socket), m_stream(stream), m_cancel(cancel), m_total(total),
      m_token(token) {
    m_clock.start();
    connect(m_socket, &QTcpSocket::readyRead, this, &RangeConnection::onReadyRead);
    connect(m_socket, &QTcpSocket::bytesWritten, this, &RangeConnection::onBytesWritten);
    connect(m_socket, &QTcpSocket::disconnected, this, &RangeConnection::onDisconnected);
}

void RangeConnection::onReadyRead() {
    if (m_phase != Phase::ReadingHead) {
        (void)m_socket->readAll(); // 已定档：对端再发的字节一概不收（本侧马上收尾）
        return;
    }
    m_head += m_socket->readAll();
    if (m_head.size() > kMaxHeadBytes) {
        finish(Ending::HeadTooLong);
        return;
    }
    if (m_head.contains("\r\n\r\n"))
        serve();
}

void RangeConnection::onBytesWritten() {
    if (m_pumping)
        return; // 自己 write() 同拍回吐的通报，外层 pump 的循环会继续消化
    pump();
}

void RangeConnection::onDisconnected() {
    if (m_phase == Phase::Finished)
        return;
    if (m_phase == Phase::ClosingSent) {
        // 本侧的 FIN 已被 ack：这条彻底交付完，可以自毁了（搬运缓冲随之释放，
        // 长片数百问的取数不攒这个）
        m_phase = Phase::Finished;
        m_socket->deleteLater();
        return;
    }
    finish(Ending::PeerGone); // 对端先走：交付未足额是它的常态（seek 换连），不当异常
}

void RangeConnection::serve() {
    // 请求行：METHOD SP PATH SP VERSION
    const QList<QByteArray> parts = m_head.left(m_head.indexOf("\r\n")).split(' ');
    const bool isGet = parts.value(0) == "GET";
    m_isHead = parts.value(0) == "HEAD";
    const QByteArray path = parts.value(1);

    // 方法 → 路径 → Range，这一序不可换；不合规的一律收掉这条连接
    if (!isGet && !m_isHead) {
        reply(405, "Allow: GET, HEAD\r\n", 0, 0);
        return;
    }
    if (path != "/" + m_token) {
        reply(404, QByteArray(), 0, 0);
        return;
    }

    QByteArray rangeValue;
    const int lineEnd = m_head.indexOf("\r\n");
    const QList<QByteArray> headerLines = m_head.mid(lineEnd + 2).split('\n');
    for (const QByteArray &line : headerLines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.left(6).toLower() == "range:") {
            rangeValue = trimmed.mid(6).trimmed();
            break;
        }
    }

    const RangeSpec range = rangeValue.isEmpty() ? RangeSpec{} : parseRange(rangeValue, m_total);
    if (range.kind == RangeKind::Unsatisfiable) {
        reply(416, "Content-Range: bytes */" + QByteArray::number(m_total) + "\r\n", 0, 0);
        return;
    }
    if (range.kind == RangeKind::Partial) {
        const QByteArray extra = "Content-Range: bytes " + QByteArray::number(range.start) + "-" +
                                 QByteArray::number(range.end) + "/" + QByteArray::number(m_total) +
                                 "\r\n";
        reply(206, extra, range.start, range.end - range.start + 1);
        return;
    }
    reply(200, QByteArray(), 0, m_total);
}

void RangeConnection::reply(int status, const QByteArray &extra, qint64 bodyStart,
                            qint64 bodyLength) {
    QByteArray head;
    head += "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusReason(status) + "\r\n";
    head += "Content-Length: " + QByteArray::number(bodyLength) + "\r\n";
    head += extra;
    head += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\n";
    head += "Connection: close\r\n\r\n";

    // HEAD 的 Content-Length 照 GET 报（那是"真要取会给多少"），但一字节都不喂
    m_reply = {true, status, bodyStart, m_isHead ? qint64(0) : bodyLength};
    m_bodyPos = bodyStart;
    m_remaining = m_reply.promised;
    m_phase = Phase::Serving;
    m_pumping = true; // 写头同样会同拍回吐 bytesWritten，先进保护
    m_socket->write(head);
    m_pumping = false;
    pump();
}

void RangeConnection::pump() {
    if (m_phase != Phase::Serving)
        return;
    if (m_cancel.load()) {
        finish(Ending::Cancelled);
        return;
    }
    m_pumping = true;
    while (m_socket->bytesToWrite() < kHighWaterBytes) {
        if (m_remaining <= 0) {
            m_pumping = false;
            finish(Ending::Done); // 承诺量已交齐：本侧发 FIN，让对端确认拿齐
            return;
        }
        const qint64 want = qMin<qint64>(kChunkBytes, m_remaining);
        if (m_chunk.isEmpty())
            m_chunk.resize(kChunkBytes); // 这块搬运缓冲只在真要喂体时才占
        if (m_stream.seek(m_bodyPos) < 0) {
            m_pumping = false;
            finish(Ending::SeekFailed);
            return;
        }
        const qint64 got = m_stream.read(m_chunk.data(), want);
        if (got <= 0) {
            // kEof / kError（kNoData 同判：本桥唯一在役来源是本地文件解密流，按契约
            // 不外抛）——交付不足额即断，Content-Length 已诺，对端按错误收
            m_pumping = false;
            finish(Ending::StreamEnded);
            return;
        }
        m_socket->write(m_chunk.constData(), got);
        m_bodyPos += got;
        m_remaining -= got;
    }
    m_pumping = false;
}

void RangeConnection::finish(Ending ending) {
    if (m_phase == Phase::Finished || m_phase == Phase::ClosingSent)
        return;
    if (m_reply.sent)
        OXBOW_LOG_DEBUG() << "StreamRangeServer:" << (m_isHead ? "HEAD" : "GET") << m_reply.status
                          << rangeFormText(m_reply.status) << "body" << m_reply.bodyStart << '+'
                          << m_reply.promised << "delivered" << m_reply.promised - m_remaining
                          << "in" << m_clock.elapsed() << "ms" << endingText(ending);
    if (ending == Ending::SeekFailed || ending == Ending::StreamEnded ||
        ending == Ending::HeadTooLong)
        // 这一问没交付足额。SeekFailed / StreamEnded 是流的读取出了问题（SubtitleExtractor
        // 会因此收到 ffmpeg 的非零退出码）；Content-Length 已诺而未兑现，对端按截断收
        OXBOW_LOG_WARNING() << "StreamRangeServer:" << endingText(ending) << "after promising"
                            << m_reply.promised << "bytes at" << m_reply.bodyStart;

    if (ending == Ending::Done) {
        // 交齐了才优雅收尾：发 FIN，等 ack 的那拍自毁——不占线程，也不攒搬运缓冲
        m_phase = Phase::ClosingSent;
        m_socket->disconnectFromHost();
        if (m_socket->state() == QAbstractSocket::UnconnectedState) {
            m_phase = Phase::Finished;
            m_socket->deleteLater(); // 对端早已走，没人再来 ack
        }
        return;
    }
    m_phase = Phase::Finished;
    if (ending != Ending::PeerGone)
        m_socket->abort(); // 取消与真错误：不等握手，立即断
    m_socket->deleteLater();
}

} // namespace

StreamRangeServer::StreamRangeServer(QObject *parent) : QThread(parent) {}

StreamRangeServer::~StreamRangeServer() {
    // 先收口再 join：实例不得在服务线程运行中被拆。join 不设超时——唯一的长等点是
    // openStream（端点契约明允阻塞），超时后继续拆成员等于把仍在运行的线程从对象下面
    // 抽走；循环期内取消旗最迟一拍生效，正常路径等不到什么
    stop();
    wait();
}

void StreamRangeServer::launch(const std::shared_ptr<StreamEndpoint> &endpoint,
                               const QString &identity) {
    Q_ASSERT(!isRunning());
    Q_ASSERT(endpoint); // run 内直接解引用；空端点属调用方违约
    m_endpoint = endpoint;
    m_identity = identity;
    // 128 位十六进制 token（每次实例随机，寿命等于一次取数）
    m_token = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(u'-');
    Q_ASSERT(m_token.size() == 32);
    m_cancel.store(false);
    start();
}

void StreamRangeServer::stop() {
    m_cancel.store(true);
    QMutexLocker l(&m_streamMutex);
    if (m_stream)
        m_stream->cancel(); // ByteStream 契约：任意线程、不阻塞、能打断阻塞中的 read
}

// 服务线程退出（含早退）时摘掉成员侧那份流：run 的局部 shared_ptr 出作用域即完成
// 实际释放，成员不留一份把流的寿命拖到对象 deleteLater 之后
void StreamRangeServer::releaseStream() {
    QMutexLocker l(&m_streamMutex);
    m_stream.reset();
}

void StreamRangeServer::run() {
    // 起流必须在服务线程内（端点契约：绝不在 GUI 线程起流）；失败文案原样透传
    std::shared_ptr<ByteStream> stream;
    QString failureReason;
    if (const auto opened = m_endpoint->openStream(m_identity))
        stream = opened.value();
    else
        failureReason = opened.error();
    if (!stream) {
        if (!m_cancel.load()) // open 期取消：尽力而为，结果丢弃不发失败
            emit failed(failureReason);
        return;
    }
    {
        QMutexLocker l(&m_streamMutex);
        if (m_cancel.load())
            return; // open 期取消：等 openStream 返回后丢弃结果（generation 保证不落地）
        m_stream = stream;
    }
    const qint64 total = stream->size();
    if (total < 0) {
        releaseStream();
        emit failed(QStringLiteral("stream has no known size"));
        return;
    }
    // 桥要还的就是 seek 这件事：ffmpeg 拿到不能回跳的输入，在 moov 在尾的 mp4 上是静默
    // 失败（退出码 0、产出为空），症状比直接报错更难查。因此"可寻位"不作运行期判据、
    // 而钉成断言——申报 supportsSecondaryDecode 的实体必须同时给得出可寻位与总长已知，
    // 落空即调用方违约，不是一种可以返回的取数结果
    Q_ASSERT(stream->isSeekable());

    // 监听 loopback 临时端口；URL 路径只认本次实例的 token
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        releaseStream();
        emit failed(server.errorString());
        return;
    }
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(QStringLiteral("127.0.0.1"));
    url.setPort(server.serverPort());
    url.setPath(QStringLiteral("/") + m_token);
    OXBOW_LOG_DEBUG().noquote() << "StreamRangeServer: ready port" << server.serverPort() << "token"
                                << m_token.left(6) << "size" << total;
    emit ready(url, total);

    // 并行服务：每条连接一个应答对象，取字节与写 socket 全在这一条线程里交替推进。
    // stream 引用的是本函数的局部 shared_ptr——server 后声明、先析构，连接对象随它的
    // 子 socket 一起没，那时 shared_ptr 还在
    QEventLoop loop;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &stream, total, this] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            auto *connection = new RangeConnection(socket, *stream, total, m_token, m_cancel);
            Q_UNUSED(connection); // 父即 socket，随它一并销毁
        }
    });

    // 取消旗的观察点：事件循环空转（不再有 readyRead / bytesWritten 通报）时，最迟这一拍
    // 收口。除此之外本桥不设任何计时判据
    QTimer watchdog;
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [this, &loop] {
        if (m_cancel.load())
            loop.quit();
    });
    watchdog.setInterval(kCancelWatchdogMs);
    watchdog.start();
    loop.exec();
    watchdog.stop();

    // 退出收口：先切断监听，再把还在服的连接一律 abort（本次取数已经结束，继续冲积压
    // 只是白占），最后释放流
    server.close();
    const auto sockets = server.findChildren<QTcpSocket *>();
    for (QTcpSocket *socket : sockets)
        socket->abort();
    releaseStream();
}

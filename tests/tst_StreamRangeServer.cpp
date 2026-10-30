// StreamRangeServer 单元测试（N145）：回环 HTTP Range 桥的协议面与
// 取消面逐条钉死——全量 GET 与源逐字节一致、Content-Length 正确、三种单段 Range
// 形态、边界（start==size 与 bytes=-0 → 416、end 越界钳制、suffix 超长取整文件、
// 多段与畸形 Range 按无 Range 全量）、404（token 不符）/ 405（非 GET/HEAD）/ HEAD、
// open 期取消丢弃结果、服务中取消断连退线程、总长未知启动即失败、被弃下的连接不挡后续
// 请求（并行服务的立身之本——本桥不设任何计时判据，取消旗是唯一让服务线程退出的东西）。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R StreamRangeServer。

#include "pch.h"

#include <QtTest/QtTest>

#include "streams/FileByteStream.h"
#include "streams/StreamRangeServer.h"

namespace {

// 确定性内容：第 i 字节 = (i*31 + 7) & 0xFF（与 FileByteStream 同式）
QByteArray pattern(qsizetype len) {
    QByteArray out(len, Qt::Uninitialized);
    for (qsizetype i = 0; i < len; ++i)
        out[i] = char((i * 31 + 7) & 0xFF);
    return out;
}

QString writeFixture(const QTemporaryDir &dir, const QString &name, const QByteArray &data) {
    const QString path = QDir(dir.path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(data);
    file.close();
    return QDir::toNativeSeparators(path);
}

// 测试端点：identity 即文件原生路径；开期可注延迟（open 期取消例需要确定窗口）
class FileEndpoint final : public StreamEndpoint {
  public:
    FileEndpoint(QString path, int openDelayMs = 0)
        : m_path(std::move(path)), m_delayMs(openDelayMs) {}

    std::expected<std::shared_ptr<ByteStream>, QString>
    openStream(const QString &identity) const override {
        if (m_delayMs > 0)
            QThread::msleep(m_delayMs);
        if (identity != m_path)
            return std::unexpected(QStringLiteral("identity mismatch"));
        auto opened = FileByteStream::open(m_path);
        if (!opened)
            return std::unexpected(opened.error());
        return std::shared_ptr<ByteStream>(std::move(opened.value()));
    }

  private:
    QString m_path;
    int m_delayMs = 0;
};

// 总长未知的流（size < 0 即启动失败；crypt 恒已知，用桩构造这一分支）
class UnknownSizeStream final : public ByteStream {
  public:
    explicit UnknownSizeStream(QByteArray data) : m_data(std::move(data)) {}
    qint64 read(char *out, qint64 maxBytes) override {
        const qint64 n = qMin<qint64>(maxBytes, m_data.size() - m_pos);
        if (n <= 0)
            return kEof;
        memcpy(out, m_data.constData() + m_pos, size_t(n));
        m_pos += n;
        return n;
    }
    bool isSeekable() const override { return true; }
    qint64 seek(qint64 to) override {
        m_pos = qBound<qint64>(0, to, m_data.size());
        return m_pos;
    }
    qint64 size() const override { return -1; }
    Stats stats() const override { return {}; }

  private:
    QByteArray m_data;
    qint64 m_pos = 0;
};

class UnknownSizeEndpoint final : public StreamEndpoint {
  public:
    std::expected<std::shared_ptr<ByteStream>, QString> openStream(const QString &) const override {
        return std::shared_ptr<ByteStream>(new UnknownSizeStream(pattern(1024)));
    }
};

// 一发一收的最小 HTTP 客户端（阻塞式，读齐响应头 + 按需收体）
struct Reply {
    bool ok = false;
    int status = 0;
    qint64 contentLength = -1;
    QByteArray contentRange;
    QByteArray body;
};

Reply httpRequest(const QUrl &url, const QByteArray &rangeHeader = {}, bool head = false,
                  bool drainBody = true) {
    Reply r;
    QTcpSocket sock;
    sock.connectToHost(url.host(), quint16(url.port()));
    if (!sock.waitForConnected(3000))
        return r;
    QByteArray request = (head ? "HEAD " : "GET ");
    request += url.path(QUrl::FullyEncoded).toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!rangeHeader.isEmpty())
        request += rangeHeader;
    request += "Connection: close\r\n\r\n";
    sock.write(request);
    if (!sock.waitForBytesWritten(3000))
        return r;

    const QDeadlineTimer deadline(5000);
    QByteArray buf;
    while (!buf.contains("\r\n\r\n") && !deadline.hasExpired()) {
        buf += sock.readAll();
        // 零长体响应里头与 FIN 同拍到达：收数后先判完成，再判断连——顺序反了会把
        // 已到手的整份头连同断连一起弃掉
        if (buf.contains("\r\n\r\n"))
            break;
        // 等待失败只看状态，不查 error()：QTcpSocket 没有 NoError，`UnknownSocketError`
        // 兼作"尚未记任何错误"的初值，而超时不保证记下哪个码（另有一档 SocketTimeoutError，
        // 落哪个值文档从不承诺）——拿 error() 两分即把纯超时误判成 IO 错误
        if (!sock.waitForReadyRead(100) && sock.state() == QAbstractSocket::UnconnectedState)
            return r; // 真正一头未获
    }
    const int sep = buf.indexOf("\r\n\r\n");
    if (sep < 0)
        return r;
    const QByteArray headPart = buf.left(sep);
    r.body = buf.mid(sep + 4);

    const QList<QByteArray> lines = headPart.split('\n');
    if (lines.isEmpty())
        return r;
    const QList<QByteArray> statusParts = lines.first().trimmed().split(' ');
    if (statusParts.size() < 2)
        return r;
    r.status = statusParts.at(1).toInt();
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.startsWith("Content-Length:"))
            r.contentLength = line.mid(15).trimmed().toLongLong();
        else if (line.startsWith("Content-Range:"))
            r.contentRange = line.mid(14).trimmed();
    }
    if (head || !drainBody) {
        r.ok = true;
        return r;
    }
    while (r.body.size() < r.contentLength && !deadline.hasExpired()) {
        r.body += sock.readAll();
        if (!sock.waitForReadyRead(100) && sock.state() == QAbstractSocket::UnconnectedState)
            break;
    }
    r.ok = r.contentLength < 0 || r.body.size() >= r.contentLength;
    return r;
}

} // namespace

class TestStreamRangeServer : public QObject {
    Q_OBJECT

  private slots:
    void fullGetMatchesSourceWithContentLength();
    void rangeFormsDeliverExactSlices();
    void boundaryRangesFollowSpec();
    void wrongPathMethodAndHead();
    void unknownSizeFailsAtStart();
    void openPhaseCancelDiscardsResult();
    void cancelDuringServeDisconnectsAndExits();
    void abandonedConnectionNeverBlocksAnother();
};

void TestStreamRangeServer::fullGetMatchesSourceWithContentLength() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray data = pattern(200000);
    const QString path = writeFixture(dir, QStringLiteral("full.bin"), data);
    QVERIFY(!path.isEmpty());

    StreamRangeServer server;
    QUrl url;
    qint64 size = -2;
    connect(&server, &StreamRangeServer::ready, this, [&url, &size](const QUrl &u, qint64 s) {
        url = u;
        size = s;
    });
    server.launch(std::make_shared<FileEndpoint>(path), path);
    QTRY_VERIFY(!url.isEmpty());
    QCOMPARE(size, qint64(data.size()));
    QVERIFY(url.host() == QLatin1String("127.0.0.1"));
    QVERIFY(url.path().size() == 33 && url.path().startsWith(u'/'));

    const Reply r = httpRequest(url);
    QVERIFY(r.ok);
    QCOMPARE(r.status, 200);
    QCOMPARE(r.contentLength, qint64(data.size()));
    QCOMPARE(r.body, data);

    server.stop();
    QVERIFY(server.wait(5000));
}

void TestStreamRangeServer::rangeFormsDeliverExactSlices() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray data = pattern(4096);
    const QString path = writeFixture(dir, QStringLiteral("slices.bin"), data);
    QVERIFY(!path.isEmpty());

    StreamRangeServer server;
    QUrl url;
    connect(&server, &StreamRangeServer::ready, this, [&url](const QUrl &u, qint64) { url = u; });
    server.launch(std::make_shared<FileEndpoint>(path), path);
    QTRY_VERIFY(!url.isEmpty());

    // bytes=start-end
    Reply r = httpRequest(url, "Range: bytes=100-199\r\n");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 206);
    QCOMPARE(r.contentLength, qint64(100));
    QCOMPARE(r.contentRange, QByteArray("bytes 100-199/4096"));
    QCOMPARE(r.body, data.mid(100, 100));
    // bytes=start-
    r = httpRequest(url, "Range: bytes=4000-\r\n");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 206);
    QCOMPARE(r.contentRange, QByteArray("bytes 4000-4095/4096"));
    QCOMPARE(r.body, data.mid(4000));
    // bytes=-suffix
    r = httpRequest(url, "Range: bytes=-64\r\n");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 206);
    QCOMPARE(r.contentRange, QByteArray("bytes 4032-4095/4096"));
    QCOMPARE(r.body, data.right(64));

    server.stop();
    QVERIFY(server.wait(5000));
}

void TestStreamRangeServer::boundaryRangesFollowSpec() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray data = pattern(1000);
    const QString path = writeFixture(dir, QStringLiteral("edge.bin"), data);
    QVERIFY(!path.isEmpty());

    StreamRangeServer server;
    QUrl url;
    connect(&server, &StreamRangeServer::ready, this, [&url](const QUrl &u, qint64) { url = u; });
    server.launch(std::make_shared<FileEndpoint>(path), path);
    QTRY_VERIFY(!url.isEmpty());

    // start == size（恰好 EOF）→ 416，附 bytes */size
    Reply r = httpRequest(url, "Range: bytes=1000-\r\n");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 416);
    QCOMPARE(r.contentRange, QByteArray("bytes */1000"));
    // bytes=-0 → 416
    r = httpRequest(url, "Range: bytes=-0\r\n");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 416);
    // end 越界 → 钳到 size-1
    r = httpRequest(url, "Range: bytes=900-999999\r\n");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 206);
    QCOMPARE(r.contentRange, QByteArray("bytes 900-999/1000"));
    QCOMPARE(r.body, data.mid(900));
    // suffix 超长 → 取整文件
    r = httpRequest(url, "Range: bytes=-999999\r\n");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 206);
    QCOMPARE(r.contentRange, QByteArray("bytes 0-999/1000"));
    QCOMPARE(r.body, data);
    // 多段 → 忽略该头按 200 全量
    r = httpRequest(url, "Range: bytes=0-1,3-4\r\n");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 200);
    QCOMPARE(r.body, data);
    // 畸形 → 忽略该头按 200 全量
    r = httpRequest(url, "Range: bytes=abc\r\n");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 200);
    QCOMPARE(r.body, data);

    server.stop();
    QVERIFY(server.wait(5000));
}

void TestStreamRangeServer::wrongPathMethodAndHead() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray data = pattern(256);
    const QString path = writeFixture(dir, QStringLiteral("misc.bin"), data);
    QVERIFY(!path.isEmpty());

    StreamRangeServer server;
    QUrl url;
    connect(&server, &StreamRangeServer::ready, this, [&url](const QUrl &u, qint64) { url = u; });
    server.launch(std::make_shared<FileEndpoint>(path), path);
    QTRY_VERIFY(!url.isEmpty());

    // token 不符 → 404
    QUrl wrong = url;
    wrong.setPath(QStringLiteral("/deadbeefdeadbeefdeadbeefdeadbeef"));
    Reply r = httpRequest(wrong);
    QVERIFY(r.ok);
    QCOMPARE(r.status, 404);
    // 非 GET/HEAD → 405
    {
        QTcpSocket sock;
        sock.connectToHost(url.host(), quint16(url.port()));
        QVERIFY(sock.waitForConnected(3000));
        sock.write("POST " + url.path(QUrl::FullyEncoded).toUtf8() +
                   " HTTP/1.1\r\nHost: h\r\n\r\n");
        QVERIFY(sock.waitForBytesWritten(3000));
        QByteArray buf;
        const QDeadlineTimer deadline(5000);
        while (!buf.contains("\r\n\r\n") && !deadline.hasExpired()) {
            buf += sock.readAll();
            if (!sock.waitForReadyRead(100) && sock.state() == QAbstractSocket::UnconnectedState)
                break;
        }
        QVERIFY(buf.startsWith("HTTP/1.1 405"));
        QVERIFY(buf.contains("Allow: GET, HEAD"));
    }
    // HEAD：头齐全、无体
    r = httpRequest(url, {}, true);
    QVERIFY(r.ok);
    QCOMPARE(r.status, 200);
    QCOMPARE(r.contentLength, qint64(data.size()));
    QVERIFY(r.body.isEmpty());

    server.stop();
    QVERIFY(server.wait(5000));
}

void TestStreamRangeServer::unknownSizeFailsAtStart() {
    StreamRangeServer server;
    bool failedSeen = false;
    bool readySeen = false;
    QString reason;
    connect(&server, &StreamRangeServer::ready, this, [&readySeen] { readySeen = true; });
    connect(&server, &StreamRangeServer::failed, this, [&failedSeen, &reason](const QString &why) {
        failedSeen = true;
        reason = why;
    });
    server.launch(std::make_shared<UnknownSizeEndpoint>(), QStringLiteral("anything"));
    QTRY_VERIFY(failedSeen);
    QVERIFY(!readySeen);
    QVERIFY(reason.contains(QStringLiteral("size")));
    QVERIFY(server.wait(5000));
}

void TestStreamRangeServer::openPhaseCancelDiscardsResult() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFixture(dir, QStringLiteral("slow.bin"), pattern(64));
    QVERIFY(!path.isEmpty());

    StreamRangeServer server;
    bool readySeen = false;
    bool failedSeen = false;
    connect(&server, &StreamRangeServer::ready, this, [&readySeen] { readySeen = true; });
    connect(&server, &StreamRangeServer::failed, this, [&failedSeen] { failedSeen = true; });
    // 端点开期延迟 400ms：stop 的取消旗必然先落，openStream 返回后结果必须被丢弃
    server.launch(std::make_shared<FileEndpoint>(path, 400), path);
    server.stop();
    QVERIFY(server.wait(5000));
    QVERIFY(!readySeen);
    QVERIFY(!failedSeen);
}

void TestStreamRangeServer::cancelDuringServeDisconnectsAndExits() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 4 MB：客户端只读一小口即停 → 服务端写满内核缓冲后被取消旗解救、断连退线程
    const QByteArray data = pattern(4 * 1024 * 1024);
    const QString path = writeFixture(dir, QStringLiteral("big.bin"), data);
    QVERIFY(!path.isEmpty());

    StreamRangeServer server;
    QUrl url;
    bool finishedSeen = false;
    connect(&server, &StreamRangeServer::ready, this, [&url](const QUrl &u, qint64) { url = u; });
    connect(&server, &QThread::finished, this, [&finishedSeen] { finishedSeen = true; });
    server.launch(std::make_shared<FileEndpoint>(path), path);
    QTRY_VERIFY(!url.isEmpty());

    QTcpSocket sock;
    sock.connectToHost(url.host(), quint16(url.port()));
    QVERIFY(sock.waitForConnected(3000));
    sock.write("GET " + url.path(QUrl::FullyEncoded).toUtf8() + " HTTP/1.1\r\nHost: h\r\n\r\n");
    QVERIFY(sock.waitForBytesWritten(3000));
    QVERIFY(sock.waitForReadyRead(3000));
    const QByteArray first = sock.readAll(); // 读一小口即停，制造写侧背压
    QVERIFY(!first.isEmpty());

    server.stop();
    QTRY_VERIFY(finishedSeen);
    QVERIFY(server.isFinished());

    // 被服务的连接终将断开（服务端弃连）
    const QDeadlineTimer deadline(8000);
    while (sock.state() != QAbstractSocket::UnconnectedState && !deadline.hasExpired())
        sock.waitForReadyRead(100);
    QCOMPARE(sock.state(), QAbstractSocket::UnconnectedState);

    // 监听已关：新连接拒收（端口不可再连）
    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, quint16(url.port()));
    QVERIFY(!probe.waitForConnected(500));
}

// 并行服务的立身之本：一条被弃下的连接（对端不再读、也不关——实测 ffmpeg 的 seek 正是
// 这个形态）绝不能挡住后续请求。串行版里它把整串请求一起卡住，只能靠计时器或"猜它弃了"
// 来救，而那条判据拟合的是某个客户端的行为，不是协议。本桥不设计时判据，取消旗是唯一
// 能让服务线程退出的东西，所以这一例同时也在证：甲停在待发队列上不动，谁都不必等它。
void TestStreamRangeServer::abandonedConnectionNeverBlocksAnother() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 8 MB：远超两侧内核缓冲，甲不读则它的待发队列必然停在那条界限上、自己不推进
    const QByteArray data = pattern(8 * 1024 * 1024);
    const QString path = writeFixture(dir, QStringLiteral("stall.bin"), data);
    QVERIFY(!path.isEmpty());

    StreamRangeServer server;
    QUrl url;
    connect(&server, &StreamRangeServer::ready, this, [&url](const QUrl &u, qint64) { url = u; });
    server.launch(std::make_shared<FileEndpoint>(path), path);
    QTRY_VERIFY(!url.isEmpty());

    // 甲：一问之后一个字节都不读，也不关
    QTcpSocket abandoned;
    abandoned.connectToHost(url.host(), quint16(url.port()));
    QVERIFY(abandoned.waitForConnected(3000));
    abandoned.write("GET " + url.path(QUrl::FullyEncoded).toUtf8() +
                    " HTTP/1.1\r\n"
                    "Host: h\r\n\r\n");
    QVERIFY(abandoned.waitForBytesWritten(3000));
    QThread::msleep(300); // 不处理事件：让甲确实停在"对端不读"那一侧

    // 乙：完全不必等甲，自己就能跑完
    QElapsedTimer clock;
    clock.start();
    const Reply r = httpRequest(url);
    const qint64 waited = clock.elapsed();
    QVERIFY2(r.ok && r.status == 200 && r.body == data, "乙没拿到完整响应");
    QVERIFY2(waited < 1500,
             qPrintable(QStringLiteral("乙等了 %1 ms：甲挡住了服务，并行形态没生效").arg(waited)));

    // 自证甲确实被晾着（而非 8 MB 全被内核缓冲吃下后自然交付完）
    const qsizetype pending = abandoned.bytesAvailable(); // 先取存量：readAll 会清空缓冲
    const qsizetype delivered = pending + abandoned.readAll().size();
    QVERIFY2(
        delivered < data.size(),
        qPrintable(QStringLiteral("甲已拿到 %1 字节整份体，本例没造出弃连形态").arg(delivered)));

    // 收口：取数结束时被弃下的甲一并断掉，不留挂着的连接
    server.stop();
    QVERIFY(server.wait(5000));
    abandoned.waitForDisconnected(2000);
    QCOMPARE(abandoned.state(), QAbstractSocket::UnconnectedState);
}

QTEST_GUILESS_MAIN(TestStreamRangeServer)
#include "tst_StreamRangeServer.moc"

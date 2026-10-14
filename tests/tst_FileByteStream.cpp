// FileByteStream 单元测试（N98 §1：本地文件这条"第二个传输"）：读侧口径逐条钉死
// ——打开失败带可读文案、总长在 open 期一次定型、顺序读与短读不重不漏、seek 不做
// 范围检查（越过末尾恒成功返回目标偏移，随后的读落 kEof；再 seek 回来仍可解）、
// 位置守恒（Stats::pos 随 read 与 seek 一同推进）、非正长度按 kEof 收束、
// cancel 无事可做。宽字符路径（中文 + 空格）另立一例，覆盖 Windows 的 _wfopen_s 通路。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R FileByteStream。

#include "pch.h"

#include <QtTest/QtTest>

#include "streams/FileByteStream.h"

namespace {

// 确定性内容：第 i 字节 = (i*31 + 7) & 0xFF
QByteArray pattern(qsizetype len) {
    QByteArray out(len, Qt::Uninitialized);
    for (qsizetype i = 0; i < len; ++i)
        out[i] = char((i * 31 + 7) & 0xFF);
    return out;
}

// 在临时目录里落一份 fixture 文件，返回其原生路径（FileByteStream 收的是原生分隔符）
QString writeFixture(const QTemporaryDir &dir, const QString &name, const QByteArray &data) {
    const QString path = QDir(dir.path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(data);
    file.close();
    return QDir::toNativeSeparators(path);
}

} // namespace

class TestFileByteStream : public QObject {
    Q_OBJECT

  private slots:
    void missingFileReportsReadableError();
    void emptyFileIsZeroSizedAndReadsEof();
    void sequentialReadDeliversEveryByteOnce();
    void seekPastEndSucceedsThenReadsEof();
    void seekIsRepeatableAndRewinds();
    void statsTrackPositionAndTotal();
    void nonPositiveLengthReadsEof();
    void unicodeAndSpacedPathOpens();
};

void TestFileByteStream::missingFileReportsReadableError() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missing =
        QDir::toNativeSeparators(QDir(dir.path()).filePath(QStringLiteral("nope.bin")));
    auto opened = FileByteStream::open(missing);
    QVERIFY(!opened);
    QVERIFY(opened.error().contains(QStringLiteral("cannot open file")));

    // 空路径单独一条文案（不该把空串拼成"目录根"去打开）
    auto empty = FileByteStream::open(QString());
    QVERIFY(!empty);
    QVERIFY(empty.error().contains(QStringLiteral("empty file path")));
}

void TestFileByteStream::emptyFileIsZeroSizedAndReadsEof() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFixture(dir, QStringLiteral("empty.bin"), QByteArray());
    QVERIFY(!path.isEmpty());

    auto opened = FileByteStream::open(path);
    QVERIFY(bool(opened));
    const auto stream = *opened;
    QCOMPARE(stream->size(), qint64(0));
    QVERIFY(stream->isSeekable());
    char buf[16];
    QCOMPARE(stream->read(buf, 16), ByteStream::kEof);
    QCOMPARE(stream->seek(0), qint64(0));
    QCOMPARE(stream->read(buf, 16), ByteStream::kEof);
}

void TestFileByteStream::sequentialReadDeliversEveryByteOnce() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray data = pattern(200000);
    const QString path = writeFixture(dir, QStringLiteral("body.bin"), data);
    QVERIFY(!path.isEmpty());

    auto opened = FileByteStream::open(path);
    QVERIFY(bool(opened));
    const auto stream = *opened;
    QCOMPARE(stream->size(), qint64(data.size()));

    // 4 KiB 一片读尽：不重不漏，读完后恒 kEof
    QByteArray got;
    QByteArray chunk(4096, Qt::Uninitialized);
    while (true) {
        const qint64 n = stream->read(chunk.data(), chunk.size());
        if (n == ByteStream::kEof)
            break;
        QVERIFY2(n > 0, "sequential read must not error");
        got.append(chunk.constData(), int(n));
    }
    QCOMPARE(got, data);
    QCOMPARE(stream->read(chunk.data(), 8), ByteStream::kEof);
    QCOMPARE(stream->stats().pos, qint64(data.size()));
}

void TestFileByteStream::seekPastEndSucceedsThenReadsEof() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray data = pattern(1000);
    const QString path = writeFixture(dir, QStringLiteral("short.bin"), data);
    QVERIFY(!path.isEmpty());

    auto opened = FileByteStream::open(path);
    QVERIFY(bool(opened));
    const auto stream = *opened;
    char buf[8];

    // 不做范围检查：越过末尾同样成功返回目标（N98 §5 定案，与解密流同一条规则）
    QCOMPARE(stream->seek(1000), qint64(1000)); // 恰好 EOF
    QCOMPARE(stream->read(buf, 8), ByteStream::kEof);
    QCOMPARE(stream->seek(64 * 1024), qint64(64 * 1024)); // 远超总长
    QCOMPARE(stream->read(buf, 8), ByteStream::kEof);

    // seek 清掉 eof 旗：回到有效区间内必须重新供数
    QCOMPARE(stream->seek(996), qint64(996));
    QCOMPARE(stream->read(buf, 8), qint64(4)); // 只剩 4 字节，短读合法
    QVERIFY(QByteArray(buf, 4) == data.mid(996));
}

void TestFileByteStream::seekIsRepeatableAndRewinds() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray data = pattern(5000);
    const QString path = writeFixture(dir, QStringLiteral("rewind.bin"), data);
    QVERIFY(!path.isEmpty());

    auto opened = FileByteStream::open(path);
    QVERIFY(bool(opened));
    const auto stream = *opened;
    QByteArray buf(64, Qt::Uninitialized);

    QCOMPARE(stream->seek(1234), qint64(1234));
    QCOMPARE(stream->read(buf.data(), 64), qint64(64));
    const QByteArray first = buf;

    // 同位二次 seek 不改变结果（幂等），回卷后读到同一串
    QCOMPARE(stream->seek(1234), qint64(1234));
    buf.fill('\0');
    QCOMPARE(stream->read(buf.data(), 64), qint64(64));
    QVERIFY(buf == first);
    QVERIFY(QByteArray(buf) == data.mid(1234, 64));

    QCOMPARE(stream->seek(0), qint64(0));
    buf.fill('\0');
    QCOMPARE(stream->read(buf.data(), 10), qint64(10));
    QVERIFY(QByteArray(buf, 10) == data.left(10));
}

void TestFileByteStream::statsTrackPositionAndTotal() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFixture(dir, QStringLiteral("stats.bin"), pattern(300));
    QVERIFY(!path.isEmpty());

    auto opened = FileByteStream::open(path);
    QVERIFY(bool(opened));
    const auto stream = *opened;
    QCOMPARE(stream->stats().total, qint64(300));
    QCOMPARE(stream->stats().pos, qint64(0));

    char buf[128];
    QCOMPARE(stream->read(buf, 100), qint64(100));
    QCOMPARE(stream->stats().pos, qint64(100)); // 交付即推进
    QCOMPARE(stream->seek(250), qint64(250));
    QCOMPARE(stream->stats().pos, qint64(250));   // 寻位同样推进
    QCOMPARE(stream->read(buf, 128), qint64(50)); // 尾段短读
    QCOMPARE(stream->stats().pos, qint64(300));
}

void TestFileByteStream::nonPositiveLengthReadsEof() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeFixture(dir, QStringLiteral("len.bin"), pattern(32));
    QVERIFY(!path.isEmpty());

    auto opened = FileByteStream::open(path);
    QVERIFY(bool(opened));
    const auto stream = *opened;
    char buf[8];
    // 非正长度不外乎"要 0 字节"，按 kEof 收束而不是喂 0 进调用方的判定分支
    QCOMPARE(stream->read(buf, 0), ByteStream::kEof);
    QCOMPARE(stream->read(buf, -5), ByteStream::kEof);
    // cancel 契约上任意线程安全，本地盘读无阻塞可解救 ⇒ 无事可做且流仍可用
    stream->cancel();
    QCOMPARE(stream->read(buf, 8), qint64(8));
}

void TestFileByteStream::unicodeAndSpacedPathOpens() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray data = pattern(7);
    const QString name = QStringLiteral("电影 中文名 @2024.bin");
    const QString path = writeFixture(dir, name, data);
    QVERIFY(!path.isEmpty());

    auto opened = FileByteStream::open(path);
    QVERIFY2(bool(opened), "wide-char path must open through the native form");
    const auto stream = *opened;
    QCOMPARE(stream->size(), qint64(data.size()));
    char buf[8];
    QCOMPARE(stream->read(buf, 8), qint64(7));
    QVERIFY(QByteArray(buf, 7) == data);
}

QTEST_GUILESS_MAIN(TestFileByteStream)
#include "tst_FileByteStream.moc"

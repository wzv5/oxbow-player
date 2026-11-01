#include "pch.h"

#include <QtTest/QtTest>

#include "SubtitleExtractor.h"
#include "streams/StreamTransit.h"

// N143：字幕浏览器取数模块的解析与判据契约测试——srt 解析（时间行 / 行号 / 清洗 /
// 排序 / 末块无上界）、编码判别、文本字幕轨判据。
// N145 起另含取数请求的判定矩阵（外部轨与正播解绑、转接身份与失败归类、刷新丢缓存）：
// 全部走 start() 的同步发射面取证，不起进程、不碰端点与网络。

class TestSubtitleExtractor : public QObject {
    Q_OBJECT

  private slots:
    void parseBasicBlock();
    void parseCommaAndDotMilliseconds();
    void parseMultilineJoinsWithSpace();
    void parseLineNumberOnlyAsSeparator();
    void parseMissingBlankLines();
    void parseMissingEndFallsBackToNextStart();
    void parseLastBlockWithoutEndIsUnbounded();
    void parseEndBeforeStartDropped();
    void parseStableSortOutOfOrder();
    void parseEmptyTextEntryKept();
    void parseOverlapKept();
    void cleanStripsHtmlAndAssOverrides();
    void decodeUtf8Bom();
    void decodeUtf8Plain();
    void decodeUtf16LeBom();
    void decodeUtf16BeBom();
    void decodeInvalidUtf8FallsBackToLocale();
    void browsableTextSubtitleCriteria();
    void transitWithoutResolverIsSourceUnavailable();
    void malformedTransitUriIsSourceUnavailable();
    void externalSubtitleIgnoresMediaLocality();
    void localTrackNeverEntersTransitChain();
    void transitTrackWithoutFfIndexIsFetchFailure();
    void skipCacheDropsCachedEntry();
};

using SubtitleEntry = SubtitleExtractor::SubtitleEntry;

static QString formatEntry(const SubtitleEntry &entry) {
    return QStringLiteral("[%1,%2)%3")
        .arg(QString::number(entry.startSeconds),
             entry.endSeconds == std::numeric_limits<double>::infinity()
                 ? QStringLiteral("inf")
                 : QString::number(entry.endSeconds),
             entry.text.isEmpty() ? QStringLiteral("<empty>") : QStringLiteral("|") + entry.text);
}

namespace {

// 判定矩阵用例的最小文本字幕轨：sub 类型、主选（main-selection 0）、非位图
MpvController::TrackInfo textTrack(int ffIndex = 0) {
    MpvController::TrackInfo track;
    track.type = QStringLiteral("sub");
    track.codec = QStringLiteral("subrip");
    track.mainSelection = 0;
    track.ffIndex = ffIndex;
    return track;
}

// start() 的同步收口：本地性 / 身份判定与失败归类都在这一趟调用栈内定死
// （直读、缓存命中、FailedToStart 同样同步），故连上信号即可取证，不等事件循环
struct SyncResult {
    bool failedSeen = false;
    bool finishedSeen = false;
    SubtitleExtractor::Failure reason = SubtitleExtractor::Failure::NotLocal;
    QList<SubtitleEntry> entries;

    void bind(SubtitleExtractor *extractor) {
        QObject::connect(extractor, &SubtitleExtractor::failed, extractor,
                         [this](SubtitleExtractor::Failure why) {
                             failedSeen = true;
                             reason = why;
                         });
        QObject::connect(extractor, &SubtitleExtractor::finished, extractor,
                         [this](const QList<SubtitleEntry> &rows) {
                             finishedSeen = true;
                             entries = rows;
                         });
    }
};

QString writePlainFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes) {
    const QString path = QDir(dir.path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(bytes);
    file.close();
    return path;
}

// 失败归类的可读名：断言仍按枚举比对，失败时把实际值打进消息里
const char *failureName(SubtitleExtractor::Failure reason) {
    switch (reason) {
    case SubtitleExtractor::Failure::NotLocal:
        return "NotLocal";
    case SubtitleExtractor::Failure::FfmpegMissing:
        return "FfmpegMissing";
    case SubtitleExtractor::Failure::FetchFailed:
        return "FetchFailed";
    case SubtitleExtractor::Failure::SourceUnavailable:
        return "SourceUnavailable";
    }
    return "unknown";
}

// 外部字幕文件的可读正播替身：非本地、非转接（正播是什么都不问）
constexpr char kRemoteMedia[] = "webdav://server/film.mkv";
// 一份最小可直读的 srt 正文（外部轨直读路径不需要 ffmpeg）
constexpr char kSrtBody[] = "1\n00:00:01,000 --> 00:00:02,000\nHello\n";

} // namespace

void TestSubtitleExtractor::parseBasicBlock() {
    const QList<SubtitleEntry> entries =
        SubtitleExtractor::parseSrt(QStringLiteral("1\n00:00:02,000 --> 00:00:05,000\nHello world\n"
                                                   "\n2\n00:00:06,000 --> 00:00:08,000\nSecond\n"));
    QCOMPARE(entries.size(), 2);
    QCOMPARE(formatEntry(entries.at(0)), QStringLiteral("[2,5)|Hello world"));
    QCOMPARE(formatEntry(entries.at(1)), QStringLiteral("[6,8)|Second"));
}

void TestSubtitleExtractor::parseCommaAndDotMilliseconds() {
    const QList<SubtitleEntry> entries = SubtitleExtractor::parseSrt(
        QStringLiteral("00:00:01,500 --> 00:00:02,000\nA\n\n00:00:03.250 --> 00:00:04.000\nB\n"));
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).startSeconds, 1.5);
    QCOMPARE(entries.at(1).startSeconds, 3.25);
}

void TestSubtitleExtractor::parseMultilineJoinsWithSpace() {
    const QList<SubtitleEntry> entries = SubtitleExtractor::parseSrt(
        QStringLiteral("00:00:01,000 --> 00:00:02,000\nFirst line\nsecond line\n"));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).text, QStringLiteral("First line second line"));
}

void TestSubtitleExtractor::parseLineNumberOnlyAsSeparator() {
    // 行号只当块分隔：错号、缺号、复号都不影响切块与定位
    const QList<SubtitleEntry> entries = SubtitleExtractor::parseSrt(QStringLiteral(
        "99\n00:00:01,000 --> 00:00:02,000\nA\n\n\n7\n00:00:03,000 --> 00:00:04,000\nB\n"));
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).text, QStringLiteral("A"));
    QCOMPARE(entries.at(1).text, QStringLiteral("B"));
}

void TestSubtitleExtractor::parseMissingBlankLines() {
    // 时间行自身就是块边界：块间漏空行也能切
    const QList<SubtitleEntry> entries = SubtitleExtractor::parseSrt(
        QStringLiteral("00:00:01,000 --> 00:00:02,000\nA\n00:00:03,000 --> 00:00:04,000\nB\n"));
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).text, QStringLiteral("A"));
    QCOMPARE(entries.at(1).text, QStringLiteral("B"));
}

void TestSubtitleExtractor::parseMissingEndFallsBackToNextStart() {
    // 终点缺失回落下一块起点
    const QList<SubtitleEntry> entries = SubtitleExtractor::parseSrt(
        QStringLiteral("00:00:01,000 --> \nA\n\n00:00:03,500 --> 00:00:04,000\nB\n"));
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).endSeconds, 3.5);
    QCOMPARE(entries.at(1).endSeconds, 4.0);
}

void TestSubtitleExtractor::parseLastBlockWithoutEndIsUnbounded() {
    const QList<SubtitleEntry> entries =
        SubtitleExtractor::parseSrt(QStringLiteral("00:00:01,000 --> \nA\n"));
    QCOMPARE(entries.size(), 1);
    QVERIFY(std::isinf(entries.at(0).endSeconds));
}

void TestSubtitleExtractor::parseEndBeforeStartDropped() {
    // 畸形块（end 早于 start）整块丢弃，不半截入表
    const QList<SubtitleEntry> entries = SubtitleExtractor::parseSrt(QStringLiteral(
        "00:00:05,000 --> 00:00:03,000\nBad\n\n00:00:06,000 --> 00:00:07,000\nGood\n"));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.at(0).text, QStringLiteral("Good"));
}

void TestSubtitleExtractor::parseStableSortOutOfOrder() {
    // 乱序按起点稳定排序：起点相同的块保持源顺序（时间重叠不去重）
    const QList<SubtitleEntry> entries = SubtitleExtractor::parseSrt(QStringLiteral(
        "00:00:03,000 --> 00:00:04,000\nLate\n\n00:00:01,000 --> 00:00:02,000\nEarly\n\n"
        "00:00:01,000 --> 00:00:09,000\nOverlap\n"));
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries.at(0).text, QStringLiteral("Early"));
    QCOMPARE(entries.at(1).text, QStringLiteral("Overlap"));
    QCOMPARE(entries.at(2).text, QStringLiteral("Late"));
}

void TestSubtitleExtractor::parseEmptyTextEntryKept() {
    // 清洗后为空的条目保留（清屏 / 占位块）：空文本单元格占自己的时间区间
    const QList<SubtitleEntry> entries = SubtitleExtractor::parseSrt(QStringLiteral(
        "00:00:01,000 --> 00:00:02,000\n\n\n00:00:03,000 --> 00:00:04,000\nAfter\n"));
    QCOMPARE(entries.size(), 2);
    QVERIFY(entries.at(0).text.isEmpty());
    QCOMPARE(entries.at(1).text, QStringLiteral("After"));
}

void TestSubtitleExtractor::parseOverlapKept() {
    const QList<SubtitleEntry> entries = SubtitleExtractor::parseSrt(
        QStringLiteral("00:00:01,000 --> 00:00:05,000\nA\n\n00:00:02,000 --> 00:00:03,000\nB\n"));
    QCOMPARE(entries.size(), 2);
}

void TestSubtitleExtractor::cleanStripsHtmlAndAssOverrides() {
    QCOMPARE(SubtitleExtractor::cleanEntryText(
                 QStringLiteral("<i>Hello</i> <font "
                                "face=\"Arial\" size=\"28\">world</font>")),
             QStringLiteral("Hello world"));
    QCOMPARE(SubtitleExtractor::cleanEntryText(QStringLiteral("{\\an8}Top\\Nline\\h~")),
             QStringLiteral("Top line ~"));
    QCOMPARE(SubtitleExtractor::cleanEntryText(QStringLiteral("keep <this")),
             QStringLiteral("keep <this"));
}

void TestSubtitleExtractor::decodeUtf8Bom() {
    const QByteArray bytes = QByteArray::fromHex("EFBBBF") + QString("中文").toUtf8();
    bool ok = false;
    QCOMPARE(SubtitleExtractor::decodeText(bytes, &ok), QStringLiteral("中文"));
    QVERIFY(ok);
}

void TestSubtitleExtractor::decodeUtf8Plain() {
    bool ok = false;
    QCOMPARE(SubtitleExtractor::decodeText(QString("plain").toUtf8(), &ok),
             QStringLiteral("plain"));
    QVERIFY(ok);
}

void TestSubtitleExtractor::decodeUtf16LeBom() {
    const QString text = QStringLiteral("副标题 text");
    QByteArray bytes =
        QByteArray::fromHex("FFFE") + QStringEncoder(QStringConverter::Utf16LE).encode(text);
    bool ok = false;
    QCOMPARE(SubtitleExtractor::decodeText(bytes, &ok), text);
    QVERIFY(ok);
}

void TestSubtitleExtractor::decodeUtf16BeBom() {
    const QString text = QStringLiteral("副标题 text");
    QByteArray bytes =
        QByteArray::fromHex("FEFF") + QStringEncoder(QStringConverter::Utf16BE).encode(text);
    bool ok = false;
    QCOMPARE(SubtitleExtractor::decodeText(bytes, &ok), text);
    QVERIFY(ok);
}

void TestSubtitleExtractor::decodeInvalidUtf8FallsBackToLocale() {
    // 非法 UTF-8 回落系统本地编码：不产出半截失败，ok 恒真（回落链无失败出口）
    const QByteArray bytes = QByteArray::fromHex("B0B1"); // GB18030 双字节（"啊"）
    bool ok = false;
    const QString text = SubtitleExtractor::decodeText(bytes, &ok);
    QVERIFY(ok);
    QVERIFY(!text.isEmpty());
}

void TestSubtitleExtractor::browsableTextSubtitleCriteria() {
    MpvController::TrackInfo track;
    track.type = QStringLiteral("sub");
    track.mainSelection = 0;
    track.codec = QStringLiteral("subrip");
    QVERIFY(SubtitleExtractor::isBrowsableTextSubtitle(track));

    track.mainSelection = 1; // 第二字幕不在浏览范围
    QVERIFY(!SubtitleExtractor::isBrowsableTextSubtitle(track));
    track.mainSelection = 0;

    track.image = true; // 位图轨（image 字段）
    QVERIFY(!SubtitleExtractor::isBrowsableTextSubtitle(track));
    track.image = false;

    track.codec = QStringLiteral("dvd_subtitle"); // select.lua 黑名单兜底
    QVERIFY(!SubtitleExtractor::isBrowsableTextSubtitle(track));
    track.codec = QStringLiteral("hdmv_pgs_subtitle");
    QVERIFY(!SubtitleExtractor::isBrowsableTextSubtitle(track));
    track.codec = QStringLiteral("dvb_subtitle"); // 黑名单外的位图格式由 image 字段挡
    QVERIFY(SubtitleExtractor::isBrowsableTextSubtitle(track));

    track.type = QStringLiteral("audio");
    track.codec = QStringLiteral("subrip");
    QVERIFY(!SubtitleExtractor::isBrowsableTextSubtitle(track));
}

// —— 以下取数请求的判定矩阵，一律经 start() 的同步发射面 ——

void TestSubtitleExtractor::transitWithoutResolverIsSourceUnavailable() {
    const QString uri =
        StreamTransit::mint(QUuid::createUuid(), QStringLiteral("rclonecrypt:/film.mkv"));
    SubtitleExtractor extractor;
    extractor.setFfmpegPath(QStringLiteral("D:/no/such/ffmpeg.exe"));
    SyncResult result;
    result.bind(&extractor);

    SubtitleRequestOptions options;
    options.version.mtimeSecs = 1700000000;
    options.version.bytes = 12345678;
    extractor.start(textTrack(), uri, options); // 没有"列表身份 → 端点"这条通道

    QVERIFY(result.failedSeen);
    QVERIFY2(result.reason == SubtitleExtractor::Failure::SourceUnavailable,
             failureName(result.reason));
}

// 转接形态在、身份栏却解不出 uuid：这属于"这个来源取不了数"，不是"读不了本地文件"
void TestSubtitleExtractor::malformedTransitUriIsSourceUnavailable() {
    const QString malformed = QStringLiteral("oxbow://not-a-uuid/film.mkv");
    QVERIFY(StreamTransit::isTransitUri(malformed));

    SubtitleExtractor extractor;
    extractor.setFfmpegPath(QStringLiteral("D:/no/such/ffmpeg.exe"));
    SyncResult result;
    result.bind(&extractor);
    extractor.start(textTrack(), malformed, SubtitleRequestOptions{});

    QVERIFY2(result.reason == SubtitleExtractor::Failure::SourceUnavailable,
             failureName(result.reason));
}

void TestSubtitleExtractor::externalSubtitleIgnoresMediaLocality() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString srt = writePlainFile(dir, QStringLiteral("film.srt"), QByteArray(kSrtBody));
    QVERIFY(!srt.isEmpty());

    SubtitleExtractor extractor; // 直读路径不需要 ffmpeg，故意不设
    SyncResult result;
    result.bind(&extractor);

    auto track = textTrack(-1); // 外部轨缺席 ff-index 也不挡直读
    track.external = true;
    track.externalFilename = srt;
    extractor.start(track, QLatin1String(kRemoteMedia), SubtitleRequestOptions{});

    // 外部字幕只看自身：正播是 WebDAV 也照常出表（旧实现连带检查正播，判不可用）
    QVERIFY(result.finishedSeen);
    QVERIFY(!result.failedSeen);
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(formatEntry(result.entries.at(0)), QStringLiteral("[1,2)|Hello"));
}

void TestSubtitleExtractor::localTrackNeverEntersTransitChain() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = writePlainFile(dir, QStringLiteral("film.mkv"), QByteArray("dummy"));
    QVERIFY(!media.isEmpty());

    SubtitleExtractor extractor;
    bool resolverCalled = false;
    extractor.setEndpointResolver([&](const QUuid &listUuid) {
        Q_UNUSED(listUuid);
        resolverCalled = true;
        return std::shared_ptr<StreamEndpoint>();
    });
    extractor.setFfmpegPath(QStringLiteral("D:/no/such/ffmpeg.exe"));
    SyncResult result;
    result.bind(&extractor);

    extractor.start(textTrack(), media, SubtitleRequestOptions{});

    QVERIFY(!resolverCalled);
    QVERIFY(result.failedSeen);
    // 本地路径照旧走转取：起进程失败，而非来源不可用
    QVERIFY2(result.reason == SubtitleExtractor::Failure::FetchFailed, failureName(result.reason));
}

void TestSubtitleExtractor::transitTrackWithoutFfIndexIsFetchFailure() {
    const QString uri =
        StreamTransit::mint(QUuid::createUuid(), QStringLiteral("rclonecrypt:/film.mkv"));
    SubtitleExtractor extractor;
    extractor.setEndpointResolver([](const QUuid &) {
        return std::shared_ptr<StreamEndpoint>(); // 若判到这一步会落 SourceUnavailable
    });
    extractor.setFfmpegPath(QStringLiteral("D:/no/such/ffmpeg.exe"));
    SyncResult result;
    result.bind(&extractor);

    extractor.start(textTrack(-1), uri, SubtitleRequestOptions{}); // 不猜下标

    QVERIFY(result.failedSeen);
    QVERIFY2(result.reason == SubtitleExtractor::Failure::FetchFailed, failureName(result.reason));
}

void TestSubtitleExtractor::skipCacheDropsCachedEntry() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString srt = writePlainFile(dir, QStringLiteral("film.srt"), QByteArray(kSrtBody));
    QVERIFY(!srt.isEmpty());
    const QDateTime stamp = QFileInfo(srt).lastModified();

    SubtitleExtractor extractor;
    auto track = textTrack(-1);
    track.external = true;
    track.externalFilename = srt;
    const QString media = QLatin1String(kRemoteMedia);

    SyncResult first;
    first.bind(&extractor);
    extractor.start(track, media, SubtitleRequestOptions{});
    QVERIFY(first.finishedSeen);
    QCOMPARE(formatEntry(first.entries.at(0)), QStringLiteral("[1,2)|Hello"));

    // 同长度改写 + 时间戳复位：键（外部文件 + 轨 + mtime + size）保持不变，
    // 于是"命中缓存"与"重读源文件"在结果上可分
    QVERIFY(QFile::remove(srt));
    QVERIFY(!writePlainFile(dir, QStringLiteral("film.srt"),
                            QByteArray("1\n00:00:01,000 --> 00:00:02,000\nWorld\n"))
                 .isEmpty());
    QFile timed(srt); // 复位改写时间，键的 mtime 位与首取一致（setFileTime 要开着的文件）
    QVERIFY(timed.open(QIODevice::ReadWrite));
    QVERIFY(timed.setFileTime(stamp, QFileDevice::FileModificationTime));
    timed.close();

    SyncResult reuse;
    reuse.bind(&extractor);
    extractor.start(track, media, SubtitleRequestOptions{}); // 命中陈旧缓存
    QVERIFY(reuse.finishedSeen);
    QCOMPARE(formatEntry(reuse.entries.at(0)), QStringLiteral("[1,2)|Hello"));

    SyncResult reload;
    reload.bind(&extractor);
    SubtitleRequestOptions options;
    options.skipCache = true;
    extractor.start(track, media, options); // 刷新先丢同键缓存再完整重取
    QVERIFY(reload.finishedSeen);
    QCOMPARE(formatEntry(reload.entries.at(0)), QStringLiteral("[1,2)|World"));
}

QTEST_GUILESS_MAIN(TestSubtitleExtractor)
#include "tst_SubtitleExtractor.moc"

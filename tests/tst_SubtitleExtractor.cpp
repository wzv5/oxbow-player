#include "pch.h"

#include <QtTest/QtTest>

#include "SubtitleExtractor.h"

// N143：字幕浏览器取数模块的解析与判据契约测试——srt 解析（时间行 / 行号 / 清洗 /
// 排序 / 末块无上界）、编码判别、文本字幕轨判据、ffmpeg 路径选择。全部纯函数断言，
// 无 GUI、无网络、不起进程。

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
    void resolveFfmpegPathPrefersExplicit();
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

void TestSubtitleExtractor::resolveFfmpegPathPrefersExplicit() {
    // ini 显式值原样优先（含空白修剪）；候选扫描依赖磁盘与 PATH，不在单测断言之列
    QCOMPARE(SubtitleExtractor::resolveFfmpegPath(QStringLiteral(" D:/tools/ffmpeg.exe "),
                                                  QStringLiteral("/cfg")),
             QStringLiteral("D:/tools/ffmpeg.exe"));
}

QTEST_GUILESS_MAIN(TestSubtitleExtractor)
#include "tst_SubtitleExtractor.moc"

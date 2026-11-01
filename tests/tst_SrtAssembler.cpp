#include "pch.h"

#include <QtTest/QtTest>

#include "ai/SrtAssembler.h"

// N146：SrtAssembler 的流式契约测试——半行与半块（含 UTF-8 断字）、CRLF 与末尾
// 无换行、BOM、毫秒分隔两种、畸形块丢弃不影响后续、终点缺失的回填与渲染、
// 渲染序号从 1 递增与时间格式；whisper 行（单行一段、无空行封块）即到即收、
// 行到达封掉挂着的 srt 块、空文本条目（含只剩时间戳的 whisper 行）不入表且
// 不丢回填。全部经 feed / finish 的纯内存面取证。

class TestSrtAssembler : public QObject {
    Q_OBJECT

  private slots:
    void halfLineAndHalfBlockHold();
    void utf8SplitAcrossFeeds();
    void crlfAndMissingTrailingNewline();
    void leadingBomStripped();
    void bothMillisecondSeparators();
    void malformedBlockDropped();
    void missingEndBackfilledByNextBlock();
    void renderSrtNumberingAndClock();
    void whisperLinesStreamIncrementally();
    void whisperLineClosesOpenSrtBlock();
    void whisperEmptyTextLinesSkipped();
    void clearResetsAllState();
};

void TestSrtAssembler::halfLineAndHalfBlockHold() {
    SrtAssembler assembler;
    // 中断点落在首块时间行中间：喂一半不得出块，补齐后第一块只出一块
    assembler.feed("1\n00:00:01,000 --> 00:");
    QVERIFY(assembler.entries().isEmpty());
    assembler.feed("00:02,000\nhello\n\n"
                   "2\n00:00:03,000 --> 00:00:04,000\nworld\n\n");
    QCOMPARE(assembler.entries().size(), 2);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("hello"));
    QCOMPARE(assembler.entries().at(1).text, QStringLiteral("world"));
}

void TestSrtAssembler::utf8SplitAcrossFeeds() {
    SrtAssembler assembler;
    const QByteArray line =
        QString::fromUtf8("1\n00:00:01,000 --> 00:00:02,000\n你好\n\n").toUtf8();
    // 断点落在"你"的多字节序列中间：半字尾留缓冲，不得出乱码块
    const QByteArray head = line.left(line.indexOf("你") + 1);
    assembler.feed(head);
    QVERIFY(assembler.entries().isEmpty());
    assembler.feed(line.mid(head.size()));
    QCOMPARE(assembler.entries().size(), 1);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("你好"));
}

void TestSrtAssembler::crlfAndMissingTrailingNewline() {
    SrtAssembler assembler;
    assembler.feed("1\r\n00:00:01,000 --> 00:00:02,000\r\nhello\r\n\r\n"
                   "2\r\n00:00:03,000 --> 00:00:04,000\r\nworld");
    assembler.finish(); // 末块无空行封口：残余尾巴再尝试一次
    QCOMPARE(assembler.entries().size(), 2);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("hello"));
    QCOMPARE(assembler.entries().at(1).text, QStringLiteral("world"));
}

void TestSrtAssembler::leadingBomStripped() {
    SrtAssembler assembler;
    assembler.feed("\xEF\xBB\xBF"
                   "1\n00:00:01,000 --> 00:00:02,000\nhello\n\n");
    QCOMPARE(assembler.entries().size(), 1);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("hello"));
}

void TestSrtAssembler::bothMillisecondSeparators() {
    SrtAssembler assembler;
    assembler.feed("1\n00:00:01,000 --> 00:00:02,500\ncomma\n\n"
                   "2\n00:00:03.000 --> 00:00:04.500\ndot\n\n");
    QCOMPARE(assembler.entries().size(), 2);
    QCOMPARE(assembler.entries().at(0).endSeconds, 2.5);
    QCOMPARE(assembler.entries().at(1).endSeconds, 4.5);
}

void TestSrtAssembler::malformedBlockDropped() {
    SrtAssembler assembler;
    assembler.feed("1\n00:00:01,000 --> 00:00:02,000\nfine\n\n"
                   "no time line here\n\n"
                   "3\nbogus --> times\n\n"
                   "4\n00:00:05,000 --> 00:00:06,000\nafter\n\n");
    QCOMPARE(assembler.entries().size(), 2);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("fine"));
    QCOMPARE(assembler.entries().at(1).text, QStringLiteral("after"));
    // 畸形块（end 早于 start）同样整块丢
    assembler.clear();
    assembler.feed("1\n00:00:05,000 --> 00:00:01,000\nreversed\n\n"
                   "2\n00:00:07,000 --> 00:00:08,000\nok\n\n");
    QCOMPARE(assembler.entries().size(), 1);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("ok"));
}

void TestSrtAssembler::missingEndBackfilledByNextBlock() {
    // 终点缺失 = 时间行只到 -->（N143 正则箭头必配、终点可缺）
    SrtAssembler assembler;
    assembler.feed("1\n00:00:01,000 -->\nopen end\n\n"
                   "2\n00:00:03,000 --> 00:00:04,000\nclosed\n\n");
    QCOMPARE(assembler.entries().size(), 2);
    QVERIFY(!assembler.entries().at(0).endMissing);
    QCOMPARE(assembler.entries().at(0).endSeconds, 3.0); // 回落下一块起点
    // 末块缺失：保持标记，渲染以起点作终点
    assembler.clear();
    assembler.feed("1\n00:00:01,000 -->\ntail\n\n");
    assembler.finish();
    QVERIFY(assembler.entries().at(0).endMissing);
    QCOMPARE(assembler.entries().at(0).endSeconds, 1.0);
    const QString rendered = SrtAssembler::renderSrt(assembler.entries());
    QVERIFY(rendered.contains(QStringLiteral("00:00:01,000 --> 00:00:01,000")));
}

void TestSrtAssembler::renderSrtNumberingAndClock() {
    QList<SrtAssembler::Entry> entries;
    SrtAssembler::Entry first;
    first.startSeconds = 3661.0; // 01:01:01
    first.endSeconds = 3661.5;
    first.text = QStringLiteral("one");
    entries.append(first);
    SrtAssembler::Entry second;
    second.startSeconds = 2.0;
    second.endSeconds = 3.25;
    second.text = QStringLiteral("two");
    entries.append(second);

    // 序号从 1 递增、时间一律 HH:MM:SS,mmm、块间以空行分隔
    const QString rendered = SrtAssembler::renderSrt(entries);
    QCOMPARE(rendered, QStringLiteral("1\n01:01:01,000 --> 01:01:01,500\none\n\n"
                                      "2\n00:00:02,000 --> 00:00:03,250\ntwo\n\n"));
}

void TestSrtAssembler::whisperLinesStreamIncrementally() {
    SrtAssembler assembler;
    // 单行一段、无空行封块：每喂一行即出一条（whisper-cli 的默认 stdout 形态）
    assembler.feed("[00:00:00.000 --> 00:00:02.100]  hello\n");
    QCOMPARE(assembler.entries().size(), 1);
    QCOMPARE(assembler.entries().at(0).startSeconds, 0.0);
    QCOMPARE(assembler.entries().at(0).endSeconds, 2.1);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("hello"));
    assembler.feed("[01:00:03.500 --> 01:00:05.000]  world\n");
    QCOMPARE(assembler.entries().size(), 2);
    QCOMPARE(assembler.entries().at(1).startSeconds, 3603.5);
    QCOMPARE(assembler.entries().at(1).endSeconds, 3605.0);
    // 末行无换行：finish 的残行路径也收
    assembler.clear();
    assembler.feed("[00:10:00.000 --> 00:10:01.250]  tail");
    assembler.finish();
    QCOMPARE(assembler.entries().size(), 1);
    QCOMPARE(assembler.entries().at(0).endSeconds, 601.25);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("tail"));
}

void TestSrtAssembler::whisperLineClosesOpenSrtBlock() {
    SrtAssembler assembler;
    // srt 块未封口时 whisper 行到达：先封块解析入表，whisper 条目随后
    assembler.feed("1\n00:00:01,000 --> 00:00:02,000\nopen srt\n");
    QVERIFY(assembler.entries().isEmpty());
    assembler.feed("[00:00:03.000 --> 00:00:04.000]  from whisper\n");
    QCOMPARE(assembler.entries().size(), 2);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("open srt"));
    QCOMPARE(assembler.entries().at(1).text, QStringLiteral("from whisper"));
}

void TestSrtAssembler::whisperEmptyTextLinesSkipped() {
    SrtAssembler assembler;
    // -sns 抑制后只剩时间戳的行不入表
    assembler.feed("[00:00:00.000 --> 00:00:01.000]  \n"
                   "[00:00:01.000 --> 00:00:02.000]\n"
                   "[00:00:02.000 --> 00:00:03.000]  spoken\n");
    QCOMPARE(assembler.entries().size(), 1);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("spoken"));
    // srt 块的空文本条目同样不入表
    assembler.clear();
    assembler.feed("1\n00:00:01,000 --> 00:00:02,000\n   \n\n"
                   "2\n00:00:03,000 --> 00:00:04,000\nkept\n\n");
    QCOMPARE(assembler.entries().size(), 1);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("kept"));
    // 回填不受跳过影响：终点缺失的 srt 块由后续（含被跳过的）条目起点回填
    assembler.clear();
    assembler.feed("1\n00:00:01,000 -->\nopen\n\n"
                   "2\n00:00:05,000 --> 00:00:06,000\n   \n\n"
                   "3\n00:00:09,000 --> 00:00:10,000\nclosed\n\n");
    QCOMPARE(assembler.entries().size(), 2);
    QCOMPARE(assembler.entries().at(0).endSeconds, 5.0);
    QCOMPARE(assembler.entries().at(1).text, QStringLiteral("closed"));
}

void TestSrtAssembler::clearResetsAllState() {
    SrtAssembler assembler;
    assembler.feed("\xEF\xBB\xBF"
                   "1\n00:00:01,000 --> 00:00:02,000\nhello");
    assembler.clear();
    QVERIFY(assembler.entries().isEmpty());
    // 复位后 BOM 状态也复位：再喂带 BOM 的流仍被剥除
    assembler.feed("\xEF\xBB\xBF"
                   "1\n00:00:01,000 --> 00:00:02,000\nagain\n\n");
    QCOMPARE(assembler.entries().size(), 1);
    QCOMPARE(assembler.entries().at(0).text, QStringLiteral("again"));
}

QTEST_GUILESS_MAIN(TestSrtAssembler)
#include "tst_SrtAssembler.moc"

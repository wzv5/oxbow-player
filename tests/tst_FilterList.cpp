// N42：滤镜串编译 / 解析纯函数单元测试（FilterListWidget 静态成员）。
// 覆盖：编译（勾选位 / @label: 后插 ! / 手打 ! 归一 / 空条目丢弃）、解析
// （顶层逗号切分，[ ] 与 %n% 引号内逗号不切、只剥启用位、无损兜底）、
// 编译→解析→编译往返恒等、参数值引号选择（裸值 / [...] 配平 / %n% 不配平，
// 含非 ASCII 值的 UTF-8 字节长度口径）、顶层冒号与顶层逗号扫描。
// 无 GUI 依赖，一类一可执行（R5）：ctest --test-dir build -C Debug -R FilterList
// 跑本套件；oxbow_tests_FilterList <槽名> 跑单个用例。

#include "pch.h"

#include <QtTest/QtTest>

#include "FilterListWidget.h"

static FilterEntry fe(bool enabled, const QString &text) {
    FilterEntry e;
    e.enabled = enabled;
    e.text = text;
    return e;
}

class TestFilterList : public QObject {
    Q_OBJECT

  private slots:
    // 编译：勾选位、@label: 后插 !、手打 ! 归一、空条目丢弃、空列表得空串
    void compileEntriesBasics();
    // 解析：顶层逗号切分（[ ] 与 %n% 引号内逗号不切）、只剥启用位、无损兜底
    void parseStringSplitting();
    // 往返：编译 → 解析 → 编译 恒等（复制 / 粘贴整串的往返基础）
    void filterStringRoundTrip();
    // 参数值引号选择：裸值、[...] 配平包裹、%n% 不配平回退、值首 [ 必须包裹
    void quoteParamValueSelection();
    // 顶层冒号定位（@ 前缀 + 括号 / 引号内冒号不算）与顶层逗号检测
    void topLevelScanHelpers();
};

void TestFilterList::compileEntriesBasics() {
    // 基础编译：条目以逗号连接；勾选位 false → "!" 前缀
    QCOMPARE(FilterListWidget::compileEntries({fe(true, QStringLiteral("volume=6dB")),
                                               fe(false, QStringLiteral("dynaudnorm=f=250:g=15"))}),
             QStringLiteral("volume=6dB,!dynaudnorm=f=250:g=15"));
    // @label: 的 ! 写在标签之后（F8）
    QCOMPARE(FilterListWidget::compileEntries({fe(false, QStringLiteral("@ln:volume=6dB"))}),
             QStringLiteral("@ln:!volume=6dB"));
    // 手打 ! 归一：文本 ! 位上的 ! 与勾选位取或，只输出一个（不产生 !!name）
    QCOMPARE(FilterListWidget::compileEntries({fe(false, QStringLiteral("!bwdif"))}),
             QStringLiteral("!bwdif"));
    QCOMPARE(FilterListWidget::compileEntries({fe(false, QStringLiteral("@ln:!volume=6dB"))}),
             QStringLiteral("@ln:!volume=6dB"));
    QCOMPARE(FilterListWidget::compileEntries({fe(true, QStringLiteral("@ln:!volume=6dB"))}),
             QStringLiteral("@ln:!volume=6dB"));
    // 空条目 / 全空白条目丢弃（F5）；全空 → 空串；条目全文去首尾空白
    QCOMPARE(
        FilterListWidget::compileEntries({fe(true, QString()), fe(true, QStringLiteral("  "))}),
        QString());
    QCOMPARE(FilterListWidget::compileEntries({fe(true, QStringLiteral(" bwdif "))}),
             QStringLiteral("bwdif"));
}

void TestFilterList::parseStringSplitting() {
    // 顶层逗号切分；[ ] 内逗号不切
    const QList<FilterEntry> bracketed =
        FilterListWidget::parseString(QStringLiteral("loudnorm=I=-14,[a,b],volume=6"));
    QCOMPARE(bracketed.size(), 3);
    QCOMPARE(bracketed[1].text, QStringLiteral("[a,b]"));
    QVERIFY(bracketed[0].enabled && bracketed[1].enabled);
    // %n% 引号内逗号不切、不配平括号不参与深度计数。引号长度须与内容一致：
    // "a]b,c" 恰 5 字符 → %5%（声明过长会把分隔逗号一并吃掉，mpv 同语义）
    const QList<FilterEntry> pct =
        FilterListWidget::parseString(QStringLiteral("lavfi=%5%a]b,c,volume=6"));
    QCOMPARE(pct.size(), 2);
    QCOMPARE(pct[0].text, QStringLiteral("lavfi=%5%a]b,c"));
    QCOMPARE(pct[1].text, QStringLiteral("volume=6"));
    // 只剥启用位：! 在段首 / @label: 冒号后；其余位置的 ! 保留
    const QList<FilterEntry> bangs =
        FilterListWidget::parseString(QStringLiteral("!bwdif,@ln:!volume=6dB,keep!mark"));
    QVERIFY(!bangs[0].enabled);
    QCOMPARE(bangs[0].text, QStringLiteral("bwdif"));
    QVERIFY(!bangs[1].enabled);
    QCOMPARE(bangs[1].text, QStringLiteral("@ln:volume=6dB"));
    QVERIFY(bangs[2].enabled);
    QCOMPARE(bangs[2].text, QStringLiteral("keep!mark"));
    // 无损兜底：空段保留为空条目；不校验滤镜名、不解释参数
    const QList<FilterEntry> holes = FilterListWidget::parseString(QStringLiteral("a,,b"));
    QCOMPARE(holes.size(), 3);
    QCOMPARE(holes[1].text, QString());
}

void TestFilterList::filterStringRoundTrip() {
    const QList<FilterEntry> entries = {fe(true, QStringLiteral("loudnorm=I=-14:TP=-1:LRA=11")),
                                        fe(false, QStringLiteral("dynaudnorm=f=250:g=15")),
                                        fe(true, QStringLiteral("@ln:volume=6dB")),
                                        fe(true, QStringLiteral("lavfi=[a,b]"))};
    const QString compiled = FilterListWidget::compileEntries(entries);
    QCOMPARE(compiled, QStringLiteral("loudnorm=I=-14:TP=-1:LRA=11,!dynaudnorm=f=250:g=15,"
                                      "@ln:volume=6dB,lavfi=[a,b]"));
    // 编译 → 解析 → 编译 恒等（含 [ ] 内逗号不拆散）
    const QList<FilterEntry> reparsed = FilterListWidget::parseString(compiled);
    QCOMPARE(reparsed.size(), entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        QCOMPARE(reparsed[i].enabled, entries[i].enabled);
        QCOMPARE(reparsed[i].text, entries[i].text);
    }
    QCOMPARE(FilterListWidget::compileEntries(reparsed), compiled);
    // 含 %n% 引号内逗号的串同样往返一致（%5% = "a]b,c" 的字节长）
    const QString pct = QStringLiteral("lavfi=%5%a]b,c,volume=6");
    QCOMPARE(FilterListWidget::compileEntries(FilterListWidget::parseString(pct)), pct);
    // 非 ASCII 参数值：%n% 长度按 UTF-8 字节计（"字]幕,x" = 3+1+3+1+1 = 9 字节，
    // 而非 5 个字符），扫描侧按同口径换算——引号段恰在值尾收束，其后的分隔
    // 逗号仍被识别为条目边界（按字符跳过会把 volume=6 一并吞进引号段）
    const QString pctUtf8 = QStringLiteral("lavfi=%9%字]幕,x,volume=6");
    const QList<FilterEntry> u8 = FilterListWidget::parseString(pctUtf8);
    QCOMPARE(u8.size(), 2);
    QCOMPARE(u8[0].text, QStringLiteral("lavfi=%9%字]幕,x"));
    QCOMPARE(u8[1].text, QStringLiteral("volume=6"));
    QCOMPARE(FilterListWidget::compileEntries(u8), pctUtf8);
}

void TestFilterList::quoteParamValueSelection() {
    // 无特殊字符：裸写
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("6")), QStringLiteral("6"));
    // 中括号出现在值中部且无 , = : ——不需引号，裸写（mpv 只把值首的 [ 当引号）
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("split[a][b]")),
             QStringLiteral("split[a][b]"));
    // 含逗号 / 冒号：[...] 包裹
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("a,b")), QStringLiteral("[a,b]"));
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("x:y")), QStringLiteral("[x:y]"));
    // 首尾空白：引号包裹
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral(" v ")), QStringLiteral("[ v ]"));
    // 值首为 [：mpv 一律视作引号开启，裸写会被误读，必须包裹；配平 → [...]
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("[a][b]x")),
             QStringLiteral("[[a][b]x]"));
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("[a]b")), QStringLiteral("[[a]b]"));
    // 未触发包裹条件的值（无 , = : / 首尾空白 / 值首 [）：即使含孤立中括号也
    // 裸写——mpv 只把值首 [ 当引号，值中部的括号是字面量，裸写可直接往返
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("a]b")), QStringLiteral("a]b"));
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("a[b")), QStringLiteral("a[b"));
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("split[a][b]")),
             QStringLiteral("split[a][b]"));
    // 不配平仅在已触发包裹时决定形态：回退 %n%，长度按 UTF-8 字节计
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("a]b,c")),
             QStringLiteral("%5%a]b,c"));
    // 值首 [ 且不配平：同样回退 %n%
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("[a]b]")),
             QStringLiteral("%5%[a]b]"));
    // 非 ASCII：长度按 UTF-8 字节计（"字]幕,x" 为 9 字节、5 个字符）
    QCOMPARE(FilterListWidget::quoteParamValue(QStringLiteral("字]幕,x")),
             QStringLiteral("%9%字]幕,x"));
}

void TestFilterList::topLevelScanHelpers() {
    // 顶层逗号检测：值内逗号（括号 / 引号保护）不算
    QVERIFY(FilterListWidget::hasTopLevelComma(QStringLiteral("a,b")));
    QVERIFY(!FilterListWidget::hasTopLevelComma(QStringLiteral("lavfi=[a,b]")));
    QVERIFY(!FilterListWidget::hasTopLevelComma(QStringLiteral("lavfi=%3%a,b")));
    QVERIFY(!FilterListWidget::hasTopLevelComma(QStringLiteral("plain")));
    QVERIFY(FilterListWidget::hasTopLevelComma(QStringLiteral("x,[a]")));
}

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN）
QTEST_GUILESS_MAIN(TestFilterList)

#include "tst_FilterList.moc"

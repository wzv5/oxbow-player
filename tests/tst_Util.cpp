// Util::segmentName / naturalCompare 单元测试：QStringView 零拷贝分段重构的验证。
// 覆盖：分段正确性（文本/数值/前导零/ASCII 判定/超限降级）、排序语义（数值序/
// 跨类型/前缀/大小写不敏感）、视图有效性（拥有者+移动后仍有效）、调用方整链排序。
// 无 GUI 依赖，一类一可执行（R5）：ctest --test-dir build -C Debug -R Util 跑本套件；
//   mpvqt_tests_Util <槽名> 跑单个用例。

#include "pch.h"

#include <QtTest/QtTest>

#include "Util.h"

// 中文片段统一经 fromUtf8 构造：窄字面量在 MSVC /utf-8 与 Clang 默认 UTF-8
// 源码编码下均为 UTF-8，比 QStringLiteral 更不受编译器字面量编码影响
static QString epName(int season, int episode) {
    return QString::fromUtf8("第") + QString::number(season) + QString::fromUtf8("季第") +
           QString::number(episode) + QString::fromUtf8("集");
}

class TestUtil : public QObject {
    Q_OBJECT

  private slots:
    // 空串 → 无段；空与空比较为等键
    void emptyInputYieldsNoSegments();
    // 纯文本 → 单个文本段，视图内容与原串一致
    void pureTextIsSingleSegment();
    // 混合名切分：第 | 1 | 季第 | 2 | 集
    void mixedNameSplitsIntoSegments();
    // 前导零不敏感：01 与 1 同值，比较为等键
    void leadingZerosAreInsensitive();
    // 仅 ASCII 0-9 视为数字：全角数字不断开，整体为文本段
    void onlyAsciiDigitsSplit();
    // 超 qlonglong 的数字串降级为文本段；恰为最大值时仍是数值段
    void overflowDigitsDowngradeToText();
    // 数值按大小排：2 < 9 < 10，剧集多段名逐段正确
    void numericOrdering();
    // 跨类型：数值段恒小于文本段
    void numberAlwaysLessThanText();
    // 前缀较短者小；空序列最小
    void shorterPrefixIsSmaller();
    // 文本段大小写不敏感
    void textCompareIsCaseInsensitive();
    // 视图引用拥有者：storage 与输入一致，各文本视图为对应子串
    void viewsReferenceOwnerStorage();
    // 临时量输入与搬移后视图依然有效（模拟调用方三元表达式 + QVector 持有）
    void temporariesAndMovesKeepViewsValid();
    // 整链：乱序剧集表经 stable_sort + naturalCompare（新签名）恢复数值序
    void endToEndCallerSort();
};

void TestUtil::emptyInputYieldsNoSegments() {
    const Util::SegmentedName out = Util::segmentName(QString());
    QVERIFY(out.segs.isEmpty());
    QVERIFY(out.storage.isEmpty());
    QCOMPARE(Util::naturalCompare(out, out), 0);
}

void TestUtil::pureTextIsSingleSegment() {
    const Util::SegmentedName out = Util::segmentName(QStringLiteral("abc"));
    QCOMPARE(out.segs.size(), 1);
    QVERIFY(!out.segs[0].isNumber);
    QCOMPARE(out.segs[0].text.toString(), QStringLiteral("abc"));
}

void TestUtil::mixedNameSplitsIntoSegments() {
    const Util::SegmentedName out = Util::segmentName(epName(1, 2));
    QCOMPARE(out.segs.size(), 5);
    QVERIFY(!out.segs[0].isNumber);
    QCOMPARE(out.segs[0].text.toString(), QString::fromUtf8("第"));
    QVERIFY(out.segs[1].isNumber);
    QCOMPARE(out.segs[1].number, 1);
    QVERIFY(!out.segs[2].isNumber);
    QCOMPARE(out.segs[2].text.toString(), QString::fromUtf8("季第"));
    QVERIFY(out.segs[3].isNumber);
    QCOMPARE(out.segs[3].number, 2);
    QVERIFY(!out.segs[4].isNumber);
    QCOMPARE(out.segs[4].text.toString(), QString::fromUtf8("集"));
}

void TestUtil::leadingZerosAreInsensitive() {
    const Util::SegmentedName a = Util::segmentName(QStringLiteral("01"));
    const Util::SegmentedName b = Util::segmentName(QStringLiteral("1"));
    QVERIFY(a.segs.size() == 1 && a.segs[0].isNumber);
    QVERIFY(b.segs.size() == 1 && b.segs[0].isNumber);
    QCOMPARE(a.segs[0].number, 1);
    QCOMPARE(Util::naturalCompare(a, b), 0); // 等键，交由稳定排序保序
}

void TestUtil::onlyAsciiDigitsSplit() {
    const Util::SegmentedName out =
        Util::segmentName(QStringLiteral("ab") + QString::fromUtf8("１２") + QStringLiteral("cd"));
    QCOMPARE(out.segs.size(), 1); // 全角数字不断开
    QVERIFY(!out.segs[0].isNumber);
    QCOMPARE(out.segs[0].text.toString(),
             QStringLiteral("ab") + QString::fromUtf8("１２") + QStringLiteral("cd"));
}

void TestUtil::overflowDigitsDowngradeToText() {
    // 20 位全 9：超出 qlonglong 上限，降级为文本段
    const Util::SegmentedName big = Util::segmentName(QStringLiteral("file99999999999999999999"));
    QCOMPARE(big.segs.size(), 2);
    QVERIFY(!big.segs[0].isNumber);
    QCOMPARE(big.segs[0].text.toString(), QStringLiteral("file"));
    QVERIFY(!big.segs[1].isNumber);
    QCOMPARE(big.segs[1].text.toString(), QStringLiteral("99999999999999999999"));
    // 恰为上限 9223372036854775807：仍是数值段
    const Util::SegmentedName max = Util::segmentName(QStringLiteral("9223372036854775807"));
    QCOMPARE(max.segs.size(), 1);
    QVERIFY(max.segs[0].isNumber);
    QCOMPARE(max.segs[0].number, (std::numeric_limits<qlonglong>::max)());
}

void TestUtil::numericOrdering() {
    const Util::SegmentedName n2 = Util::segmentName(QStringLiteral("2"));
    const Util::SegmentedName n9 = Util::segmentName(QStringLiteral("9"));
    const Util::SegmentedName n10 = Util::segmentName(QStringLiteral("10"));
    QVERIFY(Util::naturalCompare(n2, n10) < 0);
    QVERIFY(Util::naturalCompare(n10, n2) > 0);
    QVERIFY(Util::naturalCompare(n9, n10) < 0);

    const Util::SegmentedName e2 = Util::segmentName(epName(1, 2));
    const Util::SegmentedName e9 = Util::segmentName(epName(1, 9));
    const Util::SegmentedName e10 = Util::segmentName(epName(1, 10));
    const Util::SegmentedName s2e1 = Util::segmentName(epName(2, 1));
    const Util::SegmentedName s10e1 = Util::segmentName(epName(10, 1));
    QVERIFY(Util::naturalCompare(e2, e9) < 0);
    QVERIFY(Util::naturalCompare(e9, e10) < 0);
    QVERIFY(Util::naturalCompare(e10, s2e1) < 0);
    QVERIFY(Util::naturalCompare(s2e1, s10e1) < 0);
    QVERIFY(Util::naturalCompare(e2, e2) == 0);
}

void TestUtil::numberAlwaysLessThanText() {
    const Util::SegmentedName num = Util::segmentName(QStringLiteral("12a"));
    const Util::SegmentedName txt = Util::segmentName(QStringLiteral("ab"));
    // 首段即跨类型：数值段 < 文本段
    QVERIFY(Util::naturalCompare(num, txt) < 0);
    QVERIFY(Util::naturalCompare(txt, num) > 0);
}

void TestUtil::shorterPrefixIsSmaller() {
    const Util::SegmentedName shortName = Util::segmentName(QStringLiteral("file"));
    const Util::SegmentedName longName = Util::segmentName(QStringLiteral("file1"));
    QVERIFY(Util::naturalCompare(shortName, longName) < 0);
    QVERIFY(Util::naturalCompare(longName, shortName) > 0);

    const Util::SegmentedName empty = Util::segmentName(QString());
    QVERIFY(Util::naturalCompare(empty, shortName) < 0);
    QVERIFY(Util::naturalCompare(shortName, empty) > 0);
}

void TestUtil::textCompareIsCaseInsensitive() {
    const Util::SegmentedName upper = Util::segmentName(QStringLiteral("ABC"));
    const Util::SegmentedName lower = Util::segmentName(QStringLiteral("abc"));
    QCOMPARE(Util::naturalCompare(upper, lower), 0);
}

void TestUtil::viewsReferenceOwnerStorage() {
    const QString input = epName(3, 12);
    const Util::SegmentedName out = Util::segmentName(input);
    QCOMPARE(out.storage, input); // 拥有者持有原串
    QCOMPARE(out.segs.size(), 5);
    // 各文本视图拼回即原串（数值段以十进制文本还原后亦然）
    QString rebuilt;
    for (const Util::NameSegment &seg : out.segs)
        rebuilt += seg.isNumber ? QString::number(seg.number) : seg.text.toString();
    QCOMPARE(rebuilt, input);
}

void TestUtil::temporariesAndMovesKeepViewsValid() {
    // 调用方空实体分支：右值临时量输入，产出空段且比较不崩
    const Util::SegmentedName empty = Util::segmentName(QString());
    QVERIFY(empty.segs.isEmpty());

    // 调用方持有模式：搬进 QVector 后视图仍有效（QString 移动不搬运堆数据块）
    QVector<Util::SegmentedName> segs;
    segs.resize(2);
    segs[0] = Util::segmentName(epName(1, 2));
    segs[1] = Util::segmentName(epName(1, 10));
    QVERIFY(Util::naturalCompare(segs[0], segs[1]) < 0);
    QCOMPARE(segs[0].segs[3].number, 2);
    QCOMPARE(segs[1].segs[3].number, 10);
    QCOMPARE(segs[1].segs[2].text.toString(), QString::fromUtf8("季第"));
}

void TestUtil::endToEndCallerSort() {
    // 与 PlaylistWidget::sortListItems 同形：预分段一次 + stable_sort + 新签名比较
    const QList<int> episodes{1, 2, 9, 10, 11, 20, 100};
    QStringList ascending;
    for (int s = 1; s <= 2; ++s)
        for (int e : episodes)
            ascending << epName(s, e);

    QStringList input = ascending;
    std::reverse(input.begin(), input.end()); // 确定性乱序：整体倒置

    QVector<Util::SegmentedName> segs(input.size());
    for (int i = 0; i < input.size(); ++i)
        segs[i] = Util::segmentName(input[i]);

    std::vector<int> order(size_t(input.size()));
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = int(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](int x, int y) { return Util::naturalCompare(segs[x], segs[y]) < 0; });

    QStringList result;
    for (int idx : order)
        result << input[idx];
    QCOMPARE(result, ascending);
}

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN）
QTEST_GUILESS_MAIN(TestUtil)

#include "tst_Util.moc"

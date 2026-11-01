// Util::segmentName / naturalCompare 单元测试：QStringView 零拷贝分段重构的验证。
// 覆盖：分段正确性（文本/数值/前导零/ASCII 判定/超限降级）、排序语义（数值序/
// 跨类型/前缀/大小写不敏感）、视图有效性（拥有者+移动后仍有效）、调用方整链排序。
// 无 GUI 依赖，一类一可执行（R5）：ctest --test-dir build -C Debug -R Util 跑本套件；
//   oxbow_tests_Util <槽名> 跑单个用例。

#include "pch.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "Util.h"
#include "streams/StreamTransit.h"

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
    // ——N37：Util::Url::key 网络分支（匹配形态收敛）——
    // 等价书写收敛为同一键：scheme/host 大小写、默认端口、unreserved 转义、hex 大小写
    void networkMatchKeyConvergesEquivalentForms();
    // reserved 转义不参与收敛：%2F 与 / 不得混同（语义不同）
    void networkMatchKeyKeepsReservedEscapes();
    // 健壮性：畸形转义原样保留、非网络串不崩
    void networkMatchKeyRobustness();
    // ——N37：Util::Url::fileName（播放项显示文件名唯一出口）——
    // 本地取末段；http(s) URL 剥 query / fragment 后取末段并 percent-decode
    void fileNameExtraction();
    // ——N37：Util::Url::toNative 三分支——本地转原生分隔符、http(s) 走 URL 规范化、
    // 其余网络协议原样返回
    void toNativeBranches();
    // ——N48：Util::Url::stripCredentials（凭据剥离纯函数，匹配 / 比对 / 展示共用）——
    // 仅 http(s) 生效、只删 authority 的 userinfo 段、其余字节逐字不动、幂等
    void stripCredentialsShapesInput();
    // ——N48：Util::Url::insertCredentials（剥离的插入侧对偶）——
    // 拼接插入、入参先剥（绝不产出双段）、凭据段空 / 非 http(s) / 无 authority 恒等
    void insertCredentialsInsertsOrLeavesAlone();
    // ——N48：Util::Url::pathEnd（显示名取末段与基 URL 尾斜杠补位共用的分界口径）——
    void pathEndFindsSpecBoundary();
    // ——N54：Util::Url::isMissingLocal（本地来源缺失判据，书签失效清理与书签跳转共用）——
    // 存在/不存在/目录/file:///网络与流/oxbow:// 折叠/相对名绝对化后判定
    void missingLocalJudgesByForm();
    // ——N54：nextSequenceNumber（播放列表与书签共用的自动命名算法）——
    // 最大序号 +1 而非第一个空号；前缀不匹配与序号段解析失败一律不计入
    void nextSequenceNumberTakesMaxPlusOne();
    // ——N61 §2.1：Util::Url::identityFromMpvEcho（mpv 回显整形）——
    // 已定形者逐字节原样（判据 R 的守门用例：转接串不折叠、正斜杠绝对串不反斜杠化）
    void mpvEchoKeepsDefiniteFormsByteIdentical();
    // 未定形者补形态：产出自满足工厂断言谓词；补不出形态者按实测产物钉住（由工厂的
    // 回显入口落占位实体，见 tst_PlaylistEntity）
    void mpvEchoShapesUndefinedItems();
    // ——N105 §3：Util::Url::toIdentity 的"补不出形态"族（静态表读取侧丢弃判据的前提）——
    // 无路径可折的两支 file: 与 file:// 归一产物为空串、非定形（与 N61 用例
    // mpvEchoShapesUndefinedItems 经 identityFromMpvEcho 所钉的同源行为互为印证；
    // 此处钉 §3 直接调用的 toIdentity 入口）。带根斜杠的 file:/ 不在此族——它折成
    // 盘根、判据放行，处置待裁决（见规格待裁决），故本用例不钉
    void toIdentityBareFileSchemeStaysIndefinite();
    // ——N124：仅精确 "-" 判真；整形对 "-" 原样返回，普通相对路径仍照旧补形态
    void mpvStdinCriteriaAndEchoShaping();
    // ——N97：Util::listFiles（目录一层的枚举，DirProvider 与视频区拖放共用）——
    // 命中清单者被收、子目录与其内部文件不进表、空清单不过滤、不存在的目录得空；
    // 判据钉在与"同 flags + NoSort 的 entryInfoList"逐项一致上（过滤位与排序位都未变）
    void listFilesEnumeratesOneLevel();
    // 扩展名大小写不敏感（守门用例）：目录展开走 QDir 名称过滤器，而拖放的单文件判据
    // 是 suffix().toLower() 比对小写清单——两支口径必须一致，否则"目录里的 1.MKV
    // 漏掉、单文件 1.MKV 却能播"。钉住大写后缀照样被收
    void listFilesIgnoresSuffixCase();
    // ——N131：Util::Url::splitEntryText（手输对话框与两处拖放共用的条目切分）——
    // 换行三种写法同为分隔符（';' 不算）、逐段去空白、空段丢弃、保序不去重、不做定形
    void splitEntryTextSplits();
    // ——N132：Util::Url::entriesFromUrls（两处拖放共用的 QUrl → 已定形串表）——
    // 本地折路径转分隔符、http 归一后保留原编码、其余协议原样、未定形者丢弃
    void entriesFromUrlsMixed();
    // ——N133：Util::Url::entriesFromDrop（两处拖放共用的来源出口）——
    // urls 侧照 entriesFromUrls；文本侧只收已定形串（file: 折成本地串再判）；
    // 转接形态两侧都丢；两者皆无得空表
    void entriesFromDropShapes();
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

// ——N37：Util::Url::key 网络分支——

void TestUtil::networkMatchKeyConvergesEquivalentForms() {
    // scheme / host 大小写归一
    QCOMPARE(Util::Url::key(QStringLiteral("HTTP://Host/a.mp4")),
             Util::Url::key(QStringLiteral("http://host/a.mp4")));
    // 默认端口去除（http 80 / https 443）；非默认端口保留
    QCOMPARE(Util::Url::key(QStringLiteral("http://host:80/a.mp4")),
             Util::Url::key(QStringLiteral("http://host/a.mp4")));
    QCOMPARE(Util::Url::key(QStringLiteral("https://host:443/a.mp4")),
             Util::Url::key(QStringLiteral("https://host/a.mp4")));
    QCOMPARE(Util::Url::key(QStringLiteral("http://host:8080/a.mp4")),
             Util::Url::key(QStringLiteral("http://host:8080/a.mp4")));
    // unreserved 字符（~）转义与否等价；转义 hex 大小写等价
    QCOMPARE(Util::Url::key(QStringLiteral("http://host/dav/%7E/u/a.mp4")),
             Util::Url::key(QStringLiteral("http://host/dav/~/u/a.mp4")));
    QCOMPARE(Util::Url::key(QStringLiteral("http://host/a%2fb.mp4")),
             Util::Url::key(QStringLiteral("http://host/a%2Fb.mp4")));
    // 组合形态：同一 URL 的多种书写收敛为同一键
    QCOMPARE(Util::Url::key(QStringLiteral("HTTPS://Server:443/Dav/%7Euser/%41.mp4")),
             Util::Url::key(QStringLiteral("https://server/Dav/~user/A.mp4")));
    // N48：userinfo 被剥除——键恒为无凭据形态，改密后既有记忆继续有效
    QCOMPARE(Util::Url::key(QStringLiteral("http://u:p@HOST/a.mp4")),
             QStringLiteral("http://host/a.mp4"));
    // 同资源不同账号收敛为同一键（身份即资源）
    QCOMPARE(Util::Url::key(QStringLiteral("http://a@host/x")),
             Util::Url::key(QStringLiteral("http://b:pw@host/x")));
}

void TestUtil::networkMatchKeyKeepsReservedEscapes() {
    // %2F 与 / 不得混同：reserved 转义不解码，解码即改变路径段语义
    const QString escaped = Util::Url::key(QStringLiteral("http://host/a%2Fb.mp4"));
    const QString literal = Util::Url::key(QStringLiteral("http://host/a/b.mp4"));
    QVERIFY(escaped != literal);
    // 非 unreserved 的转义一律保留（仅 hex 统一大写），不产生字面量
    QVERIFY(Util::Url::key(QStringLiteral("http://host/a%20b.mp4")) !=
            Util::Url::key(QStringLiteral("http://host/a b.mp4")));
}

void TestUtil::networkMatchKeyRobustness() {
    // 畸形转义（% 后不足两位 / 非 hex）原样保留，不崩、不误删
    QCOMPARE(Util::Url::key(QStringLiteral("http://host/a%2")), QStringLiteral("http://host/a%2"));
    QCOMPARE(Util::Url::key(QStringLiteral("http://host/a%zz")),
             QStringLiteral("http://host/a%zz"));
    // 非网络串走本地分支（Windows 转小写），恒不崩；空串恒空
    const QString local = Util::Url::key(QStringLiteral("C:\\dir\\a.mp4"));
    QVERIFY(!local.isEmpty());
    QVERIFY(Util::Url::key(QString()).isEmpty());
}

void TestUtil::fileNameExtraction() {
    // 本地路径：QFileInfo::fileName 口径（本轮行为不变）。喂串按本平台形态——POSIX 下
    // 反斜杠是文件名的合法字符，不得当分隔符切（判据随 QFileInfo 的平台口径）
#ifdef Q_OS_WIN
    QCOMPARE(Util::Url::fileName(QStringLiteral("C:\\dir\\a b.mp4")), QStringLiteral("a b.mp4"));
    QCOMPARE(Util::Url::fileName(QStringLiteral("C:\\dir\\noext")), QStringLiteral("noext"));
#else
    QCOMPARE(Util::Url::fileName(QStringLiteral("/dir/a b.mp4")), QStringLiteral("a b.mp4"));
    QCOMPARE(Util::Url::fileName(QStringLiteral("/dir/noext")), QStringLiteral("noext"));
#endif
    // URL：末段 percent-decode（列表显示名与标题 / OSD 同一口径）
    QCOMPARE(Util::Url::fileName(QStringLiteral("http://u:p@host/dav/a%20b.mp4")),
             QStringLiteral("a b.mp4"));
    // query / fragment 先剥再取末段（避免混入 "?token=xx"）
    QCOMPARE(Util::Url::fileName(QStringLiteral("http://host/dav/x.mkv?token=1%2F2")),
             QStringLiteral("x.mkv"));
    QCOMPARE(Util::Url::fileName(QStringLiteral("http://host/dav/%E4%B8%AD.mp4#frag")),
             QString::fromUtf8("中.mp4"));
    // 以 / 结尾：解后为空，调用方按"未知"处理；无路径且无尾斜杠时末段即
    // 主机名（N35 起 HttpEntity 显示名的既有口径，非空）
    QVERIFY(Util::Url::fileName(QStringLiteral("http://host/dav/")).isEmpty());
    QCOMPARE(Util::Url::fileName(QStringLiteral("http://host")), QStringLiteral("host"));
    // 解码仅限 http(s)：其余协议的地址里字面 %XX 不是编码，末段原样返回
    // （与 UnknownEntity 列表显示同形）
    QCOMPARE(Util::Url::fileName(QStringLiteral("rtsp://host/live/a%20b.mp4")),
             QStringLiteral("a%20b.mp4"));
    // 非 http(s) 网络键维持 N35 既有形态：分隔符归一、不转小写、不收敛
    QCOMPARE(Util::Url::key(QStringLiteral("rtsp://Host/live/a b.mp4")),
             QStringLiteral("rtsp://Host/live/a b.mp4"));
    QVERIFY(Util::Url::key(QStringLiteral("rtsp://Host/live/a.mp4")) !=
            Util::Url::key(QStringLiteral("rtsp://host/live/a.mp4")));
    // N148：转接串取尾段解码，转义集字符还原成明文（比对侧 cacheBaseName 反向不解码）
    QCOMPARE(Util::Url::fileName(QStringLiteral("oxbow://0a1b-2c/x%2Fy/a%3Fb.mkv")),
             QStringLiteral("a?b.mkv"));
    QCOMPARE(Util::Url::fileName(QStringLiteral("oxbow://0a1b-2c/x/a%25b.mkv")),
             QStringLiteral("a%b.mkv"));
    QCOMPARE(Util::Url::fileName(QStringLiteral("oxbow://0a1b-2c/x/%E4%B8%AD%E6%96%87.mkv")),
             QString::fromUtf8("中文.mkv"));
    // 无尾段的旧串解出整条身份（含目录）——已接受的退化面
    QCOMPARE(Util::Url::fileName(QStringLiteral("oxbow://0a1b-2c/x%2Fy%2Fold.mkv")),
             QStringLiteral("x/y/old.mkv"));
}

void TestUtil::toNativeBranches() {
    // 本地：转原生分隔符（既有行为）
    QCOMPARE(Util::Url::toNative(QStringLiteral("C:/dir/a.mp4")),
             QDir::toNativeSeparators(QStringLiteral("C:/dir/a.mp4")));
    // http(s)：QUrl 规范化 + 与匹配形态同内核的收敛（默认端口剥离、转义收敛）
    // ——"入口即规范"，等价书写不产出重复条目
    QCOMPARE(Util::Url::toNative(QStringLiteral("HTTP://Host/a%20b.mp4")),
             QStringLiteral("http://host/a%20b.mp4"));
    QCOMPARE(Util::Url::toNative(QStringLiteral("http://Host:80/dav/%41.mp4")),
             QStringLiteral("http://host/dav/A.mp4"));
    QCOMPARE(Util::Url::toNative(QStringLiteral("http://host/dav/%7Eu/%2fb")),
             QStringLiteral("http://host/dav/~u/%2Fb"));
    // 其余网络协议：原样返回（不被 toNativeSeparators 破坏，也不经 URL 重序列化）
    QCOMPARE(Util::Url::toNative(QStringLiteral("rtsp://host/live/a b.mp4")),
             QStringLiteral("rtsp://host/live/a b.mp4"));
}

// N48：凭据剥离纯函数。产出一旦经 QUrl 重序列化，除 userinfo 之外的段也可能变形，
// 而它要参与跨来源逐字节比对（实体身份串 / mpv 回显），故钉死"只删 userinfo 段、
// 其余字节逐字不动"与幂等
void TestUtil::stripCredentialsShapesInput() {
    // 常规：删 authority 的 userinfo 段（含尾随 '@'）
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("http://u:p@host/dav/a.mp4")),
             QStringLiteral("http://host/dav/a.mp4"));
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("https://u@host:8443/a.mp4")),
             QStringLiteral("https://host:8443/a.mp4"));
    // 无 userinfo / 空 authority / 无 "://" / 非 http(s)：恒等返回（绝不凭空写入空段）
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("http://host/a.mp4")),
             QStringLiteral("http://host/a.mp4"));
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("http://:@host/a.mp4")),
             QStringLiteral("http://host/a.mp4"));
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("C:/dir/a.mp4")),
             QStringLiteral("C:/dir/a.mp4"));
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("rtsp://u:p@host/live")),
             QStringLiteral("rtsp://u:p@host/live"));
    QCOMPARE(Util::Url::stripCredentials(QString()), QString());
    // path / query 里的 '@' 永不触及（authority 终止于首个 '/' / '?' / '#'）
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("http://u:p@host/a@b.mp4")),
             QStringLiteral("http://host/a@b.mp4"));
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("http://u:p@host/a.mp4?t=x@y")),
             QStringLiteral("http://host/a.mp4?t=x@y"));
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("http://u:p@host#f@g")),
             QStringLiteral("http://host#f@g"));
    // 其余字节逐字不动：编码形态（hex 大小写 / %20 / %2F）与 host 大小写原样保留
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("HTTP://u:p@Host/Dav/%2fa%20b")),
             QStringLiteral("HTTP://Host/Dav/%2fa%20b"));
    // authority 内多个 '@'：字面 '@' 是分隔符，取最后一个（userinfo 内的 '@' 必为编码）
    QCOMPARE(Util::Url::stripCredentials(QStringLiteral("http://u%40x:p@host/a.mp4")),
             QStringLiteral("http://host/a.mp4"));
    // 幂等：重复剥离结果不变（比较形态会在回显侧与本地侧各剥一次）
    const QString once = Util::Url::stripCredentials(QStringLiteral("http://u:p@host/a.mp4"));
    QCOMPARE(Util::Url::stripCredentials(once), once);
}

// N48：插入侧与剥离侧成对——插入是纯字符串拼接（其余字节逐字不动），故剥离即还原；
// 入参先剥，带 userinfo 的串绝不产出双段
void TestUtil::insertCredentialsInsertsOrLeavesAlone() {
    const QString clean = QStringLiteral("http://host/dav/a.mp4");
    QCOMPARE(Util::Url::insertCredentials(clean, QStringLiteral("u:pw")),
             QStringLiteral("http://u:pw@host/dav/a.mp4"));
    // 剥离即还原（往返不变式），且 path / query 里的 '@' 不受影响
    const QString withAt = QStringLiteral("http://host/dav/a@b.mp4?t=x@y");
    QCOMPARE(
        Util::Url::stripCredentials(Util::Url::insertCredentials(withAt, QStringLiteral("u:pw"))),
        withAt);
    // 入参先剥：已有 userinfo 的串不产出双段
    QCOMPARE(Util::Url::insertCredentials(QStringLiteral("http://old:cred@host/dav/a.mp4"),
                                          QStringLiteral("u:pw")),
             QStringLiteral("http://u:pw@host/dav/a.mp4"));
    // 凭据段为空：恒等（不产出空 userinfo 段）
    QCOMPARE(Util::Url::insertCredentials(clean, QString()), clean);
    // 非 http(s) / 无 "://" / authority 为空：恒等，绝不注入
    QCOMPARE(
        Util::Url::insertCredentials(QStringLiteral("rtsp://host/live"), QStringLiteral("u:pw")),
        QStringLiteral("rtsp://host/live"));
    QCOMPARE(Util::Url::insertCredentials(QStringLiteral("C:/dir/a.mp4"), QStringLiteral("u:pw")),
             QStringLiteral("C:/dir/a.mp4"));
    QCOMPARE(Util::Url::insertCredentials(QStringLiteral("http:///a.mp4"), QStringLiteral("u:pw")),
             QStringLiteral("http:///a.mp4"));
}

// N48：path 段与 query / fragment 的分界（首个 '?' 或 '#'；无则 size()）——纯字符串，
// 不去重、不解析，本地路径里的 '?' / '#' 由调用方负责不误用
void TestUtil::pathEndFindsSpecBoundary() {
    QCOMPARE(Util::Url::pathEnd(QStringLiteral("http://host/dav/a.mp4")), qsizetype(21));
    QCOMPARE(Util::Url::pathEnd(QStringLiteral("http://host/dav?token=1")), qsizetype(15));
    QCOMPARE(Util::Url::pathEnd(QStringLiteral("http://host/dav#frag")), qsizetype(15));
    // 两者并存取较小者（先出现者）
    QCOMPARE(Util::Url::pathEnd(QStringLiteral("http://host/dav#f?q")), qsizetype(15));
    QCOMPARE(Util::Url::pathEnd(QStringLiteral("http://host/dav?q#f")), qsizetype(15));
    // 空串 / 无分界：size()
    QCOMPARE(Util::Url::pathEnd(QString()), qsizetype(0));
    QCOMPARE(Util::Url::pathEnd(QStringLiteral("http://host/dav/")), qsizetype(16));
    // 分界即首字符
    QCOMPARE(Util::Url::pathEnd(QStringLiteral("?q")), qsizetype(0));
}

// N54：本地来源缺失判据（书签失效清理与书签跳转共用）。三件事各钉一刀：
// 网络/流恒假（不可判定 ≠ 缺失）、目录不算缺失（QFileInfo 而非 QFile 的存在性）、
// 相对名先绝对化再判定、file:// 与 oxbow:// 经 Util::Url::toIdentity 折叠后再判
void TestUtil::missingLocalJudgesByForm() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString here = dir.filePath(QStringLiteral("here.mkv"));
    {
        QFile f(here);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    const QString gone = dir.filePath(QStringLiteral("gone.mkv"));

    QVERIFY(!Util::Url::isMissingLocal(here));
    QVERIFY(Util::Url::isMissingLocal(gone));
    // 目录：QFile::exists 在 Windows 判假，会被误当失效——判据取 QFileInfo::exists，
    // 与 LocalFileEntity::exists() 同口径（mpv 能播目录，目录不是失效项）
    QVERIFY(!Util::Url::isMissingLocal(dir.path()));
    QVERIFY(!Util::Url::isMissingLocal(QString()));

    // 网络 / 流：保持原样、无从以文件存在性判定，一律假
    QVERIFY(!Util::Url::isMissingLocal(QStringLiteral("http://host/dav/a.mkv")));
    QVERIFY(!Util::Url::isMissingLocal(QStringLiteral("https://user:pw@host/a.mkv")));
    QVERIFY(!Util::Url::isMissingLocal(QStringLiteral("rtsp://host/stream")));
    // N52 折叠：转接 URI 先还原成条目身份（此处是 http URL）→ 仍判假
    QVERIFY(
        !Util::Url::isMissingLocal(QStringLiteral("oxbow://11111111-2222-3333-4444-555555555555/"
                                                  "http%3A%2F%2Fhost%2Fdav%2Fa.mkv")));

    // file:// 形态转本地后判定
    QVERIFY(!Util::Url::isMissingLocal(QUrl::fromLocalFile(here).toString()));
    QVERIFY(Util::Url::isMissingLocal(QUrl::fromLocalFile(gone).toString()));

    // 相对名：绝对化（基于进程 CWD 的纯字符串变换）后判定，不因"非绝对路径"早退
    QVERIFY(Util::Url::isMissingLocal(QStringLiteral("oxbow-absent-probe-file.mkv")));
}

// N54：自动命名的单点。钉住两件事——① 取"最大 +1"而不是"第一个空号"（1 与 3 并存时
// 出 4，不出 2；删掉中间一条也不复用其号）；② 只有"前缀 + 纯数字"才占号，译文名、
// 手改名、带尾巴的名一律跳过，不会把新号挤到不可预期的位置
void TestUtil::nextSequenceNumberTakesMaxPlusOne() {
    const QString p = QStringLiteral("Bookmark ");
    QCOMPARE(Util::nextSequenceNumber(p, {}), 1);
    QCOMPARE(Util::nextSequenceNumber(p, {QStringLiteral("Bookmark 1")}), 2);
    QCOMPARE(
        Util::nextSequenceNumber(p, {QStringLiteral("Bookmark 1"), QStringLiteral("Bookmark 3")}),
        4); // 关键用例：空号 2 不复用
    QCOMPARE(
        Util::nextSequenceNumber(p, {QStringLiteral("Bookmark 7"), QStringLiteral("Bookmark 2"),
                                     QStringLiteral("Bookmark 40")}),
        41); // 最大者胜出，与出现次序无关
    // 非本算法生出的名字不占号
    QCOMPARE(
        Util::nextSequenceNumber(p, {QStringLiteral("My intro"), QStringLiteral("Bookmark 3b"),
                                     QStringLiteral("Bookmark "), QStringLiteral("Bookmark -5"),
                                     QStringLiteral("Playlist 99")}),
        1);
    // 前导零按十进制解析（"007" → 7）
    QCOMPARE(Util::nextSequenceNumber(p, {QStringLiteral("Bookmark 007")}), 8);
    // 前缀区分大小写与逐字节：另一个前缀自成一号段（播放列表与书签各自独立计数）
    QCOMPARE(Util::nextSequenceNumber(QStringLiteral("Playlist "),
                                      {QStringLiteral("Playlist 5"), QStringLiteral("Bookmark 9")}),
             6);
}

// N61 §2.1：整形的另一侧——**已定形者逐字节不动**。这组用例是判据 R（N56 §2.5）的
// 守门人：任何"顺手归一一下"的改动都会让回刷判据把真失配看成一致——正斜杠绝对串被
// 转成反斜杠，或 oxbow:// 被折成条目身份（后者等于把转接形态与条目身份在判据上合并，
// 转接开关切换后界面永不收编）。实测 mpv 对绝对正斜杠下发原样回显，故该形态必须活着
void TestUtil::mpvEchoKeepsDefiniteFormsByteIdentical() {
    // 全平台同形的定形支：正斜杠绝对串（两平台 isAbsolute 均为真）、含 "://" 者
    // （网络 / 带凭据 / file URL / 转接协议——判据认协议头，不碰内容）、空串
    QStringList definite{
        QStringLiteral("/dir/test2.mp4"),
        QStringLiteral("http://host/dav/a.mkv"),      // 网络
        QStringLiteral("https://user:pw@host/a.mkv"), // 带凭据的网络串（整形不问凭据）
        QStringLiteral("file:///D:/dir/a.mkv"),       // file URL 绝对形态：含 "://" → 原样
        QStringLiteral("oxbow://11111111-2222-3333-4444-555555555555/http%3A%2F%2Fhost%2Fdav"),
        QString(), // 空串（回显条目无 filename 的异常形）
    };
#ifdef Q_OS_WIN
    // Windows 专属形态（盘符绝对 + 正 / 反斜杠、UNC、盘符相对——Qt 的 isAbsolute 判绝对
    // → 原样，§2.1 残留②）：POSIX 下这些串都不绝对，喂进去必然走绝对化支
    definite << QStringLiteral("D:/dir/test2.mp4") << QStringLiteral("D:\\dir\\test2.mp4")
             << QStringLiteral("\\\\server\\share\\a.mkv") << QStringLiteral("D:test2.mp4");
#endif
    for (const QString &s : definite)
        QCOMPARE(Util::Url::identityFromMpvEcho(s), s);
    // 幂等：判据侧下一轮回显仍走同一道整形，必须得同一形态（§2.2 同形链）
    for (const QString &s : definite) {
        const QString once = Util::Url::identityFromMpvEcho(s);
        QCOMPARE(Util::Url::identityFromMpvEcho(once), once);
    }
}

// N61 §2.1：未定形支补形态。整形产物必须能过 PlaylistEntity::fromPath 那句断言
// （即"崩溃不再"这条正面断言），且产物自身仍是已定形（故两侧共用一条判据不自反）
void TestUtil::mpvEchoShapesUndefinedItems() {
    const QString cwd = QDir::currentPath();
    // 期望值一律按进程 CWD 做纯字符串拼接（toIdentity 的既有语义，不访问文件系统）
    const auto nativeRel = [&cwd](const QString &rel) {
        return QDir::toNativeSeparators(QDir::cleanPath(cwd + QLatin1Char('/') + rel));
    };
    const QString absHere = nativeRel(QStringLiteral("test2.mp4"));
    const QString absSub = nativeRel(QStringLiteral("sub/test2.mp4"));
    const QString absSibling = nativeRel(QStringLiteral("../sibling/test2.mp4"));
    // 「file: 前缀 + 已绝对」支的期望产物：不经 CWD，折成本平台的绝对本地串
#ifdef Q_OS_WIN
    const QString absPlain = QDir::toNativeSeparators(QStringLiteral("D:/dir/test2.mp4"));
#else
    const QString absPlain = QStringLiteral("/dir/test2.mp4");
#endif

    // 纯相对、./ 与 ../
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("test2.mp4")), absHere);
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("./sub/test2.mp4")), absSub);
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("../sibling/test2.mp4")), absSibling);
    // file: 前缀的相对形态——实测 Qt 6.11 的 QUrl::toLocalFile() 对 file scheme 的
    // 相对路径原样返回相对段（不要求绝对、不返空），故各支都折到与无前言版同一份身份；
    // 已绝对的那两支不经 CWD，直接折成本地绝对串
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:test2.mp4")), absHere);
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:./sub/test2.mp4")), absSub);
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:../sibling/test2.mp4")),
             absSibling);
#ifdef Q_OS_WIN
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:D:/dir/test2.mp4")), absPlain);
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:/D:/dir/test2.mp4")), absPlain);
#else
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:/dir/test2.mp4")), absPlain);
    // POSIX 特有支：盘符串在本平台**不是**形态，按相对串经 CWD 绝对化（Windows 上同串
    // 走定形直返，见上一条 #ifdef 支）
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("D:/dir/test2.mp4")),
             nativeRel(QStringLiteral("D:/dir/test2.mp4")));
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:D:/dir/test2.mp4")),
             nativeRel(QStringLiteral("D:/dir/test2.mp4")));
#endif

    // 整形产物恒已定形（= 再进工厂不会撞断言）且再经同一道整形逐字节不动（§2.2 同形链）
    const QStringList shapedList{absHere, absSub, absSibling, absPlain};
    for (const QString &s : shapedList) {
        QVERIFY(QFileInfo(s).isAbsolute());
        QCOMPARE(Util::Url::identityFromMpvEcho(s), s);
    }

#ifdef Q_OS_WIN
    // UNC 经 file: 前缀仍保住双反斜杠头部（实测 toIdentity 的既有产物，不被绝对化抹平）
    // ——UNC 是 Windows 专属形态，POSIX 下整串是相对路径
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:\\\\server\\share\\a.mkv")),
             QStringLiteral("\\\\server\\share\\a.mkv"));
#endif
    // file: 形态带百分号转义时顺带解码一次（QUrl 固有行为，§2.2 已声明的让步范围内）
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:a%20b.mkv")),
             nativeRel(QStringLiteral("a b.mkv")));

    // 补不出形态的那一支：裸 file: 的空路径整形产出**空串**——不是进程 CWD 目录
    // （实测 QFileInfo("").absoluteFilePath() 返空串），与"回显条目 filename 为空"
    // 同形。整形只补形态、不兜形，这类串由工厂的回显入口 PlaylistEntity::fromMpvEcho
    // 落 UnknownEntity（N61 变更记录第 1 条），用例在 tst_PlaylistEntity
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("file:")), QString());
    QVERIFY(QFileInfo(QString()).absoluteFilePath().isEmpty());
    QVERIFY(!Util::Url::isDefiniteForm(Util::Url::identityFromMpvEcho(QStringLiteral("file:"))));
}

// N105 §3 第 3 步："归一产物仍非定形者才丢"的直接前提。无路径可折的空 path 形态
// （裸 file: 与 file://，INI 手写异常形）经 toIdentity 须落空串——空路径经
// toLocalFile 得空串、再由 QFileInfo("").absoluteFilePath() 返空串收口（本行实测
// 依据见上文 mpvEchoShapesUndefinedItems 的同源断言）。若 Qt 改动令 toLocalFile
// 对空 path 返回 "/" 一类根形态，归一会把它折成当前盘根、判据放行成盘根条目，
// 本用例即失败（届时重开该边界处置的裁决）
void TestUtil::toIdentityBareFileSchemeStaysIndefinite() {
    QCOMPARE(Util::Url::toIdentity(QStringLiteral("file:")), QString());
    QVERIFY(!Util::Url::isDefiniteForm(Util::Url::toIdentity(QStringLiteral("file:"))));
    QCOMPARE(Util::Url::toIdentity(QStringLiteral("file://")), QString());
    QVERIFY(!Util::Url::isDefiniteForm(Util::Url::toIdentity(QStringLiteral("file://"))));
}

// N124：判据只认精确的 "-"；整形出口对 "-" 原样返回，不殃及普通相对路径
void TestUtil::mpvStdinCriteriaAndEchoShaping() {
    QVERIFY(Util::Url::isMpvStdin(QStringLiteral("-")));
    QVERIFY(!Util::Url::isMpvStdin(QString()));
    QVERIFY(!Util::Url::isMpvStdin(QStringLiteral(" -")));
    QVERIFY(!Util::Url::isMpvStdin(QStringLiteral("- ")));
    QVERIFY(!Util::Url::isMpvStdin(QStringLiteral("./-")));
    QVERIFY(!Util::Url::isMpvStdin(QStringLiteral("--")));
    QVERIFY(!Util::Url::isMpvStdin(QStringLiteral("/dev/stdin")));
    QVERIFY(!Util::Url::isMpvStdin(QStringLiteral("fd://0")));
    QVERIFY(!Util::Url::isMpvStdin(QStringLiteral("fdclose://0")));

    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("-")), QStringLiteral("-"));
    QCOMPARE(Util::Url::identityFromMpvEcho(Util::Url::identityFromMpvEcho(QStringLiteral("-"))),
             QStringLiteral("-"));
    const QString absRel = QDir::toNativeSeparators(
        QFileInfo(QStringLiteral("still-relative.mkv")).absoluteFilePath());
    QCOMPARE(Util::Url::identityFromMpvEcho(QStringLiteral("still-relative.mkv")), absRel);
}

void TestUtil::listFilesEnumeratesOneLevel() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = root.path();
    // 根内两个命中（mkv / mp3）+ 一个不命中（txt）+ 一个子目录（内含命中文件）
    QVERIFY(QDir(dir).mkdir(QStringLiteral("sub")));
    const auto touch = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write("x") == 1;
    };
    QVERIFY(touch(dir + QStringLiteral("/b.mkv")));
    QVERIFY(touch(dir + QStringLiteral("/a.mp3")));
    QVERIFY(touch(dir + QStringLiteral("/c.txt")));
    QVERIFY(touch(dir + QStringLiteral("/sub/d.mkv")));

    const QStringList exts{QStringLiteral("mkv"), QStringLiteral("mp3")};
    // 与"直接按同 flags / NoSort 调 entryInfoList"逐项一致：过滤位、排序位都没变
    const QFileInfoList expect = QDir(dir).entryInfoList(
        Util::extensionPatterns(exts), QDir::Files | QDir::Readable, QDir::NoSort);
    const QFileInfoList got = Util::listFiles(dir, exts);
    QCOMPARE(got.size(), expect.size());
    for (qsizetype i = 0; i < got.size(); ++i)
        QCOMPARE(got[i].absoluteFilePath(), expect[i].absoluteFilePath());

    // 命中集合：根内两个命中文件，子目录与其内部文件都不进表（只一层）
    QStringList names;
    for (const QFileInfo &fi : got)
        names << fi.fileName();
    QCOMPARE(names.size(), 2);
    QVERIFY(names.contains(QStringLiteral("b.mkv")));
    QVERIFY(names.contains(QStringLiteral("a.mp3")));
    QVERIFY(!names.contains(QStringLiteral("c.txt")));
    QVERIFY(!names.contains(QStringLiteral("d.mkv")));
    QVERIFY(!names.contains(QStringLiteral("sub")));

    // 空清单即不过滤：根内三个文件全收，子目录仍不进表
    QStringList all;
    for (const QFileInfo &fi : Util::listFiles(dir, {}))
        all << fi.fileName();
    QCOMPARE(all.size(), 3);
    QVERIFY(all.contains(QStringLiteral("c.txt")));

    // 返回的是 QFileInfo：路径为绝对路径，供调用方 toNative 定形后造实体
    const QFileInfoList one = Util::listFiles(dir, {QStringLiteral("mkv")});
    QCOMPARE(one.size(), 1);
    QVERIFY(QFileInfo(one.first().absoluteFilePath()).isAbsolute());

    // 不存在的目录 → 空表（不抛、不崩）
    QVERIFY(Util::listFiles(dir + QStringLiteral("/nonexistent"), exts).isEmpty());
}

void TestUtil::listFilesIgnoresSuffixCase() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir = root.path();
    const auto touch = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write("x") == 1;
    };
    // 后缀全大写与混合大小写各一（Windows 上文件名保留书写形态，不归小写）
    QVERIFY(touch(dir + QStringLiteral("/UPPER.MKV")));
    QVERIFY(touch(dir + QStringLiteral("/Mixed.Mkv")));
    QVERIFY(touch(dir + QStringLiteral("/plain.mkv")));

    QStringList names;
    for (const QFileInfo &fi : Util::listFiles(dir, {QStringLiteral("mkv")}))
        names << fi.fileName();
    QCOMPARE(names.size(), 3);
    QVERIFY(names.contains(QStringLiteral("UPPER.MKV")));
    QVERIFY(names.contains(QStringLiteral("Mixed.Mkv")));

    // 清单侧恒小写由 DirProvider::parseExtensions 转小写保证；此处顺带钉住过滤器
    // 本身不敏感——大小写反过来的组合同样命中
    QCOMPARE(Util::listFiles(dir, {QStringLiteral("MKV")}).size(), 3);
    QCOMPARE(Util::listFiles(dir, {QStringLiteral("Mkv")}).size(), 3);
}

void TestUtil::splitEntryTextSplits() {
    // 期望值一律先落 QStringList 变量：QCOMPARE 是宏，实参里的花括号初始化列表带
    // 逗号会被当成多个实参（编译期报错，与被测代码无关）
    // 单条：既不带分隔符也不带换行
    const QStringList single{QStringLiteral("https://host/a.mp4")};
    QCOMPARE(Util::Url::splitEntryText(QStringLiteral("https://host/a.mp4")), single);

    // 换行是唯一的分隔符；逐段去首尾空白；空段与纯空白段一并丢掉
    const QStringList wantMixed{QStringLiteral("a.mkv"), QStringLiteral("https://host/b.mp4"),
                                QStringLiteral("C:/dir/c.mkv")};
    const QStringList mixed = Util::Url::splitEntryText(
        QStringLiteral("  a.mkv \n \t\n\n https://host/b.mp4  \n\n\n C:/dir/c.mkv \n"));
    QCOMPARE(mixed, wantMixed);

    // 三种换行写法等价（\r\n 不当两个分隔符）
    const QStringList wantCrlf{QStringLiteral("x.mkv"), QStringLiteral("y.mkv"),
                               QStringLiteral("z.mkv"), QStringLiteral("w.mkv")};
    const QStringList crlf =
        Util::Url::splitEntryText(QStringLiteral("x.mkv\r\ny.mkv\rz.mkv\nw.mkv"));
    QCOMPARE(crlf, wantCrlf);

    // 保序、不去重（重复条目由用户自己负责，与拖放 / 添加文件同）
    const QStringList wantDup{QStringLiteral("a"), QStringLiteral("a"), QStringLiteral("a")};
    QCOMPARE(Util::Url::splitEntryText(QStringLiteral("a\na\na")), wantDup);

    // 全空输入一律空表：空串、只含换行、只含空白
    QVERIFY(Util::Url::splitEntryText(QString()).isEmpty());
    QVERIFY(Util::Url::splitEntryText(QStringLiteral("\n\n  \n\t\n")).isEmpty());

    // ';' 不再是分隔符（多行框一行一条已无歧义，而 Windows 路径与 URL 的 query 合法含
    // 它）：含分号的串整条保留，不被动切成两条
    const QStringList wantSemicolon{QStringLiteral("https://host/a?x=1;y=2"),
                                    QStringLiteral("C:/dir/a ;b.mkv")};
    QCOMPARE(Util::Url::splitEntryText(QStringLiteral("https://host/a?x=1;y=2\nC:/dir/a ;b.mkv")),
             wantSemicolon);

    // 只切分、不定形：段内原样保留（含空格的本地路径也不被动过）
    const QStringList wantSpaced{QStringLiteral("dir/a b.mkv")};
    QCOMPARE(Util::Url::splitEntryText(QStringLiteral("dir/a b.mkv")), wantSpaced);
}

void TestUtil::entriesFromUrlsMixed() {
    // 本地：Qt 判 isLocalFile，折成本地路径并转原生分隔符（%20 解码为空格是本地文件名语义）
    const QStringList local{QDir::toNativeSeparators(QStringLiteral("C:/dir/a b.mp4"))};
    QCOMPARE(Util::Url::entriesFromUrls({QUrl(QStringLiteral("file:///C:/dir/a%20b.mp4"))}), local);

    // http：host 归小写、默认端口剥离，而 %20 必须保住（PrettyDecoded 会解出空格）
    const QStringList http{QStringLiteral("http://host/a%20b.mp4")};
    QCOMPARE(Util::Url::entriesFromUrls({QUrl(QStringLiteral("http://Host:80/a%20b.mp4"))}), http);

    // 其余协议：toNative 不再加工，但 QUrl 序列化这一层仍会归小写 scheme / host 并把
    // 空格编成 %20（实测如此；喂 mpv 要的就是编码后的串）
    const QStringList rtsp{QStringLiteral("rtsp://host/live/a%20b.mp4")};
    QCOMPARE(Util::Url::entriesFromUrls({QUrl(QStringLiteral("rtsp://Host/live/a b.mp4"))}), rtsp);

    // 未定形者丢：空 QUrl 与无 scheme 的相对 QUrl（未定形串进工厂会撞 Debug 断言）
    QVERIFY(Util::Url::entriesFromUrls({QUrl()}).isEmpty());
    QVERIFY(Util::Url::entriesFromUrls({QUrl(QStringLiteral("a.mkv"))}).isEmpty());

    // N133：转接形态一并丢（大小写不敏感）——外部来路的 oxbow:// 没有落点，留下来会
    // 撞 Playlist::playTargets 那句"转接项必带 cache"的 Debug 断言
    const QString transit =
        StreamTransit::mint(QUuid::createUuid(), QStringLiteral("https://host/a.mkv"));
    QVERIFY(StreamTransit::isTransitUri(transit));
    QVERIFY(Util::Url::entriesFromUrls({QUrl(transit)}).isEmpty());
    const QString upperScheme = QStringLiteral("OXBOW://") + transit.mid(8);
    QVERIFY(StreamTransit::isTransitUri(upperScheme));
    QVERIFY(Util::Url::entriesFromUrls({QUrl(upperScheme)}).isEmpty());

    // 原序、不去重；混合来源按入序交错
    const QStringList mixed{QDir::toNativeSeparators(QStringLiteral("C:/dir/x.mkv")),
                            QStringLiteral("https://host/y.mp4"),
                            QDir::toNativeSeparators(QStringLiteral("C:/dir/x.mkv"))};
    QCOMPARE(Util::Url::entriesFromUrls({QUrl(QStringLiteral("file:///C:/dir/x.mkv")),
                                         QUrl(QStringLiteral("https://host/y.mp4")),
                                         QUrl(QStringLiteral("file:///C:/dir/x.mkv"))}),
             mixed);
}

void TestUtil::entriesFromDropShapes() {
    const QString transit =
        StreamTransit::mint(QUuid::createUuid(), QStringLiteral("https://host/a.mkv"));

    // urls 侧：本地与链接照收，按原序
    QMimeData urlsMime;
    urlsMime.setUrls(
        {QUrl(QStringLiteral("file:///C:/dir/x.mkv")), QUrl(QStringLiteral("https://host/y.mp4"))});
    const QStringList want{QDir::toNativeSeparators(QStringLiteral("C:/dir/x.mkv")),
                           QStringLiteral("https://host/y.mp4")};
    QCOMPARE(Util::Url::entriesFromDrop(urlsMime), want);

    // 纯文本侧：只收已定形串（相对串与转接串都丢），按原序
    QMimeData textMime;
    textMime.setText(QStringLiteral("https://host/z.mp4\nnot-a-path\nD:/dir/w.mkv\n") + transit);
    const QStringList wantText{QStringLiteral("https://host/z.mp4"),
                               QDir::toNativeSeparators(QStringLiteral("D:/dir/w.mkv"))};
    QCOMPARE(Util::Url::entriesFromDrop(textMime), wantText);

    // file: 文本形态折成本地路径收下——判据认它（含 "://"），而 toNative 按本地串加工会
    // 把它毁成非定形态，那样的串落到工厂即撞 Debug 断言（N56 §2.6）；未编码的空格照样
    // 认（TolerantMode，与 toNative 的 http 支同口径）
    QMimeData fileMime;
    fileMime.setText(QStringLiteral("file:///C:/dir/a%20b.mp4\nfile:///C:/dir/d e.mp4"));
    const QStringList wantFile{QDir::toNativeSeparators(QStringLiteral("C:/dir/a b.mp4")),
                               QDir::toNativeSeparators(QStringLiteral("C:/dir/d e.mp4"))};
    QCOMPARE(Util::Url::entriesFromDrop(fileMime), wantFile);

    // urls 非空即不吃 text：浏览器拖链接同时给两种 mime，不该落下两份
    QMimeData bothMime;
    bothMime.setUrls({QUrl(QStringLiteral("https://host/y.mp4"))});
    bothMime.setText(transit);
    const QStringList wantBoth{QStringLiteral("https://host/y.mp4")};
    QCOMPARE(Util::Url::entriesFromDrop(bothMime), wantBoth);

    // 两者皆无 → 空表
    QMimeData emptyMime;
    QVERIFY(Util::Url::entriesFromDrop(emptyMime).isEmpty());
}

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN）
QTEST_GUILESS_MAIN(TestUtil)

#include "tst_Util.moc"

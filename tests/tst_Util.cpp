// Util::segmentName / naturalCompare 单元测试：QStringView 零拷贝分段重构的验证。
// 覆盖：分段正确性（文本/数值/前导零/ASCII 判定/超限降级）、排序语义（数值序/
// 跨类型/前缀/大小写不敏感）、视图有效性（拥有者+移动后仍有效）、调用方整链排序。
// 无 GUI 依赖，一类一可执行（R5）：ctest --test-dir build -C Debug -R Util 跑本套件；
//   mpvqt_tests_Util <槽名> 跑单个用例。

#include "pch.h"

#include <QTemporaryDir>
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
    // ——N37：normalizePathForMatch 网络分支（匹配形态收敛）——
    // 等价书写收敛为同一键：scheme/host 大小写、默认端口、unreserved 转义、hex 大小写
    void networkMatchKeyConvergesEquivalentForms();
    // reserved 转义不参与收敛：%2F 与 / 不得混同（语义不同）
    void networkMatchKeyKeepsReservedEscapes();
    // 健壮性：畸形转义原样保留、非网络串不崩
    void networkMatchKeyRobustness();
    // ——N37：playUrlFileName（播放项显示文件名唯一出口）——
    // 本地取末段；http(s) URL 剥 query / fragment 后取末段并 percent-decode
    void playUrlFileNameExtraction();
    // ——N37：nativePath 三分支——本地转原生分隔符、http(s) 走 URL 规范化、
    // 其余网络协议原样返回
    void nativePathBranches();
    // ——N48：stripUrlCredentials（凭据剥离纯函数，匹配 / 比对 / 展示 / 文案共用）——
    // 仅 http(s) 生效、只删 authority 的 userinfo 段、其余字节逐字不动、幂等
    void stripUrlCredentialsShapesInput();
    // ——N48：insertUrlCredentials（剥离的插入侧对偶）——
    // 拼接插入、入参先剥（绝不产出双段）、凭据段空 / 非 http(s) / 无 authority 恒等
    void insertUrlCredentialsInsertsOrLeavesAlone();
    // ——N48：urlPathEnd（显示名取末段与基 URL 尾斜杠补位共用的分界口径）——
    void urlPathEndFindsSpecBoundary();
    // ——N54：isMissingLocalSource（本地来源缺失判据，书签失效清理与书签跳转共用）——
    // 存在/不存在/目录/file:///网络与流/mpvqt:// 折叠/相对名绝对化后判定
    void missingLocalSourceJudgesByForm();
    // ——N54：nextSequenceNumber（播放列表与书签共用的自动命名算法）——
    // 最大序号 +1 而非第一个空号；前缀不匹配与序号段解析失败一律不计入
    void nextSequenceNumberTakesMaxPlusOne();
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

// ——N37：normalizePathForMatch 网络分支——

void TestUtil::networkMatchKeyConvergesEquivalentForms() {
    // scheme / host 大小写归一
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("HTTP://Host/a.mp4")),
             Util::normalizePathForMatch(QStringLiteral("http://host/a.mp4")));
    // 默认端口去除（http 80 / https 443）；非默认端口保留
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("http://host:80/a.mp4")),
             Util::normalizePathForMatch(QStringLiteral("http://host/a.mp4")));
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("https://host:443/a.mp4")),
             Util::normalizePathForMatch(QStringLiteral("https://host/a.mp4")));
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("http://host:8080/a.mp4")),
             Util::normalizePathForMatch(QStringLiteral("http://host:8080/a.mp4")));
    // unreserved 字符（~）转义与否等价；转义 hex 大小写等价
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("http://host/dav/%7E/u/a.mp4")),
             Util::normalizePathForMatch(QStringLiteral("http://host/dav/~/u/a.mp4")));
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("http://host/a%2fb.mp4")),
             Util::normalizePathForMatch(QStringLiteral("http://host/a%2Fb.mp4")));
    // 组合形态：同一 URL 的多种书写收敛为同一键
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("HTTPS://Server:443/Dav/%7Euser/%41.mp4")),
             Util::normalizePathForMatch(QStringLiteral("https://server/Dav/~user/A.mp4")));
    // N48：userinfo 被剥除——键恒为无凭据形态，改密后既有记忆继续有效
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("http://u:p@HOST/a.mp4")),
             QStringLiteral("http://host/a.mp4"));
    // 同资源不同账号收敛为同一键（身份即资源）
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("http://a@host/x")),
             Util::normalizePathForMatch(QStringLiteral("http://b:pw@host/x")));
}

void TestUtil::networkMatchKeyKeepsReservedEscapes() {
    // %2F 与 / 不得混同：reserved 转义不解码，解码即改变路径段语义
    const QString escaped = Util::normalizePathForMatch(QStringLiteral("http://host/a%2Fb.mp4"));
    const QString literal = Util::normalizePathForMatch(QStringLiteral("http://host/a/b.mp4"));
    QVERIFY(escaped != literal);
    // 非 unreserved 的转义一律保留（仅 hex 统一大写），不产生字面量
    QVERIFY(Util::normalizePathForMatch(QStringLiteral("http://host/a%20b.mp4")) !=
            Util::normalizePathForMatch(QStringLiteral("http://host/a b.mp4")));
}

void TestUtil::networkMatchKeyRobustness() {
    // 畸形转义（% 后不足两位 / 非 hex）原样保留，不崩、不误删
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("http://host/a%2")),
             QStringLiteral("http://host/a%2"));
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("http://host/a%zz")),
             QStringLiteral("http://host/a%zz"));
    // 非网络串走本地分支（Windows 转小写），恒不崩；空串恒空
    const QString local = Util::normalizePathForMatch(QStringLiteral("C:\\dir\\a.mp4"));
    QVERIFY(!local.isEmpty());
    QVERIFY(Util::normalizePathForMatch(QString()).isEmpty());
}

void TestUtil::playUrlFileNameExtraction() {
    // 本地路径：QFileInfo::fileName 口径（本轮行为不变）
    QCOMPARE(Util::playUrlFileName(QStringLiteral("C:\\dir\\a b.mp4")), QStringLiteral("a b.mp4"));
    QCOMPARE(Util::playUrlFileName(QStringLiteral("C:\\dir\\noext")), QStringLiteral("noext"));
    // URL：末段 percent-decode（列表显示名与标题 / OSD 同一口径）
    QCOMPARE(Util::playUrlFileName(QStringLiteral("http://u:p@host/dav/a%20b.mp4")),
             QStringLiteral("a b.mp4"));
    // query / fragment 先剥再取末段（避免混入 "?token=xx"）
    QCOMPARE(Util::playUrlFileName(QStringLiteral("http://host/dav/x.mkv?token=1%2F2")),
             QStringLiteral("x.mkv"));
    QCOMPARE(Util::playUrlFileName(QStringLiteral("http://host/dav/%E4%B8%AD.mp4#frag")),
             QString::fromUtf8("中.mp4"));
    // 以 / 结尾：解后为空，调用方按"未知"处理；无路径且无尾斜杠时末段即
    // 主机名（N35 起 HttpEntity 显示名的既有口径，非空）
    QVERIFY(Util::playUrlFileName(QStringLiteral("http://host/dav/")).isEmpty());
    QCOMPARE(Util::playUrlFileName(QStringLiteral("http://host")), QStringLiteral("host"));
    // 解码仅限 http(s)：其余协议的地址里字面 %XX 不是编码，末段原样返回
    // （与 UnknownEntity 列表显示同形）
    QCOMPARE(Util::playUrlFileName(QStringLiteral("rtsp://host/live/a%20b.mp4")),
             QStringLiteral("a%20b.mp4"));
    // 非 http(s) 网络键维持 N35 既有形态：分隔符归一、不转小写、不收敛
    QCOMPARE(Util::normalizePathForMatch(QStringLiteral("rtsp://Host/live/a b.mp4")),
             QStringLiteral("rtsp://Host/live/a b.mp4"));
    QVERIFY(Util::normalizePathForMatch(QStringLiteral("rtsp://Host/live/a.mp4")) !=
            Util::normalizePathForMatch(QStringLiteral("rtsp://host/live/a.mp4")));
}

void TestUtil::nativePathBranches() {
    // 本地：转原生分隔符（既有行为）
    QCOMPARE(Util::nativePath(QStringLiteral("C:/dir/a.mp4")),
             QDir::toNativeSeparators(QStringLiteral("C:/dir/a.mp4")));
    // http(s)：QUrl 规范化 + 与匹配形态同内核的收敛（默认端口剥离、转义收敛）
    // ——"入口即规范"，等价书写不产出重复条目
    QCOMPARE(Util::nativePath(QStringLiteral("HTTP://Host/a%20b.mp4")),
             QStringLiteral("http://host/a%20b.mp4"));
    QCOMPARE(Util::nativePath(QStringLiteral("http://Host:80/dav/%41.mp4")),
             QStringLiteral("http://host/dav/A.mp4"));
    QCOMPARE(Util::nativePath(QStringLiteral("http://host/dav/%7Eu/%2fb")),
             QStringLiteral("http://host/dav/~u/%2Fb"));
    // 其余网络协议：原样返回（不被 toNativeSeparators 破坏，也不经 URL 重序列化）
    QCOMPARE(Util::nativePath(QStringLiteral("rtsp://host/live/a b.mp4")),
             QStringLiteral("rtsp://host/live/a b.mp4"));
}

// N48：凭据剥离纯函数。产出一旦经 QUrl 重序列化，除 userinfo 之外的段也可能变形，
// 而它要参与跨来源逐字节比对（实体 playUrl / mpv 回显），故钉死"只删 userinfo 段、
// 其余字节逐字不动"与幂等
void TestUtil::stripUrlCredentialsShapesInput() {
    // 常规：删 authority 的 userinfo 段（含尾随 '@'）
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("http://u:p@host/dav/a.mp4")),
             QStringLiteral("http://host/dav/a.mp4"));
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("https://u@host:8443/a.mp4")),
             QStringLiteral("https://host:8443/a.mp4"));
    // 无 userinfo / 空 authority / 无 "://" / 非 http(s)：恒等返回（绝不凭空写入空段）
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("http://host/a.mp4")),
             QStringLiteral("http://host/a.mp4"));
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("http://:@host/a.mp4")),
             QStringLiteral("http://host/a.mp4"));
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("C:/dir/a.mp4")),
             QStringLiteral("C:/dir/a.mp4"));
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("rtsp://u:p@host/live")),
             QStringLiteral("rtsp://u:p@host/live"));
    QCOMPARE(Util::stripUrlCredentials(QString()), QString());
    // path / query 里的 '@' 永不触及（authority 终止于首个 '/' / '?' / '#'）
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("http://u:p@host/a@b.mp4")),
             QStringLiteral("http://host/a@b.mp4"));
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("http://u:p@host/a.mp4?t=x@y")),
             QStringLiteral("http://host/a.mp4?t=x@y"));
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("http://u:p@host#f@g")),
             QStringLiteral("http://host#f@g"));
    // 其余字节逐字不动：编码形态（hex 大小写 / %20 / %2F）与 host 大小写原样保留
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("HTTP://u:p@Host/Dav/%2fa%20b")),
             QStringLiteral("HTTP://Host/Dav/%2fa%20b"));
    // authority 内多个 '@'：字面 '@' 是分隔符，取最后一个（userinfo 内的 '@' 必为编码）
    QCOMPARE(Util::stripUrlCredentials(QStringLiteral("http://u%40x:p@host/a.mp4")),
             QStringLiteral("http://host/a.mp4"));
    // 幂等：重复剥离结果不变（比较形态会在回显侧与本地侧各剥一次）
    const QString once = Util::stripUrlCredentials(QStringLiteral("http://u:p@host/a.mp4"));
    QCOMPARE(Util::stripUrlCredentials(once), once);
}

// N48：插入侧与剥离侧成对——插入是纯字符串拼接（其余字节逐字不动），故剥离即还原；
// 入参先剥，带 userinfo 的串绝不产出双段
void TestUtil::insertUrlCredentialsInsertsOrLeavesAlone() {
    const QString clean = QStringLiteral("http://host/dav/a.mp4");
    QCOMPARE(Util::insertUrlCredentials(clean, QStringLiteral("u:pw")),
             QStringLiteral("http://u:pw@host/dav/a.mp4"));
    // 剥离即还原（往返不变式），且 path / query 里的 '@' 不受影响
    const QString withAt = QStringLiteral("http://host/dav/a@b.mp4?t=x@y");
    QCOMPARE(Util::stripUrlCredentials(Util::insertUrlCredentials(withAt, QStringLiteral("u:pw"))),
             withAt);
    // 入参先剥：已有 userinfo 的串不产出双段
    QCOMPARE(Util::insertUrlCredentials(QStringLiteral("http://old:cred@host/dav/a.mp4"),
                                        QStringLiteral("u:pw")),
             QStringLiteral("http://u:pw@host/dav/a.mp4"));
    // 凭据段为空：恒等（不产出空 userinfo 段）
    QCOMPARE(Util::insertUrlCredentials(clean, QString()), clean);
    // 非 http(s) / 无 "://" / authority 为空：恒等，绝不注入
    QCOMPARE(Util::insertUrlCredentials(QStringLiteral("rtsp://host/live"), QStringLiteral("u:pw")),
             QStringLiteral("rtsp://host/live"));
    QCOMPARE(Util::insertUrlCredentials(QStringLiteral("C:/dir/a.mp4"), QStringLiteral("u:pw")),
             QStringLiteral("C:/dir/a.mp4"));
    QCOMPARE(Util::insertUrlCredentials(QStringLiteral("http:///a.mp4"), QStringLiteral("u:pw")),
             QStringLiteral("http:///a.mp4"));
}

// N48：path 段与 query / fragment 的分界（首个 '?' 或 '#'；无则 size()）——纯字符串，
// 不去重、不解析，本地路径里的 '?' / '#' 由调用方负责不误用
void TestUtil::urlPathEndFindsSpecBoundary() {
    QCOMPARE(Util::urlPathEnd(QStringLiteral("http://host/dav/a.mp4")), qsizetype(21));
    QCOMPARE(Util::urlPathEnd(QStringLiteral("http://host/dav?token=1")), qsizetype(15));
    QCOMPARE(Util::urlPathEnd(QStringLiteral("http://host/dav#frag")), qsizetype(15));
    // 两者并存取较小者（先出现者）
    QCOMPARE(Util::urlPathEnd(QStringLiteral("http://host/dav#f?q")), qsizetype(15));
    QCOMPARE(Util::urlPathEnd(QStringLiteral("http://host/dav?q#f")), qsizetype(15));
    // 空串 / 无分界：size()
    QCOMPARE(Util::urlPathEnd(QString()), qsizetype(0));
    QCOMPARE(Util::urlPathEnd(QStringLiteral("http://host/dav/")), qsizetype(16));
    // 分界即首字符
    QCOMPARE(Util::urlPathEnd(QStringLiteral("?q")), qsizetype(0));
}

// N54：本地来源缺失判据（书签失效清理与书签跳转共用）。三件事各钉一刀：
// 网络/流恒假（不可判定 ≠ 缺失）、目录不算缺失（QFileInfo 而非 QFile 的存在性）、
// 相对名先绝对化再判定、file:// 与 mpvqt:// 经 resolveSourcePath 折叠后再判
void TestUtil::missingLocalSourceJudgesByForm() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString here = dir.filePath(QStringLiteral("here.mkv"));
    {
        QFile f(here);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    const QString gone = dir.filePath(QStringLiteral("gone.mkv"));

    QVERIFY(!Util::isMissingLocalSource(here));
    QVERIFY(Util::isMissingLocalSource(gone));
    // 目录：QFile::exists 在 Windows 判假，会被误当失效——判据取 QFileInfo::exists，
    // 与 LocalFileEntity::exists() 同口径（mpv 能播目录，目录不是失效项）
    QVERIFY(!Util::isMissingLocalSource(dir.path()));
    QVERIFY(!Util::isMissingLocalSource(QString()));

    // 网络 / 流：保持原样、无从以文件存在性判定，一律假
    QVERIFY(!Util::isMissingLocalSource(QStringLiteral("http://host/dav/a.mkv")));
    QVERIFY(!Util::isMissingLocalSource(QStringLiteral("https://user:pw@host/a.mkv")));
    QVERIFY(!Util::isMissingLocalSource(QStringLiteral("rtsp://host/stream")));
    // N52 折叠：转接 URI 先还原成条目身份（此处是 http URL）→ 仍判假
    QVERIFY(
        !Util::isMissingLocalSource(QStringLiteral("mpvqt://11111111-2222-3333-4444-555555555555/"
                                                   "http%3A%2F%2Fhost%2Fdav%2Fa.mkv")));

    // file:// 形态转本地后判定
    QVERIFY(!Util::isMissingLocalSource(QUrl::fromLocalFile(here).toString()));
    QVERIFY(Util::isMissingLocalSource(QUrl::fromLocalFile(gone).toString()));

    // 相对名：绝对化（基于进程 CWD 的纯字符串变换）后判定，不因"非绝对路径"早退
    QVERIFY(Util::isMissingLocalSource(QStringLiteral("mpvqt-absent-probe-file.mkv")));
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

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN）
QTEST_GUILESS_MAIN(TestUtil)

#include "tst_Util.moc"

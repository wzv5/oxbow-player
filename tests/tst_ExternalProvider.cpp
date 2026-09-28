// ExternalProvider 单元测试（N63 / N65）：解析纯函数夹具——直调 parseSource /
// normalizeSource / validateInput / splitGroupTitles（纯逻辑、不碰网络），枚举形状
// 用例经本地清单文件走 enumerate（不发起任何网络请求）。覆盖：定形幂等五支 /
// 参数校验四档 / 三态定性的判据顺序 / 流清单白名单逐个标签与位次限定（VOD 档、
// ENDLIST 截断档、master 档、#EXT-X-VERSION 反例、混排反例）/ 分组文本支的组行
// 与条目行 / 异常三档 / 裸 URI 行两支照收 / 同名多源不判重 / 同层同 URL 重复行
// 不合并 / EXTINF 属性区引号外逗号分界 / 行尾与 BOM / 非法 UTF-8 降级 / 显示名
// 三档回退链 / group-title 多值拆分 / 一条目入多组的归属份数 / 整树交付形状 /
// 非空 parent 违约守候 / 警告换代与无计数纪律 / N65 条目级单文件选项（两键解析、
// 键名映射、值原样、后写覆盖、空值跳过、白名单外键、悬空指令、裸目标行、注入与
// 多组份数）/ INI 往返。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R External 跑本套件。

#include "pch.h"

#include <QtTest/QtTest>

#include <QTemporaryDir>

#include "ExternalProvider.h"
#include "HttpClient.h"
#include "ServiceRegistry.h"
#include "Util.h"

namespace {

// http(s) 条目经 PlaylistEntity::fromPath 出生时要求注册表里有 HttpClient
// （Debug 断言守候）——枚举形状用例统一用带 client 的注册表；本地条目不取用它
std::shared_ptr<ServiceRegistry> makeRegistry() {
    auto registry = std::make_shared<ServiceRegistry>();
    registry->provide(std::make_shared<HttpClient>(HttpClient::Config{}));
    return registry;
}

// 本地写一份清单并造提供者（enumerate 用例不碰网络的取源支）
struct LocalList {
    QTemporaryDir dir;
    QString path;
    std::shared_ptr<ExternalProvider> provider;

    LocalList(const QByteArray &content, const QString &fileName = QStringLiteral("list.m3u")) {
        path = QDir(dir.path()).filePath(fileName);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
        file.close();
        provider = std::make_shared<ExternalProvider>(ExternalProvider::normalizeSource(path),
                                                      makeRegistry());
    }
};

ExternalProvider::Parsed parse(const QString &text, const QString &base = QString()) {
    return ExternalProvider::parseSource(text, base);
}

} // namespace

class TestExternalProvider : public QObject {
    Q_OBJECT

  private slots:
    // ——定形与校验（§2.1）——

    // 定形幂等：本地绝对 / 相对 / file:// / http / 带 query 五支，已定形串再定形不变
    void normalizeSourceIdempotent();
    // 校验四档各拒一次：空 / rtsp:// / 目录 / 不存在的本地路径
    void validateInputRejects();
    // 无后缀与 .txt 的存在文件不被拒——不设扩展名白名单（§1.2 立足点）
    void validateInputAcceptsExtensionless();

    // ——能力位（§2.1 表）——
    void capabilityBits();

    // ——三态定性（§2.2）——

    // 判据顺序：#EXTM3U 起头或任一 #EXTINF 走 m3u 支；#genre# 行走分组文本支；
    // 皆无走裸清单支
    void grammarBranchOrder();
    // 白名单逐个标签的流判别：含维护者给定起点之外的 VOD 档 / 截断档 / master 档
    void streamWhitelistTags();
    // 位次限定两支：#EXT-X-VERSION 单独出现不判流；混排清单仍按频道清单解析
    void streamPositionQualifiers();

    // ——m3u 支（§2.3）——

    // 属性区与标题区按"引号外的第一个逗号"分界；时长与标题间前导空格；多值拆分
    void extinfAttrParsing();
    // 行尾 \r\n / \n / \r 与 UTF-8 BOM 四种形态解析一致
    void lineEndingsAndBom();
    // 无 #EXTINF 的裸目标行照收（base M3U 标准形态），标题走回退链末档
    void bareTargetLinesAccepted();
    // #EXTVLCOPT 白名单键落地进条目选项；#EXTGRP 等未知指令静默忽略
    void extvlcoptAndUnknownDirectives();
    // #EXTINF 悬空到文件尾：不收入、malformed 警告合并为一条
    void danglingExtinf();

    // ——N65：条目级单文件选项（§2.2 / §2.3）——

    // 两键解析矩阵：EXTVLCOPT 行与 EXTINF 引号属性两路来源、键名大小写不敏感、
    // 后写覆盖（两路同现与同键多行）、空值不注入、键名映射成型、值逐字节原样、
    // 两键同发互不覆盖、悬空指令行不招 malformed、裸目标行吃得到 pending 选项
    void perEntryOptions();
    // 白名单外键：不注入且置"有被忽略的键"旗标（新警告恰一条、串里无键名无计数）；
    // 无冒号整行按未知指令忽略；分组文本支与裸清单支的 # 行仍不产选项；
    // 键域恒为 {user-agent, referrer}
    void perEntryOptionsWhitelistBoundary();
    // 枚举注入：选项随实体携带（makeEntity 注入），一条目入多组各带一份
    void perEntryOptionsInjection();

    // ——分组文本支（§2.3 / §2.2 第②支）——

    // 组行开组、名,URL 成条、空行与纯空白行忽略且不记警告、同名组行合并保首次序
    void groupedTextBasics();
    // 两行坏数据各验一次（V9 修订）：含 NUL 残行、只有逗号没有第二列；rtmp:// /
    // rtsp:// 第二列按 V9 为合法条目；其余条目一条不少，malformed 警告恰一条且无计数
    void groupedTextAnomalies();
    // 裸 URL 行在分组文本支照收：组行之后归那个组（V10 收紧后组行前场景退场）
    void groupedTextBareLines();

    // ——相对目标定形（§2.3 基址两支）——
    void relativeTargetResolution();

    // ——枚举形状（§2.4 / §2.7，本地清单，不碰网络）——

    // 整树交付：根层一次调用带齐全部具名组、键长 1、无组名条目不在 descendants、
    // 同层容器 id 互不相同、多组归属各现造一份实体（互不共享实例）
    void wholeTreeDeliveryShape();
    // 非空 parent 违约守候：返回空 Enumeration、不解析（Debug 撞 Q_ASSERT，跳过）
    void nonEmptyParentIsViolation();
    // 流命中：只回清单自身一项、descendants 与 containers 皆空
    void streamSourceSingleEntity();
    // 显示名三档回退链：EXTINF 标题 → tvg-name → URL 末段（剥 query / fragment）
    void displayNameFallbackChain();
    // 同名多源：三行同名不同 URL 各成一实体且键互异；同层同 URL 重复行不合并
    void sameNameAndDuplicateRows();
    // 非法 UTF-8 字节降级为替换字符，整份仍成功
    void invalidUtf8Degrades();

    // ——警告与空清单（§2.8）——
    void warningDiscipline();

    // ——INI 往返（§2.1）——
    void settingsRoundTrip();
};

void TestExternalProvider::normalizeSourceIdempotent() {
    // 本地绝对：转原生分隔符
    const QString local = ExternalProvider::normalizeSource(QStringLiteral("D:/media/list.m3u"));
    QCOMPARE(local, QDir::toNativeSeparators(QStringLiteral("D:/media/list.m3u")));
    // 相对：基于进程 CWD 转绝对
    const QString relative = ExternalProvider::normalizeSource(QStringLiteral("sub/list.m3u"));
    QVERIFY(QDir::isAbsolutePath(relative));
    // file:// 折本地盘符串
    const QString folded =
        ExternalProvider::normalizeSource(QStringLiteral("file:///D:/media/list.m3u"));
    QCOMPARE(folded, QDir::toNativeSeparators(QStringLiteral("D:/media/list.m3u")));
    // http：scheme / host 归小写、去默认端口
    QCOMPARE(ExternalProvider::normalizeSource(QStringLiteral("HTTP://EXAMPLE.com:80/a.m3u")),
             QStringLiteral("http://example.com/a.m3u"));
    // 带 query：query 原样保留在后
    const QString queried =
        ExternalProvider::normalizeSource(QStringLiteral("https://Host.tv/a.m3u8?token=A%20b"));
    QVERIFY(queried.startsWith(QStringLiteral("https://host.tv/a.m3u8?token=")));
    // 幂等：五支已定形串再定形逐字节不变
    for (const QString &s :
         {local, relative, folded,
          ExternalProvider::normalizeSource(QStringLiteral("http://example.com/a.m3u")), queried}) {
        QCOMPARE(ExternalProvider::normalizeSource(s), s);
    }
}

void TestExternalProvider::validateInputRejects() {
    // 空
    QVERIFY(ExternalProvider::validateInput(QStringLiteral("  ")).has_value());
    // 含 :// 而 isHttp 为假者拒（rtsp:// 这类本提供者取不回的"清单地址"）
    QVERIFY(ExternalProvider::validateInput(QStringLiteral("rtsp://server/live")).has_value());
    QVERIFY(ExternalProvider::validateInput(QStringLiteral("ftp://server/a.m3u")).has_value());
}

void TestExternalProvider::validateInputAcceptsExtensionless() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 目录拒：仅支持单个清单文件
    QVERIFY(ExternalProvider::validateInput(dir.path()).has_value());
    // 不存在的本地路径拒
    QVERIFY(
        ExternalProvider::validateInput(QDir(dir.path()).filePath(QStringLiteral("missing.m3u")))
            .has_value());
    // 无后缀 / .txt 的存在文件不被拒——后缀不能定性正是本篇立足点
    for (const QString &name : {QStringLiteral("playlist"), QStringLiteral("list.txt")}) {
        QFile file(QDir(dir.path()).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#EXTM3U\nhttp://a/1\n");
        file.close();
        QCOMPARE(ExternalProvider::validateInput(QDir(dir.path()).filePath(name)), std::nullopt);
    }
    // http(s) 地址不做探测，直接通过
    QCOMPARE(ExternalProvider::validateInput(QStringLiteral("https://example.com/list.m3u")),
             std::nullopt);
}

void TestExternalProvider::capabilityBits() {
    ExternalProvider provider(QStringLiteral("http://example.com/list.m3u"), makeRegistry());
    QCOMPARE(provider.typeKey(), QStringLiteral("external"));
    QVERIFY(!provider.supportsModify());
    QVERIFY(provider.supportsSort());
    QVERIFY(provider.enumeratesWholeTree()); // 恒真，类型级事实
    // 转接不覆写：基类默认 Unsupported ⇒ transit 键不落盘、下发恒走直发形态
    QCOMPARE(provider.transitCapability(), PlaylistProvider::TransitCapability::Unsupported);
    QCOMPARE(provider.defaultSortRule().key, SortKey::None); // 默认不排，保住作者序
    QCOMPARE(provider.defaultSortRule().ascending, true);
}

void TestExternalProvider::grammarBranchOrder() {
    // #EXTM3U 起头 → m3u 支
    {
        const auto out = parse(QStringLiteral("#EXTM3U\nhttp://a/1\n"));
        QVERIFY(!out.isStreamSource);
        QCOMPARE(out.entries.size(), 1);
        QVERIFY(out.groupOrder.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
    // 无头但含 #EXTINF → m3u 支
    {
        const auto out = parse(QStringLiteral("#EXTINF:-1,TV\nhttp://a/1\n"));
        QCOMPARE(out.entries.size(), 1);
        QCOMPARE(out.entries.at(0).title, QStringLiteral("TV"));
    }
    // 首个数据行是 <字段>,#genre# 组行 → 分组文本支
    {
        const auto out = parse(QStringLiteral("组,#genre#\n名,http://a/1\n"));
        QCOMPARE(out.groupOrder, QStringList{QStringLiteral("组")});
        QCOMPARE(out.entries.size(), 1);
        QCOMPARE(out.entries.at(0).title, QStringLiteral("名"));
    }
    // 皆无 → 裸清单支：条目全无标题、根层平铺
    {
        const auto out = parse(QStringLiteral("http://a/1\nhttp://a/2\n"));
        QCOMPARE(out.entries.size(), 2);
        QVERIFY(out.entries.at(0).title.isEmpty());
        QVERIFY(out.entries.at(0).groups.isEmpty());
        QVERIFY(out.groupOrder.isEmpty());
    }
    // 装饰头跳过（V8 反转 + V10 收紧）：首个**数据行**（非空且非 # 行）是组行
    // 即走分组文本支——带 #EXTM3U / #EXT-X-VERSION 装饰头的 tvbox 导出不受影响
    {
        const auto out =
            parse(QStringLiteral("#EXTM3U x-tvg-url=\"http://epg/x.gz\"\n#EXT-X-VERSION:3\n"
                                 "组,#genre#\n名,http://a/1\n"));
        QVERIFY(!out.isStreamSource);
        QCOMPARE(out.groupOrder, (QStringList{QStringLiteral("组")}));
        QCOMPARE(out.entries.size(), 1);
        QCOMPARE(out.entries.at(0).title, QStringLiteral("名"));
        QCOMPARE(out.entries.at(0).groups, (QStringList{QStringLiteral("组")}));
    }
    // V10 收紧：#genre# 行不在首个数据行就不作数——m3u 文件中段夹带该形态行
    // 仍按 m3u 支解析（空 base 下该行定形为空落异常③）
    {
        const auto out = parse(QStringLiteral("#EXTM3U\nhttp://a/1\nX,#genre#\nhttp://a/2\n"));
        QVERIFY(!out.isStreamSource);
        QVERIFY(out.groupOrder.isEmpty());
        QCOMPARE(out.entries.size(), 2);
        QVERIFY(
            out.warnings.contains(QStringLiteral("Malformed lines were skipped in this list.")));
    }
    // V10 连带：首数据行不是组行的文件即使后文有组行也按裸清单支——组行整行当
    // 目标。本例直调传空 base 故定形为空落异常③；生产中 base 恒非空（清单所在
    // 目录或清单 URL），同样那行拼成绝对路径而成条目。该形态无现实素材（tvbox 表
    // 组行先于条目），按不可达处理、不设判据，见变更记录 V10 的连带后果补全
    {
        const auto out = parse(QStringLiteral("http://a/0\nG,#genre#\nhttp://a/1\n"));
        QVERIFY(out.groupOrder.isEmpty());
        QCOMPARE(out.entries.size(), 2);
        QCOMPARE(out.warnings.size(), 1);
    }
    // 已知边界（V8）：无头且无组行的 名,URL 表——裸清单支整行当目标，逗号不分列
    // （URL 的 query 可含逗号，主动分列会把合法地址切坏；代价只是显示名难看）
    {
        const auto out = parse(QStringLiteral("名,http://a/1?x=1,2\n"));
        QCOMPARE(out.entries.size(), 1);
        QCOMPARE(out.entries.at(0).target, QStringLiteral("名,http://a/1?x=1,2"));
        QVERIFY(out.entries.at(0).title.isEmpty());
    }
}

void TestExternalProvider::streamWhitelistTags() {
    // 七个白名单标签逐一命中：直播档（维护者给定起点）、其余必备标签（补齐 VOD
    // 档）、主清单档、以及全文任意位置命中的 ENDLIST
    const QList<QByteArray> cases = {
        QByteArrayLiteral("#EXT-X-MEDIA-SEQUENCE:4111\nseg.ts\n"),
        QByteArrayLiteral("#EXT-X-TARGETDURATION:6\nseg0.ts\nseg1.ts\n"),
        QByteArrayLiteral("#EXT-X-PLAYLIST-TYPE:VOD\nseg0.ts\n"),
        QByteArrayLiteral("#EXT-X-STREAM-INF:BANDWIDTH=1280000\nvar.m3u8\n"),
        QByteArrayLiteral("#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=1\niframe.m3u8\n"),
        QByteArrayLiteral("#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\"\n"),
        QByteArrayLiteral("#EXTM3U\n#EXTINF:-1,x\nhttp://a/1\n#EXT-X-ENDLIST\n"),
        // VOD 档：不带 MEDIA-SEQUENCE——单判据会漏、白名单补上的那一档
        QByteArrayLiteral("#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-TARGETDURATION:6\nseg0.ts\n"
                          "#EXT-X-ENDLIST\n"),
    };
    for (const QByteArray &body : cases) {
        const auto out = parse(QString::fromUtf8(body));
        QVERIFY2(out.isStreamSource, body.constData());
        // 流命中：解析产出整份丢弃
        QVERIFY(out.entries.isEmpty());
        QVERIFY(out.groupOrder.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
}

void TestExternalProvider::streamPositionQualifiers() {
    // 反例一：只多了 #EXT-X-VERSION 的频道清单必须仍按清单解析（宽判据会把整表
    // 压成一行的那个陷阱——VERSION 不进白名单）
    {
        const auto out =
            parse(QStringLiteral("#EXT-X-VERSION:3\n#EXTM3U\n#EXTINF:-1,A\nhttp://a/1\n"));
        QVERIFY(!out.isStreamSource);
        QCOMPARE(out.entries.size(), 1);
    }
    // 反例二：混排清单——两条频道之间夹一段被注释保留的分片块（含
    // #EXT-X-TARGETDURATION），按"全文行首命中"会整表消失，位次限定把它挡住
    {
        const auto out = parse(QStringLiteral("#EXTM3U\n#EXTINF:-1,A\nhttp://a/1\n"
                                              "#EXT-X-TARGETDURATION:6\n#EXTINF:6,seg\nseg0.ts\n"
                                              "#EXTINF:-1,B\nhttp://a/2\n"));
        QVERIFY(!out.isStreamSource);
        QCOMPARE(out.entries.size(), 2);
        QCOMPARE(out.entries.at(0).target, QStringLiteral("http://a/1"));
        QCOMPARE(out.entries.at(1).target, QStringLiteral("http://a/2"));
    }
}

void TestExternalProvider::extinfAttrParsing() {
    // tvg-logo 值内含逗号：按引号外的第一个逗号分界，标题不被切进属性区
    {
        const auto out = parse(
            QStringLiteral("#EXTINF:-1 tvg-logo=\"http://x/a,b.png\" group-title=\"News\",CNN HD\n"
                           "http://a/1\n"));
        QCOMPARE(out.entries.size(), 1);
        QCOMPARE(out.entries.at(0).title, QStringLiteral("CNN HD"));
        QCOMPARE(out.entries.at(0).groups, QStringList{QStringLiteral("News")});
    }
    // 时长与标题之间的前导空格随时长一并吃掉；无属性区
    {
        const auto out =
            parse(QStringLiteral("#EXTINF:-1 ,名字\nhttp://a/1\n#EXTINF:,第二个\nhttp://a/2\n"));
        QCOMPARE(out.entries.at(0).title, QStringLiteral("名字"));
        QCOMPARE(out.entries.at(1).title, QStringLiteral("第二个"));
    }
    // tvg-name 只进显示名回退链，不进组名
    {
        const auto out =
            parse(QStringLiteral("#EXTINF:-1 tvg-name=\"X\" tvg-id=\"1\",T\nhttp://a/1\n"));
        QCOMPARE(out.entries.at(0).tvgName, QStringLiteral("X"));
        QVERIFY(out.entries.at(0).groups.isEmpty());
    }
    // 标准 iptv-org 行形：属性区在首逗号之左、标题在右——属性值不含逗号时边界
    // 逗号就是"引号外的第一个逗号"（回归：实现曾把分界读反，标题与组名全落空）
    {
        const auto out = parse(
            QStringLiteral("#EXTINF:-1 tvg-id=\"00sReplay.us@SD\" tvg-logo=\"https://i/x.jpg\" "
                           "group-title=\"Animation\",00s Replay\nhttp://a/1\n"));
        QCOMPARE(out.entries.at(0).title, QStringLiteral("00s Replay"));
        QCOMPARE(out.entries.at(0).groups, (QStringList{QStringLiteral("Animation")}));
        QVERIFY(out.entries.at(0).tvgName.isEmpty()); // 未给 tvg-name，须为空
    }
    // 组名多值拆分（splitGroupTitles 直查）：分号拆、去空段、组内去重保序
    QCOMPARE(ExternalProvider::splitGroupTitles(QStringLiteral("Animation;Kids;Music")),
             (QStringList{QStringLiteral("Animation"), QStringLiteral("Kids"),
                          QStringLiteral("Music")}));
    QCOMPARE(ExternalProvider::splitGroupTitles(QStringLiteral("A;;B;A;")),
             (QStringList{QStringLiteral("A"), QStringLiteral("B")}));
    // 条目侧多值：一条目入多组
    {
        const auto out =
            parse(QStringLiteral("#EXTINF:-1 group-title=\"Animation;Kids\",n1\nhttp://a/1\n"));
        QCOMPARE(out.entries.at(0).groups,
                 (QStringList{QStringLiteral("Animation"), QStringLiteral("Kids")}));
    }
}

void TestExternalProvider::lineEndingsAndBom() {
    const char *bodies[] = {
        "#EXTM3U\n#EXTINF:-1,A\nhttp://a/1\n#EXTINF:-1,B\nhttp://a/2\n",           // LF
        "#EXTM3U\r\n#EXTINF:-1,A\r\nhttp://a/1\r\n#EXTINF:-1,B\r\nhttp://a/2\r\n", // CRLF
        "#EXTM3U\r#EXTINF:-1,A\rhttp://a/1\r#EXTINF:-1,B\rhttp://a/2\r",           // CR
    };
    for (const char *body : bodies) {
        const auto out = parse(QString::fromUtf8(body));
        QCOMPARE(out.entries.size(), 2);
        QCOMPARE(out.entries.at(0).title, QStringLiteral("A"));
        QCOMPARE(out.entries.at(1).target, QStringLiteral("http://a/2"));
    }
    // UTF-8 BOM：剥掉后首行仍识别为 #EXTM3U
    const auto out = parse(QStringLiteral("\uFEFF#EXTM3U\nhttp://a/1\n"));
    QCOMPARE(out.entries.size(), 1);
    QVERIFY(out.warnings.isEmpty());
}

void TestExternalProvider::bareTargetLinesAccepted() {
    // 无 #EXTINF 的裸目标行照收：无标题无组，按文件序入列
    const auto out =
        parse(QStringLiteral("#EXTM3U\nhttp://a/1\nhttp://a/2\n#EXTINF:-1,T\nhttp://a/3\n"));
    QCOMPARE(out.entries.size(), 3);
    QVERIFY(out.entries.at(0).title.isEmpty());
    QVERIFY(out.entries.at(1).title.isEmpty());
    QCOMPARE(out.entries.at(2).title, QStringLiteral("T"));
    QVERIFY(out.warnings.isEmpty());
}

void TestExternalProvider::extvlcoptAndUnknownDirectives() {
    // N65：白名单键落地进 pending 选项槽、目标行消费（键名映射 user-agent）——
    // EXTVLCOPT 紧随 #EXTINF 之后（现实两路齐发的顺序，§2.2"属性区先入、其后
    // EXTVLCOPT 行覆盖同键"）；其余未知指令（含 #EXTGRP）静默忽略；只含白名单
    // 键的清单不再出现任何警告
    const auto out = parse(QStringLiteral("#EXTM3U x-tvg-url=\"http://epg/x.xml\"\n"
                                          "#EXTINF:-1,T\n"
                                          "#EXTVLCOPT:http-user-agent=Mozilla\n"
                                          "http://a/1\n"
                                          "#EXTGRP:分组\n#EXT-X-VERSION:3\n# random comment\n"));
    QCOMPARE(out.entries.size(), 1);
    QCOMPARE(out.entries.at(0).options,
             (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("Mozilla")}}));
    QVERIFY(out.warnings.isEmpty());
}

void TestExternalProvider::danglingExtinf() {
    // #EXTINF 到文件尾仍无目标行：不收入条目，悬空与零条目各自一条警告
    const auto out = parse(QStringLiteral("#EXTM3U\n#EXTINF:-1,T\n"));
    QVERIFY(out.entries.isEmpty());
    bool hasMalformed = false;
    bool hasNoPlayable = false;
    for (const QString &w : out.warnings) {
        hasMalformed |= w == QStringLiteral("Malformed lines were skipped in this list.");
        hasNoPlayable |= w == QStringLiteral("No playable entries were found in this playlist.");
    }
    QVERIFY(hasMalformed);
    QVERIFY(hasNoPlayable);
    QCOMPARE(out.warnings.size(), 2);
    // 悬空的 EXTINF 被后续条目顶替：只合并出一条 malformed，好条目不受牵连
    const auto out2 = parse(QStringLiteral("#EXTINF:-1,dead\n#EXTINF:-1,alive\nhttp://a/1\n"));
    QCOMPARE(out2.entries.size(), 1);
    QCOMPARE(out2.entries.at(0).title, QStringLiteral("alive"));
}

void TestExternalProvider::perEntryOptions() {
    // 两键从 #EXTVLCOPT 行解出：键名映射成型、两键同发互不覆盖（EXTVLCOPT
    // 紧随 #EXTINF 的规范顺序）
    {
        const auto out = parse(QStringLiteral("#EXTM3U\n"
                                              "#EXTINF:-1,T\n"
                                              "#EXTVLCOPT:http-user-agent=UA1\n"
                                              "#EXTVLCOPT:http-referrer=http://r/\n"
                                              "http://a/1\n"));
        QCOMPARE(out.entries.size(), 1);
        QCOMPARE(out.entries.at(0).options,
                 (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("UA1")},
                              {QStringLiteral("referrer"), QStringLiteral("http://r/")}}));
        QVERIFY(out.warnings.isEmpty());
    }
    // 两键从 #EXTINF 引号属性解出；裸值形态（到空白止）对这两键不取
    {
        const auto out =
            parse(QStringLiteral("#EXTINF:-1 http-user-agent=\"UA/2.0\" "
                                 "http-referrer=\"http://r2/\" tvg-name=X,T\nhttp://a/1\n"));
        QCOMPARE(out.entries.size(), 1);
        QCOMPARE(out.entries.at(0).options,
                 (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("UA/2.0")},
                              {QStringLiteral("referrer"), QStringLiteral("http://r2/")}}));
        QCOMPARE(out.entries.at(0).tvgName, QStringLiteral("X"));
    }
    {
        // 裸值 http-user-agent=UA/2.0 more（无引号）会被空白截断——不取，选项为空
        const auto out =
            parse(QStringLiteral("#EXTINF:-1 http-user-agent=UA/2.0 more,T\nhttp://a/1\n"));
        QVERIFY(out.entries.at(0).options.isEmpty());
    }
    // 键名大小写不敏感（与既有前缀判定同一口径）
    {
        const auto out =
            parse(QStringLiteral("#EXTINF:-1,T\n#EXTVLCOPT:HTTP-User-Agent=UpperUA\nhttp://a/1\n"));
        QCOMPARE(out.entries.at(0).options,
                 (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("UpperUA")}}));
    }
    // 后写覆盖之一：属性区先入，其后 #EXTVLCOPT 行覆盖同键
    {
        const auto out = parse(QStringLiteral(
            "#EXTINF:-1 "
            "http-user-agent=\"attr-UA\",T\n#EXTVLCOPT:http-user-agent=opt-UA\nhttp://a/1\n"));
        QCOMPARE(out.entries.at(0).options,
                 (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("opt-UA")}}));
    }
    // 后写覆盖之二：同键多行，后者覆盖前者
    {
        const auto out = parse(QStringLiteral("#EXTM3U\n"
                                              "#EXTINF:-1,T\n"
                                              "#EXTVLCOPT:http-referrer=http://first/\n"
                                              "#EXTVLCOPT:http-referrer=http://second/\n"
                                              "http://a/1\n"));
        QCOMPARE(out.entries.at(0).options,
                 (QVariantMap{{QStringLiteral("referrer"), QStringLiteral("http://second/")}}));
    }
    // 空值不注入：等号后为空或全空白 → 键跳过，不发空头、不置旗标（无警告）
    {
        const auto out = parse(QStringLiteral("#EXTM3U\n#EXTINF:-1,T\n#EXTVLCOPT:http-user-agent="
                                              "\n#EXTVLCOPT:http-referrer=   \nhttp://a/1\n"));
        QCOMPARE(out.entries.size(), 1);
        QVERIFY(out.entries.at(0).options.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
    // 属性区空引号形态同理不注入
    {
        const auto out = parse(QStringLiteral("#EXTINF:-1 http-user-agent=\"\",T\nhttp://a/1\n"));
        QVERIFY(out.entries.at(0).options.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
    // 值逐字节原样：含逗号 / 空白 / 反斜杠的取值不被 trim 内部、加前缀或转义——
    // 成型只做键名映射（plain String 选项不过列表解析器），两条目各验一组
    {
        const auto out =
            parse(QStringLiteral("#EXTM3U\n"
                                 "#EXTINF:-1,T1\n"
                                 "#EXTVLCOPT:http-user-agent=Mozilla/5.0 (KHTML, like Gecko) X\n"
                                 "#EXTVLCOPT:http-referrer=http://h/,q=1&r=2\n"
                                 "http://a/1\n"
                                 "#EXTINF:-1,T2\n"
                                 "#EXTVLCOPT:http-user-agent=back\\slash,comma\n"
                                 "#EXTVLCOPT:http-referrer=http://h/a\\b\n"
                                 "http://a/2\n"));
        QCOMPARE(out.entries.size(), 2);
        QCOMPARE(out.entries.at(0).options,
                 (QVariantMap{{QStringLiteral("user-agent"),
                               QStringLiteral("Mozilla/5.0 (KHTML, like Gecko) X")},
                              {QStringLiteral("referrer"), QStringLiteral("http://h/,q=1&r=2")}}));
        QCOMPARE(out.entries.at(1).options,
                 (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("back\\slash,comma")},
                              {QStringLiteral("referrer"), QStringLiteral("http://h/a\\b")}}));
    }
    // 值内含 '='：分界取第一个等号，其余归值
    {
        const auto out =
            parse(QStringLiteral("#EXTINF:-1,T\n#EXTVLCOPT:http-user-agent=a=b=c\nhttp://a/1\n"));
        QCOMPARE(out.entries.at(0).options,
                 (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("a=b=c")}}));
    }
    // 悬空指令行：#EXTVLCOPT 到文件尾无目标行消费 → 丢弃、不报 malformed
    {
        const auto out = parse(QStringLiteral(
            "#EXTM3U\n#EXTINF:-1,T\nhttp://a/1\n#EXTVLCOPT:http-user-agent=dangling\n"));
        QCOMPARE(out.entries.size(), 1);
        QVERIFY(out.entries.at(0).options.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
    // 被 #EXTINF 顶替的未消费选项：丢弃，且不串到下一条目（§2.2 悬空条款——
    // "或被下一条 #EXTINF 顶替"仍没有目标行消费即丢弃；防的是悬空条目的门票
    // 粘到下一条目上）。先于 #EXTINF 出现的 EXTVLCOPT 同受此规则约束
    {
        const auto out = parse(
            QStringLiteral("#EXTM3U\n#EXTVLCOPT:http-user-agent=lost\n#EXTINF:-1,T\nhttp://a/1\n"));
        QCOMPARE(out.entries.size(), 1);
        QVERIFY(out.entries.at(0).options.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
    // 无 #EXTINF 的裸目标行也吃得到 pending 选项
    {
        const auto out =
            parse(QStringLiteral("#EXTM3U\n#EXTVLCOPT:http-user-agent=UA\nhttp://a/1\n"));
        QCOMPARE(out.entries.size(), 1);
        QCOMPARE(out.entries.at(0).options,
                 (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("UA")}}));
    }
    // 键域恒为 {user-agent, referrer}：任何输入都不得产第三个键（上文各例的
    // options 键集已逐一比对，此处再钉一次双键同发时的键集）
    {
        const auto out = parse(QStringLiteral(
            "#EXTM3U\n#EXTVLCOPT:http-user-agent=A\n#EXTVLCOPT:http-referrer=B\nhttp://a/1\n"));
        QCOMPARE(out.entries.at(0).options.keys().size(), 2);
        QVERIFY(out.entries.at(0).options.contains(QStringLiteral("user-agent")));
        QVERIFY(out.entries.at(0).options.contains(QStringLiteral("referrer")));
    }
}

void TestExternalProvider::perEntryOptionsWhitelistBoundary() {
    const QString kNewWarning =
        QStringLiteral("Some per-entry options in this playlist are not supported and were "
                       "ignored.");
    // 白名单外键（VLC 其余旋钮 / 未知键）：不注入、置旗标恰一条、串里不见键名与计数
    {
        const auto out = parse(QStringLiteral("#EXTM3U\n"
                                              "#EXTINF:-1,T\n"
                                              "#EXTVLCOPT:network-caching=1000\n"
                                              "#EXTVLCOPT:http-cookie=a=b\n"
                                              "#EXTVLCOPT:bogus-key=1\n"
                                              "http://a/1\n"));
        QCOMPARE(out.entries.size(), 1);
        QVERIFY(out.entries.at(0).options.isEmpty());
        QCOMPARE(out.warnings.size(), 1);
        QCOMPARE(out.warnings.at(0), kNewWarning);
        QVERIFY(!out.warnings.at(0).contains(QLatin1Char('3'))); // 无计数
        QVERIFY(!out.warnings.at(0).contains(QStringLiteral("network-caching")));
        QVERIFY(!out.warnings.at(0).contains(QStringLiteral("cookie")));
    }
    // 白名单键与白名单外键混排：白名单键照常落地，警告只在白名单外键时出现
    {
        const auto out = parse(QStringLiteral(
            "#EXTM3U\n#EXTVLCOPT:http-user-agent=UA\n#EXTVLCOPT:input-repeat=1\nhttp://a/1\n"));
        QCOMPARE(out.entries.at(0).options,
                 (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("UA")}}));
        QCOMPARE(out.warnings.size(), 1);
        QCOMPARE(out.warnings.at(0), kNewWarning);
    }
    // #EXTVLCOPT 后无冒号：整行按未知指令静默忽略——算内容（零条目警告判据）、
    // 不置旗标、不产选项
    {
        const auto out = parse(QStringLiteral(
            "#EXTM3U\n#EXTVLCOPT http-user-agent=no-colon\n#EXTINF:-1,T\nhttp://a/1\n"));
        QCOMPARE(out.entries.size(), 1);
        QVERIFY(out.entries.at(0).options.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
    // 无 '=' 的指令行：无值可取，静默忽略、不置旗标
    {
        const auto out = parse(
            QStringLiteral("#EXTM3U\n#EXTVLCOPT:http-user-agent\n#EXTINF:-1,T\nhttp://a/1\n"));
        QVERIFY(out.entries.at(0).options.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
    // 冒号后键为空（"#EXTVLCOPT:=x"）：无键可映射，静默忽略、不置旗标——
    // "有被忽略的键"只认真键名
    {
        const auto out =
            parse(QStringLiteral("#EXTM3U\n#EXTINF:-1,T\n#EXTVLCOPT:=x\nhttp://a/1\n"));
        QVERIFY(out.entries.at(0).options.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
    // 分组文本支与裸清单支一字不动：# 行静默忽略但算内容，不产选项、不置旗标
    {
        // 裸清单支（首个数据行非组行、无 m3u 指令）
        const auto bare =
            parse(QStringLiteral("#EXTVLCOPT:http-user-agent=x\nhttp://a/1\nhttp://a/2\n"));
        QCOMPARE(bare.entries.size(), 2);
        QVERIFY(bare.entries.at(0).options.isEmpty());
        QVERIFY(bare.warnings.isEmpty());
        // 分组文本支
        const auto grouped =
            parse(QStringLiteral("G,#genre#\n#EXTVLCOPT:http-user-agent=x\nn,http://a/1\n"));
        QCOMPARE(grouped.entries.size(), 1);
        QVERIFY(grouped.entries.at(0).options.isEmpty());
        QVERIFY(grouped.warnings.isEmpty());
    }
    // 旧警告（N63 的 headers ignored 串）在任何输入下都不再出现
    {
        const auto out = parse(QStringLiteral(
            "#EXTM3U\n#EXTVLCOPT:http-user-agent=UA\n#EXTVLCOPT:http-referrer=R\nhttp://a/1\n"));
        for (const QString &w : out.warnings)
            QVERIFY2(!w.contains(QStringLiteral("HTTP headers")), qPrintable(w));
    }
}

void TestExternalProvider::perEntryOptionsInjection() {
    // makeEntity 注入：选项随实体携带（playOptions 只读出口）；一条目入多组时
    // 各现造实体各带一份（互不共享实例）；无选项条目 playOptions 为空
    LocalList list(QByteArrayLiteral("#EXTM3U\n"
                                     "#EXTINF:-1 group-title=\"A;B\" "
                                     "http-user-agent=\"UA/3\",n1\nhttp://a/1\n"
                                     "#EXTINF:-1 group-title=\"A\",n2\nhttp://a/2\n"));
    const auto result = list.provider->enumerate({});
    QVERIFY(result.has_value());
    QVERIFY(result->entities.empty()); // 两条目均入具名组
    const auto &levelA = result->descendants[QStringList{QStringLiteral("A")}];
    const auto &levelB = result->descendants[QStringList{QStringLiteral("B")}];
    QCOMPARE(levelA.entities.size(), 2);
    QCOMPARE(levelB.entities.size(), 1);
    // n1 带选项；n2 无选项（空 map）
    QCOMPARE(levelA.entities.at(0)->title(), QStringLiteral("n1"));
    QCOMPARE(levelA.entities.at(0)->playOptions(),
             (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("UA/3")}}));
    QCOMPARE(levelA.entities.at(1)->title(), QStringLiteral("n2"));
    QVERIFY(levelA.entities.at(1)->playOptions().isEmpty());
    // B 组里的 n1 副本同样带选项（各现造一份，不共享实例）
    QVERIFY(levelA.entities.at(0) != levelB.entities.at(0));
    QCOMPARE(levelB.entities.at(0)->playOptions(),
             (QVariantMap{{QStringLiteral("user-agent"), QStringLiteral("UA/3")}}));
}

void TestExternalProvider::groupedTextBasics() {
    const auto out = parse(QStringLiteral("A,#genre#\n"
                                          "\n"
                                          "   \n"
                                          "n1,http://a/1\n"
                                          "B,#genre#\n"
                                          "n2,http://a/2?auth=xy\n"
                                          "A,#genre#\n"));
    // 组序 = 首次出现序；同名组行合并；空行与纯空白行忽略且不记警告
    QCOMPARE(out.groupOrder, (QStringList{QStringLiteral("A"), QStringLiteral("B")}));
    QCOMPARE(out.entries.size(), 2);
    QCOMPARE(out.entries.at(0).title, QStringLiteral("n1"));
    QCOMPARE(out.entries.at(0).groups, QStringList{QStringLiteral("A")});
    QCOMPARE(out.entries.at(1).target, QStringLiteral("http://a/2?auth=xy"));
    QVERIFY(out.warnings.isEmpty());
}

void TestExternalProvider::groupedTextAnomalies() {
    // 两行坏数据（V9 修订）：含 NUL 残行（异常档②）/ 只有逗号没有第二列（第二列
    // 定形后为空，异常档③）——两行都不成条目，其余条目一条不少。rtmp:// /
    // rtsp:// 第二列按 V9 放宽为合法条目（与 m3u 支"原样收口"对齐，不再落异常档）
    const auto out = parse(QStringLiteral("G,#genre#\n"
                                          "n1,http://a/1\n"
                                          "n2,rtmp://server/live\n"
                                          "\u0001残行\n"
                                          "n3,rtsp://server/live\n"
                                          ",\n"
                                          "n4,https://a/4\n"));
    QCOMPARE(out.entries.size(), 4);
    QCOMPARE(out.entries.at(0).target, QStringLiteral("http://a/1"));
    QCOMPARE(out.entries.at(1).target, QStringLiteral("rtmp://server/live"));
    QCOMPARE(out.entries.at(1).title, QStringLiteral("n2"));
    QCOMPARE(out.entries.at(2).target, QStringLiteral("rtsp://server/live"));
    QCOMPARE(out.entries.at(3).target, QStringLiteral("https://a/4"));
    QCOMPARE(out.warnings.size(), 1);
    QCOMPARE(out.warnings.at(0), QStringLiteral("Malformed lines were skipped in this list."));
    // 串里不见条数（§2.8 文案纪律）
    QVERIFY(!out.warnings.at(0).contains(QLatin1Char('2')));
}

void TestExternalProvider::groupedTextBareLines() {
    // 裸 URL 行（无逗号）不是异常：组行之后照收、归那个组（V10 后 txt 文件的
    // 首个数据行必是组行，"首组行之前的裸行落根层"场景随判据收紧退场）
    const auto out = parse(QStringLiteral("G,#genre#\n"
                                          "http://a/1\n"
                                          "n,http://a/2\n"));
    QCOMPARE(out.entries.size(), 2);
    QCOMPARE(out.entries.at(0).groups, (QStringList{QStringLiteral("G")}));
    QCOMPARE(out.entries.at(1).groups, (QStringList{QStringLiteral("G")}));
    QVERIFY(out.warnings.isEmpty());
}

void TestExternalProvider::relativeTargetResolution() {
    // 本地基址：相对目标按清单文件所在目录拼接，绝对目标原样收口
    {
        const QString base = QDir::toNativeSeparators(QStringLiteral("D:/media"));
        const auto out = parse(QStringLiteral("sub/rel.ts\nD:\\abs\\x.ts\n"), base);
        QCOMPARE(out.entries.at(0).target,
                 QDir::toNativeSeparators(QStringLiteral("D:/media/sub/rel.ts")));
        QCOMPARE(out.entries.at(1).target, QDir::toNativeSeparators(QStringLiteral("D:/abs/x.ts")));
    }
    // http 基址：相对目标经 resolved 展开
    {
        const auto out = parse(QStringLiteral("rel.ts\nhttp://other/x.m3u8\n"),
                               QStringLiteral("http://host/dir/pl.m3u"));
        QCOMPARE(out.entries.at(0).target, QStringLiteral("http://host/dir/rel.ts"));
        QCOMPARE(out.entries.at(1).target, QStringLiteral("http://other/x.m3u8"));
    }
    // rtmp / srt 等绝对形态原样收口（两支基址同）
    {
        const auto out = parse(QStringLiteral("rtmp://srv/live\nsrt://srv:9000\n"),
                               QStringLiteral("http://host/pl.m3u"));
        QCOMPARE(out.entries.at(0).target, QStringLiteral("rtmp://srv/live"));
        QCOMPARE(out.entries.at(1).target, QStringLiteral("srt://srv:9000"));
    }
}

void TestExternalProvider::wholeTreeDeliveryShape() {
    LocalList list(QByteArrayLiteral("#EXTM3U\n"
                                     "#EXTINF:-1 group-title=\"B;A\",n1\nhttp://a/1\n"
                                     "#EXTINF:-1,n0\nhttp://a/0\n"
                                     "#EXTINF:-1 group-title=\"A\",n2\nhttp://a/2\n"));
    const auto result = list.provider->enumerate({});
    QVERIFY(result.has_value());
    const PlaylistProvider::Enumeration &out = *result;
    // 根层：容器组在前（首次出现序 B、A），无组名条目 n0 跟在实体段
    QCOMPARE(out.containers.size(), 2);
    QCOMPARE(out.containers.at(0).id, QStringLiteral("B"));
    QCOMPARE(out.containers.at(1).id, QStringLiteral("A"));
    for (const PlaylistProvider::Container &c : out.containers) {
        QCOMPARE(c.id, c.title);      // 容器四字段：id 与 title 同为组名
        QVERIFY(!c.meta.has_value()); // meta 留空
        QVERIFY(c.path.isEmpty());    // 组不是可打开的位置
    }
    QCOMPARE(out.entities.size(), 1);
    QCOMPARE(out.entities.at(0)->path(), QStringLiteral("http://a/0"));
    // 子孙层：键长 1、首段 ∈ 根层 containers；根层自身与无组名条目不在其中
    QCOMPARE(out.descendants.size(), 2);
    QVERIFY(out.descendants.contains(QStringList{QStringLiteral("A")}));
    QVERIFY(out.descendants.contains(QStringList{QStringLiteral("B")}));
    const PlaylistProvider::LevelData &levelA = out.descendants[QStringList{QStringLiteral("A")}];
    const PlaylistProvider::LevelData &levelB = out.descendants[QStringList{QStringLiteral("B")}];
    QVERIFY(levelA.containers.isEmpty()); // 组内再无可分之物
    QCOMPARE(levelA.entities.size(), 2);  // n1、n2 按文件序
    QCOMPARE(levelA.entities.at(0)->path(), QStringLiteral("http://a/1"));
    QCOMPARE(levelA.entities.at(1)->path(), QStringLiteral("http://a/2"));
    QCOMPARE(levelB.entities.size(), 1);
    // 一条目入多组：各现造一份实体，互不共享实例（归属计数放大，§2.3）
    QVERIFY(levelA.entities.at(0) != levelB.entities.at(0));
    QVERIFY(out.warnings.isEmpty());
}

void TestExternalProvider::nonEmptyParentIsViolation() {
#if defined(QT_NO_DEBUG)
    LocalList list(QByteArrayLiteral("#EXTM3U\n#EXTINF:-1 group-title=\"A\",n\nhttp://a/1\n"));
    const auto result = list.provider->enumerate({QStringLiteral("A")});
    QVERIFY(result.has_value());
    QVERIFY(result->containers.isEmpty());
    QVERIFY(result->entities.empty()); // 实体表是 std::vector，判空用 empty()
    QVERIFY(result->descendants.isEmpty());
#else
    QSKIP("Debug 构建下非空 parent 撞 Q_ASSERT（调用方违约显影）；Release 契约返空结果");
#endif
}

void TestExternalProvider::streamSourceSingleEntity() {
    LocalList list(QByteArrayLiteral("#EXT-X-MEDIA-SEQUENCE:4111\n#EXTINF:6,seg\nseg0.ts\n"),
                   QStringLiteral("cctv1hd.m3u8"));
    const auto result = list.provider->enumerate({});
    QVERIFY(result.has_value());
    const PlaylistProvider::Enumeration &out = *result;
    QCOMPARE(out.entities.size(), 1);
    // 身份 = 清单路径自身（定形后的原生分隔符形态，与提供者 sourcePath 同源）
    QCOMPARE(out.entities.at(0)->path(), list.provider->sourcePath());
    QVERIFY(out.containers.isEmpty());
    QVERIFY(out.descendants.isEmpty()); // 树只有根层的合法整树交付
    QVERIFY(out.warnings.isEmpty());
}

void TestExternalProvider::displayNameFallbackChain() {
    LocalList list(QByteArrayLiteral("#EXTM3U\n"
                                     "#EXTINF:-1,Channel One\nhttp://a/1\n"
                                     "#EXTINF:-1 tvg-name=\"Fallback Name\"\nhttp://a/2\n"
                                     "http://host/path/file.m3u8?token=xx\n"));
    const auto result = list.provider->enumerate({});
    QVERIFY(result.has_value());
    const auto &entities = result->entities;
    QCOMPARE(entities.size(), 3);
    // 第一档：EXTINF 逗号后的标题
    QCOMPARE(entities.at(0)->title(), QStringLiteral("Channel One"));
    // 第二档：属性区 tvg-name（iptv-org 常见形态：标题缺失而 tvg-name 在）
    QCOMPARE(entities.at(1)->title(), QStringLiteral("Fallback Name"));
    // 第三档：URL 末段（剥 query / fragment，不出现 index.m3u8?token=xx 噪声）
    QCOMPARE(entities.at(2)->title(), QStringLiteral("file.m3u8"));
}

void TestExternalProvider::sameNameAndDuplicateRows() {
    LocalList list(QByteArrayLiteral("#EXTM3U\n"
                                     "#EXTINF:-1 group-title=\"G\",Same\nhttp://a/1\n"
                                     "#EXTINF:-1 group-title=\"G\",Same\nhttp://a/2\n"
                                     "#EXTINF:-1 group-title=\"G\",Same\nhttp://a/3\n"
                                     "#EXTINF:-1 group-title=\"G\",Dup\nhttp://dup/1\n"
                                     "#EXTINF:-1 group-title=\"G\",Dup\nhttp://dup/1\n"));
    const auto result = list.provider->enumerate({});
    QVERIFY(result.has_value());
    const auto &level = result->descendants[QStringList{QStringLiteral("G")}];
    // 同名多源：三行并列各成一实体、键互异——绝不做按名判重（§2.3 硬要求）
    QCOMPARE(level.entities.size(), 5);
    QVERIFY(level.entities.at(0)->key() != level.entities.at(1)->key());
    QVERIFY(level.entities.at(1)->key() != level.entities.at(2)->key());
    QVERIFY(level.entities.at(0)->title() == level.entities.at(1)->title());
    // 同层同 URL 重复行原样产出：同层两行同 key，不崩、不合并、不报错
    QCOMPARE(level.entities.at(3)->key(), level.entities.at(4)->key());
    QVERIFY(result->warnings.isEmpty());
}

void TestExternalProvider::invalidUtf8Degrades() {
    // 非法 UTF-8 字节按替换字符降级：一处坏字节不废整份
    QByteArray body = QByteArrayLiteral("#EXTM3U\n#EXTINF:-1,Ch");
    body += char(0xFF); // 单独拼接，避免 "\xffa" 被读成一个越界的十六进制转义
    body += QByteArrayLiteral("annel\nhttp://a/1\n");
    LocalList list(body);
    const auto result = list.provider->enumerate({});
    QVERIFY(result.has_value());
    QCOMPARE(result->entities.size(), 1);
    QVERIFY(result->entities.at(0)->title().contains(QChar(0xFFFD)));
    QVERIFY(result->warnings.isEmpty());
}

void TestExternalProvider::warningDiscipline() {
    // 空清单（只有 #EXTM3U）：成功返回空结果、不带警告
    {
        const auto out = parse(QStringLiteral("#EXTM3U\n"));
        QVERIFY(out.entries.isEmpty());
        QVERIFY(out.warnings.isEmpty());
    }
    // 全是注释：零条目 + "No playable entries" 一条（作者大概写的不是清单）
    {
        const auto out = parse(QStringLiteral("#EXTM3U\n# hello\n# world\n"));
        QVERIFY(out.entries.isEmpty());
        QCOMPARE(out.warnings.size(), 1);
        QCOMPARE(out.warnings.at(0),
                 QStringLiteral("No playable entries were found in this playlist."));
    }
    // 空文件：零条目零警告
    QVERIFY(parse(QStringLiteral("")).warnings.isEmpty());
    // 三条警告可同现且文案一律无计数（§2.8）：异常行用控制字符制造——rtsp://
    // 在条目侧是合法目标（§2.1：清单侧才不允许），不能拿来当异常。
    // N65：白名单外键触发换代后的新警告（取代 N63 的 headers ignored 旧串）
    {
        const auto out = parse(QStringLiteral(
            "#EXTM3U\n#EXTVLCOPT:network-caching=1000\n#EXTINF:-1,T\n\x01\n#EXTINF:-1,T2\n"));
        QVERIFY(out.entries.isEmpty());
        QCOMPARE(out.warnings.size(), 3);
        QVERIFY(out.warnings.contains(
            QStringLiteral("No playable entries were found in this playlist.")));
        QVERIFY(
            out.warnings.contains(QStringLiteral("Malformed lines were skipped in this list.")));
        QVERIFY(out.warnings.contains(QStringLiteral(
            "Some per-entry options in this playlist are not supported and were ignored.")));
    }
}

void TestExternalProvider::settingsRoundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString ini = QDir(dir.path()).filePath(QStringLiteral("t.ini"));
    {
        QSettings settings(ini, QSettings::IniFormat);
        ExternalProvider provider(QStringLiteral("http://example.com/list.m3u"), makeRegistry());
        provider.setSortRule(SortRule{SortKey::FileName, false});
        provider.saveSettings(settings);
        settings.sync();
    }
    QSettings settings(ini, QSettings::IniFormat);
    // 类型键经 writeProviderKey 单点写
    QCOMPARE(settings.value(QStringLiteral("provider")).toString(), QStringLiteral("external"));
    QCOMPARE(settings.value(QStringLiteral("extPath")).toString(),
             QStringLiteral("http://example.com/list.m3u"));
    // Unsupported ⇒ transit 键不落盘（基类只对 Optional 写）
    QVERIFY(!settings.contains(QStringLiteral("transit")));
    const auto loaded = std::dynamic_pointer_cast<ExternalProvider>(
        ExternalProvider::load(settings, makeRegistry()));
    QVERIFY(loaded);
    QCOMPARE(loaded->sourcePath(), QStringLiteral("http://example.com/list.m3u"));
    QCOMPARE(loaded->sortRule().key, SortKey::FileName);
    QCOMPARE(loaded->sortRule().ascending, false);
}

QTEST_MAIN(TestExternalProvider)
#include "tst_ExternalProvider.moc"

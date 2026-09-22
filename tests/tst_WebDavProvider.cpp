// WebDavProvider 单元测试（N37）：multistatus 解析纯函数夹具——直调
// WebDavProvider::parseMultistatus，纯逻辑、不碰网络。覆盖：正常条目（D: 前缀与
// 无前缀两种写法）/ 空 multistatus / 无 resourcetype / 绝对 href / 带百分号编码
// href / 目录自条目跳过 / 非法 XML / 畸形数值属性 / 无凭据 URL 保持简洁（不出现
// 空 userinfo）/ collection href 缺尾斜杠被补齐。
// N46 增补：容器 id 的无凭据形态构造（containerId）与浏览模式 INI 取值
// （browseModeFromInt）。
// N48 增补：条目恒无凭据（解析侧期望由"嵌入"改为"剥离"，自条目判定在带凭据基
// URL 下仍须跳过）、playUrl 直发形态的注入与剥离往返不变式、非 http(s) href 跳过并
// 合并告警。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R WebDavProvider 跑本套件。

#include "pch.h"

#include <QtTest/QtTest>

#include "ServiceRegistry.h"
#include "Util.h"
#include "WebDavProvider.h"

namespace {
// 基 URL 与凭据（与断言中的期望身份串对应）
const QString kBase = QStringLiteral("http://server/dav/");
const QString kUser = QStringLiteral("u");
const QString kPassword = QStringLiteral("pw");

QUrl baseUrlWithCreds() {
    QUrl url(kBase, QUrl::ParsingMode::TolerantMode);
    url.setUserName(kUser);
    url.setPassword(kPassword);
    return url;
}

// N48：带凭据的提供者实例（枚举路径不参与，凭据只用于 directPlayUrl（经 playUrl 直发形态））
std::shared_ptr<WebDavProvider> providerWith(const QString &user, const QString &password) {
    return std::make_shared<WebDavProvider>(kBase, user, password,
                                            WebDavProvider::BrowseMode::Layered, QStringList(),
                                            std::make_shared<ServiceRegistry>());
}

// 无凭据的期望形态（条目恒无凭据，凭据皆空则不设）
QString expectUrl(const QString &path) { return kBase + path; }

// 带凭据的期望形态（containerId 的入参形态：剥离前的串）
QString expectUrlAuth(const QString &path) {
    return QStringLiteral("http://u:pw@server/dav/") + path;
}
} // namespace

class TestWebDavProvider : public QObject {
    Q_OBJECT

  private slots:
    // 正常条目：D: 前缀写法——文件（size + modified + 无 collection）与子目录分列
    void normalEntriesPrefixed();
    // 无前缀写法（默认命名空间）：解析结果与前缀写法一致——前缀本身不可依赖
    void normalEntriesPrefixless();
    // 空 multistatus：成功返回空产出
    void emptyMultistatusYieldsNothing();
    // 无 resourcetype：按普通文件处理（非 collection）
    void noResourcetypeIsFile();
    // 绝对 href：直接采用（N48：凭据不进条目）
    void absoluteHrefAdopted();
    // 无凭据：绝对 href 自带的 userinfo 须被剥除（凭据来源唯一——皆空即无凭据）
    void absoluteHrefCredentialsStrippedWhenNoCreds();
    // 无凭据 + 相对 href（服务端最常见形状）：不得凭空写入空的 userinfo 段
    // ——那会产出 "http://:@server/…" 这种非标准形态（N37 实施修订）
    void noCredsKeepsUrlClean();
    // collection 的 href 省略尾斜杠（RFC 4918 §5.2 / §8.3 为 SHOULD 级，现实常省略）：
    // 解析侧补齐，否则它作下一跳基 URL 时 resolved 会丢一层目录
    void collectionHrefWithoutTrailingSlashIsNormalized();
    // 命名空间缺失（服务端省略 xmlns 的畸形报文）：回退纯 localName 匹配，
    // 不被整树 skip 吞成"空成功"
    void noNamespaceFallback();
    // 带百分号编码 href：编码形态原样保留在身份串中（中文文件名场景）
    void percentEncodedHrefPreserved();
    // 目录自条目：与请求 URL 等价（带 / 不带尾斜杠两种写法）一律跳过
    void selfEntriesSkipped();
    // 非法 XML：返回 unexpected
    void invalidXmlIsUnexpected();
    // 畸形数值属性：getcontentlength 非数字按"值未知"处理，不产出、不崩溃
    void malformedPropsAreUnknown();
    // 真实服务端形状（rclone serve webdav）：multistatus 包裹层 + 属性嵌于
    // propstat/prop + 目录自条目带双 propstat（404 属性块）+ collection 内联
    // 重声明 xmlns + <D:status> 元素——防"flat 扫描 / 下吞包裹层"回归
    void rcloneShapedResponse();
    // N44：URL 定形静态纯函数——trim、补尾斜杠、尾斜杠只补在 path 上、幂等。
    // 合并后枚举侧"给 collection 补尾斜杠"同用此函数
    void normalizeRootUrlShapesInputAndIsIdempotent();
    // N44：validateInput 三条（空 / 非 http(s) / 内嵌 userinfo）与合法 URL 通过
    void validateInputCoversEmptySchemeAndCredentials();
    // N44：parseExtensions 口径（与目录页同形，两份实现）
    void parseExtensionsNormalizesTokens();
    // N46：容器 id = collection 身份串的无凭据形态——剥 userinfo、编码形态
    // 与尾斜杠原样保留，持久化进 queueRoot 不携带凭据（N46 §2.4）
    void containerIdStripsCredentialsKeepsTrailingSlash();
    // N46：浏览模式的 INI int 取值映射，取值域外降级分层浏览（新建列表默认）
    void browseModeFromIntMapsAndFallsBack();
    // N48：喂 mpv 的播放形态——authority 起点注入凭据，strip(embed(u)) == u 往返
    // 不变式（含分隔符 / 空格 / 非 ASCII / sub-delims 类字符的凭据），空凭据恒等、
    // path 中的 '@' 不受影响、非 http(s) 不注入
    void directPlayUrlInjectsAndRoundTrips();
    // N48：非 http(s) 的 href 不产出条目（实体与容器皆无）并回传一条合并告警——
    // 条目恒 http(s) 是凭据注入与剥离对称成立的前提
    void nonHttpHrefSkippedWithMergedWarning();
};

void TestWebDavProvider::normalEntriesPrefixed() {
    const QByteArray body = "<?xml version=\"1.0\"?>"
                            "<D:multistatus xmlns:D=\"DAV:\">"
                            "<D:response>"
                            "<D:href>/dav/a%20b.mp4</D:href>"
                            "<D:propstat><D:prop>"
                            "<D:getcontentlength>1234</D:getcontentlength>"
                            "<D:getlastmodified>Sun, 13 Sep 2026 10:00:00 GMT</D:getlastmodified>"
                            "<D:resourcetype/>"
                            "</D:prop></D:propstat>"
                            "</D:response>"
                            "<D:response>"
                            "<D:href>/dav/sub/</D:href>"
                            "<D:propstat><D:prop>"
                            "<D:resourcetype><D:collection/></D:resourcetype>"
                            "</D:prop></D:propstat>"
                            "</D:response>"
                            "</D:multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(result->files.size(), 1);
    QCOMPARE(result->collections.size(), 1);
    QCOMPARE(result->files.first().path, expectUrl(QStringLiteral("a%20b.mp4")));
    QVERIFY(result->files.first().size.has_value());
    QCOMPARE(*result->files.first().size, qint64(1234));
    // HTTP 日期解析正确（UTC 时刻恒定，本地时区无关）
    QVERIFY(result->files.first().modified.has_value());
    QCOMPARE(result->files.first().modified->toUTC(),
             QDateTime(QDate(2026, 9, 13), QTime(10, 0), QTimeZone::UTC));
    QCOMPARE(result->collections.first().path, expectUrl(QStringLiteral("sub/")));
}

void TestWebDavProvider::normalEntriesPrefixless() {
    // 默认命名空间声明 xmlns="DAV:"，元素无前缀——与 D: 前缀写法同解
    const QByteArray body = "<?xml version=\"1.0\"?>"
                            "<multistatus xmlns=\"DAV:\">"
                            "<response>"
                            "<href>/dav/c.mp4</href>"
                            "<propstat><prop>"
                            "<getcontentlength>7</getcontentlength>"
                            "</prop></propstat>"
                            "</response>"
                            "<response>"
                            "<href>/dav/dir/</href>"
                            "<propstat><prop>"
                            "<resourcetype><collection/></resourcetype>"
                            "</prop></propstat>"
                            "</response>"
                            "</multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(result->files.size(), 1);
    QCOMPARE(result->collections.size(), 1);
    QCOMPARE(result->files.first().path, expectUrl(QStringLiteral("c.mp4")));
    QCOMPARE(*result->files.first().size, qint64(7));
}

void TestWebDavProvider::emptyMultistatusYieldsNothing() {
    const QByteArray body = "<?xml version=\"1.0\"?><D:multistatus xmlns:D=\"DAV:\"/>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QVERIFY(result->files.isEmpty());
    QVERIFY(result->collections.isEmpty());
}

void TestWebDavProvider::noResourcetypeIsFile() {
    const QByteArray body = "<?xml version=\"1.0\"?>"
                            "<D:multistatus xmlns:D=\"DAV:\">"
                            "<D:response><D:href>/dav/bare.mkv</D:href></D:response>"
                            "</D:multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(result->files.size(), 1);
    QVERIFY(result->collections.isEmpty());
    QCOMPARE(result->files.first().path, expectUrl(QStringLiteral("bare.mkv")));
    // 未提供的属性 = 值未知
    QVERIFY(!result->files.first().size.has_value());
    QVERIFY(!result->files.first().modified.has_value());
}

void TestWebDavProvider::absoluteHrefAdopted() {
    const QByteArray body = "<?xml version=\"1.0\"?>"
                            "<D:multistatus xmlns:D=\"DAV:\">"
                            "<D:response>"
                            "<D:href>http://other-svc/media/x.mp4</D:href>"
                            "</D:response>"
                            "</D:multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(result->files.size(), 1);
    // 绝对 href 直接采用（不继承基路径）；N48：凭据不进条目——即便提供者配了凭据，
    // 条目的 userinfo 也必须为空（凭据只在请求与播放时注入）
    QCOMPARE(result->files.first().path, QStringLiteral("http://other-svc/media/x.mp4"));
}

void TestWebDavProvider::absoluteHrefCredentialsStrippedWhenNoCreds() {
    const QByteArray body =
        "<?xml version=\"1.0\"?>"
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:href>http://evil:pw@other-svc/media/x.mp4</D:href></D:response>"
        "</D:multistatus>";
    // 提供者无凭据：href 自带的 userinfo 必须被剥除，不进身份串（不落盘 /
    // 不回刷）——顺带钉死"对已有 userinfo 的 URL，空串赋值即清空且不残留空段"
    // 这一 Qt 行为（与 noCredsKeepsUrlClean 成对：本无 userinfo 时不得凭空写入）。
    // N48：剥离已与"提供者是否配了凭据"无关（条目恒无凭据），本用例守住 href 侧
    QUrl base(QStringLiteral("http://server/dav/"), QUrl::ParsingMode::TolerantMode);
    const auto result = WebDavProvider::parseMultistatus(body, base);
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(result->files.size(), 1);
    QCOMPARE(result->files.first().path, QStringLiteral("http://other-svc/media/x.mp4"));
}

void TestWebDavProvider::noCredsKeepsUrlClean() {
    // 无凭据时若无条件写入空 userinfo，QUrl 会把该段标记为 present 且空，
    // 序列化产出 "http://:@server/…"——既非标准也不简洁（N37 实施修订）
    const QByteArray body = "<?xml version=\"1.0\"?>"
                            "<D:multistatus xmlns:D=\"DAV:\">"
                            "<D:response><D:href>/dav/a.mp4</D:href></D:response>"
                            "</D:multistatus>";
    QUrl base(kBase, QUrl::ParsingMode::TolerantMode); // 基无凭据
    const auto result = WebDavProvider::parseMultistatus(body, base);
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(result->files.size(), 1);
    QCOMPARE(result->files.first().path, expectUrl(QStringLiteral("a.mp4")));
    QVERIFY(!result->files.first().path.contains(QLatin1Char('@')));
}

void TestWebDavProvider::collectionHrefWithoutTrailingSlashIsNormalized() {
    const QByteArray body =
        "<?xml version=\"1.0\"?>"
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:href>/dav/sub</D:href>"
        "<D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop>"
        "</D:propstat></D:response>"
        "</D:multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QVERIFY(result->files.isEmpty());
    QCOMPARE(result->collections.size(), 1);
    // 补齐尾斜杠：它作下一跳 PROPFIND 的基 URL，缺斜杠时 resolved 会丢一层目录
    QCOMPARE(result->collections.first().path, expectUrl(QStringLiteral("sub/")));
}

void TestWebDavProvider::noNamespaceFallback() {
    // 省略 xmlns 的畸形报文：命名空间为空时回退纯 localName 匹配——
    // 否则全部 response 被整树 skip，静默返回"空成功"
    const QByteArray body =
        "<?xml version=\"1.0\"?>"
        "<multistatus>"
        "<response><href>/dav/x.mp4</href>"
        "<propstat><prop><getcontentlength>5</getcontentlength></prop></propstat>"
        "</response>"
        "</multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(result->files.size(), 1);
    QCOMPARE(result->files.first().path, expectUrl(QStringLiteral("x.mp4")));
    QCOMPARE(*result->files.first().size, qint64(5));
}

void TestWebDavProvider::percentEncodedHrefPreserved() {
    const QByteArray body =
        "<?xml version=\"1.0\"?>"
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:href>/dav/%E4%B8%AD%E6%96%87%20name.mp4</D:href></D:response>"
        "</D:multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(result->files.size(), 1);
    // 身份串恒为编码形态（显示名由实体层 percent-decode，不在解析层做）
    QCOMPARE(result->files.first().path,
             expectUrl(QStringLiteral("%E4%B8%AD%E6%96%87%20name.mp4")));
}

void TestWebDavProvider::selfEntriesSkipped() {
    const QByteArray body =
        "<?xml version=\"1.0\"?>"
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:href>/dav/</D:href>"
        "<D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop>"
        "</D:propstat></D:response>"
        "<D:response><D:href>/dav</D:href></D:response>"
        "</D:multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    // 两种自条目写法（带 / 不带尾斜杠）都不产出
    QVERIFY(result->files.isEmpty());
    QVERIFY(result->collections.isEmpty());
}

void TestWebDavProvider::invalidXmlIsUnexpected() {
    // 未闭合的根元素：hasError 命中，返回 unexpected
    const QByteArray body = "<D:multistatus xmlns:D=\"DAV:\"><D:response>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    QVERIFY(!result.has_value());
    QVERIFY(!result.error().isEmpty());
}

void TestWebDavProvider::malformedPropsAreUnknown() {
    const QByteArray body = "<?xml version=\"1.0\"?>"
                            "<D:multistatus xmlns:D=\"DAV:\">"
                            "<D:response>"
                            "<D:href>/dav/odd.mp4</D:href>"
                            "<D:propstat><D:prop>"
                            "<D:getcontentlength>not-a-number</D:getcontentlength>"
                            "<D:getlastmodified>garbage date</D:getlastmodified>"
                            "</D:prop></D:propstat>"
                            "</D:response>"
                            "</D:multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(result->files.size(), 1);
    // 两个属性都解析失败 = 值未知（不产出、不崩溃）
    QVERIFY(!result->files.first().size.has_value());
    QVERIFY(!result->files.first().modified.has_value());
}

void TestWebDavProvider::rcloneShapedResponse() {
    // 逐字节取自 rclone serve webdav 的真实应答结构（属性名 / 嵌套层次 /
    // 自条目双 propstat 全保留）。曾实测两个缺陷都只在此形状暴露：
    // multistatus 包裹层被整树 skip（产出恒空）、属性嵌 propstat/prop 未下探
    // （size/modified 恒未知、目录误判文件）。
    // 基 URL 与抓取时一致（服务器根），自条目 href "/" 才能按 resolved 展开
    // 与请求 URL 等价而被跳过——绝对路径 "/" 会替换基 URL 的整个 path，
    // 若基带子路径（如 /dav/）则展开为服务器根，属不同资源不属自条目
    const QUrl base = [] {
        QUrl url(QStringLiteral("http://server/"), QUrl::ParsingMode::TolerantMode);
        url.setUserName(kUser);
        url.setPassword(kPassword);
        return url;
    }();
    const QByteArray body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                            "<D:multistatus xmlns:D=\"DAV:\">"
                            "<D:response><D:href>/</D:href><D:propstat><D:prop>"
                            "<D:getlastmodified>Sun, 13 Sep 2026 12:00:46 GMT</D:getlastmodified>"
                            "<D:resourcetype><D:collection xmlns:D=\"DAV:\"/></D:resourcetype>"
                            "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
                            "<D:propstat><D:prop><D:getcontentlength></D:getcontentlength></D:prop>"
                            "<D:status>HTTP/1.1 404 Not Found</D:status></D:propstat></D:response>"
                            "<D:response><D:href>/dump-media-info.lua</D:href><D:propstat><D:prop>"
                            "<D:getcontentlength>2683</D:getcontentlength>"
                            "<D:getlastmodified>Wed, 26 Aug 2026 13:14:02 GMT</D:getlastmodified>"
                            "<D:resourcetype></D:resourcetype></D:prop>"
                            "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
                            "<D:response><D:href>/test1.mkv</D:href><D:propstat><D:prop>"
                            "<D:getcontentlength>7067475</D:getcontentlength>"
                            "<D:getlastmodified>Thu, 27 Aug 2026 06:42:55 GMT</D:getlastmodified>"
                            "<D:resourcetype></D:resourcetype></D:prop>"
                            "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
                            "<D:response><D:href>/test2.mp4</D:href><D:propstat><D:prop>"
                            "<D:getcontentlength>7195105</D:getcontentlength>"
                            "<D:getlastmodified>Mon, 12 Jan 2026 09:52:57 GMT</D:getlastmodified>"
                            "<D:resourcetype></D:resourcetype></D:prop>"
                            "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
                            "</D:multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, base);
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    // 目录自条目（href "/"）跳过；其余 3 条全为文件，无子目录误判
    QCOMPARE(result->files.size(), 3);
    QVERIFY(result->collections.isEmpty());
    // 属性自 propstat/prop 包裹层正确取得；自条目的 404 属性块（空
    // getcontentlength）不误产出
    QCOMPARE(result->files.first().path, QStringLiteral("http://server/dump-media-info.lua"));
    QCOMPARE(*result->files.first().size, qint64(2683));
    QVERIFY(result->files.first().modified.has_value());
    const auto &mkv = result->files.at(1);
    QCOMPARE(mkv.path, QStringLiteral("http://server/test1.mkv"));
    QCOMPARE(*mkv.size, qint64(7067475));
    const auto &mp4 = result->files.at(2);
    QCOMPARE(mp4.path, QStringLiteral("http://server/test2.mp4"));
    QCOMPARE(*mp4.size, qint64(7195105));
}

// N44：URL 定形是页内取值与 hasChanges 判定的唯一出口；幂等是硬要求——
// 已定形的 URL 再定形必须结果不变，否则回显未改动会被误判成有改动
void TestWebDavProvider::normalizeRootUrlShapesInputAndIsIdempotent() {
    // 补尾斜杠（基目录语义）
    QCOMPARE(WebDavProvider::normalizeRootUrl(QStringLiteral("http://host/dav")),
             QStringLiteral("http://host/dav/"));
    // trim
    QCOMPARE(WebDavProvider::normalizeRootUrl(QStringLiteral("  http://host/dav  ")),
             QStringLiteral("http://host/dav/"));
    // 尾斜杠只补在 path 上：不塞进 query / fragment
    QCOMPARE(WebDavProvider::normalizeRootUrl(QStringLiteral("http://host/dav?token=1")),
             QStringLiteral("http://host/dav/?token=1"));
    QCOMPARE(WebDavProvider::normalizeRootUrl(QStringLiteral("http://host/dav#frag")),
             QStringLiteral("http://host/dav/#frag"));
    // 已有尾斜杠不重复补
    QCOMPARE(WebDavProvider::normalizeRootUrl(QStringLiteral("http://host/dav/")),
             QStringLiteral("http://host/dav/"));
    // 空输入（含纯空白）原样为空
    QVERIFY(WebDavProvider::normalizeRootUrl(QString()).isEmpty());
    QVERIFY(WebDavProvider::normalizeRootUrl(QStringLiteral("   ")).isEmpty());
    // 幂等：定形结果再定形不变
    const QString shaped = WebDavProvider::normalizeRootUrl(QStringLiteral("http://host/dav"));
    QCOMPARE(WebDavProvider::normalizeRootUrl(shaped), shaped);
    const QString withQuery =
        WebDavProvider::normalizeRootUrl(QStringLiteral("http://host/dav?token=1"));
    QCOMPARE(WebDavProvider::normalizeRootUrl(withQuery), withQuery);
}

// N44：三条校验与重构前逐条一致；不做连通性探测（N37 口径不变）
void TestWebDavProvider::validateInputCoversEmptySchemeAndCredentials() {
    // 空 ⇒ 报错
    QVERIFY(WebDavProvider::validateInput(QString()).has_value());
    // 非 http(s) ⇒ 报错（含无 scheme 的裸主机名）
    QVERIFY(WebDavProvider::validateInput(QStringLiteral("ftp://host/dav/")).has_value());
    QVERIFY(WebDavProvider::validateInput(QStringLiteral("host/dav")).has_value());
    // 内嵌 userinfo ⇒ 报错（凭据只走用户名 / 密码栏，避免两个来源）
    QVERIFY(WebDavProvider::validateInput(QStringLiteral("http://u:pw@host/dav/")).has_value());
    // 合法 http(s) ⇒ 通过
    QVERIFY(!WebDavProvider::validateInput(QStringLiteral("http://host/dav/")).has_value());
    QVERIFY(!WebDavProvider::validateInput(QStringLiteral("https://host/dav/")).has_value());
    // 入参可以是未定形的原始文本（函数内部自行定形）
    QVERIFY(!WebDavProvider::validateInput(QStringLiteral("https://host/dav")).has_value());
}

// N44：扩展名编辑区文本 → 规范清单；空表即不过滤
void TestWebDavProvider::parseExtensionsNormalizesTokens() {
    QCOMPARE(WebDavProvider::parseExtensions(QStringLiteral("MP4 .mkv   avi avi .TS")),
             QStringList({QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"),
                          QStringLiteral("ts")}));
    QCOMPARE(WebDavProvider::parseExtensions(QStringLiteral(".WebM")),
             QStringList({QStringLiteral("webm")}));
    QVERIFY(WebDavProvider::parseExtensions(QString()).isEmpty());
    QVERIFY(WebDavProvider::parseExtensions(QStringLiteral("  \t ")).isEmpty());
}

// N46 §2.4：容器 id = collection 条目身份串的无凭据形态。N48 起条目身份已恒
// 无凭据（resolveEntryUrl 恒剥），本函数对现产出即恒等；剥离动作仍保留（id 是持久化
// 进 queueRoot 与作请求基 URL 的身份，形态恒定是硬要求）。编码形态（%20 / 中文）与
// collection 尾斜杠原样保留
void TestWebDavProvider::containerIdStripsCredentialsKeepsTrailingSlash() {
    // 带凭据身份串 → 剥 userinfo，尾斜杠保留
    QCOMPARE(WebDavProvider::containerId(expectUrlAuth(QStringLiteral("sub/"))),
             expectUrl(QStringLiteral("sub/")));
    // 无凭据形态输入原样等价（本无 userinfo 不得凭空写入空段）
    QCOMPARE(WebDavProvider::containerId(expectUrl(QStringLiteral("sub/"))),
             expectUrl(QStringLiteral("sub/")));
    // 绝对 href（他主机）同样只剥凭据、URL 原样——id 即解析期构造的 URL
    QCOMPARE(WebDavProvider::containerId(QStringLiteral("http://evil:pw@other-svc/media/")),
             QStringLiteral("http://other-svc/media/"));
    // 编码形态原样保留（中文 / 空格目录名场景）
    QCOMPARE(
        WebDavProvider::containerId(expectUrlAuth(QStringLiteral("%E4%B8%AD%E6%96%87%20name/"))),
        expectUrl(QStringLiteral("%E4%B8%AD%E6%96%87%20name/")));
    // 产出恒不含 userinfo
    QVERIFY(!WebDavProvider::containerId(expectUrlAuth(QStringLiteral("sub/")))
                 .contains(QLatin1Char('@')));
}

// N46 §2.1：INI 的 int 取值 → 浏览模式；取值域外（损坏 / 未知值）降级 Layered
void TestWebDavProvider::browseModeFromIntMapsAndFallsBack() {
    using BM = WebDavProvider::BrowseMode;
    QCOMPARE(WebDavProvider::browseModeFromInt(0), BM::Layered);
    QCOMPARE(WebDavProvider::browseModeFromInt(1), BM::FilesOnly);
    QCOMPARE(WebDavProvider::browseModeFromInt(2), BM::FilesRecursive);
    QCOMPARE(WebDavProvider::browseModeFromInt(-1), BM::Layered);
    QCOMPARE(WebDavProvider::browseModeFromInt(3), BM::Layered);
    QCOMPARE(WebDavProvider::browseModeFromInt(99), BM::Layered);
}

// N48：喂 mpv 的播放形态——authority 起点插入 `编码后user:编码后pass@`，剥离即还原
// （往返不变式构造性成立、剥离幂等，§2.5 的对称施加会重复剥）。凭据皆空恒等返回
void TestWebDavProvider::directPlayUrlInjectsAndRoundTrips() {
    // 空凭据：恒等（不注入、不产出空 userinfo 段）
    const auto plain = providerWith(QString(), QString());
    QCOMPARE(plain->playUrl(expectUrl(QStringLiteral("a.mp4"))),
             expectUrl(QStringLiteral("a.mp4")));

    // 含分隔符 / 空格 / 非 ASCII / sub-delims 的凭据：注入后剥离必须逐字还原
    const QString clean = expectUrl(QStringLiteral("sub/a%20b.mp4"));
    const auto tricky = providerWith(QStringLiteral("u@x"), QStringLiteral("p:w@ss/word+!$&=% 中"));
    const QString played = tricky->playUrl(clean);
    QVERIFY(played != clean);
    QVERIFY(played.startsWith(QStringLiteral("http://")));
    QVERIFY(played.endsWith(QStringLiteral("@server/dav/sub/a%20b.mp4")));
    QCOMPARE(Util::Url::stripCredentials(played), clean);
    // 注入段的编码形态与既有 QUrl 路径逐字相同（对拍：同一 URL 走 setUserName /
    // setPassword + FullyEncoded，比 prefix 到首个 '@' 为止的编码字节）
    {
        const QString u = QStringLiteral("u@x");
        const QString p = QStringLiteral("p:w@ss/word+!$&=% 中");
        QUrl ref(clean, QUrl::ParsingMode::TolerantMode);
        ref.setUserName(u);
        ref.setPassword(p);
        const QString refStr = ref.toString(QUrl::ComponentFormattingOption::FullyEncoded);
        QCOMPARE(played.left(played.indexOf(QLatin1Char('@')) + 1),
                 refStr.left(refStr.indexOf(QLatin1Char('@')) + 1));
    }
    // 幂等：再剥一次结果不变（比较形态在回显侧与本地侧各剥一次）
    QCOMPARE(Util::Url::stripCredentials(Util::Url::stripCredentials(played)),
             Util::Url::stripCredentials(played));

    // path 中的 '@' 不受影响：定位规则只作用于 authority 段
    const QString atPath = expectUrl(QStringLiteral("a@b.mp4"));
    QCOMPARE(Util::Url::stripCredentials(tricky->playUrl(atPath)), atPath);

    // query 中的 '@' 同理（authority 终止于 '?'）
    const QString atQuery = expectUrl(QStringLiteral("a.mp4?token=x@y"));
    QCOMPARE(Util::Url::stripCredentials(tricky->playUrl(atQuery)), atQuery);

    // 非 http(s) 入参不注入（条目恒 http(s) 由解析侧保证，此处为同口径的守）
    QCOMPARE(tricky->playUrl(QStringLiteral("rtsp://host/live")),
             QStringLiteral("rtsp://host/live"));
}

// N48：非 http(s) 的 href 不产出条目（实体与容器皆无）并回传一条合并告警
void TestWebDavProvider::nonHttpHrefSkippedWithMergedWarning() {
    const QByteArray body =
        "<?xml version=\"1.0\"?>"
        "<D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:href>rtsp://cam/stream1</D:href></D:response>"
        "<D:response><D:href>ftp://host/a.mp4</D:href></D:response>"
        "<D:response><D:href>rtsp://cam/dir/</D:href>"
        "<D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop>"
        "</D:propstat></D:response>"
        "<D:response><D:href>/dav/ok.mp4</D:href></D:response>"
        "</D:multistatus>";
    const auto result = WebDavProvider::parseMultistatus(body, baseUrlWithCreds());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    // 跨协议条目一律跳过（文件与 collection 皆不产出），http(s) 条目照常
    QCOMPARE(result->files.size(), 1);
    QCOMPARE(result->files.first().path, expectUrl(QStringLiteral("ok.mp4")));
    QVERIFY(result->collections.isEmpty());
    // 同层同类合并为一条（含首个示例 URL，且不含凭据——文案进消息框）
    QCOMPARE(result->warnings.size(), 1);
    QVERIFY(result->warnings.first().contains(QStringLiteral("rtsp://cam/stream1")));
    QVERIFY(!result->warnings.first().contains(QLatin1Char('@')));
}

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN），无 GUI 依赖
QTEST_GUILESS_MAIN(TestWebDavProvider)

#include "tst_WebDavProvider.moc"

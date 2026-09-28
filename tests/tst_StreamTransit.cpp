#include "pch.h"

#include <QtTest/QtTest>

#include "StreamTransit.h"

// N52 L1：mpvqt:// 语法纯函数的契约测试——往返、幂等、形态不合恒等与前缀判定。
// 全部为纯字符串断言，无 GUI，无网络。
// 注意：mint 的凭据拒绝分支由 Q_ASSERT 守候，Debug 构建下构造 userinfo 输入
// 会触发断言中止，故本套件不覆盖该分支（Release 行为由代码走查确认）。

class TestStreamTransit : public QObject {
    Q_OBJECT

  private slots:
    void roundtripPreservesIdentity();
    void roundtripUnicodeAndSpecials();
    void unwrapIdempotent();
    void unwrapMalformedReturnsAsIs();
    void isTransitUriPrefixOnly();
    void nilIdentityNeverMatches();
    void prefixNotHandTruncated();
};

// 列表身份（N57 §2.4）：入参一律 QUuid，字面串只是 mint 的产物与 INI 形态
static const QString kUuidText = QStringLiteral("0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0");
static const QUuid kUuid = QUuid::fromString(kUuidText);

void TestStreamTransit::roundtripPreservesIdentity() {
    // 身份 → URI → 身份 严格互逆
    const QStringList identities{
        QStringLiteral("https://dav.example.com/dav/a.mp4"),
        QStringLiteral("https://dav.example.com/dav/dir with space/b.mp3"),
        QStringLiteral("C:/Users/someone/Videos/c.mkv"),
        QStringLiteral("item-id-12345"),
    };
    for (const QString &id : identities) {
        const QString uri = StreamTransit::mint(kUuid, id);
        QVERIFY(!uri.isEmpty());
        QVERIFY(uri.startsWith(QLatin1String("mpvqt://")));
        QCOMPARE(StreamTransit::unwrap(uri), id);
    }
}

void TestStreamTransit::roundtripUnicodeAndSpecials() {
    // '/' 编成 %2F（整段只占一个路径段）；中文与空格经 UTF-8 百分号编码往返无损
    const QString id = QStringLiteral("https://dav.example.com/dav/中文 目录/影片 第1集.mp4");
    const QString uri = StreamTransit::mint(kUuid, id);
    QVERIFY(!uri.contains(QStringLiteral("中文")));
    QCOMPARE(uri.count(u'/'), 3); // scheme 后两个 + 条目段分隔一个（段内 '/' 已编码）
    QCOMPARE(StreamTransit::unwrap(uri), id);
}

void TestStreamTransit::unwrapIdempotent() {
    const QString id = QStringLiteral("https://dav.example.com/dav/a.mp4");
    const QString uri = StreamTransit::mint(kUuid, id);
    const QString once = StreamTransit::unwrap(uri);
    QCOMPARE(StreamTransit::unwrap(once), once);
}

void TestStreamTransit::unwrapMalformedReturnsAsIs() {
    // 非本协议 / 无条目段 / authority 空：一律原样返回
    const QStringList malformed{
        QStringLiteral(""),
        QStringLiteral("https://dav.example.com/dav/a.mp4"),
        QStringLiteral("mpvqt:"),
        QStringLiteral("mpvqt://"),
        QStringLiteral("mpvqt://") + kUuidText,
        QStringLiteral("mpvqt://") + kUuidText + QStringLiteral("/"),
        QStringLiteral("mpvqt:///"),
    };
    for (const QString &p : malformed)
        QCOMPARE(StreamTransit::unwrap(p), p);
}

void TestStreamTransit::isTransitUriPrefixOnly() {
    QVERIFY(StreamTransit::isTransitUri(QStringLiteral("mpvqt://abc/def")));
    QVERIFY(StreamTransit::isTransitUri(QStringLiteral("MPVQT://abc/def"))); // 大小写不敏感
    QVERIFY(!StreamTransit::isTransitUri(QStringLiteral("xmpvqt://abc/def")));
    QVERIFY(!StreamTransit::isTransitUri(QStringLiteral("mpvqt:/abc")));
    QVERIFY(!StreamTransit::isTransitUri(QStringLiteral("https://example.com/mpvqt://x")));
}

void TestStreamTransit::nilIdentityNeverMatches() {
    // N57 §2.4：身份入参改 QUuid 后，"传空串即拒铸"这一档换为 **nil 身份的往返判等**
    // ——mint / playUrl 入口的 nil 守候是 Q_ASSERT，Debug 下先中止，Release 的返空分支
    // 不可与之共存于同一用例，故本用例根本不传 nil 去铸造。
    // 空串与非法串一律解析为 nil（全零 = 无身份；INI 缺键即走这条路）
    QVERIFY(QUuid::fromString(QString()).isNull());
    QVERIFY(QUuid::fromString(QStringLiteral("nope")).isNull());
    QVERIFY(QUuid{} != kUuid); // nil 恒不命中任何在册身份
    // 可观察差分②：手改大小写与带花括号的存量值自本版起归一命中（旧为 toLower 串比，不认）
    QVERIFY(QUuid::fromString(kUuidText.toUpper()) == kUuid);
    QVERIFY(QUuid::fromString(QStringLiteral("{%1}").arg(kUuidText)) == kUuid);
    // mint 与 listUuidOfUri 成对：URI 段取回的身份与铸造时所用同一
    const QString uri = StreamTransit::mint(kUuid, QStringLiteral("https://x/a"));
    QVERIFY(StreamTransit::listUuidOfUri(uri) == kUuid);
    QVERIFY(StreamTransit::listUuidOfUri(QStringLiteral("mpvqt://nope/x")).isNull());
    QVERIFY(StreamTransit::listUuidOfUri(QStringLiteral("https://x/a")).isNull());
    // 条目身份为空仍拒铸（纯数据判据，前面没有断言拦路）
    QVERIFY(StreamTransit::mint(kUuid, QString()).isEmpty());
}

void TestStreamTransit::prefixNotHandTruncated() {
    // N51 坑 1 的回归位：前缀长度吃掉 UUID 头两位时，unwrap 产出即错。
    // uuid 段本身含 '-' 与十六进制字符，一旦被截断往返必失配。
    const QString id = QStringLiteral("https://dav.example.com/dav/0123456789abcdef.mp4");
    const QString uri = StreamTransit::mint(kUuid, id);
    QCOMPARE(StreamTransit::unwrap(uri), id);
    QVERIFY(uri.mid(8).startsWith(kUuidText)); // 偏移 8 = sizeof("mpvqt://")-1
}

QTEST_GUILESS_MAIN(TestStreamTransit)
#include "tst_StreamTransit.moc"

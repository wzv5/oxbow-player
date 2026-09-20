#include "pch.h"

#include <QtTest/QtTest>

#include "StreamTransit.h"

// N52 L1：mpvqt:// 语法纯函数的契约测试——往返、幂等、形态不合恒等、逐文件
// 选项与前缀判定。全部为纯字符串断言，无 GUI，无网络。
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
    void perFileOptionsByScheme();
    void mintEmptyInputRefused();
    void prefixNotHandTruncated();
};

static const char *kUuid = "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";

void TestStreamTransit::roundtripPreservesIdentity() {
    // 身份 → URI → 身份 严格互逆
    const QStringList identities{
        QStringLiteral("https://dav.example.com/dav/a.mp4"),
        QStringLiteral("https://dav.example.com/dav/dir with space/b.mp3"),
        QStringLiteral("C:/Users/someone/Videos/c.mkv"),
        QStringLiteral("item-id-12345"),
    };
    for (const QString &id : identities) {
        const QString uri = StreamTransit::mint(QLatin1String(kUuid), id);
        QVERIFY(!uri.isEmpty());
        QVERIFY(uri.startsWith(QLatin1String("mpvqt://")));
        QCOMPARE(StreamTransit::unwrap(uri), id);
    }
}

void TestStreamTransit::roundtripUnicodeAndSpecials() {
    // '/' 编成 %2F（整段只占一个路径段）；中文与空格经 UTF-8 百分号编码往返无损
    const QString id = QStringLiteral("https://dav.example.com/dav/中文 目录/影片 第1集.mp4");
    const QString uri = StreamTransit::mint(QLatin1String(kUuid), id);
    QVERIFY(!uri.contains(QStringLiteral("中文")));
    QCOMPARE(uri.count(u'/'), 3); // scheme 后两个 + 条目段分隔一个（段内 '/' 已编码）
    QCOMPARE(StreamTransit::unwrap(uri), id);
}

void TestStreamTransit::unwrapIdempotent() {
    const QString id = QStringLiteral("https://dav.example.com/dav/a.mp4");
    const QString uri = StreamTransit::mint(QLatin1String(kUuid), id);
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
        QStringLiteral("mpvqt://") + QLatin1String(kUuid),
        QStringLiteral("mpvqt://") + QLatin1String(kUuid) + QStringLiteral("/"),
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

void TestStreamTransit::perFileOptionsByScheme() {
    const QString uri = StreamTransit::mint(QLatin1String(kUuid),
                                            QStringLiteral("https://dav.example.com/dav/a.mp4"));
    const QVariantMap opts = StreamTransit::perFileOptions(uri);
    QCOMPARE(opts.size(), 1);
    QCOMPARE(opts.value(QStringLiteral("cache")).toString(), QStringLiteral("yes"));

    QVERIFY(StreamTransit::perFileOptions(QStringLiteral("https://example.com/a.mp4")).isEmpty());
    QVERIFY(StreamTransit::perFileOptions(QStringLiteral("C:/dir/a.mp4")).isEmpty());
}

void TestStreamTransit::mintEmptyInputRefused() {
    // 空入参拒绝铸造（凭据拒绝分支由 Q_ASSERT 守候，Debug 下不构造该输入）
    QVERIFY(StreamTransit::mint(QString(), QStringLiteral("https://x/a")).isEmpty());
    QVERIFY(StreamTransit::mint(QLatin1String(kUuid), QString()).isEmpty());
}

void TestStreamTransit::prefixNotHandTruncated() {
    // N51 坑 1 的回归位：前缀长度吃掉 UUID 头两位时，unwrap 产出即错。
    // uuid 段本身含 '-' 与十六进制字符，一旦被截断往返必失配。
    const QString id = QStringLiteral("https://dav.example.com/dav/0123456789abcdef.mp4");
    const QString uri = StreamTransit::mint(QLatin1String(kUuid), id);
    QCOMPARE(StreamTransit::unwrap(uri), id);
    QVERIFY(uri.mid(8).startsWith(QLatin1String(kUuid))); // 偏移 8 = sizeof("mpvqt://")-1
}

QTEST_GUILESS_MAIN(TestStreamTransit)
#include "tst_StreamTransit.moc"

// UpdateCheck 纯函数族单元测试（N151）：只调 UpdateChecker.h 的 UpdateCheck:: 命名空间
// 函数，不实例化 QObject 服务。覆盖间隔回退、到期判定边界、latest release 解析的
// 合法与非法形。无 GUI 依赖，一类一可执行（R5）：
//   ctest --test-dir build -C Debug -R UpdateChecker 跑本套件；
//   oxbow_tests_UpdateChecker <槽名> 跑单个用例。

#include "pch.h"

#include <QtTest/QtTest>

#include "UpdateChecker.h"

class TestUpdateChecker : public QObject {
    Q_OBJECT

  private slots:
    // ——N151：intervalFromRaw 读侧只认 0/1/7，其余一律落 Daily——
    void intervalFallsBackToDaily();
    // ——N151：isDue 边界——从未检查、恰好到期、差一秒未到期、每 7 天、时钟回拨
    void dueByElapsedTime();
    // ——N151：parseLatestRelease 合法形——tag 去 v、published_at 保 UTC、html_url 入页
    void parsesLatestRelease();
    // ——N151：parseLatestRelease 非法形——坏 JSON、缺字段、tag 无 v、tag 带后缀、
    // 非法 published_at、非 https html_url
    void rejectsMalformedRelease();
};

void TestUpdateChecker::intervalFallsBackToDaily() {
    QCOMPARE(int(UpdateCheck::intervalFromRaw(0)), int(UpdateCheck::Interval::Off));
    QCOMPARE(int(UpdateCheck::intervalFromRaw(1)), int(UpdateCheck::Interval::Daily));
    QCOMPARE(int(UpdateCheck::intervalFromRaw(7)), int(UpdateCheck::Interval::Weekly));
    // 0/1/7 之外落 Daily：负值、未定义档位、大值
    QCOMPARE(int(UpdateCheck::intervalFromRaw(-3)), int(UpdateCheck::Interval::Daily));
    QCOMPARE(int(UpdateCheck::intervalFromRaw(2)), int(UpdateCheck::Interval::Daily));
    QCOMPARE(int(UpdateCheck::intervalFromRaw(30)), int(UpdateCheck::Interval::Daily));
}

void TestUpdateChecker::dueByElapsedTime() {
    using UpdateCheck::Interval;
    using UpdateCheck::isDue;
    const qint64 now = 1000000000;
    const qint64 day = 86400;
    // 从未检查（0 与负值）即到期；间隔为关恒不到期
    QVERIFY(isDue(0, Interval::Daily, now));
    QVERIFY(isDue(-5, Interval::Daily, now));
    QVERIFY(!isDue(0, Interval::Off, now));
    QVERIFY(!isDue(now, Interval::Off, now));
    // 恰好到期：now - last == 间隔
    QVERIFY(isDue(now - day, Interval::Daily, now));
    // 差一秒未到期
    QVERIFY(!isDue(now - day + 1, Interval::Daily, now));
    // 每 7 天：六天不到期、恰好七天到期
    QVERIFY(!isDue(now - 6 * day, Interval::Weekly, now));
    QVERIFY(isDue(now - 7 * day, Interval::Weekly, now));
    // 时钟回拨（last > now）按到期
    QVERIFY(isDue(now + 3600, Interval::Daily, now));
}

namespace {
// 合法 release 的基线 JSON：各非法形用例在此之上逐字段破坏
QByteArray validReleaseJson() {
    return R"({
        "tag_name": "v1.2.3",
        "published_at": "2026-11-01T08:30:00Z",
        "html_url": "https://github.com/wzv5/oxbow-player/releases/tag/v1.2.3",
        "draft": false,
        "prerelease": false
    })";
}
} // namespace

void TestUpdateChecker::parsesLatestRelease() {
    const auto release = UpdateCheck::parseLatestRelease(validReleaseJson());
    QVERIFY(release.has_value());
    QCOMPARE(release->version, QStringLiteral("1.2.3"));
    QCOMPARE(release->publishedAt.toSecsSinceEpoch(), qint64(1793521800));
    QCOMPARE(release->pageUrl.toString(),
             QStringLiteral("https://github.com/wzv5/oxbow-player/releases/tag/v1.2.3"));
}

void TestUpdateChecker::rejectsMalformedRelease() {
    const auto parse = [](const QByteArray &json) { return UpdateCheck::parseLatestRelease(json); };
    // 坏 JSON（裸串不进宏参：moc 的宏展开对多行裸串不认）
    const QByteArray badJson = R"({not json)";
    QVERIFY(!parse(badJson));
    // 缺字段：tag_name / published_at / html_url 各缺一次
    const QByteArray missingTag = R"({"published_at": "2026-11-01T08:30:00Z",
        "html_url": "https://github.com/wzv5/oxbow-player/releases/tag/v1.2.3"})";
    QVERIFY(!parse(missingTag));
    const QByteArray missingPublished = R"({"tag_name": "v1.2.3",
        "html_url": "https://github.com/wzv5/oxbow-player/releases/tag/v1.2.3"})";
    QVERIFY(!parse(missingPublished));
    const QByteArray missingUrl = R"({"tag_name": "v1.2.3",
        "published_at": "2026-11-01T08:30:00Z"})";
    QVERIFY(!parse(missingUrl));
    // tag 无 v 前缀
    const QByteArray tagNoV = R"({"tag_name": "1.2.3",
        "published_at": "2026-11-01T08:30:00Z",
        "html_url": "https://github.com/wzv5/oxbow-player/releases/tag/v1.2.3"})";
    QVERIFY(!parse(tagNoV));
    // tag 的 v 后不接数字
    const QByteArray tagNoDigit = R"({"tag_name": "vbeta",
        "published_at": "2026-11-01T08:30:00Z",
        "html_url": "https://github.com/wzv5/oxbow-player/releases/tag/vbeta"})";
    QVERIFY(!parse(tagNoDigit));
    // tag 带后缀：去 v 后非纯点分数字
    const QByteArray tagWithSuffix = R"({"tag_name": "v1.2.3-beta.1",
        "published_at": "2026-11-01T08:30:00Z",
        "html_url": "https://github.com/wzv5/oxbow-player/releases/tag/v1.2.3"})";
    QVERIFY(!parse(tagWithSuffix));
    // 非法 published_at
    const QByteArray badPublished = R"({"tag_name": "v1.2.3",
        "published_at": "yesterday",
        "html_url": "https://github.com/wzv5/oxbow-player/releases/tag/v1.2.3"})";
    QVERIFY(!parse(badPublished));
    // 非 https html_url
    const QByteArray httpUrl = R"({"tag_name": "v1.2.3",
        "published_at": "2026-11-01T08:30:00Z",
        "html_url": "http://github.com/wzv5/oxbow-player/releases/tag/v1.2.3"})";
    QVERIFY(!parse(httpUrl));
    // 顶层数组不是对象
    const QByteArray topArray = QByteArrayLiteral("[]");
    QVERIFY(!parse(topArray));
}

QTEST_GUILESS_MAIN(TestUpdateChecker)

#include "tst_UpdateChecker.moc"

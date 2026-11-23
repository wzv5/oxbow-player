// N179：ScriptListSpec 单元测试；<槽名> 跑单个用例

#include "pch.h"

#include <QtTest/QtTest>

#include "playlist/ScriptListSpec.h"

using namespace ScriptListSpec;

namespace {

QJsonObject makeItem(const QString &url) { return QJsonObject{{QLatin1String("url"), url}}; }

QByteArray wrapLayer(const QJsonValue &containers, const QJsonValue &items) {
    return QJsonDocument(QJsonObject{{QLatin1String("containers"), containers},
                                     {QLatin1String("items"), items}})
        .toJson(QJsonDocument::Compact);
}

// 本地绝对路径恒满足 isDefiniteForm
constexpr QLatin1String kLocalPath("C:\\media\\a.mkv");

} // namespace

class TestScriptListSpec : public QObject {
    Q_OBJECT

  private slots:
    void topLevelNotObjectIsInvalid() {
        QVERIFY(!parseLayer(QByteArrayLiteral("\"text\"")).valid);
        QVERIFY(!parseLayer(QByteArrayLiteral("[1,2]")).valid);
        QVERIFY(!parseLayer(QByteArrayLiteral("no json")).valid);
    }

    void arrayFieldTypeErrorIsInvalid() {
        QVERIFY(!parseLayer(wrapLayer(QLatin1String("not-array"), QJsonArray())).valid);
        QVERIFY(!parseLayer(wrapLayer(QJsonArray(), QLatin1String("not-array"))).valid);
    }

    void containerInvalidIdAndDuplicateKeepFirst() {
        const QJsonArray containers = {
            QJsonObject{{QLatin1String("id"), QLatin1String("s1")},
                        {QLatin1String("title"), QLatin1String("First")}},
            QJsonObject{{QLatin1String("id"), QLatin1String("s1")},
                        {QLatin1String("title"), QLatin1String("Second")}},
            QJsonObject{{QLatin1String("id"), QLatin1String("bad id!")}},
            QJsonObject{{QLatin1String("title"), QLatin1String("no id")}},
            QJsonObject{{QLatin1String("id"), QLatin1String("s2")}},
            QLatin1String("not-an-object"),
        };
        const LayerData layer = parseLayer(wrapLayer(containers, QJsonArray()));
        QVERIFY(layer.valid);
        QCOMPARE(layer.containers.size(), 2);
        QCOMPARE(layer.containers.at(0).id, QStringLiteral("s1"));
        QCOMPARE(layer.containers.at(0).title, QStringLiteral("First")); // 同层重复留首
        QCOMPARE(layer.containers.at(1).id, QStringLiteral("s2"));
        QVERIFY(layer.items.isEmpty());
    }

    void containerTitleDefaultsToId() {
        const QJsonArray containers = {
            QJsonObject{{QLatin1String("id"), QLatin1String("s1")}},
            // title 类型不符按未给，缺省落 id
            QJsonObject{{QLatin1String("id"), QLatin1String("s2")},
                        {QLatin1String("title"), 123},
                        {QLatin1String("path"), QLatin1String("display path")}},
        };
        const LayerData layer = parseLayer(wrapLayer(containers, QJsonArray()));
        QVERIFY(layer.valid);
        QCOMPARE(layer.containers.at(0).title, QStringLiteral("s1"));
        QCOMPARE(layer.containers.at(1).title, QStringLiteral("s2"));
        QCOMPARE(layer.containers.at(1).path, QStringLiteral("display path"));
    }

    void itemNonDefiniteUrlSkipped() {
        const QJsonArray items = {
            makeItem(QLatin1String("relative/path.mkv")), // 无盘符无 scheme：非定形
            makeItem(QLatin1String("")),
            QJsonObject{{QLatin1String("displayName"), QLatin1String("no url")}},
            makeItem(kLocalPath),
            makeItem(QLatin1String("https://host/a.mkv")),
        };
        const LayerData layer = parseLayer(wrapLayer(QJsonArray(), items));
        QVERIFY(layer.valid);
        QCOMPARE(layer.items.size(), 2);
        QCOMPARE(layer.skippedCount, 3);
    }

    void itemNamesEachDefaultToEmpty() {
        const QJsonArray items = {
            QJsonObject{{QLatin1String("url"), kLocalPath},
                        {QLatin1String("displayName"), QLatin1String("Show Name")},
                        {QLatin1String("filename"), QLatin1String("file.mkv")}},
            QJsonObject{{QLatin1String("url"), kLocalPath}},
            QJsonObject{{QLatin1String("url"), kLocalPath},
                        {QLatin1String("displayName"), QLatin1String("Only Display")}},
        };
        const LayerData layer = parseLayer(wrapLayer(QJsonArray(), items));
        QVERIFY(layer.valid);
        QCOMPARE(layer.items.at(0).displayName, QStringLiteral("Show Name"));
        QCOMPARE(layer.items.at(0).filename, QStringLiteral("file.mkv"));
        QVERIFY(layer.items.at(1).displayName.isEmpty());
        QVERIFY(layer.items.at(1).filename.isEmpty());
        QCOMPARE(layer.items.at(2).displayName, QStringLiteral("Only Display"));
        QVERIFY(layer.items.at(2).filename.isEmpty());
    }

    void optionsTakeScalarsOnly() {
        const QJsonArray items = {
            QJsonObject{{QLatin1String("url"), kLocalPath},
                        {QLatin1String("options"),
                         QJsonObject{{QLatin1String("start"), 42},
                                     {QLatin1String("volume"), 0.5},
                                     {QLatin1String("mute"), true},
                                     {QLatin1String("sub-file"), QLatin1String("C:\\media\\a.ass")},
                                     {QLatin1String("list"), QJsonArray{1, 2}},
                                     {QLatin1String("obj"), QJsonObject{}},
                                     {QLatin1String("nul"), QJsonValue()}}}}};
        const LayerData layer = parseLayer(wrapLayer(QJsonArray(), items));
        QVERIFY(layer.valid);
        const QVariantMap &options = layer.items.at(0).options;
        QCOMPARE(options.size(), 4);
        QCOMPARE(options.value(QStringLiteral("start")).toString(), QStringLiteral("42"));
        QCOMPARE(options.value(QStringLiteral("volume")).toString(), QStringLiteral("0.5"));
        QCOMPARE(options.value(QStringLiteral("mute")).toString(), QStringLiteral("true"));
        QCOMPARE(options.value(QStringLiteral("sub-file")).toString(),
                 QStringLiteral("C:\\media\\a.ass"));
    }

    void fileKeysGateOnTypeAndPositive() {
        const QJsonArray items = {
            QJsonObject{
                {QLatin1String("url"), kLocalPath},
                {QLatin1String("file"), QJsonObject{{QLatin1String("size"), 1234567890},
                                                    {QLatin1String("created"), 1763664000},
                                                    {QLatin1String("modified"), 1763700000}}}},
            QJsonObject{
                {QLatin1String("url"), kLocalPath},
                {QLatin1String("file"), QJsonObject{{QLatin1String("size"), QLatin1String("str")},
                                                    {QLatin1String("created"), -5},
                                                    {QLatin1String("modified"), 0},
                                                    {QLatin1String("unknown"), 1}}}},
        };
        const LayerData layer = parseLayer(wrapLayer(QJsonArray(), items));
        QVERIFY(layer.valid);
        QCOMPARE(layer.items.at(0).fileSize.value_or(0), 1234567890);
        QCOMPARE(layer.items.at(0).fileCreated.value_or(0), 1763664000);
        QCOMPARE(layer.items.at(0).fileModified.value_or(0), 1763700000);
        QVERIFY(!layer.items.at(1).fileSize.has_value());
        QVERIFY(!layer.items.at(1).fileCreated.has_value());
        QVERIFY(!layer.items.at(1).fileModified.has_value());
    }

    void metadataKeysGateAndAliasesIgnored() {
        const QJsonArray items = {
            QJsonObject{{QLatin1String("url"), kLocalPath},
                        {QLatin1String("metadata"),
                         QJsonObject{{QLatin1String("format"), QLatin1String("MPEG-4")},
                                     {QLatin1String("duration"), 1260},
                                     {QLatin1String("recordedDate"), 1763664000},
                                     {QLatin1String("title"), QLatin1String("Media Title")},
                                     {QLatin1String("album"), QLatin1String("Album")},
                                     {QLatin1String("performer"), QLatin1String("Uploader")},
                                     {QLatin1String("date"), QLatin1String("2026")},
                                     {QLatin1String("author"), QLatin1String("Writer")}}}},
            QJsonObject{{QLatin1String("url"), kLocalPath},
                        {QLatin1String("metadata"), QJsonObject{{QLatin1String("duration"), -1},
                                                                {QLatin1String("title"), 123}}}},
        };
        const LayerData layer = parseLayer(wrapLayer(QJsonArray(), items));
        QVERIFY(layer.valid);
        const ItemData &full = layer.items.at(0);
        QCOMPARE(full.metaFormat, QStringLiteral("MPEG-4"));
        QCOMPARE(full.metaDuration.value_or(0), 1260.0);
        QCOMPARE(full.metaRecordedDate.toSecsSinceEpoch(), 1763664000);
        QCOMPARE(full.metaTitle, QStringLiteral("Media Title"));
        QCOMPARE(full.metaAlbum, QStringLiteral("Album"));
        QCOMPARE(full.metaPerformer, QStringLiteral("Uploader"));
        const ItemData &bad = layer.items.at(1);
        QVERIFY(bad.metaFormat.isEmpty());
        QVERIFY(!bad.metaDuration.has_value());
        QVERIFY(!bad.metaRecordedDate.isValid());
        QVERIFY(bad.metaTitle.isEmpty()); // 类型不符按未给；别名键同不入册
        QVERIFY(bad.metaAlbum.isEmpty());
        QVERIFY(bad.metaPerformer.isEmpty());
    }

    void scriptErrorCarriedWholeLayer() {
        // 空串 / null 等同未给
        QVERIFY(parseLayer(QByteArrayLiteral("{\"error\":\"\"}")).error.isEmpty());
        QVERIFY(parseLayer(QByteArrayLiteral("{\"error\":null}")).error.isEmpty());
        // 非空：文案 trim 后带出，解析在此收口，内容不再产出
        const LayerData layer =
            parseLayer(QByteArrayLiteral("{\"error\":\"  yt-dlp is not enabled  \","
                                         "\"containers\":[{\"id\":\"s1\"}],"
                                         "\"items\":[{\"url\":\"C:\\\\media\\\\a.mkv\"}]}"));
        QVERIFY(layer.valid);
        QCOMPARE(layer.error, QStringLiteral("yt-dlp is not enabled"));
        QVERIFY(layer.containers.isEmpty());
        QVERIFY(layer.items.isEmpty());
        QCOMPARE(layer.skippedCount, 0);
    }

    void scriptErrorWinsOverFieldTypeError() {
        // 错误先于内容生效：同层的坏类型不再判整层非法
        const LayerData layer =
            parseLayer(QByteArrayLiteral("{\"error\":\"failed\",\"items\":\"not-array\"}"));
        QVERIFY(layer.valid);
        QCOMPARE(layer.error, QStringLiteral("failed"));
    }

    void outOfRangeNumbersMeanNotGiven() {
        // 越界不猜值：static_cast 出 INT64_MIN 一类的垃圾比"未给"更糟
        const QJsonArray items = {
            QJsonObject{{QLatin1String("url"), kLocalPath},
                        {QLatin1String("file"), QJsonObject{{QLatin1String("size"), 1e30},
                                                            {QLatin1String("created"), 9.3e18},
                                                            {QLatin1String("modified"), 9.2e18}}}},
            QJsonObject{
                {QLatin1String("url"), kLocalPath},
                {QLatin1String("metadata"), QJsonObject{{QLatin1String("recordedDate"), 1e30},
                                                        {QLatin1String("duration"), 1e30}}}},
        };
        const LayerData layer = parseLayer(wrapLayer(QJsonArray(), items));
        QVERIFY(layer.valid);
        QVERIFY(!layer.items.at(0).fileSize.has_value());
        QVERIFY(!layer.items.at(0).fileCreated.has_value());
        // 9.2e18 尚在 int64 内（上界取严格小于 2^63），不猜值也不误杀
        QVERIFY(layer.items.at(0).fileModified.has_value());
        QVERIFY(!layer.items.at(1).metaRecordedDate.isValid());
        QCOMPARE(layer.items.at(1).metaDuration.value_or(0), 1e30); // duration 留 double，不转换
    }

    void optionNumbersStayInRangeText() {
        // 超 int64 的数字不猜整数文本，落 double 形态
        const QJsonArray items = {
            QJsonObject{{QLatin1String("url"), kLocalPath},
                        {QLatin1String("options"), QJsonObject{{QLatin1String("big"), 1e30},
                                                               {QLatin1String("exact"), 42},
                                                               {QLatin1String("half"), 42.5}}}}};
        const LayerData layer = parseLayer(wrapLayer(QJsonArray(), items));
        QVERIFY(layer.valid);
        const QVariantMap &options = layer.items.at(0).options;
        QVERIFY(options.value(QStringLiteral("big")).toString().contains(QLatin1String("e+")));
        QCOMPARE(options.value(QStringLiteral("exact")).toString(), QStringLiteral("42"));
        QCOMPARE(options.value(QStringLiteral("half")).toString(), QStringLiteral("42.5"));
    }

    void scriptErrorWrongTypeIsInvalid() {
        QVERIFY(!parseLayer(QByteArrayLiteral("{\"error\":42}")).valid);
        QVERIFY(!parseLayer(QByteArrayLiteral("{\"error\":{}}")).valid);
    }

    void missingArraysMeanEmpty() {
        const LayerData layer =
            parseLayer(QJsonDocument(QJsonObject{}).toJson(QJsonDocument::Compact));
        QVERIFY(layer.valid);
        QVERIFY(layer.containers.isEmpty());
        QVERIFY(layer.items.isEmpty());
        QCOMPARE(layer.skippedCount, 0);
    }
};

QTEST_GUILESS_MAIN(TestScriptListSpec)

#include "tst_ScriptListSpec.moc"

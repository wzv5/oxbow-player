// N179：ScriptListProvider 单元测试；取层以 QtConcurrent 起、主线程投递应答

#include "pch.h"

#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest/QtTest>

#include "RecordingPort.h"
#include "ServiceRegistry.h"
#include "playlist/ScriptListChannel.h"
#include "playlist/ScriptListSpec.h"
#include "providers/ScriptListProvider.h"

using ScriptListSpec::eventName;
using ScriptListSpec::kEventNeedLayer;

namespace {

// 注册表 + 端口 + 通道 + demo 数据源；registerDemo=false 留给首载等注册用例，
// idMs 默认取短值免得每个用例都等满生产窗口
struct Stage {
    explicit Stage(int replyMs = 10000, int idMs = 50, bool registerDemo = true)
        : registry(std::make_shared<ServiceRegistry>()), port(std::make_shared<RecordingPort>()) {
        registry->provide<MpvApiPort>(port);
        channel = std::make_shared<ScriptListChannel>(registry, replyMs, idMs);
        registry->provide<ScriptListChannel>(channel);
        if (registerDemo)
            channel->registerList(QStringLiteral("demo"), QStringLiteral("owner-a"));
    }

    std::shared_ptr<ServiceRegistry> registry;
    std::shared_ptr<RecordingPort> port;
    std::shared_ptr<ScriptListChannel> channel;
};

// 完整注入面的一层；不用 raw string：moc 对含引号的多行字面量解析不稳
const char kFullLayer[] =
    "{"
    "  \"containers\": [ { \"id\": \"s1\", \"title\": \"Season 1\", \"path\": \"p\" } ],"
    "  \"items\": ["
    "    { \"url\": \"C:\\\\media\\\\a.mkv\", \"displayName\": \"Display Name\","
    "      \"filename\": \"custom.mkv\","
    "      \"options\": { \"start\": \"42\", \"sub-file\": \"C:\\\\media\\\\a.ass\" },"
    "      \"file\": { \"size\": 123, \"created\": 1763664000, \"modified\": 1763700000 },"
    "      \"metadata\": { \"format\": \"MPEG-4\", \"duration\": 1260,"
    "                     \"recordedDate\": 1763664000, \"title\": \"Media Title\","
    "                     \"album\": \"Album\", \"performer\": \"Uploader\" } },"
    "    { \"url\": \"https://host/episode.mkv\" }"
    "  ]"
    "}";

// 名字对照层：两名齐给 / 只给 displayName / 只给 filename
const char kNamesLayer[] =
    "{"
    "  \"items\": ["
    "    { \"url\": \"C:\\\\media\\\\a.mkv\", \"displayName\": \"Display Name\","
    "      \"filename\": \"custom.mkv\" },"
    "    { \"url\": \"C:\\\\media\\\\b.mkv\", \"displayName\": \"Only Display\" },"
    "    { \"url\": \"C:\\\\media\\\\c.mkv\", \"filename\": \"only-file.mkv\" }"
    "  ]"
    "}";

// 非定形 url 的跳过层
const char kSkippedLayer[] =
    "{ \"items\": [ { \"url\": \"bad/path.mkv\" }, { \"url\": \"C:\\\\ok.mkv\" } ] }";

} // namespace

class TestScriptListProvider : public QObject {
    Q_OBJECT

  private slots:
    void saveLoadRoundTripPreservesId() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("ini.ini"));
        {
            QSettings write(file, QSettings::IniFormat);
            ScriptListProvider provider(QStringLiteral("my-id"),
                                        std::make_shared<ServiceRegistry>());
            provider.saveSettings(write);
        }
        QSettings read(file, QSettings::IniFormat);
        QCOMPARE(read.value(QStringLiteral("provider")).toString(), QStringLiteral("script"));
        auto provider = ScriptListProvider::load(read, std::make_shared<ServiceRegistry>());
        QVERIFY(provider != nullptr);
        QCOMPARE(provider->typeKey(), QStringLiteral("script"));
        QCOMPARE(std::static_pointer_cast<ScriptListProvider>(provider)->scriptId(),
                 QStringLiteral("my-id"));
    }

    void loadHandlesMissingEmptyIllegalId() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("ini.ini"));
        // 缺失：组里只有 provider 键（writeProviderKey 的产物）
        {
            QSettings settings(file, QSettings::IniFormat);
            ScriptListProvider(QStringLiteral(""), std::make_shared<ServiceRegistry>())
                .saveSettings(settings);
        }
        QSettings missing(file, QSettings::IniFormat);
        auto provider = ScriptListProvider::load(missing, std::make_shared<ServiceRegistry>());
        QVERIFY(provider != nullptr); // 恒非空：回落静态表即失去 script 类型
        QVERIFY(std::static_pointer_cast<ScriptListProvider>(provider)->scriptId().isEmpty());

        QSettings empty(file, QSettings::IniFormat);
        empty.setValue(QStringLiteral("scriptId"), QString());
        provider = ScriptListProvider::load(empty, std::make_shared<ServiceRegistry>());
        QVERIFY(std::static_pointer_cast<ScriptListProvider>(provider)->scriptId().isEmpty());

        QSettings illegal(file, QSettings::IniFormat);
        illegal.setValue(QStringLiteral("scriptId"), QStringLiteral("bad id!"));
        provider = ScriptListProvider::load(illegal, std::make_shared<ServiceRegistry>());
        QVERIFY(std::static_pointer_cast<ScriptListProvider>(provider)->scriptId().isEmpty());
    }

    void capabilityBitsAndDefaultSort() {
        ScriptListProvider provider(QStringLiteral("demo"), std::make_shared<ServiceRegistry>());
        QVERIFY(!provider.supportsModify());
        QVERIFY(provider.supportsSort());
        const SortRule rule = provider.defaultSortRule();
        QCOMPARE(rule.key, SortKey::EnumerationOrder);
        QVERIFY(rule.ascending);
        QCOMPARE(provider.transitCapability(), PlaylistProvider::TransitCapability::Unsupported);
    }

    void enumerateBuildsEntitiesAndInjections() {
        Stage stage;
        ScriptListProvider provider(QStringLiteral("demo"), stage.registry);

        auto future = QtConcurrent::run([&provider] { return provider.enumerate(); });
        const SendRecord need = stage.port->takeOne();
        QCOMPARE(need.name, eventName(kEventNeedLayer));
        stage.channel->deliverLayer(QStringLiteral("owner-a"), need.args.at(1),
                                    QByteArray(kFullLayer));
        const auto reply = future.result();
        QVERIFY(reply.has_value());

        const PlaylistProvider::Enumeration &layer = *reply;
        QCOMPARE(layer.containers.size(), 1);
        QCOMPARE(layer.containers.at(0).id, QStringLiteral("s1"));
        QCOMPARE(layer.containers.at(0).title, QStringLiteral("Season 1"));
        QCOMPARE(layer.containers.at(0).path, QStringLiteral("p"));
        QVERIFY(layer.warnings.isEmpty());
        QCOMPARE(layer.entities.size(), 2);

        // 完整条目：三种名字各落各处，file / options / metadata 按注入生效
        const PlaylistEntity::Ptr &full = layer.entities.at(0);
        QCOMPARE(full->path(), QStringLiteral("C:\\media\\a.mkv"));
        QCOMPARE(full->fileName(), QStringLiteral("custom.mkv"));
        QCOMPARE(full->title(), QStringLiteral("Display Name"));
        QCOMPARE(full->fileMeta().size.value_or(0), 123);
        QCOMPARE(full->fileMeta().created->toSecsSinceEpoch(), 1763664000);
        QCOMPARE(full->fileMeta().modified->toSecsSinceEpoch(), 1763700000);
        QCOMPARE(full->playOptions().value(QStringLiteral("start")).toString(),
                 QStringLiteral("42"));
        QCOMPARE(full->playOptions().value(QStringLiteral("sub-file")).toString(),
                 QStringLiteral("C:\\media\\a.ass"));
        const PlaylistEntity::MediaMeta *meta = full->cachedMediaMeta(); // 注入视同已求值，缓存直读
        QVERIFY(meta != nullptr);
        QCOMPARE(meta->format, QStringLiteral("MPEG-4"));
        QCOMPARE(meta->duration.value_or(0), 1260.0);
        QCOMPARE(meta->recordedDate.toSecsSinceEpoch(), 1763664000);
        QCOMPARE(meta->title, QStringLiteral("Media Title")); // 媒体标题 ≠ 显示名
        QCOMPARE(meta->album, QStringLiteral("Album"));
        QCOMPARE(meta->performer, QStringLiteral("Uploader"));

        // 最小条目：fileName 实体自派生、title 联动为文件名，无注入缓存
        const PlaylistEntity::Ptr &minimal = layer.entities.at(1);
        QCOMPARE(minimal->fileName(), QStringLiteral("episode.mkv"));
        QCOMPARE(minimal->title(), QStringLiteral("episode.mkv"));
        QVERIFY(minimal->cachedFileMeta() == nullptr);
        QVERIFY(minimal->cachedMediaMeta() == nullptr);
        QVERIFY(minimal->playOptions().isEmpty());
    }

    void namesKeepTheirOwnSlots() {
        Stage stage;
        ScriptListProvider provider(QStringLiteral("demo"), stage.registry);

        auto future = QtConcurrent::run([&provider] { return provider.enumerate(); });
        const SendRecord need = stage.port->takeOne();
        stage.channel->deliverLayer(QStringLiteral("owner-a"), need.args.at(1),
                                    QByteArray(kNamesLayer));
        const auto reply = future.result();
        QVERIFY(reply.has_value());
        QCOMPARE(reply->entities.size(), 3);

        // 两名齐给：先 filename 后 displayName，显示名不被文件名冲掉
        const PlaylistEntity::Ptr &both = reply->entities.at(0);
        QCOMPARE(both->fileName(), QStringLiteral("custom.mkv"));
        QCOMPARE(both->title(), QStringLiteral("Display Name"));
        // 只给 displayName：文件名保持 url 末段（实体自派生）
        const PlaylistEntity::Ptr &displayOnly = reply->entities.at(1);
        QCOMPARE(displayOnly->fileName(), QStringLiteral("b.mkv"));
        QCOMPARE(displayOnly->title(), QStringLiteral("Only Display"));
        // 只给 filename：显示名按实体契约联动为文件名
        const PlaylistEntity::Ptr &fileOnly = reply->entities.at(2);
        QCOMPARE(fileOnly->fileName(), QStringLiteral("only-file.mkv"));
        QCOMPARE(fileOnly->title(), QStringLiteral("only-file.mkv"));
    }

    void skipWarningOnlyOnRoot() {
        Stage stage;
        ScriptListProvider provider(QStringLiteral("demo"), stage.registry);

        // 根层：跳过项 warning 归属根层
        auto rootFuture = QtConcurrent::run([&provider] { return provider.enumerate(); });
        const SendRecord rootNeed = stage.port->takeOne();
        stage.channel->deliverLayer(QStringLiteral("owner-a"), rootNeed.args.at(1),
                                    QByteArray(kSkippedLayer));
        const auto root = rootFuture.result();
        QVERIFY(root.has_value());
        QCOMPARE(root->entities.size(), 1);
        QCOMPARE(root->warnings.size(), 1);

        // 子层：缺席即失败口径，解释性文案只住根层
        auto subFuture = QtConcurrent::run(
            [&provider] { return provider.enumerate(QStringList{QStringLiteral("s1")}); });
        const SendRecord subNeed = stage.port->takeOne();
        QCOMPARE(subNeed.args.at(2), QStringLiteral("s1")); // parent-path 以 / 连接
        stage.channel->deliverLayer(QStringLiteral("owner-a"), subNeed.args.at(1),
                                    QByteArray(kSkippedLayer));
        const auto sub = subFuture.result();
        QVERIFY(sub.has_value());
        QCOMPARE(sub->entities.size(), 1);
        QVERIFY(sub->warnings.isEmpty());
    }

    void firstLoadWaitsForRegistration() {
        Stage stage(10000, 500, /*registerDemo=*/false); // 不注册 demo：首载须等它出现
        ScriptListProvider provider(QStringLiteral("demo"), stage.registry);

        auto future = QtConcurrent::run([&provider] { return provider.enumerate(); });
        QTest::qWait(20); // 先让等待进锁
        QVERIFY(stage.channel->registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        const SendRecord need = stage.port->takeOne(); // 等注册放行后才发 need-layer
        QCOMPARE(need.name, eventName(kEventNeedLayer));
        stage.channel->deliverLayer(QStringLiteral("owner-a"), need.args.at(1),
                                    QByteArray(kFullLayer));
        const auto reply = future.result();
        QVERIFY(reply.has_value());
        QCOMPARE(reply->entities.size(), 2);
    }

    void firstLoadWaitIsOneShot() {
        Stage stage(10000, 200, /*registerDemo=*/false); // 注入短 id 窗口，不注册 demo
        ScriptListProvider provider(QStringLiteral("demo"), stage.registry);
        const QString noScript = QStringLiteral("No script is providing this list.");

        QElapsedTimer timer;
        timer.start();
        auto first = provider.enumerate();
        QVERIFY(!first.has_value());
        QCOMPARE(first.error(), noScript);
        QVERIFY(timer.elapsed() >= 120); // 首次等满窗口

        timer.restart();
        auto second = provider.enumerate();
        QVERIFY(!second.has_value());
        QCOMPARE(second.error(), noScript);
        QVERIFY(timer.elapsed() < 100); // 之后不再等
    }

    void scriptErrorTextGoesToPlaceholder() {
        Stage stage;
        ScriptListProvider provider(QStringLiteral("demo"), stage.registry);
        const QString scriptError =
            QStringLiteral("yt-dlp is not enabled: set ytdl=yes to resolve this list.");

        auto future = QtConcurrent::run([&provider] { return provider.enumerate(); });
        const SendRecord need = stage.port->takeOne();
        stage.channel->deliverLayer(
            QStringLiteral("owner-a"), need.args.at(1),
            QJsonDocument(
                QJsonObject{{QStringLiteral("error"), scriptError},
                            {QStringLiteral("items"),
                             QJsonArray{QJsonObject{
                                 {QStringLiteral("url"), QStringLiteral("https://host/a.mkv")}}}}})
                .toJson(QJsonDocument::Compact)); // 错误优先：条目随之作废
        const auto reply = future.result();
        QVERIFY(!reply.has_value());
        QCOMPARE(reply.error(), scriptError); // 原样上屏，不经 translate
    }

    void errorTextMapping() {
        Stage stage;
        const auto registry = stage.registry;

        const QString noScript = QStringLiteral("No script is providing this list.");
        const QString gone = QStringLiteral("The script providing this list is no longer running.");
        const QString timeout = QStringLiteral("The script list did not respond in time.");
        const QString invalid = QStringLiteral("The script list returned invalid data.");

        // 未绑定 id：不发送直接占位
        ScriptListProvider unbound(QStringLiteral(""), registry);
        auto unboundReply = unbound.enumerate();
        QVERIFY(!unboundReply.has_value());
        QCOMPARE(unboundReply.error(), noScript);
        QCOMPARE(stage.port->recordCount(), 0);

        // id 在册但脚本未注册此 id（换绑后常见）
        ScriptListProvider unregistered(QStringLiteral("other"), registry);
        auto unregisteredReply = unregistered.enumerate();
        QVERIFY(!unregisteredReply.has_value());
        QCOMPARE(unregisteredReply.error(), noScript);

        // 发送失败（脚本退出后 script-message-to 报错）
        ScriptListProvider bound(QStringLiteral("demo"), registry);
        stage.port->setReturnCode(-5);
        auto sendFailedReply = bound.enumerate();
        QVERIFY(!sendFailedReply.has_value());
        QCOMPARE(sendFailedReply.error(), gone);
        stage.port->takeOne(); // 排水：发送失败的那次记录要消费掉，别留给下一用例
        stage.port->setReturnCode(0);

        // 数据非法：整层非法 JSON
        auto invalidFuture = QtConcurrent::run([&bound] { return bound.enumerate(); });
        const SendRecord need = stage.port->takeOne();
        stage.channel->deliverLayer(QStringLiteral("owner-a"), need.args.at(1),
                                    QByteArrayLiteral("[]"));
        auto invalidReply = invalidFuture.result();
        QVERIFY(!invalidReply.has_value());
        QCOMPARE(invalidReply.error(), invalid);

        // 超时：文案不含时长，注入短值即可，不必等满生产窗口
        auto slowPort = std::make_shared<RecordingPort>();
        registry->provide<MpvApiPort>(slowPort);
        auto slowChannel = std::make_shared<ScriptListChannel>(registry, 100);
        registry->provide<ScriptListChannel>(slowChannel); // 覆盖 provide，下次 resolve 取新
        slowChannel->registerList(QStringLiteral("demo"), QStringLiteral("owner-a"));
        auto timeoutReply = bound.enumerate();
        QVERIFY(!timeoutReply.has_value());
        QCOMPARE(timeoutReply.error(), timeout);
    }
};

QTEST_GUILESS_MAIN(TestScriptListProvider)

#include "tst_ScriptListProvider.moc"

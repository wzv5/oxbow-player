// N179：ScriptListChannel 单元测试；请求以 QtConcurrent 起、主线程投递应答

#include "pch.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtTest/QtTest>

#include "RecordingPort.h"
#include "ServiceRegistry.h"
#include "playlist/ScriptListChannel.h"
#include "playlist/ScriptListSpec.h"

using ScriptListSpec::eventName;
using ScriptListSpec::kEventCancelLayer;
using ScriptListSpec::kEventNeedLayer;
using Error = ScriptListChannel::Error;

namespace {

// 注册表 + 端口 + 通道：通道只持注册表弱引用，三层必须同生
struct Env {
    explicit Env(int replyMs = 10000, int idMs = kScriptListIdWaitMs)
        : registry(std::make_shared<ServiceRegistry>()), port(std::make_shared<RecordingPort>()) {
        registry->provide<MpvApiPort>(port);
        channel = std::make_unique<ScriptListChannel>(registry, replyMs, idMs);
    }

    std::shared_ptr<ServiceRegistry> registry;
    std::shared_ptr<RecordingPort> port;
    std::unique_ptr<ScriptListChannel> channel;
};

} // namespace

class TestScriptListChannel : public QObject {
    Q_OBJECT

  private slots:
    void registersWithSameOwnerIdempotent() {
        Env env;
        auto &channel = *env.channel;
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a"))); // 幂等
        QVERIFY(!channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-b")));
        QVERIFY(channel.registerList(QStringLiteral("other"), QStringLiteral("owner-b")));
        QCOMPARE(env.port->recordCount(), 0); // 注册不发消息
    }

    void rejectsOtherOwner() {
        Env env;
        auto &channel = *env.channel;
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        QVERIFY(!channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-b")));
        QCOMPARE(channel.registeredIds(), QStringList{QStringLiteral("demo")});
        QVERIFY(channel.isRegistered(QStringLiteral("demo")));
        QVERIFY(!channel.isRegistered(QStringLiteral("nope")));
    }

    void registeredIdsSorted() {
        Env env;
        auto &channel = *env.channel;
        QVERIFY(channel.registerList(QStringLiteral("zeta"), QStringLiteral("o")));
        QVERIFY(channel.registerList(QStringLiteral("alpha"), QStringLiteral("o")));
        QVERIFY(channel.registerList(QStringLiteral("mid"), QStringLiteral("o")));
        const QStringList expectedIds = {QStringLiteral("alpha"), QStringLiteral("mid"),
                                         QStringLiteral("zeta")};
        QCOMPARE(channel.registeredIds(), expectedIds);
    }

    void unregisteredIdReturnsNotRegistered() {
        Env env;
        auto &channel = *env.channel;
        const auto reply = channel.requestLayer(QStringLiteral("nope"), QString());
        QVERIFY(!reply.has_value());
        QCOMPARE(reply.error(), Error::NotRegistered);
        QCOMPARE(env.port->recordCount(), 0); // 未注册不发送
    }

    void sendFailureReturnsSendFailed() {
        Env env;
        env.port->setReturnCode(-5); // script-message-to 对不存在 client 失败的模拟
        auto &channel = *env.channel;
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        const auto reply = channel.requestLayer(QStringLiteral("demo"), QString());
        QVERIFY(!reply.has_value());
        QCOMPARE(reply.error(), Error::SendFailed);
        QCOMPARE(env.port->recordCount(), 1); // 发送失败当场撤销，无 cancel 补发
    }

    void missingPortReturnsSendFailed() {
        auto registry = std::make_shared<ServiceRegistry>(); // 只装注册表，不 provide 端口
        ScriptListChannel channel(registry, 10000);
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        const auto reply = channel.requestLayer(QStringLiteral("demo"), QString());
        QVERIFY(!reply.has_value());
        QCOMPARE(reply.error(), Error::SendFailed); // 装配缺失与发送失败同一分流
    }

    void timeoutRepliesCancelLayerOnce() {
        Env env(150); // 注入短超时
        auto &channel = *env.channel;
        auto &port = *env.port;
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        auto future = QtConcurrent::run([&channel] {
            return channel.requestLayer(QStringLiteral("demo"), QStringLiteral("s1"));
        });
        SendRecord need = port.takeOne();
        QCOMPARE(need.name, eventName(kEventNeedLayer));
        QCOMPARE(need.owner, QStringLiteral("owner-a"));
        QCOMPARE(need.args.at(0), QStringLiteral("demo"));
        QCOMPARE(need.args.at(2), QStringLiteral("s1"));
        QVERIFY(!need.args.at(1).isEmpty()); // reqid 十进制串
        SendRecord cancel;
        QVERIFY(port.tryTakeOne(&cancel, 5000)); // 超时后回投恰一次
        QCOMPARE(cancel.name, eventName(kEventCancelLayer));
        QCOMPARE(cancel.args, (QStringList{QStringLiteral("demo"), need.args.at(1)}));
        const auto reply = future.result();
        QVERIFY(!reply.has_value());
        QCOMPARE(reply.error(), Error::Timeout);
        QCOMPARE(port.recordCount(), 0); // 无多余发送
    }

    void deliverPairsByOwnerAndReqid() {
        Env env;
        auto &channel = *env.channel;
        auto &port = *env.port;
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        auto future = QtConcurrent::run(
            [&channel] { return channel.requestLayer(QStringLiteral("demo"), QString()); });
        SendRecord need = port.takeOne();
        channel.deliverLayer(QStringLiteral("owner-a"), need.args.at(1), QStringLiteral("{}"));
        const auto reply = future.result();
        QVERIFY(reply.has_value());
        QCOMPARE(*reply, QStringLiteral("{}"));
    }

    void wrongOwnerAndLateReplyDropped() {
        Env env;
        auto &channel = *env.channel;
        auto &port = *env.port;
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        auto future = QtConcurrent::run(
            [&channel] { return channel.requestLayer(QStringLiteral("demo"), QString()); });
        SendRecord need = port.takeOne();
        channel.deliverLayer(QStringLiteral("owner-b"), need.args.at(1),
                             QStringLiteral("wrong")); // owner 不符：丢弃，请求继续等
        channel.deliverLayer(QStringLiteral("owner-a"), need.args.at(1), QStringLiteral("right"));
        const auto reply = future.result();
        QVERIFY(reply.has_value());
        QCOMPARE(*reply, QStringLiteral("right"));
        channel.deliverLayer(QStringLiteral("owner-a"), need.args.at(1),
                             QStringLiteral("late")); // 已消费的 reqid：无挂起项可配
        QCOMPARE(port.recordCount(), 0);
    }

    void waitRegisteredResolvesWhenIdAppears() {
        Env env;
        auto &channel = *env.channel;
        auto future = QtConcurrent::run(
            [&channel] { return channel.waitRegistered(QStringLiteral("demo"), 5000); });
        QTest::qWait(20); // 先让等待进锁
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        QVERIFY(future.result());
    }

    void waitRegisteredTimesOut() {
        Env env;
        QVERIFY(!env.channel->waitRegistered(QStringLiteral("demo"), 100));
    }

    void waitRegisteredWakesOnClose() {
        Env env;
        auto &channel = *env.channel;
        auto future = QtConcurrent::run(
            [&channel] { return channel.waitRegistered(QStringLiteral("demo"), 5000); });
        QTest::qWait(20);
        channel.close();
        QVERIFY(!future.result());
    }

    void closeWakesWaiterAndClearsTables() {
        Env env;
        auto &channel = *env.channel;
        auto &port = *env.port;
        QVERIFY(channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        auto future = QtConcurrent::run(
            [&channel] { return channel.requestLayer(QStringLiteral("demo"), QString()); });
        SendRecord need = port.takeOne();
        channel.close();
        const auto reply = future.result();
        QVERIFY(!reply.has_value());
        QCOMPARE(reply.error(), Error::Closed);
        QCOMPARE(port.recordCount(), 0); // close 之后再无发送
        QVERIFY(!channel.registerList(QStringLiteral("demo"), QStringLiteral("owner-a")));
        QVERIFY(channel.registeredIds().isEmpty());
        QVERIFY(!channel.isRegistered(QStringLiteral("demo")));
    }
};

QTEST_GUILESS_MAIN(TestScriptListChannel)

#include "tst_ScriptListChannel.moc"

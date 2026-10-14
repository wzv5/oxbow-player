#include "pch.h"

#include <QtTest/QtTest>

#include "streams/HttpByteStream.h"

// N52 L3：HttpByteStream 策略状态机的 fake 测试——以脚本化 IHttpFeed 喂出
// seek 分岔 / 位置守恒 / 206 校验三档 / 断连自愈 / 早断映射与排除项 / 收敛
// 上限 / cancel 三态 / 看门狗两段，断言纯状态演进（stats、提交起点、abort
// 状态），不打日志。小超时经 Timeouts 注入（只传值不定值）。
// 注意 onJob 在新作业入列【前】调用：jobs.isEmpty() 判"首播作业"。

namespace {

constexpr qint64 kEof = ByteStream::kEof;
constexpr qint64 kError = ByteStream::kError;
constexpr qint64 kNoData = ByteStream::kNoData;
constexpr qint64 kSeekUnsupported = ByteStream::kSeekUnsupported;
constexpr qint64 kSeekError = ByteStream::kSeekError;

struct FakeJob final : IHttpFeed::Job {
    qint64 start = 0;           // 本次提交起点（Range 装配的断言依据）
    int status = 206;           // 响应头落定后的状态码
    qint64 declaredTotal = 100; // 声明总长；-1 = 无声明
    QList<qint64> script;       // 逐次 read 返回脚本（>0 交付字节数；空走默认交付）
    int headerDelaySlices = 0;  // awaitHeaders 消耗的切片数（>0 返回假）
    qint64 delivered = 0;       // 本作业已交付字节（相对 start）
    bool aborted = false;
};

class FakeFeed final : public IHttpFeed {
  public:
    QList<std::shared_ptr<FakeJob>> jobs;
    QList<qint64> starts;                 // 每次 submit 的起点（断言 Range 起点 = 目标偏移）
    std::function<void(FakeJob *)> onJob; // 提交时配置新作业（入列前调用）

    JobPtr submit(const HttpStreamTarget &, qint64 startOffset) override {
        auto j = std::make_shared<FakeJob>();
        j->start = startOffset;
        if (onJob)
            onJob(j.get());
        starts.append(startOffset);
        jobs.append(j);
        return j;
    }

    bool awaitHeaders(const JobPtr &jp, int) override {
        auto j = std::static_pointer_cast<FakeJob>(jp);
        if (j->aborted)
            return true; // 撤销落定即"已终态"
        if (j->headerDelaySlices > 0) {
            j->headerDelaySlices--;
            return false;
        }
        return true;
    }

    qint64 read(const JobPtr &jp, char *out, qint64 maxBytes, int) override {
        auto j = std::static_pointer_cast<FakeJob>(jp);
        if (j->aborted)
            return kError;
        if (!j->script.isEmpty()) {
            const qint64 v = j->script.takeFirst();
            if (v > 0)
                return deliver(j, out, maxBytes, v);
            return v; // 哨兵直通
        }
        if (j->declaredTotal >= 0) {
            const qint64 remaining = j->declaredTotal - (j->start + j->delivered);
            if (remaining <= 0)
                return kEof;
            return deliver(j, out, maxBytes, remaining);
        }
        return deliver(j, out, maxBytes, maxBytes);
    }

    int statusCodeOf(const JobPtr &jp) const override {
        return std::static_pointer_cast<FakeJob>(jp)->status;
    }

    qint64 totalBytesOf(const JobPtr &jp) const override {
        return std::static_pointer_cast<FakeJob>(jp)->declaredTotal;
    }

    void abort(const JobPtr &jp) override { std::static_pointer_cast<FakeJob>(jp)->aborted = true; }

  private:
    static qint64 deliver(const std::shared_ptr<FakeJob> &j, char *out, qint64 maxBytes,
                          qint64 want) {
        const qint64 n = qMin(want, maxBytes);
        memset(out, 'x', static_cast<size_t>(n));
        j->delivered += n;
        return n;
    }
};

std::expected<std::shared_ptr<HttpByteStream>, QString>
openOn(const std::shared_ptr<FakeFeed> &feed, const HttpByteStream::Timeouts &to = {}) {
    return HttpByteStream::open(feed, HttpStreamTarget{QStringLiteral("fake://stream"), {}, 0}, to);
}

// 生产路径由 mpv 传入真实可写缓冲；测试统一经此供给——fake 的 deliver 以
// memset 填充交付字节，空缓冲会直接崩（首跑即崩在此处，教训入注释）
qint64 streamRead(HttpByteStream &s, qint64 maxBytes) {
    static thread_local std::vector<char> buf(64 * 1024);
    return s.read(buf.data(), maxBytes);
}

} // namespace

class TestHttpByteStream : public QObject {
    Q_OBJECT

  private slots:
    void seekFork_threeWays();
    void positionConservation_onFailedRestart();
    void probeValidation_rejectsFullResponse();
    void selfHeal_onReadError();
    void earlyClose_mappingAndExclusions();
    void convergenceCap();
    void cancel_threeStates();
    void watchdogs_twoSegments();
};

// —— 用例 1：seek 分岔 ——
void TestHttpByteStream::seekFork_threeWays() {
    auto feed = std::make_shared<FakeFeed>();
    auto r = openOn(feed);
    QVERIFY(r);
    auto s = r.value();

    // a) 原位（含起流期的结构性 seek(0)）：零成本，不发请求
    QCOMPARE(s->seek(0), 0);
    QCOMPARE(feed->starts.size(), 1);
    QCOMPARE(s->stats().skipHits, 1);
    QCOMPARE(s->stats().skipBytes, 0);

    // b) 阈值内前向：跳读——丢弃不发请求，pos 推进
    QCOMPARE(streamRead(*s, 10), 10);
    QCOMPARE(s->seek(42), 42);
    QCOMPARE(feed->starts.size(), 1);
    QCOMPARE(s->stats().pos, 42);
    QCOMPARE(s->stats().skipHits, 2);
    QCOMPARE(s->stats().skipBytes, 32);

    // c) 后向：探测式重开，重启起点 = 目标偏移
    QCOMPARE(s->seek(2), 2);
    QCOMPARE(feed->starts.size(), 2);
    QCOMPARE(feed->starts.last(), 2);
    QCOMPARE(s->stats().pos, 2);

    // d) 超阈值前向：同样走重开
    const qint64 farOff = 2 + 64 * 1024 + 1;
    QCOMPARE(s->seek(farOff), farOff);
    QCOMPARE(feed->starts.size(), 3);
    QCOMPARE(feed->starts.last(), farOff);
}

// —— 用例 2：位置守恒 ——
void TestHttpByteStream::positionConservation_onFailedRestart() {
    auto feed = std::make_shared<FakeFeed>();
    feed->onJob = [&feed](FakeJob *j) {
        if (!feed->jobs.isEmpty())
            j->headerDelaySlices = 999; // 后续作业等不到头（探测超时）
    };
    auto r = openOn(feed, HttpByteStream::Timeouts{50, 5000});
    QVERIFY(r);
    auto s = r.value();

    QCOMPARE(streamRead(*s, 10), 10);
    const qint64 posBefore = s->stats().pos;

    // restartAt 失败：pos 不推进，旧连接原位保留（未被 abort）
    QCOMPARE(s->seek(2), kSeekError);
    QCOMPARE(s->stats().pos, posBefore);
    QVERIFY(!feed->jobs.at(0)->aborted);
    QCOMPARE(feed->starts.size(), 2);

    // 旧作业仍可继续读
    QCOMPARE(streamRead(*s, 5), 5);
    QCOMPARE(s->stats().pos, posBefore + 5);
}

// —— 用例 3：206 校验——200 全量判死与粘旗 ——
void TestHttpByteStream::probeValidation_rejectsFullResponse() {
    auto feed = std::make_shared<FakeFeed>();
    feed->onJob = [&feed](FakeJob *j) {
        if (!feed->jobs.isEmpty())
            j->status = 200; // 探测回 200 全量
    };
    auto r = openOn(feed);
    QVERIFY(r);
    auto s = r.value();

    QCOMPARE(streamRead(*s, 10), 10);

    // 200 全量：判死——弃探测保旧连接、返 unsupported
    QCOMPARE(s->seek(2), kSeekUnsupported);
    QCOMPARE(s->stats().pos, 10);
    QVERIFY(!feed->jobs.at(0)->aborted); // 旧连接原位保留，绝不从头重播
    QVERIFY(feed->jobs.at(1)->aborted);  // 探测作业被弃
    QCOMPARE(s->stats().reopens, 0);     // 判死路径不计数

    // 粘旗：后续 seek 快返，不再开探测连接
    QCOMPARE(s->seek(1), kSeekUnsupported);
    QCOMPARE(feed->starts.size(), 2);
}

// —— 用例 4：断连自愈 ——
void TestHttpByteStream::selfHeal_onReadError() {
    auto feed = std::make_shared<FakeFeed>();
    feed->onJob = [&feed](FakeJob *j) {
        if (feed->jobs.isEmpty())
            j->script = {10, kError}; // 首作业：交付 10 字节后断连
    };
    auto r = openOn(feed);
    QVERIFY(r);
    auto s = r.value();

    QCOMPARE(streamRead(*s, 64), 10);
    // 自愈：按 pos 重开一次，重开起点与断点逐字节相等
    const qint64 n = streamRead(*s, 64);
    QVERIFY(n > 0);
    const QList<qint64> expectStarts{0, 10};
    QCOMPARE(feed->starts, expectStarts);
    QCOMPARE(s->stats().reopens, 1);
    QVERIFY(feed->jobs.at(0)->aborted); // 晋升后旧作业被回收
}

// —— 用例 5：早断映射 + 排除项 ——
void TestHttpByteStream::earlyClose_mappingAndExclusions() {
    // a) 早断：未到声明总长通道终止 → 判错误走自愈重开（不得报 EOF 骗过去）
    {
        auto feed = std::make_shared<FakeFeed>();
        feed->onJob = [&feed](FakeJob *j) {
            if (feed->jobs.isEmpty())
                j->script = {10, kEof}; // 交付 10 后服务器提前关连接
        };
        auto r = openOn(feed);
        QVERIFY(r);
        auto s = r.value();
        QCOMPARE(streamRead(*s, 64), 10);
        const qint64 n = streamRead(*s, 64); // 早断 → 自愈 → 新作业交付余量
        QVERIFY(n > 0);
        const QList<qint64> expectStarts{0, 10};
        QCOMPARE(feed->starts, expectStarts);
        QCOMPARE(s->stats().reopens, 1);
    }
    // b) 排除项 2：无声明总长的流不参与判定——EOF 就是 EOF，不重开
    {
        auto feed = std::make_shared<FakeFeed>();
        feed->onJob = [&feed](FakeJob *j) {
            j->declaredTotal = -1;
            if (feed->jobs.isEmpty())
                j->script = {10, kEof};
        };
        auto r = openOn(feed);
        QVERIFY(r);
        auto s = r.value();
        QCOMPARE(streamRead(*s, 64), 10);
        QCOMPARE(streamRead(*s, 64), kEof);
        QCOMPARE(feed->starts.size(), 1);
        QCOMPARE(s->stats().reopens, 0);
    }
    // c) 排除项 1：撤销收尾不算早断——按撤销上抛，不触发重开
    {
        auto feed = std::make_shared<FakeFeed>();
        feed->onJob = [&feed](FakeJob *j) {
            if (feed->jobs.isEmpty())
                j->script = {kNoData, kError, 10};
        };
        auto r = openOn(feed);
        QVERIFY(r);
        auto s = r.value();
        s->cancel();
        QCOMPARE(streamRead(*s, 64), kError);
        QCOMPARE(feed->starts.size(), 1); // 无重开
        QCOMPARE(s->stats().reopens, 0);
    }
}

// —— 用例 6：收敛上限 ——
void TestHttpByteStream::convergenceCap() {
    auto feed = std::make_shared<FakeFeed>();
    // 注意：脚本条目被逐次消费，耗尽即落回默认交付路径——收敛场景要求作业
    // 【每次】read 都早断，脚本必须给足条目数，否则场景中途被默认交付"治好"
    feed->onJob = [&feed](FakeJob *j) {
        if (!feed->jobs.isEmpty())
            j->script = QList<qint64>(1000, kEof); // 每次重开后的作业在同一 pos 立即再早断
    };
    auto r = openOn(feed);
    QVERIFY(r);
    auto s = r.value();

    QCOMPARE(streamRead(*s, 10), 10);
    feed->jobs.at(0)->script = QList<qint64>(1000, kEof); // 首作业此后每次 read 即早断（10 < 100）

    // 同一 pos 反复早断：自愈重开 3 次后判死上抛
    QCOMPARE(streamRead(*s, 64), kError); // 第 1 次自愈
    QCOMPARE(streamRead(*s, 64), kError); // 第 2 次
    QCOMPARE(streamRead(*s, 64), kError); // 第 3 次
    QCOMPARE(s->stats().reopens, 3);
    QCOMPARE(streamRead(*s, 64), kError); // 判死：不再重开
    QCOMPARE(streamRead(*s, 64), kError);
    QCOMPARE(feed->starts.size(), 4);
    QCOMPARE(feed->starts.last(), 10); // 重开起点恒为断点
}

// —— 用例 7：cancel 三态 ——
void TestHttpByteStream::cancel_threeStates() {
    // a) 撤销旗置位后作业被补 abort（槽内可达）
    {
        auto feed = std::make_shared<FakeFeed>();
        auto r = openOn(feed);
        QVERIFY(r);
        r.value()->cancel();
        QVERIFY(feed->jobs.at(0)->aborted);
    }
    // b) 撤销后的 read 按撤销上抛，不触发重开
    {
        auto feed = std::make_shared<FakeFeed>();
        feed->onJob = [&feed](FakeJob *j) {
            if (feed->jobs.isEmpty())
                j->script = {10};
        };
        auto r = openOn(feed);
        QVERIFY(r);
        auto s = r.value();
        s->cancel();
        QCOMPARE(streamRead(*s, 64), kError);
        QCOMPARE(feed->starts.size(), 1);
    }
    // c) seek 期撤销：快返偶发失败，不开探测连接
    {
        auto feed = std::make_shared<FakeFeed>();
        auto r = openOn(feed);
        QVERIFY(r);
        auto s = r.value();
        s->cancel();
        QCOMPARE(s->seek(2), kSeekError);
        QCOMPARE(feed->starts.size(), 1);
    }
    // d) 侧线程撤销打断切片循环（撤销旗在切片边界被检查；真实阻塞唤醒由
    //    HttpClient 的 abort 事件承担，N50 已测）
    {
        auto feed = std::make_shared<FakeFeed>();
        feed->onJob = [](FakeJob *j) {
            j->declaredTotal = -1;
            j->script = QList<qint64>(1000, kNoData); // 永不交付
        };
        auto r = openOn(feed);
        QVERIFY(r);
        auto s = r.value();
        std::thread killer([s]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            s->cancel();
        });
        QCOMPARE(streamRead(*s, 64), kError);
        killer.join();
    }
}

// —— 用例 8：看门狗两段（小超时注入） ——
void TestHttpByteStream::watchdogs_twoSegments() {
    // a) 等头零进展达阈值：起流失败（有界），作业被 abort 收尾
    {
        auto feed = std::make_shared<FakeFeed>();
        feed->onJob = [](FakeJob *j) { j->headerDelaySlices = 999; };
        auto r = openOn(feed, HttpByteStream::Timeouts{50, 5000});
        QVERIFY(!r);
        QVERIFY(r.error().contains(QStringLiteral("timed out")));
        QVERIFY(feed->jobs.at(0)->aborted);
    }
    // b) read 连续零字节达阈值：主动 abort 当前作业并按错误走自愈重开
    {
        auto feed = std::make_shared<FakeFeed>();
        feed->onJob = [&feed](FakeJob *j) {
            if (feed->jobs.isEmpty())
                j->script = {kNoData, kNoData, 10};
        };
        auto r = openOn(feed, HttpByteStream::Timeouts{5000, 250}); // 读阈值 = 1 个切片
        QVERIFY(r);
        auto s = r.value();
        const qint64 n = streamRead(*s, 64);
        QVERIFY(n > 0); // 第 1 片零字节触发看门狗 → 自愈重开 → 新作业交付
        const QList<qint64> expectStarts{0, 0};
        QCOMPARE(feed->starts, expectStarts);
        QVERIFY(feed->jobs.at(0)->aborted);
        QCOMPARE(s->stats().reopens, 1);
    }
    // c) 有字节到达即清零计时：零字节切片未达阈值不误伤（慢链路不是错误）
    {
        auto feed = std::make_shared<FakeFeed>();
        feed->onJob = [&feed](FakeJob *j) {
            if (feed->jobs.isEmpty())
                j->script = {kNoData, 10};
        };
        auto r = openOn(feed, HttpByteStream::Timeouts{5000, 1000}); // 读阈值 = 4 个切片
        QVERIFY(r);
        auto s = r.value();
        QCOMPARE(streamRead(*s, 64), 10);
        QCOMPARE(s->stats().reopens, 0);
    }
}

QTEST_GUILESS_MAIN(TestHttpByteStream)
#include "tst_HttpByteStream.moc"

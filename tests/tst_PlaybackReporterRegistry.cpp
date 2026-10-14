#include "pch.h"

#include <QtTest/QtTest>

#include "playbackreport/PlaybackReporterRegistry.h"

#include "ServiceRegistry.h"
#include "playlist/PlaylistRegistry.h"  // kDefaultListUuid：回落判据的"默认 tab 不猜"档
#include "providers/PlaylistProvider.h" // 归属注入口的形参类型

// N74 §2：报告者注册表的纯数据契约——注册顺序即取快照顺序（同键多实例并存、按
// 各自注册位次留下，归属列表身份随表项交回）、lock 失败即过期并在取快照时剔掉且
// 其余项次序不变、快照交出的强引用只管到本次快照存活期（注册表本身不延长寿命）、
// 在册布尔由 add 置位且不因过期回拨。投递路由（谁认领哪条来源）的判据就住在表上
// （`claimsSource`）：它是"表项归属 ↔ 事件来源"这对值的纯谓词，与表项形状同族；
// 驱动器链接 mpv 侧、需要 handle 才实例化，判据住那儿即这条安全属性零覆盖，故本
// 套件既验表的形状与归属列的忠实搬运，也验两支路由谓词的组合——`claimsSource`
// 的五种组合（N74）与 `resolveSourceUuid` 的五档边界（N99：来源侧判定，与表项侧
// 合成"宁漏不串"的完整规则）。add 的空指针断言在 Debug 构建里先 qAbort，与返回
// 值档不可共存于同一用例，故本套件不构造该入参（与 tst_PlaylistRegistry 对断言
// 分支的处置同口径）。
namespace {
using Entry = PlaybackReporterRegistry::Entry;

QStringList keysOf(const std::vector<Entry> &snapshot) {
    QStringList out;
    for (const auto &entry : snapshot)
        out << entry.key;
    return out;
}

QList<QUuid> ownersOf(const std::vector<Entry> &snapshot) {
    QList<QUuid> out;
    for (const auto &entry : snapshot)
        out << entry.listUuid;
    return out;
}

// 只记不发的 reporter：本套件验的是表的形状，不是事件语义
class RecordingReporter final : public PlaybackReporter {
  public:
    explicit RecordingReporter(QString tag) : m_tag(std::move(tag)) {}
    void playbackEvent(const PlaybackEvent &ev) override { m_events.append(ev); }
    QString tag() const { return m_tag; }

  private:
    QString m_tag;
    QVector<PlaybackEvent> m_events;
};

// 归属解析用例的最小实体桩：经子类公开转发盖章（与 tst_PlaylistEntity 的
// TestEntity 同一条授权通道，不放宽可见性）；判据只吃 path / matches / ownerUuid
class StampableEntity final : public PlaylistEntity {
  public:
    StampableEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<StampableEntity>(*this); }
    bool isLocalFile() const override { return true; }
    bool exists() const override { return true; }
    bool supportsFileMeta() const override { return false; }
    bool supportsMediaMeta() const override { return false; }

    void applyOwnership(QUuid uuid, std::weak_ptr<const PlaylistProvider> provider) {
        setOwnership(std::move(uuid), std::move(provider));
    }

  protected:
    FileMeta computeFileMeta() const override { return {}; }
    MediaMeta computeMediaMeta() const override { return {}; }
    QIcon computeIcon() const override { return {}; } // GUILESS 环境不触碰像素后端
};
} // namespace

class TestPlaybackReporterRegistry : public QObject {
    Q_OBJECT

  private slots:
    void emptyTableYieldsNothing();
    void snapshotKeepsRegistrationOrder();
    void ownerListUuidCarriedThrough();
    void sameOwnerReregistrationSupersedes();
    void expiredEntryPrunedOnceSeen();
    void snapshotLifetimeEndsWithTheSnapshot();
    void hasEntryNeverResets();
    void routingPredicatePrefersMissingOverWrong();
    void sourceResolutionEntityMatched();
    void sourceResolutionMismatchYieldsNil();
    void sourceResolutionFallbackForNonDefault();
    void sourceResolutionDefaultTabStaysSilent();
    void sourceResolutionNoFallbackYieldsNil();
};

void TestPlaybackReporterRegistry::emptyTableYieldsNothing() {
    PlaybackReporterRegistry reg;
    QVERIFY(reg.snapshot().empty());
    QVERIFY(!reg.hasEntry()); // 线程启动判据的第二条件：未装配任何报告者即不起线程
}

void TestPlaybackReporterRegistry::snapshotKeepsRegistrationOrder() {
    PlaybackReporterRegistry reg;
    const auto first = std::make_shared<RecordingReporter>("jf-1");
    const auto other = std::make_shared<RecordingReporter>("lastfm");
    const auto second = std::make_shared<RecordingReporter>("jf-2");
    const QUuid listA = QUuid::createUuid();
    const QUuid listB = QUuid::createUuid();
    reg.add(QStringLiteral("jellyfin"), listA, first);
    reg.add(QStringLiteral("lastfm"), listA, other);
    reg.add(QStringLiteral("jellyfin"), listB, second); // 同键第二家：不去重，都在册

    const std::vector<Entry> snap = reg.snapshot();
    const QStringList expectKeys = {QStringLiteral("jellyfin"), QStringLiteral("lastfm"),
                                    QStringLiteral("jellyfin")};
    QVERIFY(keysOf(snap) == expectKeys);
    QVERIFY(snap.at(0).reporter.get() == first.get()); // 顺序 = 注册顺序，不按键归并
    QVERIFY(snap.at(2).reporter.get() == second.get());
}

void TestPlaybackReporterRegistry::ownerListUuidCarriedThrough() {
    PlaybackReporterRegistry reg;
    const auto mine = std::make_shared<RecordingReporter>("mine");
    const auto ownerless = std::make_shared<RecordingReporter>("ownerless");
    const QUuid listUuid = QUuid::createUuid();
    reg.add(QStringLiteral("jellyfin"), listUuid, mine);
    reg.add(QStringLiteral("lastfm"), QUuid(), ownerless); // nil 归属：驱动器一律投

    const std::vector<Entry> snap = reg.snapshot();
    const QList<QUuid> expectOwners = {listUuid, QUuid()};
    QVERIFY(ownersOf(snap) == expectOwners);
    QVERIFY(snap.at(0).listUuid == listUuid); // 归属列忠实搬运，不被键同化
}

void TestPlaybackReporterRegistry::sameOwnerReregistrationSupersedes() {
    PlaybackReporterRegistry reg;
    const QUuid list = QUuid::createUuid();
    // 换源 / 重新授权：旧实例可能仍被在途任务的 shared_ptr 吊着（N55 §2.6），
    // 同 (键, 归属) 再注册必须顶掉旧的，否则一条列表两个报告者并存 = 重复投递
    auto stale = std::make_shared<RecordingReporter>("stale");
    reg.add(QStringLiteral("jellyfin"), list, stale);
    const auto fresh = std::make_shared<RecordingReporter>("fresh");
    reg.add(QStringLiteral("jellyfin"), list, fresh);

    const std::vector<Entry> snap = reg.snapshot();
    QVERIFY(snap.size() == 1);
    QVERIFY(snap.at(0).reporter.get() == fresh.get());

    // 另一条列表同键并存不受影响（归属不同即两册都在）
    reg.add(QStringLiteral("jellyfin"), QUuid::createUuid(), stale);
    QVERIFY(reg.snapshot().size() == 2);
}

void TestPlaybackReporterRegistry::expiredEntryPrunedOnceSeen() {
    PlaybackReporterRegistry reg;
    const auto before = std::make_shared<RecordingReporter>("before");
    auto middle = std::make_shared<RecordingReporter>("middle");
    const auto after = std::make_shared<RecordingReporter>("after");
    const QUuid list = QUuid::createUuid();
    reg.add(QStringLiteral("a"), list, before);
    reg.add(QStringLiteral("b"), list, middle);
    reg.add(QStringLiteral("c"), list, after);

    middle.reset(); // 提供方析构（换源后旧实例被丢弃）：注册表不持强引用
    const QStringList expect = {QStringLiteral("a"), QStringLiteral("c")};
    QVERIFY(keysOf(reg.snapshot()) == expect); // 本次即剔掉
    QVERIFY(keysOf(reg.snapshot()) == expect); // 二次取表结果同形（已被清理，非每次重试）
}

void TestPlaybackReporterRegistry::snapshotLifetimeEndsWithTheSnapshot() {
    PlaybackReporterRegistry reg;
    auto reporter = std::make_shared<RecordingReporter>("x");
    reg.add(QStringLiteral("jellyfin"), QUuid::createUuid(), reporter);
    RecordingReporter *born = reporter.get();

    std::vector<Entry> snap = reg.snapshot();
    reporter.reset(); // 快照已交出的副本把寿命延到本次消费结束（扇出中途析构安全）
    QVERIFY(snap.size() == 1);
    QVERIFY(snap.at(0).reporter.get() == born);
    QVERIFY(born->tag() == QStringLiteral("x")); // 经快照副本仍可用，此刻没人持它

    snap.clear(); // 消费者放掉即再无强引用，下次取表见其过期
    QVERIFY(reg.snapshot().empty());
}

void TestPlaybackReporterRegistry::hasEntryNeverResets() {
    PlaybackReporterRegistry reg;
    {
        auto reporter = std::make_shared<RecordingReporter>("x");
        reg.add(QStringLiteral("jellyfin"), QUuid::createUuid(), reporter);
        QVERIFY(reg.hasEntry());
    }
    QVERIFY(reg.snapshot().empty());
    QVERIFY(reg.hasEntry()); // 过期清理不回拨：判据据此至多多一次空转的线程启动，无害
}

// 投递路由判据（驱动器只调它，判据本身住表上——与表项形状同族，且不依赖 mpv 侧，
// 故能在这儿测全四种组合）。核心是"宁漏不串"：来源不明时绝不投给有归属的报告者
void TestPlaybackReporterRegistry::routingPredicatePrefersMissingOverWrong() {
    const QUuid owner = QUuid::createUuid();
    const QUuid other = QUuid::createUuid();
    QVERIFY(PlaybackReporterRegistry::claimsSource(QUuid(), QUuid())); // 无归属 + 无来源
    QVERIFY(PlaybackReporterRegistry::claimsSource(QUuid(), owner));   // 无归属 + 有来源
    QVERIFY(!PlaybackReporterRegistry::claimsSource(owner, QUuid()));  // 有归属 + 无来源
    QVERIFY(PlaybackReporterRegistry::claimsSource(owner, owner));     // 同源
    QVERIFY(!PlaybackReporterRegistry::claimsSource(owner, other));    // 别册
}

// 归属解析（N99）：来源侧判定的五档边界。构造用同一身份串喂实体与 identity，
// matches 的键级判等恒命中/恒不命中两态可造
void TestPlaybackReporterRegistry::sourceResolutionEntityMatched() {
    auto registry = std::make_shared<ServiceRegistry>();
    const QUuid owner = QUuid::createUuid();
    auto entity = std::make_shared<StampableEntity>(QStringLiteral("D:/media/a.mp4"), registry);
    entity->applyOwnership(owner, {});
    QVERIFY(PlaybackReporterRegistry::resolveSourceUuid(entity, QStringLiteral("D:/media/a.mp4"),
                                                        QUuid()) == owner);
}

void TestPlaybackReporterRegistry::sourceResolutionMismatchYieldsNil() {
    auto registry = std::make_shared<ServiceRegistry>();
    auto entity = std::make_shared<StampableEntity>(QStringLiteral("D:/media/a.mp4"), registry);
    entity->applyOwnership(QUuid::createUuid(), {});
    // 载荷与现读行号不原子（GUI 被占住期间 mpv 换过行）：判等不符一律交 nil，宁漏不串
    QVERIFY(PlaybackReporterRegistry::resolveSourceUuid(entity, QStringLiteral("D:/media/b.mp4"),
                                                        QUuid::createUuid())
                .isNull());
}

void TestPlaybackReporterRegistry::sourceResolutionFallbackForNonDefault() {
    // 实体解析不到（越界 / 层不在缓存 / 异步推进中）：非默认 tab 回落该列表身份
    const QUuid listUuid = QUuid::createUuid();
    QVERIFY(PlaybackReporterRegistry::resolveSourceUuid(
                PlaylistEntity::Ptr{}, QStringLiteral("D:/media/a.mp4"), listUuid) == listUuid);
}

void TestPlaybackReporterRegistry::sourceResolutionDefaultTabStaysSilent() {
    // 默认 tab 里混得进收编项（归属是别家列表），歧义区不猜：交 nil
    QVERIFY(PlaybackReporterRegistry::resolveSourceUuid(
                PlaylistEntity::Ptr{}, QStringLiteral("D:/media/a.mp4"), kDefaultListUuid)
                .isNull());
}

void TestPlaybackReporterRegistry::sourceResolutionNoFallbackYieldsNil() {
    // 正播列表身份本身取不到（listOf 为空）：无兜底可落，交 nil
    QVERIFY(PlaybackReporterRegistry::resolveSourceUuid(PlaylistEntity::Ptr{},
                                                        QStringLiteral("D:/media/a.mp4"), QUuid())
                .isNull());
}

QTEST_GUILESS_MAIN(TestPlaybackReporterRegistry)
#include "tst_PlaybackReporterRegistry.moc"

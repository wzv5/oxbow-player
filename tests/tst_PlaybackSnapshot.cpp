#include "pch.h"

#include <QtTest/QtTest>

#include "playbackreport/PlaybackSnapshot.h"

// N99：播放态快照——纯值 + 一把不外露的锁。isPlaying 派生自 identity（"此刻在播"
// 的判据，会话态不进公开快照）、toEvent 是事件构造的类型出口（Start 的 position
// 例外由 startSession 显式置 0，不在本类型内）、GuardedPlaybackSnapshot 锁内改并交出
// 写后的值 + 锁内取副本。纯值类型无 GUI 依赖，GUILESS 直测
class TestPlaybackSnapshot : public QObject {
    Q_OBJECT

  private slots:
    void defaultIsNotPlaying();
    void isPlayingDerivedFromIdentity();
    void toEventCarriesAllFields();
    void guardedCopyAndUpdate();
};

void TestPlaybackSnapshot::defaultIsNotPlaying() {
    PlaybackSnapshot snap;
    QVERIFY(!snap.isPlaying());
    QCOMPARE(snap.positionSeconds, 0.0);
    QCOMPARE(snap.durationSeconds, 0.0);
    QVERIFY(!snap.paused);
    QVERIFY(snap.metadata.isEmpty());
    QVERIFY(snap.listUuid.isNull());
}

// "线程未起但正播"与"Gated 但正播"两态自动答对的根基：playing 是派生不是存储。
// endFile 只清身份、其余字段原样保留——identity 为空 ⇒ 其余字段是"仅供续播的陈旧
// 值"（拉取出口契约）
void TestPlaybackSnapshot::isPlayingDerivedFromIdentity() {
    PlaybackSnapshot snap;
    snap.identity = QStringLiteral("jellyfin://srv/item");
    QVERIFY(snap.isPlaying());
    snap.listUuid = QUuid::createUuid();
    snap.positionSeconds = 42.0;
    snap.durationSeconds = 90.0;
    snap.identity.clear();
    QVERIFY(!snap.isPlaying());
    QCOMPARE(snap.positionSeconds, 42.0); // 字段不清，随身份回落成"陈旧值"
    QCOMPARE(snap.durationSeconds, 90.0);
}

void TestPlaybackSnapshot::toEventCarriesAllFields() {
    PlaybackSnapshot snap;
    snap.listUuid = QUuid::createUuid();
    snap.identity = QStringLiteral("jellyfin://srv/item");
    snap.positionSeconds = 12.5;
    snap.durationSeconds = 100.0;
    snap.paused = true;
    snap.metadata.insert(QStringLiteral("Title"), QStringLiteral("t"));
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);

    const PlaybackEvent ev = snap.toEvent(PlaybackEvent::Phase::Start, token);
    QVERIFY(ev.phase == PlaybackEvent::Phase::Start);
    QCOMPARE(ev.sessionToken, token);
    QCOMPARE(ev.identity, snap.identity);
    QCOMPARE(ev.positionSeconds, snap.positionSeconds);
    QCOMPARE(ev.durationSeconds, snap.durationSeconds);
    QVERIFY(ev.paused);
    QVERIFY(ev.metadata == snap.metadata);
}

void TestPlaybackSnapshot::guardedCopyAndUpdate() {
    GuardedPlaybackSnapshot guarded;
    QVERIFY(!guarded.copy().isPlaying());
    // update 交出写后的那一份：取值规则靠一次加锁同时完成"判—写—取"，不再补一次 copy
    const PlaybackSnapshot written = guarded.update([](PlaybackSnapshot &s) {
        s.identity = QStringLiteral("x");
        s.positionSeconds = 3.0;
    });
    QCOMPARE(written.identity, QStringLiteral("x"));
    QCOMPARE(written.positionSeconds, 3.0);
    QVERIFY(written.isPlaying());
    QCOMPARE(guarded.copy().positionSeconds, 3.0); // 写确实落进存储，不是只交出借方
    // 条件写穿的落空分支：mutate 提前返回即字段不动，交回的仍是当前值——驱动器据此判
    // "身份已变、本轮作废"（不写穿与作废必须出自同一次加锁，否则比对基准会漂）
    const PlaybackSnapshot untouched = guarded.update([](PlaybackSnapshot &s) {
        if (s.identity != QStringLiteral("y"))
            return;
        s.positionSeconds = 99.0;
    });
    QCOMPARE(untouched.identity, QStringLiteral("x"));
    QCOMPARE(untouched.positionSeconds, 3.0);
    // copy 交出的是字段副本：改副本不回写（拉取出口"任意线程随时可读"的安全性根基）
    PlaybackSnapshot mutated = guarded.copy();
    mutated.positionSeconds = 99.0;
    QCOMPARE(guarded.copy().positionSeconds, 3.0);
}

QTEST_GUILESS_MAIN(TestPlaybackSnapshot)
#include "tst_PlaybackSnapshot.moc"

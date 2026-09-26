#include "pch.h"

#include <QtTest/QtTest>

#include "PlaylistRegistry.h"
#include "ProviderCatalog.h" // createDefaultProvider：提供者成品来源
#include "ServiceRegistry.h"

// N57 §2.2 / §2.4 / §2.5：列表注册表的纯模型层契约——铸造构造性不撞车、存量身份
// 接回与撞车退新、保留身份不可被夺走、按身份直查对 nil / 非法 / 已摘除恒不命中、
// 摘除即析构、N62 改序出口的不可行即 false 与权威序搬移。全部无 widget、无 mpv。
// 断言守候的分支（默认列表不可摘除 / 不可改名换源）在 Debug 构建里会先 qAbort，
// 与"返回值"档不可共存于同一用例，故本套件不构造这些入参（Release 行为由代码走查
// 确认，与 tst_StreamTransit 对 mint 的处置同一口径）。
class TestPlaylistRegistry : public QObject {
    Q_OBJECT

  private:
    static std::shared_ptr<PlaylistProvider> newProvider() {
        return createDefaultProvider(std::make_shared<ServiceRegistry>());
    }

  private slots:
    void mintedIdsNeverCollide();
    void storedUuidAdoptedUnlessTaken();
    void reservedIdCannotBeSeized();
    void missingOrBrokenIdMeansNewMint();
    void findNeverMatchesNilOrRemoved();
    void defaultListAlwaysResolvable();
    void moveListRejectsImpossibleTargets();
    void moveListReordersAuthoritativeSequence();
};

void TestPlaylistRegistry::mintedIdsNeverCollide() {
    PlaylistRegistry reg(newProvider());
    QSet<QString> seen;
    for (int i = 0; i < 8; ++i) {
        Playlist *list =
            reg.append(QStringLiteral("Playlist %1").arg(i + 1), newProvider(), QUuid());
        QVERIFY(list != nullptr);
        QVERIFY(!list->uuid().isNull());
        QVERIFY(list->uuid() != kDefaultListUuid);        // 保留身份不会被夺走
        QVERIFY(!seen.contains(list->uuid().toString())); // 逐一不撞车
        seen.insert(list->uuid().toString());
    }
    for (const QString &id : seen)
        QVERIFY(reg.find(QUuid::fromString(id)) != nullptr);
}

void TestPlaylistRegistry::storedUuidAdoptedUnlessTaken() {
    PlaylistRegistry reg(newProvider());
    const QUuid stored = QUuid::createUuid();
    Playlist *first = reg.append(QStringLiteral("A"), newProvider(), stored);
    QVERIFY(first->uuid() == stored); // 接回存量值：重启后身份逐一相同（§2.7 加载行）

    // 同一存量值再来一次：判撞车 → 另铸新值，且原持有者不变
    Playlist *second = reg.append(QStringLiteral("B"), newProvider(), stored);
    QVERIFY(second->uuid() != stored);
    QVERIFY(second->uuid() != first->uuid());
    QVERIFY(reg.find(stored) == first);
}

void TestPlaylistRegistry::reservedIdCannotBeSeized() {
    PlaylistRegistry reg(newProvider());
    // 手改 INI 把某持久列表身份写成保留值：判撞车改用新铸（§四"默认 tab"一条）
    Playlist *hijacker = reg.append(QStringLiteral("Hijack"), newProvider(), kDefaultListUuid);
    QVERIFY(hijacker->uuid() != kDefaultListUuid);
    QVERIFY(!hijacker->isDefault());
    QVERIFY(reg.find(kDefaultListUuid) == reg.defaultList()); // 默认列表仍是它
    QVERIFY(reg.defaultList()->isDefault());
}

void TestPlaylistRegistry::missingOrBrokenIdMeansNewMint() {
    PlaylistRegistry reg(newProvider());
    // 空串 / 非法串 / 全零一律解析为 nil ⇒ 按缺键新铸（不设迁移、不猜旧形态）
    const QStringList broken{QString(), QStringLiteral("nope"),
                             QStringLiteral("00000000-0000-0000-0000-000000000000")};
    for (const QString &text : broken) {
        QVERIFY(QUuid::fromString(text).isNull());
        Playlist *list = reg.append(QStringLiteral("L"), newProvider(), QUuid::fromString(text));
        QVERIFY(!list->uuid().isNull());
    }
    QVERIFY(reg.find(QUuid::fromString(broken.at(2))) == nullptr);
}

void TestPlaylistRegistry::findNeverMatchesNilOrRemoved() {
    PlaylistRegistry reg(newProvider());
    QVERIFY(reg.find(QUuid()) == nullptr);             // nil 恒不命中（本类不产生 nil 身份的列表）
    QVERIFY(reg.find(QUuid::createUuid()) == nullptr); // 未登记的身份
    Playlist *list = reg.append(QStringLiteral("Gone"), newProvider(), QUuid());
    const QUuid id = list->uuid();
    QVERIFY(reg.find(id) == list);
    QVERIFY(reg.remove(id));
    QVERIFY(reg.find(id) == nullptr); // 摘除即析构，双查皆空
    QVERIFY(!reg.remove(id));         // 重复摘除返 false，不误伤
}

void TestPlaylistRegistry::defaultListAlwaysResolvable() {
    PlaylistRegistry reg(newProvider());
    QVERIFY(reg.defaultList() != nullptr);
    QVERIFY(reg.defaultList()->isDefault());
    QVERIFY(reg.find(kDefaultListUuid) == reg.defaultList());
    QVERIFY(reg.defaultList()->provider() != nullptr); // 一次成型：无"无提供者"中间态
    QVERIFY(!reg.defaultList()->name().isEmpty());
}

// N62 §2.2：改序出口的"不可行即 false"档——越界、nil、未知身份、默认保留身份，
// 逐一拒绝后权威序与成员不得有任何变化
void TestPlaylistRegistry::moveListRejectsImpossibleTargets() {
    PlaylistRegistry reg(newProvider());
    Playlist *a = reg.append(QStringLiteral("A"), newProvider(), QUuid());
    Playlist *b = reg.append(QStringLiteral("B"), newProvider(), QUuid());
    QVERIFY(a && b);
    QVERIFY(!reg.moveList(a->uuid(), -1));          // 负下标
    QVERIFY(!reg.moveList(a->uuid(), 2));           // 越过末位（count = 2）
    QVERIFY(!reg.moveList(QUuid(), 0));             // nil 恒不命中
    QVERIFY(!reg.moveList(QUuid::createUuid(), 0)); // 未登记身份
    QVERIFY(!reg.moveList(kDefaultListUuid, 0));    // 默认保留身份：结构上不在 vector 内
    QCOMPARE(reg.persistentCount(), 2);             // 逐一失败后成员与序原样
    QVERIFY(reg.persistentAt(0) == a);
    QVERIFY(reg.persistentAt(1) == b);
}

// N62 §2.2：`to` 是移动完成后的终位下标，前移后移两方向同义；原位移动幂等成功
void TestPlaylistRegistry::moveListReordersAuthoritativeSequence() {
    PlaylistRegistry reg(newProvider());
    Playlist *a = reg.append(QStringLiteral("A"), newProvider(), QUuid());
    Playlist *b = reg.append(QStringLiteral("B"), newProvider(), QUuid());
    Playlist *c = reg.append(QStringLiteral("C"), newProvider(), QUuid());
    QVERIFY(a && b && c);
    QVERIFY(reg.moveList(b->uuid(), 1)); // 原位：成功且序不变
    QVERIFY(reg.persistentAt(0) == a);
    QVERIFY(reg.persistentAt(1) == b);
    QVERIFY(reg.persistentAt(2) == c);
    QVERIFY(reg.moveList(c->uuid(), 0)); // 前移到头：C, A, B
    QVERIFY(reg.persistentAt(0) == c);
    QVERIFY(reg.persistentAt(1) == a);
    QVERIFY(reg.persistentAt(2) == b);
    QVERIFY(reg.moveList(c->uuid(), 2)); // 后移到尾：A, B, C
    QVERIFY(reg.persistentAt(0) == a);
    QVERIFY(reg.persistentAt(1) == b);
    QVERIFY(reg.persistentAt(2) == c);
    QVERIFY(reg.moveList(a->uuid(), 1)); // 中段交换：B, A, C
    QVERIFY(reg.persistentAt(0) == b);
    QVERIFY(reg.persistentAt(1) == a);
    QVERIFY(reg.persistentAt(2) == c);
}

QTEST_GUILESS_MAIN(TestPlaylistRegistry)
#include "tst_PlaylistRegistry.moc"

// BookmarkManager 单元测试（N54：书签来源的落盘与读回；N77：分组改按列表与内容
// 二元组）。覆盖：两键往返；数组键与 Bookmarks/<i>/<j>/… 数字子键不串台；含分隔符
// 与非 ASCII 的 id 逐字节还原；空来源不落键、缺键即空来源（旧 INI 形态）；"根层"
// 与"整条无来源"可区分；组内首见即定不覆盖；跨列表同内容各成一组、互不追加互不
// 波及；本地同键合并读与"全部组本地"判据；过户迁移与合并；旧 INI 按自带 uuid 归组
// （混账不拆）；removeInvalid 保留目录形态键、远端项按归属判失效。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R BookmarkManager 跑本套件；
//   mpvqt_tests_BookmarkManager <槽名> 跑单个用例。

#include "pch.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "BookmarkManager.h"
#include "PlaylistRegistry.h" // kDefaultListUuid：过户目标组的保留身份
#include "Util.h"

using Origin = BookmarkManager::Origin;
using Group = BookmarkManager::Group;

class TestBookmarkManager : public QObject {
    Q_OBJECT

  private slots:
    void init();

    void originRoundTrip();
    void numericSubkeysNoAlias();
    void oddIdsAndPaths();
    void emptyOriginWritesNoKeys();
    void missingKeysYieldEmptyOrigin();
    void rootLevelDistinguishedFromNoOrigin();
    void originFirstWriteWins();
    void removeInvalidKeepsDirectories();
    // N54：远端项按归属判失效（无来源 uuid = 数据异常；来源列表已删 = 跳转必被拒）
    void removeInvalidDropsRemoteWithoutLiveList();
    // N56 §2.3：写侧剥除取消——书签展示路径按实体身份串原样保留（含 userinfo）
    void addBookmarkKeepsRawIdentity();
    // N77 §2.6：跨列表同内容各成一组——写侧零串扰，删 A 不影响 B 组
    void crossGroupIsolation();
    // N77 §2.3：本地显示豁免——同键全部组按组序合并读；远端键不满足判据走严格口径
    void localMergedRead();
    // N77 §2.5：过户——目标缺席整体迁移（uuid 改默认、层清空），已存在合并（并组
    // 排序不去重、展示保留目标既有）
    void transferToDefaultMigrates();
    void transferToDefaultMerges();
    // N77 §2.2：旧 INI 按条目自带 uuid 归组——同键不同 uuid 拆成不同组，nil 独立
    // 成组且互相并入；组内多条目照常读回（混账不拆）
    void loadGroupsByEntryUuid();

  private:
    // 每个用例一份独立 INI（init 里换名），避免用例间互扰
    QString m_ini;
    int m_seq = 0;
    QTemporaryDir m_dir;

    static QUuid defaultUuid() {
        return QUuid::fromString(QStringLiteral("2f1a9c33-7b04-4e58-9a2d-1c6f0b3d8e44"));
    }
    static Origin makeOrigin(const QUuid &uuid = defaultUuid()) {
        return Origin{uuid,
                      {QStringLiteral("D:/media/动漫"), QStringLiteral("D:/media/动漫/2024")}};
    }
    // N77：查询一律经组——(uuid, key(path))
    static Group groupOf(const QString &path, const QUuid &uuid) {
        return {uuid, Util::Url::key(path)};
    }
    static QUuid uuidFrom(const char *literal) {
        return QUuid::fromString(QString::fromLatin1(literal));
    }
};

void TestBookmarkManager::init() {
    QVERIFY(m_dir.isValid());
    m_ini = m_dir.filePath(QStringLiteral("bm%1.ini").arg(++m_seq));
}

void TestBookmarkManager::originRoundTrip() {
    const QString path = QStringLiteral("D:/media/动漫/ep01.mkv");
    const Origin o = makeOrigin();
    {
        BookmarkManager mgr(m_ini);
        mgr.load();
        mgr.addBookmark(path, 12.5, QStringLiteral("Bookmark 1"), o);
    }
    BookmarkManager again(m_ini);
    again.load();
    const Group g = groupOf(path, o.listUuid);
    const Origin got = again.bookmarkOrigin(g);
    // QUuid 无 QTest 字符串转换，判等两侧统一取字面串比对（等价：同一 toString 形态）
    QCOMPARE(got.listUuid.toString(), o.listUuid.toString());
    QCOMPARE(got.levelIds, o.levelIds);
    // 书签本体不受来源字段影响：一条、时间原样
    const auto marks = again.bookmarks(g);
    QCOMPARE(marks.size(), 1);
    QCOMPARE(marks.at(0).time, 12.5);
}

// 数组键与"同组内数字子键"共存是两代方案共同的格式风险：levelIds 走 QSettings 原生
// QStringList（形如 levelIds\1=…），而书签条目用的是 0\0\time、0\1\time 这类数字
// 子键。两者必须各自还原，不得互相吃掉。
void TestBookmarkManager::numericSubkeysNoAlias() {
    const QString fileA = QStringLiteral("D:/media/a.mkv");
    const QString fileB = QStringLiteral("D:/media/b.mkv");
    const QUuid uuidB = uuidFrom("bbbbbbbb-1111-2222-3333-444444444444");
    {
        BookmarkManager mgr(m_ini);
        mgr.load();
        mgr.addBookmark(fileA, 10.0, QStringLiteral("A1"), makeOrigin());
        mgr.addBookmark(fileA, 20.0, QStringLiteral("A2"), makeOrigin()); // 同文件第二条：数字子键
        mgr.addBookmark(fileB, 30.0, QStringLiteral("B1"),
                        Origin{uuidB, {QStringLiteral("D:/single")}}); // 第二条书签组
    }
    BookmarkManager again(m_ini);
    again.load();
    const Group gA = groupOf(fileA, defaultUuid());
    const Group gB = groupOf(fileB, uuidB);
    const auto a = again.bookmarks(gA);
    QCOMPARE(a.size(), 2);
    QCOMPARE(a.at(0).title, QStringLiteral("A1"));
    QCOMPARE(a.at(1).title, QStringLiteral("A2"));
    QCOMPARE(a.at(1).time, 20.0);
    QCOMPARE(again.bookmarks(gB).size(), 1);
    QCOMPARE(again.bookmarkOrigin(gA).levelIds.size(), 2);
    QCOMPARE(again.bookmarkOrigin(gA).levelIds.at(1), QStringLiteral("D:/media/动漫/2024"));
    QCOMPARE(again.bookmarkOrigin(gB).levelIds,
             QStringList{QStringLiteral("D:/single")}); // 单元素数组键同样还原
    QCOMPARE(again.allFiles().size(), 2);
}

void TestBookmarkManager::oddIdsAndPaths() {
    const QString file = QString::fromUtf8("D:/媒体;带,逗号%号/第3季#final.mkv");
    const Origin o{uuidFrom("cafe0000-0000-0000-0000-000000000001"),
                   {QString::fromUtf8("D:/媒体/a;b"), QString::fromUtf8("D:/媒体/c,d"),
                    QStringLiteral("D:/media/100%"), QStringLiteral("D:\\win\\native\\path"),
                    QString::fromUtf8("https://host/dav/目录")}};
    {
        BookmarkManager mgr(m_ini);
        mgr.load();
        mgr.addBookmark(file, 1.0, QStringLiteral("X"), o);
    }
    BookmarkManager again(m_ini);
    again.load();
    const Group g = groupOf(file, o.listUuid);
    const Origin got = again.bookmarkOrigin(g);
    QCOMPARE(got.levelIds, o.levelIds); // 逐字节：分隔符、百分号、反斜杠、非 ASCII 均不转义丢
    QCOMPARE(got.listUuid.toString(), o.listUuid.toString());
    QCOMPARE(again.displayPath(g), file);
}

// 空来源（默认 tab 出生）不留任何痕迹：两个键都不写，而非写空串占位
void TestBookmarkManager::emptyOriginWritesNoKeys() {
    const QString file = QStringLiteral("D:/plain.mkv");
    {
        BookmarkManager mgr(m_ini);
        mgr.load();
        mgr.addBookmark(file, 5.0, QStringLiteral("B"), Origin{});
    }
    const QSettings s(m_ini, QSettings::IniFormat); // 直读落盘形态，不经模型
    QVERIFY(!s.contains(QStringLiteral("Bookmarks/0/listUuid")));
    QVERIFY(!s.contains(QStringLiteral("Bookmarks/0/levelIds")));
    QVERIFY(s.contains(QStringLiteral("Bookmarks/0/path"))); // 本体照写
    BookmarkManager again(m_ini);
    again.load();
    QVERIFY(!again.bookmarkOrigin(groupOf(file, QUuid())).isValid());
}

// 旧 INI 形态：只有 path/count/time/title 两代键，无来源键 → 空来源，不迁移不猜测
void TestBookmarkManager::missingKeysYieldEmptyOrigin() {
    const QString file = QStringLiteral("D:/legacy.mkv");
    const Group g = groupOf(file, QUuid());
    {
        QSettings s(m_ini, QSettings::IniFormat);
        s.setValue(QStringLiteral("Bookmarks/size"), 1);
        s.setValue(QStringLiteral("Bookmarks/0/path"), file);
        s.setValue(QStringLiteral("Bookmarks/0/count"), 1);
        s.setValue(QStringLiteral("Bookmarks/0/0/time"), 42.0);
        s.setValue(QStringLiteral("Bookmarks/0/0/title"), QStringLiteral("Old"));
        s.sync();
    }
    BookmarkManager mgr(m_ini);
    mgr.load();
    QCOMPARE(mgr.bookmarks(g).size(), 1);
    QCOMPARE(mgr.bookmarks(g).at(0).time, 42.0);
    const Origin o = mgr.bookmarkOrigin(g);
    QVERIFY(!o.isValid());
    QVERIFY(o.listUuid.isNull()); // N57 §2.4：nil 即无身份（旧为空串）
    QVERIFY(o.levelIds.isEmpty());
    // 回写一次也不无中生有：仍不落来源键
    mgr.save();
    const QSettings after(m_ini, QSettings::IniFormat);
    QVERIFY(!after.contains(QStringLiteral("Bookmarks/0/listUuid")));
}

// uuid 有值 + levelIds 空 = 根层（静态手工列表即此形态），必须与"整条无来源"可区分
void TestBookmarkManager::rootLevelDistinguishedFromNoOrigin() {
    const QString fileRoot = QStringLiteral("D:/root-level.mkv");
    const QString fileNone = QStringLiteral("D:/no-origin.mkv");
    const QUuid uuid = uuidFrom("9e8d7c6b-5a49-3b2c-1d0e-fabcdef01234");
    {
        BookmarkManager mgr(m_ini);
        mgr.load();
        mgr.addBookmark(fileRoot, 1.0, QStringLiteral("R"), Origin{uuid, {}});
        mgr.addBookmark(fileNone, 2.0, QStringLiteral("N"), Origin{});
    }
    BookmarkManager again(m_ini);
    again.load();
    const Origin root = again.bookmarkOrigin(groupOf(fileRoot, uuid));
    QVERIFY(root.isValid());
    QVERIFY(root.levelIds.isEmpty());
    QVERIFY(!again.bookmarkOrigin(groupOf(fileNone, QUuid())).isValid());
}

// N77：组内首见即定——同组（同 uuid 同键）再加书签只追加条目，来源与展示路径均不
// 被后来者改写（展示路径换了大小写形态也仍保首见原样）；跨 uuid 不再共组，见
// crossGroupIsolation
void TestBookmarkManager::originFirstWriteWins() {
    const QString file = QStringLiteral("D:/twice.mkv");
    const QString fileRecase = QStringLiteral("D:/Twice.MKV"); // 同键异形
    const Group g = groupOf(file, defaultUuid());
    const Origin o = makeOrigin();
    BookmarkManager mgr(m_ini);
    mgr.load();
    mgr.addBookmark(file, 1.0, QStringLiteral("First"), o);
    mgr.addBookmark(fileRecase, 2.0, QStringLiteral("Second"), o);
    QCOMPARE(mgr.bookmarks(g).size(), 2); // 组内条目照常追加
    QCOMPARE(mgr.bookmarkOrigin(g).listUuid.toString(), o.listUuid.toString());
    QCOMPARE(mgr.displayPath(g), file); // 展示路径仍是首见形态
    BookmarkManager again(m_ini);
    again.load();
    QCOMPARE(again.bookmarks(g).size(), 2);
    QCOMPARE(again.bookmarkOrigin(g).listUuid.toString(),
             o.listUuid.toString()); // 落盘后仍是首见者
    QCOMPARE(again.displayPath(g), file);
}

// removeInvalid 的目录形态口径（N54 §2.7）：判据取 QFileInfo::exists，目录不再被当
// 失效清除（mpv 能播目录，实体侧 exists() 同形）；不存在的文件照清，且来源一并清掉。
// N57 §2.4：谓参必传——本例两项都是本地项，磁盘就是归属，存活判据根本不参与，故给
// 一个恒真谓词（原"无谓词"入参形态随未接档一并消失）
void TestBookmarkManager::removeInvalidKeepsDirectories() {
    const QString dir = m_dir.path(); // 真实存在的目录
    const QString missing = m_dir.filePath(QStringLiteral("gone.mkv"));
    BookmarkManager mgr(m_ini);
    mgr.load();
    mgr.addBookmark(dir, 1.0, QStringLiteral("Dir"), makeOrigin());
    mgr.addBookmark(missing, 2.0, QStringLiteral("Gone"), makeOrigin());
    QCOMPARE(mgr.allFiles().size(), 2);
    QCOMPARE(mgr.removeInvalid([](const QUuid &) { return true; }), 1);
    QCOMPARE(mgr.allFiles().size(), 1);
    const Group gDir = groupOf(dir, defaultUuid());
    QVERIFY(mgr.bookmarks(gDir).size() == 1);
    QVERIFY(mgr.bookmarkOrigin(gDir).isValid()); // 保留项的来源不丢
    // 被清掉的失效项：模型与落盘两侧都不留来源残骸
    const QSettings s(m_ini, QSettings::IniFormat);
    QCOMPARE(s.value(QStringLiteral("Bookmarks/size")).toInt(), 1);
    QVERIFY(!s.contains(QStringLiteral("Bookmarks/1/listUuid")));
}

// N54：远端项按归属判失效（不做 INI 兼容与迁移 → 无来源身份即数据异常；来源列表已删
// → 跳转必被拒，因为远端不回退、凭据与转接端点都随列表一起没了）。本地项不看归属：
// 磁盘就是它的归属。
// N57 §2.4：原"判据未接只清无来源那一半"的档已删（生产侧唯一调用方恒接入谓词，该档
// 无调用形态），锁它的那半条用例随之退场——"未接谓词"这一入参形态自本版不存在
void TestBookmarkManager::removeInvalidDropsRemoteWithoutLiveList() {
    const QUuid liveUuid = uuidFrom("aaaaaaaa-0000-0000-0000-000000000001");
    const QUuid goneUuid = uuidFrom("bbbbbbbb-0000-0000-0000-000000000002");
    const QString remoteLive = QStringLiteral("https://host/dav/live.mkv");
    const QString remoteDead = QStringLiteral("https://host/dav/dead.mkv");
    const QString remoteOrphan = QStringLiteral("https://host/dav/orphan.mkv");
    const QString localFile = m_dir.filePath(QStringLiteral("here.mkv"));
    {
        QFile f(localFile);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    BookmarkManager mgr(m_ini);
    mgr.load();
    const auto repopulate = [&]() {
        mgr.clearAll();
        mgr.addBookmark(remoteLive, 1.0, QStringLiteral("L"), Origin{liveUuid, {}});
        mgr.addBookmark(remoteDead, 2.0, QStringLiteral("D"), Origin{goneUuid, {}});
        mgr.addBookmark(remoteOrphan, 3.0, QStringLiteral("O"), Origin{});
        mgr.addBookmark(localFile, 4.0, QStringLiteral("F"), Origin{});
    };

    // 来源列表已删者一并清，本地项恒留
    repopulate();
    const auto alive = [&](const QUuid &uuid) { return uuid == liveUuid; };
    QCOMPARE(mgr.removeInvalid(alive), 2);
    QCOMPARE(mgr.allFiles().size(), 2);
    QCOMPARE(mgr.bookmarks(groupOf(remoteLive, liveUuid)).size(), 1);
    QCOMPARE(mgr.bookmarks(groupOf(localFile, QUuid())).size(), 1);
    // 落盘侧同步：被清项的来源不留残骸（重读只看到两条）
    BookmarkManager again(m_ini);
    again.load();
    QCOMPARE(again.allFiles().size(), 2);
    QVERIFY(!again.bookmarkOrigin(groupOf(remoteDead, goneUuid)).isValid());
    QVERIFY(!again.bookmarkOrigin(groupOf(remoteOrphan, QUuid())).isValid());
}

// N56 §2.3：写侧剥除取消——addBookmark 存的串来自正播实体 path()（不含提供者注入
// 凭据，用户显式提供的凭据如实保留），展示路径 = 身份串原样。入库前剥 userinfo 的
// 旧行为随 N48 出口一并取消（§2.4：书签层完全豁免显示纪律）
void TestBookmarkManager::addBookmarkKeepsRawIdentity() {
    const QString creded = QStringLiteral("https://user:pw@host/dav/file.mkv");
    {
        BookmarkManager mgr(m_ini);
        mgr.load();
        mgr.addBookmark(creded, 5.0, QStringLiteral("Bookmark 1"), Origin{});
    }
    BookmarkManager again(m_ini);
    again.load();
    const Group g = groupOf(creded, QUuid());
    // 键恒为无凭据形态（改密后既有书签继续有效）
    QVERIFY(!g.key.contains(QStringLiteral("user:pw")));
    QCOMPARE(again.bookmarks(g).size(), 1);
    // 展示路径原样保留（含 userinfo）——INI 的 path 与展示路径逐字一致
    QCOMPARE(again.displayPath(g), creded);
}

// N77 §2.6：跨列表同内容各成一组——A、B 两列表同含一文件，各加书签互不追加、互不
// 波及；删 A 组不影响 B 组（P-A 的解：不再有"首见霸占"）
void TestBookmarkManager::crossGroupIsolation() {
    const QString file = QStringLiteral("D:/shared.mkv");
    const QUuid uuidA = uuidFrom("11111111-1111-1111-1111-111111111111");
    const QUuid uuidB = uuidFrom("22222222-2222-2222-2222-222222222222");
    BookmarkManager mgr(m_ini);
    mgr.load();
    mgr.addBookmark(file, 1.0, QStringLiteral("A1"), makeOrigin(uuidA));
    mgr.addBookmark(file, 2.0, QStringLiteral("B1"), makeOrigin(uuidB));
    QCOMPARE(mgr.allFiles().size(), 2); // 同内容两组并存
    QCOMPARE(mgr.bookmarks(groupOf(file, uuidA)).size(), 1);
    QCOMPARE(mgr.bookmarks(groupOf(file, uuidB)).size(), 1);
    // A 再加一条：只进 A 组，B 组纹丝不动
    mgr.addBookmark(file, 1.5, QStringLiteral("A2"), makeOrigin(uuidA));
    QCOMPARE(mgr.bookmarks(groupOf(file, uuidA)).size(), 2);
    QCOMPARE(mgr.bookmarks(groupOf(file, uuidB)).size(), 1);
    QCOMPARE(mgr.bookmarkOrigin(groupOf(file, uuidB)).listUuid.toString(),
             uuidB.toString()); // B 组来源不被 A 的追加波及
    // 删 A 组：B 组本体与来源完好在位
    mgr.removeFile(groupOf(file, uuidA));
    QCOMPARE(mgr.allFiles().size(), 1);
    QCOMPARE(mgr.bookmarks(groupOf(file, uuidB)).size(), 1);
    QVERIFY(mgr.bookmarkOrigin(groupOf(file, uuidB)).isValid());
    // 格式零改动的直接证明：同键双 uuid 组落盘往返后仍是两组、来源各归各位
    BookmarkManager again(m_ini);
    again.load();
    QCOMPARE(again.allFiles().size(), 1);
    QCOMPARE(again.bookmarks(groupOf(file, uuidB)).size(), 1);
    QCOMPARE(again.bookmarkOrigin(groupOf(file, uuidB)).listUuid.toString(), uuidB.toString());
    QVERIFY(!again.bookmarkOrigin(groupOf(file, uuidA)).isValid());
}

// N77 §2.3：本地显示豁免——同键全部本地组按组序合并读；远端键判据不满足（调用方
// 走严格口径）。键空间按形态分域，本地与远端键天然不同
void TestBookmarkManager::localMergedRead() {
    const QString localFile = m_dir.filePath(QStringLiteral("shared.mkv"));
    const QString remote = QStringLiteral("https://host/dav/shared.mkv");
    const QUuid uuidA = uuidFrom("33333333-3333-3333-3333-333333333333");
    const QUuid uuidB = uuidFrom("44444444-4444-4444-4444-444444444444");
    {
        BookmarkManager mgr(m_ini);
        mgr.load();
        // A 组先建且条目时间更晚、B 组条目更早：合并读仍按组序拼接（组序是记账口径，
        // 时间序由消费侧聚合器补排），此处把组序钉住
        mgr.addBookmark(localFile, 10.0, QStringLiteral("A1"), makeOrigin(uuidA));
        mgr.addBookmark(localFile, 5.0, QStringLiteral("B1"), makeOrigin(uuidB));
        // 本地键：两组全本地，判据成立，合并读按组序逐组返回
        QVERIFY(mgr.allGroupsLocal(Util::Url::key(localFile)));
        const auto merged = mgr.groupsByKey(Util::Url::key(localFile));
        QCOMPARE(merged.size(), 2);
        QCOMPARE(merged.at(0).group.listUuid.toString(), uuidA.toString());
        QCOMPARE(merged.at(0).group.key, Util::Url::key(localFile));
        QCOMPARE(merged.at(0).bookmarks.size(), 1);
        QCOMPARE(merged.at(0).bookmarks.at(0).time, 10.0); // 组序优先，不因 B 组条目更早而换位
        QCOMPARE(merged.at(1).bookmarks.at(0).time, 5.0);
        QCOMPARE(merged.at(0).origin.listUuid.toString(), uuidA.toString());
        QCOMPARE(merged.at(1).origin.listUuid.toString(), uuidB.toString());
        // 远端键（哪怕同文件名）：判据不成立
        mgr.clearAll();
        mgr.addBookmark(remote, 1.0, QStringLiteral("A1"), makeOrigin(uuidA));
        mgr.addBookmark(remote, 2.0, QStringLiteral("B1"), makeOrigin(uuidB));
        QVERIFY(!mgr.allGroupsLocal(Util::Url::key(remote)));
    }
}

// N77 §2.5：过户·迁移档——目标组缺席 ⇒ 整体迁移：组 uuid 改默认、来源 uuid 同步
// 改写、levelIds 清空（空配有效身份即根层）、组序原位保留
void TestBookmarkManager::transferToDefaultMigrates() {
    const QString file = QStringLiteral("D:/migrate.mkv");
    const QUuid uuidX = uuidFrom("55555555-5555-5555-5555-555555555555");
    const Origin o = makeOrigin(uuidX);
    BookmarkManager mgr(m_ini);
    mgr.load();
    mgr.addBookmark(file, 1.0, QStringLiteral("X1"), o);
    const int oldPos = mgr.allFiles().indexOf(groupOf(file, uuidX));
    QVERIFY(oldPos >= 0);
    mgr.transferToDefault(uuidX, Util::Url::key(file));
    const Group gDefault = groupOf(file, kDefaultListUuid);
    QCOMPARE(mgr.allFiles().size(), 1);
    QCOMPARE(mgr.allFiles().at(0).listUuid.toString(),
             kDefaultListUuid.toString()); // 旧组消失，默认组原位顶上
    QCOMPARE(mgr.allFiles().at(0).key, gDefault.key);
    QCOMPARE(mgr.allFiles().indexOf(gDefault), oldPos);
    QCOMPARE(mgr.bookmarks(gDefault).size(), 1);
    QCOMPARE(mgr.bookmarks(gDefault).at(0).title, QStringLiteral("X1"));
    const Origin got = mgr.bookmarkOrigin(gDefault); // 来源随组改写、层清空
    QCOMPARE(got.listUuid.toString(), kDefaultListUuid.toString());
    QVERIFY(got.levelIds.isEmpty());
    // 落盘形态：uuid 分量即默认身份（往返一致）
    BookmarkManager again(m_ini);
    again.load();
    QCOMPARE(again.bookmarks(gDefault).size(), 1);
    QCOMPARE(again.bookmarkOrigin(gDefault).listUuid.toString(), kDefaultListUuid.toString());
}

// N77 §2.5：过户·合并档——目标组已存在 ⇒ 书签并入后按时间稳定排序、不去重；展示串
// 保留目标既有；源组层引用丢弃
void TestBookmarkManager::transferToDefaultMerges() {
    const QString file = QStringLiteral("D:/merge.mkv");
    const QString fileRecase = QStringLiteral("D:/Merge.MKV");
    const QUuid uuidX = uuidFrom("66666666-6666-6666-6666-666666666666");
    BookmarkManager mgr(m_ini);
    mgr.load();
    // 默认组先在位（展示形态为 recase），X 组随后（展示形态为原形态）
    mgr.addBookmark(fileRecase, 3.0, QStringLiteral("D1"), makeOrigin(kDefaultListUuid));
    mgr.addBookmark(file, 1.0, QStringLiteral("X1"), makeOrigin(uuidX));
    mgr.addBookmark(file, 2.0, QStringLiteral("X2"), makeOrigin(uuidX));
    mgr.transferToDefault(uuidX, Util::Url::key(file));
    const Group gDefault = groupOf(file, kDefaultListUuid);
    QCOMPARE(mgr.allFiles().size(), 1);
    const auto merged = mgr.bookmarks(gDefault);
    QCOMPARE(merged.size(), 3); // 不去重（永不查重一贯），并入后按时间稳定排序
    QCOMPARE(merged.at(0).title, QStringLiteral("X1"));
    QCOMPARE(merged.at(1).title, QStringLiteral("X2"));
    QCOMPARE(merged.at(2).title, QStringLiteral("D1"));
    QCOMPARE(mgr.displayPath(gDefault), fileRecase); // 展示串保留目标既有
    QCOMPARE(mgr.bookmarkOrigin(gDefault).levelIds,
             (QStringList{QStringLiteral("D:/media/动漫"),
                          QStringLiteral("D:/media/动漫/2024")})); // 目标来源不动（层跟目标组）
    QVERIFY(!mgr.allFiles().contains(groupOf(file, uuidX)));
}

// N77 §2.2：旧 INI 按条目自带 uuid 归组——同键不同 uuid 拆成不同组；缺键与非法串
// 同落 nil 独立组（互并入组，不并默认组）；同组多条目（旧混账）不拆分照常读回
void TestBookmarkManager::loadGroupsByEntryUuid() {
    const QString file = QStringLiteral("D:/legacy-groups.mkv");
    const QUuid uuidA = uuidFrom("77777777-7777-7777-7777-777777777777");
    const QUuid uuidB = uuidFrom("88888888-8888-8888-8888-888888888888");
    {
        QSettings s(m_ini, QSettings::IniFormat);
        s.setValue(QStringLiteral("Bookmarks/size"), 5);
        s.setValue(QStringLiteral("Bookmarks/0/path"), file);
        s.setValue(QStringLiteral("Bookmarks/0/listUuid"),
                   uuidA.toString(QUuid::StringFormat::WithoutBraces));
        s.setValue(QStringLiteral("Bookmarks/0/count"), 2); // 旧混账：一组建两条
        s.setValue(QStringLiteral("Bookmarks/0/0/time"), 1.0);
        s.setValue(QStringLiteral("Bookmarks/0/0/title"), QStringLiteral("A-old1"));
        s.setValue(QStringLiteral("Bookmarks/0/1/time"), 2.0);
        s.setValue(QStringLiteral("Bookmarks/0/1/title"), QStringLiteral("A-old2"));
        s.setValue(QStringLiteral("Bookmarks/1/path"), file);
        s.setValue(QStringLiteral("Bookmarks/1/listUuid"),
                   uuidB.toString(QUuid::StringFormat::WithoutBraces));
        s.setValue(QStringLiteral("Bookmarks/1/count"), 1);
        s.setValue(QStringLiteral("Bookmarks/1/0/time"), 3.0);
        s.setValue(QStringLiteral("Bookmarks/1/0/title"), QStringLiteral("B-old"));
        s.setValue(QStringLiteral("Bookmarks/2/path"), file); // 缺 uuid 键
        s.setValue(QStringLiteral("Bookmarks/2/count"), 1);
        s.setValue(QStringLiteral("Bookmarks/2/0/time"), 4.0);
        s.setValue(QStringLiteral("Bookmarks/2/0/title"), QStringLiteral("N1"));
        s.setValue(QStringLiteral("Bookmarks/3/path"), file);
        s.setValue(QStringLiteral("Bookmarks/3/listUuid"), QStringLiteral("not-a-uuid"));
        s.setValue(QStringLiteral("Bookmarks/3/count"), 1);
        s.setValue(QStringLiteral("Bookmarks/3/0/time"), 5.0);
        s.setValue(QStringLiteral("Bookmarks/3/0/title"), QStringLiteral("N2"));
        s.setValue(QStringLiteral("Bookmarks/4/path"), file); // 与 0 同 uuid（理论不出现）
        s.setValue(QStringLiteral("Bookmarks/4/listUuid"),
                   uuidA.toString(QUuid::StringFormat::WithoutBraces));
        s.setValue(QStringLiteral("Bookmarks/4/count"), 1);
        s.setValue(QStringLiteral("Bookmarks/4/0/time"), 0.5);
        s.setValue(QStringLiteral("Bookmarks/4/0/title"), QStringLiteral("A-late"));
        s.sync();
    }
    BookmarkManager mgr(m_ini);
    mgr.load();
    const QString key = Util::Url::key(file);
    QCOMPARE(mgr.allFiles().size(), 3); // A、B、nil 三组（非法串与缺键同归 nil）
    const auto ga = mgr.bookmarks(Group{uuidA, key});
    QCOMPARE(ga.size(), 3);                             // 混账不拆：同组条目合并读回
    QCOMPARE(ga.at(0).title, QStringLiteral("A-late")); // 合并后按时间稳定排序
    QCOMPARE(ga.at(1).title, QStringLiteral("A-old1"));
    QCOMPARE(ga.at(2).title, QStringLiteral("A-old2"));
    QCOMPARE(mgr.bookmarks(Group{uuidB, key}).size(), 1);
    const auto gn = mgr.bookmarks(Group{QUuid(), key});
    QCOMPARE(gn.size(), 2); // 缺键与非法串并成 nil 一组
    QCOMPARE(mgr.bookmarkOrigin(Group{uuidA, key}).listUuid.toString(), uuidA.toString());
    QVERIFY(!mgr.bookmarkOrigin(Group{QUuid(), key}).isValid());
    // nil 组不并默认组：默认身份组缺席
    QVERIFY(!mgr.allFiles().contains(Group{kDefaultListUuid, key}));
}

QTEST_GUILESS_MAIN(TestBookmarkManager)

#include "tst_BookmarkManager.moc"

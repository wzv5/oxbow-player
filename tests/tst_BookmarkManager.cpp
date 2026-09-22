// BookmarkManager 单元测试（N54）：书签来源（列表身份 + 所在层）的落盘与读回。
// 覆盖：两键往返；数组键与 Bookmarks/<i>/<j>/… 数字子键不串台（本轮唯一的格式风险）；
//   含分隔符与非 ASCII 的 id 逐字节还原；空来源不落键、缺键即空来源（旧 INI 形态）；
//   "根层"与"整条无来源"可区分；同键首见即定不覆盖；removeInvalid 保留目录形态键、
//   远端项按归属判失效（无来源 / 来源列表已删两类均清，判据回调未接时只清前者）。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R BookmarkManager 跑本套件；
//   mpvqt_tests_BookmarkManager <槽名> 跑单个用例。

#include "pch.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "BookmarkManager.h"
#include "Util.h"

using Origin = BookmarkManager::Origin;

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

  private:
    // 每个用例一份独立 INI（init 里换名），避免用例间互扰
    QString m_ini;
    int m_seq = 0;
    QTemporaryDir m_dir;

    static Origin
    makeOrigin(const QString &uuid = QStringLiteral("2f1a9c33-7b04-4e58-9a2d-1c6f0b3d8e44")) {
        return Origin{uuid,
                      {QStringLiteral("D:/media/动漫"), QStringLiteral("D:/media/动漫/2024")}};
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
    const QString key = Util::Url::key(path);
    const Origin got = again.bookmarkOrigin(key);
    QCOMPARE(got.listUuid, o.listUuid);
    QCOMPARE(got.levelIds, o.levelIds);
    // 书签本体不受来源字段影响：一条、时间原样
    const auto marks = again.bookmarks(key);
    QCOMPARE(marks.size(), 1);
    QCOMPARE(marks.at(0).time, 12.5);
}

// 数组键与"同组内数字子键"共存是本轮唯一的格式风险：levelIds 走 QSettings 原生
// QStringList（形如 levelIds\1=…），而书签条目用的是 0\0\time、0\1\time 这类数字
// 子键。两者必须各自还原，不得互相吃掉。
void TestBookmarkManager::numericSubkeysNoAlias() {
    const QString fileA = QStringLiteral("D:/media/a.mkv");
    const QString fileB = QStringLiteral("D:/media/b.mkv");
    {
        BookmarkManager mgr(m_ini);
        mgr.load();
        mgr.addBookmark(fileA, 10.0, QStringLiteral("A1"), makeOrigin());
        mgr.addBookmark(fileA, 20.0, QStringLiteral("A2"), makeOrigin()); // 同文件第二条：数字子键
        mgr.addBookmark(fileB, 30.0, QStringLiteral("B1"),
                        Origin{QStringLiteral("bbbbbbbb-1111-2222-3333-444444444444"),
                               {QStringLiteral("D:/single")}}); // 第二条书签组
    }
    BookmarkManager again(m_ini);
    again.load();
    const QString keyA = Util::Url::key(fileA);
    const QString keyB = Util::Url::key(fileB);
    const auto a = again.bookmarks(keyA);
    QCOMPARE(a.size(), 2);
    QCOMPARE(a.at(0).title, QStringLiteral("A1"));
    QCOMPARE(a.at(1).title, QStringLiteral("A2"));
    QCOMPARE(a.at(1).time, 20.0);
    QCOMPARE(again.bookmarks(keyB).size(), 1);
    QCOMPARE(again.bookmarkOrigin(keyA).levelIds.size(), 2);
    QCOMPARE(again.bookmarkOrigin(keyA).levelIds.at(1), QStringLiteral("D:/media/动漫/2024"));
    QCOMPARE(again.bookmarkOrigin(keyB).levelIds,
             QStringList{QStringLiteral("D:/single")}); // 单元素数组键同样还原
    QCOMPARE(again.allFiles().size(), 2);
}

void TestBookmarkManager::oddIdsAndPaths() {
    const QString file = QString::fromUtf8("D:/媒体;带,逗号%号/第3季#final.mkv");
    const Origin o{QStringLiteral("cafe0000-0000-0000-0000-000000000001"),
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
    const Origin got = again.bookmarkOrigin(Util::Url::key(file));
    QCOMPARE(got.levelIds, o.levelIds); // 逐字节：分隔符、百分号、反斜杠、非 ASCII 均不转义丢
    QCOMPARE(got.listUuid, o.listUuid);
    QCOMPARE(again.displayPath(Util::Url::key(file)), file);
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
    QVERIFY(!again.bookmarkOrigin(Util::Url::key(file)).isValid());
}

// 旧 INI 形态：只有 path/count/time/title 两代键，无来源键 → 空来源，不迁移不猜测
void TestBookmarkManager::missingKeysYieldEmptyOrigin() {
    const QString file = QStringLiteral("D:/legacy.mkv");
    const QString key = Util::Url::key(file);
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
    QCOMPARE(mgr.bookmarks(key).size(), 1);
    QCOMPARE(mgr.bookmarks(key).at(0).time, 42.0);
    const Origin o = mgr.bookmarkOrigin(key);
    QVERIFY(!o.isValid());
    QVERIFY(o.listUuid.isEmpty());
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
    const QString uuid = QStringLiteral("9e8d7c6b-5a49-3b2c-1d0e-fabcdef01234");
    {
        BookmarkManager mgr(m_ini);
        mgr.load();
        mgr.addBookmark(fileRoot, 1.0, QStringLiteral("R"), Origin{uuid, {}});
        mgr.addBookmark(fileNone, 2.0, QStringLiteral("N"), Origin{});
    }
    BookmarkManager again(m_ini);
    again.load();
    const Origin root = again.bookmarkOrigin(Util::Url::key(fileRoot));
    QVERIFY(root.isValid());
    QVERIFY(root.levelIds.isEmpty());
    QVERIFY(!again.bookmarkOrigin(Util::Url::key(fileNone)).isValid());
}

// 同一文件从别的列表再加书签：来源首见即定，后来者不覆盖（与展示路径同纪律）
void TestBookmarkManager::originFirstWriteWins() {
    const QString file = QStringLiteral("D:/twice.mkv");
    const QString key = Util::Url::key(file);
    const Origin first = makeOrigin(QStringLiteral("11111111-1111-1111-1111-111111111111"));
    const Origin later = makeOrigin(QStringLiteral("22222222-2222-2222-2222-222222222222"));
    BookmarkManager mgr(m_ini);
    mgr.load();
    mgr.addBookmark(file, 1.0, QStringLiteral("First"), first);
    mgr.addBookmark(file, 2.0, QStringLiteral("Second"), later);
    QCOMPARE(mgr.bookmarkOrigin(key).listUuid, first.listUuid);
    QCOMPARE(mgr.bookmarks(key).size(), 2); // 条目照常追加
    BookmarkManager again(m_ini);
    again.load();
    QCOMPARE(again.bookmarkOrigin(key).listUuid, first.listUuid); // 落盘后仍是首见者
}

// removeInvalid 的目录形态口径（N54 §2.7）：判据取 QFileInfo::exists，目录不再被当
// 失效清除（mpv 能播目录，实体侧 exists() 同形）；不存在的文件照清，且来源一并清掉
void TestBookmarkManager::removeInvalidKeepsDirectories() {
    const QString dir = m_dir.path(); // 真实存在的目录
    const QString missing = m_dir.filePath(QStringLiteral("gone.mkv"));
    BookmarkManager mgr(m_ini);
    mgr.load();
    mgr.addBookmark(dir, 1.0, QStringLiteral("Dir"), makeOrigin());
    mgr.addBookmark(missing, 2.0, QStringLiteral("Gone"), makeOrigin());
    QCOMPARE(mgr.allFiles().size(), 2);
    QCOMPARE(mgr.removeInvalid(), 1);
    QCOMPARE(mgr.allFiles().size(), 1);
    const QString dirKey = Util::Url::key(dir);
    QVERIFY(mgr.bookmarks(dirKey).size() == 1);
    QVERIFY(mgr.bookmarkOrigin(dirKey).isValid()); // 保留项的来源不丢
    // 被清掉的失效项：模型与落盘两侧都不留来源残骸
    const QSettings s(m_ini, QSettings::IniFormat);
    QCOMPARE(s.value(QStringLiteral("Bookmarks/size")).toInt(), 1);
    QVERIFY(!s.contains(QStringLiteral("Bookmarks/1/listUuid")));
}

// N54：远端项按归属判失效（不做 INI 兼容与迁移 → 无来源 uuid 即数据异常；来源列表已删
// → 跳转必被拒，因为远端不回退、凭据与转接端点都随列表一起没了）。本地项不看归属：
// 磁盘就是它的归属。判据回调未接时只按“无来源”那一半清，有来源的无从判定即不删
void TestBookmarkManager::removeInvalidDropsRemoteWithoutLiveList() {
    const QString liveUuid = QStringLiteral("aaaaaaaa-0000-0000-0000-000000000001");
    const QString goneUuid = QStringLiteral("bbbbbbbb-0000-0000-0000-000000000002");
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

    // 判据未接：只清"无来源的远端项"这一半，有来源的一律留（不据未知信息误删）
    repopulate();
    QCOMPARE(mgr.removeInvalid(BookmarkManager::ListAlivePredicate{}), 1);
    QCOMPARE(mgr.allFiles().size(), 3);
    QCOMPARE(mgr.bookmarks(Util::Url::key(remoteLive)).size(), 1);
    QCOMPARE(mgr.bookmarks(Util::Url::key(remoteDead)).size(), 1);

    // 判据接上：来源列表已删者一并清，本地项恒留
    repopulate();
    const auto alive = [&](const QString &uuid) { return uuid == liveUuid; };
    QCOMPARE(mgr.removeInvalid(alive), 2);
    QCOMPARE(mgr.allFiles().size(), 2);
    QCOMPARE(mgr.bookmarks(Util::Url::key(remoteLive)).size(), 1);
    QCOMPARE(mgr.bookmarks(Util::Url::key(localFile)).size(), 1);
    // 落盘侧同步：被清项的来源不留残骸（重读只看到两条）
    BookmarkManager again(m_ini);
    again.load();
    QCOMPARE(again.allFiles().size(), 2);
    QVERIFY(!again.bookmarkOrigin(Util::Url::key(remoteDead)).isValid());
    QVERIFY(!again.bookmarkOrigin(Util::Url::key(remoteOrphan)).isValid());
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
    const QString key = Util::Url::key(creded);
    // 键恒为无凭据形态（改密后既有书签继续有效）
    QVERIFY(!key.contains(QStringLiteral("user:pw")));
    QCOMPARE(again.bookmarks(key).size(), 1);
    // 展示路径原样保留（含 userinfo）——INI 的 path 与展示路径逐字一致
    QCOMPARE(again.displayPath(key), creded);
}

QTEST_GUILESS_MAIN(TestBookmarkManager)

#include "tst_BookmarkManager.moc"

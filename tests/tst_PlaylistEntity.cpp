// PlaylistEntity 单元测试（N26）：工厂分派与往返幂等、命名派生与修正联动、
// 惰性求值与缓存清空、能力位与静态空回落、clone 深拷贝互不影响。
// 测试无副作用：文件级 case 用 QTemporaryDir 隔离，结束时自动清理。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R PlaylistEntity 跑本套件；
//   mpvqt_tests_PlaylistEntity <槽名> 跑单个用例。

#include "pch.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "HttpEntity.h"
#include "LocalFileEntity.h"
#include "PlaylistEntity.h"
#include "PlaylistProvider.h" // N55：归属注入口的形参类型与转接能力枚举
#include "ServiceRegistry.h"
#include "StaticProvider.h" // N55 §九：恒等提供者验归属现算
#include "StreamRelay.h"    // N55 §九：StreamRelayIf 取用接缝（纯头，测试桩实现）
#include "StreamTransit.h"  // N61 变更记录第 1 条：回显入口的 mint / unwrap 对照
#include "Util.h"

// 测试桩实体：compute* 带调用计数，能力位可调，保护修正入口透出。
// 计数器 mutable（compute* 为 const 惰性入口）；随拷贝复制，断言前按需归零
namespace {
class TestEntity : public PlaylistEntity {
  public:
    explicit TestEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<TestEntity>(*this); }

    bool isLocalFile() const override { return m_local; }
    bool exists() const override { return m_exists; }
    bool supportsFileMeta() const override { return m_supportFile; }
    bool supportsMediaMeta() const override { return m_supportMedia; }

    void setLocal(bool on) { m_local = on; }
    void setExists(bool on) { m_exists = on; }
    void setSupportFile(bool on) { m_supportFile = on; }
    void setSupportMedia(bool on) { m_supportMedia = on; }
    // 保护修正入口透出（命名派生联动测试）
    void applyFilename(const QString &name) { setFilename(name); }
    void applyTitle(const QString &title) { setTitle(title); }
    // N55：归属注入口透出——protected + 友元，经子类公开转发盖章，不放宽可见性
    void applyOwnership(QUuid uuid, std::weak_ptr<const PlaylistProvider> provider) {
        setOwnership(std::move(uuid), std::move(provider));
    }

    mutable int fileComputeCount = 0;
    mutable int mediaComputeCount = 0;
    mutable int iconComputeCount = 0;

  protected:
    FileMeta computeFileMeta() const override {
        ++fileComputeCount;
        FileMeta meta;
        meta.size = 123;
        return meta;
    }
    MediaMeta computeMediaMeta() const override {
        ++mediaComputeCount;
        MediaMeta meta;
        meta.duration = 4.5;
        return meta;
    }
    QIcon computeIcon() const override {
        ++iconComputeCount;
        return QIcon(); // 空图标：GUILESS 环境下不触碰像素后端
    }

  private:
    bool m_local = true;
    bool m_exists = true;
    bool m_supportFile = true;
    bool m_supportMedia = true;
};

// 测试用注册表：按需取新实例。registry 无缺省，调用点永不省略、显式传入可用实例
inline std::shared_ptr<ServiceRegistry> testRegistry() {
    return std::make_shared<ServiceRegistry>();
}

// ——N55 测试桩——
// 测试端点：openStream 恒失败。用例只走铸造与发布路径，不起流
class TestEndpoint : public StreamEndpoint {
  public:
    std::expected<std::shared_ptr<ByteStream>, QString>
    openStream(const QString &identity) const override {
        Q_UNUSED(identity);
        return std::unexpected(QStringLiteral("test endpoint never opens"));
    }
};

// 取用侧桩：只记录 publish。StreamRelayIf 为纯头抽象基（L4 实现体不入测试链接，
// 见该头注释），面向接缝编程正是 N52 留下的可测试缝
class TestRelay : public StreamRelayIf {
  public:
    void publish(const QUuid &listUuid, const std::shared_ptr<StreamEndpoint> &endpoint) override {
        publishedUuid = listUuid;
        lastEndpoint = endpoint;
    }
    QUuid publishedUuid;
    std::shared_ptr<StreamEndpoint> lastEndpoint;
};

// 转接测试提供者：Required（形态恒 mpvqt:// 铸造，偏好关不掉），端点返回测试桩
class TransitTestProvider : public PlaylistProvider {
  public:
    explicit TransitTestProvider(const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistProvider(registry) {}

    QString typeKey() const override { return QStringLiteral("test-transit"); }
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override {
        Q_UNUSED(parent);
        return Enumeration{};
    }
    bool supportsModify() const override { return false; }
    void saveSettings(QSettings &settings) const override { Q_UNUSED(settings); } // 用例不落盘
    TransitCapability transitCapability() const override { return TransitCapability::Required; }
    std::shared_ptr<StreamEndpoint> createStreamEndpoint() const override {
        return std::make_shared<TestEndpoint>();
    }
};
} // namespace

class TestPlaylistEntity : public QObject {
    Q_OBJECT

  private slots:
    // 工厂分派：本地路径 → LocalFileEntity，非 http 网络/流 URL → UnknownEntity 兜底
    void factoryDispatchesLocalAndNetwork();
    // N35 工厂分派：http(s) + registry → HttpEntity（大小写不敏感前缀）
    void factoryDispatchesHttpWithRegistry();
    // N35 HttpEntity 往返不变式：fromPath(e->path(), registry)->path() 相等
    void httpEntityRoundtrip();
    // N39 HttpEntity 能力位与元数据回落（两类能力位均为假，无网络求值）
    void httpEntityCapabilities();
    // N35 显示名：percent-decode + query/fragment 剥离；空末段回退基类行为
    void httpEntityDisplayName();
    // N35 key：网络路径保留原始大小写（逐字节）
    void httpEntityKeyPreservesCase();
    // 工厂往返幂等：fromPath(e->path(), registry)->path() == e->path()
    // （含相对路径加工；registry 必填，各用例显式传入）
    void factoryRoundtripIdempotent();
    // 构造期命名派生：fileName / suffix（无点小写）/ title（回退文件名）
    void namesDerivedInConstructor();
    // setFilename 联动：同步重算 suffix，title 未显式设置时一并同步
    void setFilenameSyncsSuffixAndTitle();
    // setTitle 显式设置后，setFilename 不再联动 title
    void setTitlePinsTitle();
    // fileMeta 惰性求值 + 实体内缓存 + refresh() 清空后重新求值
    void lazyFileMetaCachedAndRefreshed();
    // mediaMeta 惰性求值 + 实体内缓存 + refresh() 清空后重新求值
    void lazyMediaMetaCachedAndRefreshed();
    // icon 惰性求值 + 实体内缓存 + refresh() 清空后重新求值
    void lazyIconCachedAndRefreshed();
    // supportsFileMeta 为假：fileMeta() 静态空回落，compute 恒不被调，cached 恒 nullptr
    void unsupportedFileMetaNeverComputes();
    // supportsMediaMeta 为假：mediaMeta() 静态空回落，compute 恒不被调，cached 恒 nullptr
    void unsupportedMediaMetaNeverComputes();
    // UnknownEntity：能力位全假、exists 恒 true、元数据恒空、clone 身份一致
    void unknownEntityCapabilities();
    // LocalFileEntity：真实临时文件的 fileMeta 字段；不存在文件三字段全空（值未知）
    void localFileEntityFileMeta();
    // LocalFileEntity：exists 走 QFileInfo::exists
    void localFileEntityExists();
    // clone 深拷贝：副本携带已解析缓存，此后与原实体互不影响
    void cloneDeepCopiesCache();
    // key 默认实现 == Util::Url::key(path())
    void keyIsNormalizedPlayUrl();
    // N56 §2.5：键级判等——matches 原样串（形态漂移仍命中、不同串不命中）
    void matchesKeyLevelEquality();
    // N56 §2.4：显示出口——displayUrl 剥除 userinfo，path 原样保留
    void displayUrlStripsCredentials();
    // N61 变更记录第 1 条：回显专用入口 fromMpvEcho——已定形者与 fromPath 同类型同身份、
    // 转接串折回身份，补不出形态者（空 filename / 裸 file:）不撞断言而是落 UnknownEntity
    void echoEntryFallsBackToPlaceholder();
    // N55 §2.2/§2.3：归属注入口与出生属性——playUrl() 走归属现算（StaticProvider 恒等
    // 与转接提供者各验一次，断言形态出自归属提供者而非所在列表）
    void ownershipStampDrivesPlayUrl();
    // N55 §2.1：clone() 携带归属——克隆副本不经再盖章即由原归属（同一提供者 + 同一
    // 身份）加工（克隆后重新盖章的那一支随 N55 C2 的收编路径另测）
    void cloneCarriesOwnership();
    // N55 §2.3 锁失败面：归属提供者先销毁再 playUrl() → Release 契约返空串
    // （Debug 撞断言，不作断言用例：Debug 构建下本用例跳过）
    void ownershipLockFailureReturnsEmptyInRelease();
    // N55 §二：归属不参与判等——同一身份串盖不同归属，key() / matches() 结果不变
    void ownershipNotInEqualityCriteria();
};

void TestPlaylistEntity::factoryDispatchesLocalAndNetwork() {
    auto registry = std::make_shared<ServiceRegistry>();
    const QString local = QDir::tempPath() + QStringLiteral("/mpvqt_entity_dispatch.mp4");
    auto l = PlaylistEntity::fromPath(local, registry);
    QVERIFY(dynamic_cast<LocalFileEntity *>(l.get()) != nullptr);
    QVERIFY(dynamic_cast<UnknownEntity *>(l.get()) == nullptr);

    // N35：非 http 网络路径仍落 UnknownEntity 兜底（http 已改分派 HttpEntity）
    auto u =
        PlaylistEntity::fromPath(QStringLiteral("ftp://example.com/dir/video/a.mp4"), registry);
    QVERIFY(dynamic_cast<UnknownEntity *>(u.get()) != nullptr);
    QVERIFY(dynamic_cast<LocalFileEntity *>(u.get()) == nullptr);
    QVERIFY(dynamic_cast<HttpEntity *>(u.get()) == nullptr);
}

void TestPlaylistEntity::factoryDispatchesHttpWithRegistry() {
    auto registry = std::make_shared<ServiceRegistry>();
    auto h =
        PlaylistEntity::fromPath(QStringLiteral("https://example.com/dir/video/a.mp4"), registry);
    QVERIFY(dynamic_cast<HttpEntity *>(h.get()) != nullptr);
    QVERIFY(dynamic_cast<UnknownEntity *>(h.get()) == nullptr);

    // 前缀大小写不敏感
    auto upper = PlaylistEntity::fromPath(QStringLiteral("HTTP://example.com/a.mp4"), registry);
    QVERIFY(dynamic_cast<HttpEntity *>(upper.get()) != nullptr);

    // 本地路径忽略 registry（类型不受影响）
    auto l = PlaylistEntity::fromPath(
        QDir::tempPath() + QStringLiteral("/mpvqt_entity_http_local.mp4"), registry);
    QVERIFY(dynamic_cast<LocalFileEntity *>(l.get()) != nullptr);
}

void TestPlaylistEntity::httpEntityRoundtrip() {
    auto registry = std::make_shared<ServiceRegistry>();
    const QString url = QStringLiteral("https://user:pw@example.com/dir/a.mp4?token=x");
    auto e = PlaylistEntity::fromPath(url, registry);
    QVERIFY(dynamic_cast<HttpEntity *>(e.get()) != nullptr);
    QCOMPARE(e->path(), url); // 网络路径原样（含 userinfo / query）
    QCOMPARE(PlaylistEntity::fromPath(e->path(), registry)->path(), e->path());
}

void TestPlaylistEntity::httpEntityCapabilities() {
    auto registry = std::make_shared<ServiceRegistry>();
    auto e = PlaylistEntity::fromPath(QStringLiteral("https://example.com/a.mp4"), registry);
    QVERIFY(!e->isLocalFile());
    QVERIFY(e->exists()); // 不可判定，恒 true（不参与失效清理）
    // N39：两类能力位均为假——HttpEntity 已无任何取得元数据的途径（惰性 HEAD 已删），
    // fileMeta() 因此可安全调用（静态空回落，不触碰网络）
    QVERIFY(!e->supportsFileMeta());
    QVERIFY(!e->supportsMediaMeta());
    QVERIFY(!e->fileMeta().size.has_value());
    QVERIFY(!e->fileMeta().modified.has_value());
    QVERIFY(e->cachedFileMeta() == nullptr);
    QVERIFY(!e->mediaMeta().duration.has_value());
    QVERIFY(e->cachedMediaMeta() == nullptr);

    // clone：身份一致，类型不变
    auto c = e->clone();
    QVERIFY(dynamic_cast<HttpEntity *>(c.get()) != nullptr);
    QCOMPARE(c->path(), e->path());
    QVERIFY(!c->isLocalFile());
    QVERIFY(c->exists());
    QVERIFY(!c->supportsFileMeta());
    QVERIFY(!c->supportsMediaMeta());
}

void TestPlaylistEntity::httpEntityDisplayName() {
    auto registry = std::make_shared<ServiceRegistry>();

    // percent-encoded 中文与空格正确解码
    auto zh = PlaylistEntity::fromPath(
        QStringLiteral("https://example.com/dir/%E4%B8%AD%E6%96%87%20name.mp4"), registry);
    QCOMPARE(zh->fileName(), QStringLiteral("中文 name.mp4"));
    QCOMPARE(zh->title(), QStringLiteral("中文 name.mp4"));
    QCOMPARE(zh->suffix(), QStringLiteral("mp4"));

    // query / fragment 剥离：末段不混入 token
    auto q = PlaylistEntity::fromPath(
        QStringLiteral("https://example.com/v/movie.mkv?token=ab%2Fcd#frag"), registry);
    QCOMPARE(q->fileName(), QStringLiteral("movie.mkv"));
    QCOMPARE(q->title(), QStringLiteral("movie.mkv"));
    QCOMPARE(q->suffix(), QStringLiteral("mkv"));

    // 空末段（以 / 结尾）：解后为空回退基类行为（title = 身份串，fileName 空）
    auto root = PlaylistEntity::fromPath(QStringLiteral("https://example.com/"), registry);
    QCOMPARE(root->fileName(), QString());
    QCOMPARE(root->title(), QStringLiteral("https://example.com/"));
}

void TestPlaylistEntity::httpEntityKeyPreservesCase() {
    auto registry = std::make_shared<ServiceRegistry>();
    const QString url = QStringLiteral("https://Example.COM/V/a.mp4");
    auto e = PlaylistEntity::fromPath(url, registry);
    // N37：key 为匹配形态——scheme / host 归小写，path 大小写敏感逐字节保留
    // （N35 的"逐字节 key"口径随匹配形态收敛改写）；path() 本身不受影响
    QCOMPARE(e->path(), url);
    QCOMPARE(e->key(), QStringLiteral("https://example.com/V/a.mp4"));
}

void TestPlaylistEntity::factoryRoundtripIdempotent() {
    // 本地路径：入参已定形（绝对 + 原生分隔符），往返不变
    auto registry = std::make_shared<ServiceRegistry>();
    const QString local = QDir::tempPath() + QStringLiteral("/mpvqt_entity_roundtrip.mp4");
    auto e = PlaylistEntity::fromPath(local, registry);
    QCOMPARE(PlaylistEntity::fromPath(e->path(), registry)->path(), e->path());

    // N56 §2.6：工厂不再代做定形（相对→绝对等加工上移到造实体的一方），契约改为
    // "入参必须是已定形的身份串"；未定形串（相对路径、裸文件名）属调用方违约，
    // 由 Debug 断言守候——测试环境即 Debug，不可在此构造（断言会中止进程），
    // 改以"未定形输入不再被悄悄救回"为契约口径，定形正确性由入口处（Util::Url::toNative）
    // 的既有用例覆盖

    // 网络 / 流路径：原样保留（N35：用非 http scheme 验证，http 分派见 httpEntity* 用例）
    auto n = PlaylistEntity::fromPath(QStringLiteral("ftp://example.com/v/a.mp4"), registry);
    QCOMPARE(n->path(), QStringLiteral("ftp://example.com/v/a.mp4"));
    QCOMPARE(PlaylistEntity::fromPath(n->path(), registry)->path(), n->path());
}

void TestPlaylistEntity::namesDerivedInConstructor() {
    TestEntity e(QStringLiteral("some/dir/MyFile.MP4"), testRegistry());
    QCOMPARE(e.fileName(), QStringLiteral("MyFile.MP4"));
    QCOMPARE(e.suffix(), QStringLiteral("mp4")); // 无点小写
    QCOMPARE(e.title(), QStringLiteral("MyFile.MP4"));
}

void TestPlaylistEntity::setFilenameSyncsSuffixAndTitle() {
    TestEntity e(QStringLiteral("dir/a.mp4"), testRegistry());
    e.applyFilename(QStringLiteral("b.MKV"));
    QCOMPARE(e.fileName(), QStringLiteral("b.MKV"));
    QCOMPARE(e.suffix(), QStringLiteral("mkv"));
    QCOMPARE(e.title(), QStringLiteral("b.MKV")); // title 未显式设置 → 联动
}

void TestPlaylistEntity::setTitlePinsTitle() {
    TestEntity e(QStringLiteral("dir/a.mp4"), testRegistry());
    e.applyTitle(QStringLiteral("我的标题"));
    e.applyFilename(QStringLiteral("c.flv"));
    QCOMPARE(e.fileName(), QStringLiteral("c.flv"));
    QCOMPARE(e.suffix(), QStringLiteral("flv"));
    QCOMPARE(e.title(), QStringLiteral("我的标题")); // 显式设置后不再联动
}

void TestPlaylistEntity::lazyFileMetaCachedAndRefreshed() {
    TestEntity e(QStringLiteral("dir/x.mp4"), testRegistry());
    QVERIFY(e.cachedFileMeta() == nullptr); // 未求值
    QCOMPARE(e.fileMeta().size.value_or(0), 123);
    QCOMPARE(e.fileComputeCount, 1);
    const FileMeta *cached = e.cachedFileMeta();
    QVERIFY(cached != nullptr);
    QCOMPARE(cached->size.value_or(0), 123);
    e.fileMeta();
    QCOMPARE(e.fileComputeCount, 1); // 命中缓存不再求值
    e.refresh();
    QVERIFY(e.cachedFileMeta() == nullptr); // refresh 清空
    e.fileMeta();
    QCOMPARE(e.fileComputeCount, 2); // 重新求值
}

void TestPlaylistEntity::lazyMediaMetaCachedAndRefreshed() {
    TestEntity e(QStringLiteral("dir/x.mp4"), testRegistry());
    QVERIFY(e.cachedMediaMeta() == nullptr);
    QCOMPARE(e.mediaMeta().duration.value_or(0.0), 4.5);
    QCOMPARE(e.mediaComputeCount, 1);
    e.mediaMeta();
    QCOMPARE(e.mediaComputeCount, 1);
    e.refresh();
    QVERIFY(e.cachedMediaMeta() == nullptr);
    e.mediaMeta();
    QCOMPARE(e.mediaComputeCount, 2);
}

void TestPlaylistEntity::lazyIconCachedAndRefreshed() {
    TestEntity e(QStringLiteral("dir/x.mp4"), testRegistry());
    e.icon();
    QCOMPARE(e.iconComputeCount, 1);
    e.icon();
    QCOMPARE(e.iconComputeCount, 1); // 命中缓存
    e.refresh();
    e.icon();
    QCOMPARE(e.iconComputeCount, 2);
}

void TestPlaylistEntity::unsupportedFileMetaNeverComputes() {
    TestEntity e(QStringLiteral("dir/y.mp4"), testRegistry());
    e.setSupportFile(false);
    const FileMeta &fm = e.fileMeta();
    QVERIFY(!fm.size.has_value());
    QVERIFY(!fm.created.has_value());
    QVERIFY(!fm.modified.has_value());
    QCOMPARE(e.fileComputeCount, 0);        // 永不调用 computeFileMeta
    QVERIFY(e.cachedFileMeta() == nullptr); // cached 恒 nullptr
    e.fileMeta();
    QCOMPARE(e.fileComputeCount, 0); // 重复调用仍不触发求值
}

void TestPlaylistEntity::unsupportedMediaMetaNeverComputes() {
    TestEntity e(QStringLiteral("dir/y.mp4"), testRegistry());
    e.setSupportMedia(false);
    const auto &mm = e.mediaMeta();
    QVERIFY(!mm.duration.has_value());
    QVERIFY(mm.streams.isEmpty());
    QCOMPARE(e.mediaComputeCount, 0);
    QVERIFY(e.cachedMediaMeta() == nullptr);
    e.mediaMeta();
    QCOMPARE(e.mediaComputeCount, 0);
}

void TestPlaylistEntity::unknownEntityCapabilities() {
    // N35：http(s) 已分派 HttpEntity，兜底能力位用例换 ftp://（UnknownEntity 形态不变）
    auto u = PlaylistEntity::fromPath(QStringLiteral("ftp://example.com/v/a.mp4"), testRegistry());
    QVERIFY(!u->isLocalFile());
    QVERIFY(u->exists()); // 不可判定，恒 true（不参与失效清理）
    QVERIFY(!u->supportsFileMeta());
    QVERIFY(!u->supportsMediaMeta());
    QVERIFY(!u->fileMeta().size.has_value());
    QVERIFY(u->cachedFileMeta() == nullptr);
    QVERIFY(!u->mediaMeta().duration.has_value());
    QVERIFY(u->cachedMediaMeta() == nullptr);
    // 插件不做额外加工：clone 后身份与能力一致
    auto c = u->clone();
    QCOMPARE(c->path(), u->path());
    QVERIFY(!c->isLocalFile());
    QVERIFY(c->exists());
    QVERIFY(!c->supportsFileMeta());
    QVERIFY(!c->supportsMediaMeta());
}

void TestPlaylistEntity::localFileEntityFileMeta() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("video.mp4"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(2048, 'x'));
    f.close();

    auto e = PlaylistEntity::fromPath(path, testRegistry());
    QVERIFY(dynamic_cast<LocalFileEntity *>(e.get()) != nullptr);
    QVERIFY(e->supportsFileMeta());
    const FileMeta &fm = e->fileMeta();
    QVERIFY(fm.size.has_value() && *fm.size == 2048);
    QVERIFY(fm.modified.has_value() && fm.modified->isValid());

    // 不存在文件：三字段全空（"值未知"），能力位不变
    auto missing = PlaylistEntity::fromPath(QDir(dir.path()).filePath(QStringLiteral("nope.mp4")),
                                            testRegistry());
    QVERIFY(dynamic_cast<LocalFileEntity *>(missing.get()) != nullptr);
    QVERIFY(!missing->fileMeta().size.has_value());
    QVERIFY(!missing->fileMeta().created.has_value());
    QVERIFY(!missing->fileMeta().modified.has_value());
    QVERIFY(missing->supportsFileMeta());
}

void TestPlaylistEntity::localFileEntityExists() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("a.mp4"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();

    auto e = PlaylistEntity::fromPath(path, testRegistry());
    QVERIFY(e->isLocalFile());
    QVERIFY(e->exists());

    auto missing = PlaylistEntity::fromPath(QDir(dir.path()).filePath(QStringLiteral("gone.mp4")),
                                            testRegistry());
    QVERIFY(!missing->exists());
}

void TestPlaylistEntity::cloneDeepCopiesCache() {
    TestEntity e(QStringLiteral("dir/z.mp4"), testRegistry());
    e.fileMeta();
    e.mediaMeta();
    e.icon();
    auto c = e.clone();
    // 副本携带原实体已解析缓存
    QVERIFY(c->cachedFileMeta() != nullptr);
    QVERIFY(c->cachedMediaMeta() != nullptr);
    // clone 返回基类指针：桩计数器经派生指针访问（用例内可判定实际类型）
    auto *tc = static_cast<TestEntity *>(c.get());
    tc->fileComputeCount = 0; // 计数器随拷贝复制，归零观察副本自身行为
    // 一方 refresh 不清另一方
    e.refresh();
    QVERIFY(e.cachedFileMeta() == nullptr);
    QVERIFY(c->cachedFileMeta() != nullptr);
    tc->fileMeta();
    QCOMPARE(tc->fileComputeCount, 0); // 副本用拷贝来的缓存，不触发求值
    tc->refresh();
    tc->fileMeta();
    QCOMPARE(tc->fileComputeCount, 1); // 副本清缓存后自行求值
}

void TestPlaylistEntity::keyIsNormalizedPlayUrl() {
    TestEntity e(QStringLiteral("dir/k.mp4"), testRegistry());
    QCOMPARE(e.key(), Util::Url::key(e.path()));
    auto u = PlaylistEntity::fromPath(QStringLiteral("ftp://example.com/v/a.mp4"), testRegistry());
    QCOMPARE(u->key(), Util::Url::key(u->path()));
}

// N56 §2.5：键级判等经 matches——整形后的回显进判据（N61 §2.1），归一在实体内部完成；
// 键级刻意宽松（大小写 / 分隔符形态漂移仍认得出同一条内容，N30），回刷的形态级比对不走它
void TestPlaylistEntity::matchesKeyLevelEquality() {
    auto registry = std::make_shared<ServiceRegistry>();
    // 本地：大小写 / 分隔符漂移仍命中
    const QString local = QDir::tempPath() + QStringLiteral("/mpvqt_entity_matches.mp4");
    auto l = PlaylistEntity::fromPath(local, registry);
    QVERIFY(l->matches(l->path()));
    QVERIFY(l->matches(local.toLower()));
    QVERIFY(!l->matches(QDir::tempPath() + QStringLiteral("/mpvqt_entity_other.mp4")));
    // http：无凭据身份 matches 带凭据回显（N56 §2.3——凭据准入后回显含注入凭据，
    // 判据侧不剥凭据、剥凭据在键内完成）；scheme / host 大小写漂移亦命中
    auto h = PlaylistEntity::fromPath(QStringLiteral("https://example.com/dir/a.mp4"), registry);
    QVERIFY(h->matches(QStringLiteral("https://example.com/dir/a.mp4")));
    QVERIFY(h->matches(QStringLiteral("https://user:pw@example.com/dir/a.mp4")));
    QVERIFY(h->matches(QStringLiteral("HTTPS://EXAMPLE.COM/dir/a.mp4")));
    QVERIFY(!h->matches(QStringLiteral("https://example.com/dir/b.mp4")));
    // UnknownEntity（非 http 网络）：matches 自身原样；不同串不命中
    auto n = PlaylistEntity::fromPath(QStringLiteral("ftp://example.com/v/a.mp4"), testRegistry());
    QVERIFY(n->matches(QStringLiteral("ftp://example.com/v/a.mp4")));
    QVERIFY(!n->matches(QStringLiteral("ftp://example.com/v/b.mp4")));
}

// N56 §2.4：显示出口——path() 剥除 userinfo 的形态（构造期缓存）；凭据准入后
// 身份串可含用户凭据，tooltip / 属性对话框经此不再明文可见
void TestPlaylistEntity::displayUrlStripsCredentials() {
    auto registry = std::make_shared<ServiceRegistry>();
    auto h =
        PlaylistEntity::fromPath(QStringLiteral("https://user:pw@example.com/dir/a.mp4"), registry);
    // 身份保留凭据（播放 / 落盘面），显示面剥除
    QCOMPARE(h->path(), QStringLiteral("https://user:pw@example.com/dir/a.mp4"));
    QCOMPARE(h->displayUrl(), QStringLiteral("https://example.com/dir/a.mp4"));
    // 无凭据输入：displayUrl == path()
    auto plain =
        PlaylistEntity::fromPath(QStringLiteral("https://example.com/dir/a.mp4"), registry);
    QCOMPARE(plain->displayUrl(), plain->path());
}

// N61 变更记录第 1 条：工厂的回显专用入口。它只放宽一件事——不炸。三条纪律各钉一组
// 用例：① 已定形者与 fromPath 同类型同身份（正常路径零变化）；② 入口不做加工，折叠
// 仍归调用点（漏折即让 mpvqt:// 落进实体身份，这条用例把纪律钉在断言上）；
// ③ 补不出形态者不撞 fromPath 那句断言、以原串落 UnknownEntity（系统边界上降级而非
// 终止进程，且行不丢 ⇒ 实体序与 mpv 队列序对位不破）
void TestPlaylistEntity::echoEntryFallsBackToPlaceholder() {
    auto registry = testRegistry();
    const QString local = QDir::tempPath() + QStringLiteral("/mpvqt_entity_echo_local.mp4");

    // ① 已定形本地串：类型与身份与 fromPath 逐字一致
    auto l = PlaylistEntity::fromMpvEcho(local, registry);
    QVERIFY(dynamic_cast<LocalFileEntity *>(l.get()) != nullptr);
    QCOMPARE(l->path(), local);
    // http 项同口径（含凭据原样保留，与 fromPath 一样不剥）
    const QString url = QStringLiteral("https://user:pw@example.com/dir/a.mp4?token=x");
    auto h = PlaylistEntity::fromMpvEcho(url, registry);
    QVERIFY(dynamic_cast<HttpEntity *>(h.get()) != nullptr);
    QCOMPARE(h->path(), url);

    // ② 折叠归调用点：折回的 identity 进来才得到本地实体
    const QString transit = StreamTransit::mint(QUuid::createUuid(), local);
    QVERIFY(transit.startsWith(QLatin1String(StreamTransit::kScheme)));
    auto t = PlaylistEntity::fromMpvEcho(StreamTransit::unwrap(transit), registry);
    QVERIFY(dynamic_cast<LocalFileEntity *>(t.get()) != nullptr);
    QCOMPARE(t->path(), local);
    // 漏折的对照：mpvqt:// 含 "://" 属已定形，入口原样放行、不代拆——转接串就当身份
    // 落进了实体（N52 / N56 折叠纪律破）。setPlaylist 第 5 步那一句 unwrap 因此不得省
    auto unfolded = PlaylistEntity::fromMpvEcho(transit, registry);
    QVERIFY(dynamic_cast<UnknownEntity *>(unfolded.get()) != nullptr);
    QCOMPARE(unfolded->path(), transit);

    // ③ 空 filename（回显条目无 filename 键 / 非 map 条目）：落占位，且形态与原串一致
    auto empty = PlaylistEntity::fromMpvEcho(QString(), registry);
    QVERIFY(dynamic_cast<UnknownEntity *>(empty.get()) != nullptr);
    QVERIFY(dynamic_cast<LocalFileEntity *>(empty.get()) == nullptr);
    QVERIFY(empty->path().isEmpty());
    // 占位实体的能力位：不可播、不参与失效清理（exists 恒真 = 不可判定 ≠ 缺失）
    QVERIFY(!empty->isLocalFile());
    QVERIFY(empty->exists());
    QVERIFY(!empty->supportsFileMeta());
    QVERIFY(!empty->supportsMediaMeta());
    // clone 身份与类型不变（回刷命中即克隆那条路对占位项同样自洽）
    auto ec = empty->clone();
    QVERIFY(dynamic_cast<UnknownEntity *>(ec.get()) != nullptr);
    QVERIFY(ec->path().isEmpty());

    // 裸 "file:"：未经整形直喂时入口原样直陈（整形出口产出空串，用例见 tst_Util）
    auto bare = PlaylistEntity::fromMpvEcho(QStringLiteral("file:"), registry);
    QVERIFY(dynamic_cast<UnknownEntity *>(bare.get()) != nullptr);
    QCOMPARE(bare->path(), QStringLiteral("file:"));

    // 全链闭合：整形 → 折叠 → 回显入口，产物必为占位（这条串补不出形态）
    const QString shaped =
        StreamTransit::unwrap(Util::Url::identityFromMpvEcho(QStringLiteral("file:")));
    QVERIFY(!Util::Url::isDefiniteForm(shaped));
    auto chain = PlaylistEntity::fromMpvEcho(shaped, registry);
    QVERIFY(dynamic_cast<UnknownEntity *>(chain.get()) != nullptr);
    QVERIFY(chain->path().isEmpty());

    // 相对串经整形已定形 → 入口走正常分派（崩溃不再的正面断言，N61 §2.1）
    const QString shapedRel = Util::Url::identityFromMpvEcho(QStringLiteral("test2.mp4"));
    QVERIFY(Util::Url::isDefiniteForm(shapedRel));
    auto rel = PlaylistEntity::fromMpvEcho(shapedRel, registry);
    QVERIFY(dynamic_cast<LocalFileEntity *>(rel.get()) != nullptr);
    QCOMPARE(rel->path(), shapedRel);
}

// N55 §2.2/§2.3：归属注入口与出生属性。形态只问归属提供者——实体不认识"所在列表"，
// 同一实体无论放进哪个列表，playUrl() 产出都由盖章那对（uuid + 提供者）现算
void TestPlaylistEntity::ownershipStampDrivesPlayUrl() {
    auto registry = testRegistry();
    auto relay = std::make_shared<TestRelay>();
    registry->provide<StreamRelayIf>(relay);

    // ① StaticProvider（Unsupported）：恒等下发，形态 == 身份
    auto staticProvider = std::make_shared<StaticProvider>(registry);
    TestEntity e(QStringLiteral("dir/owned.mp4"), registry);
    e.applyOwnership(QUuid::createUuid(), staticProvider);
    QCOMPARE(e.playUrl(), e.path());

    // ② 转接提供者（Required）：形态 = mpvqt://<归属 uuid>/<剥凭据身份>。
    //    身份段取归属 uuid 而非任何"所在列表"——断言形态出自归属
    auto transitProvider = std::make_shared<TransitTestProvider>(registry);
    const QUuid uuidB = QUuid::createUuid();
    TestEntity t(QStringLiteral("https://example.com/dir/stream.mp4"), registry);
    t.applyOwnership(uuidB, transitProvider);
    const QString form = t.playUrl();
    QVERIFY(form.startsWith(QLatin1String(StreamTransit::kScheme)));
    QCOMPARE(StreamTransit::listUuidOfUri(form), uuidB);
    QCOMPARE(StreamTransit::unwrap(form), Util::Url::stripCredentials(t.path()));
    // publish 随铸造发生（N52 §2.6 发布即下发）：表键即归属 uuid
    QCOMPARE(relay->publishedUuid, uuidB);
    QVERIFY(relay->lastEndpoint != nullptr);

    // ③ 再盖章即显式赋盖（不做"空则补盖"）：换归属后形态随新归属现算
    t.applyOwnership(QUuid::createUuid(), staticProvider);
    QCOMPARE(t.playUrl(), t.path());
}

// N55 §2.1：clone() 携带归属——拷贝构造默认复制两件套。克隆未经再盖章即由原归属
// 加工；若归属未携带，克隆侧 playUrl() 走锁失败断言路径（Debug 即显影），产不出形态
void TestPlaylistEntity::cloneCarriesOwnership() {
    auto registry = testRegistry();
    auto relay = std::make_shared<TestRelay>();
    registry->provide<StreamRelayIf>(relay);
    auto transitProvider = std::make_shared<TransitTestProvider>(registry);
    const QUuid uuid = QUuid::createUuid();
    TestEntity e(QStringLiteral("dir/carried.mp4"), registry);
    e.applyOwnership(uuid, transitProvider);
    const QString before = e.playUrl();

    auto c = e.clone();
    auto *tc = static_cast<TestEntity *>(c.get());
    QCOMPARE(tc->playUrl(), before); // 同一提供者 + 同一 uuid：形态逐字节一致
    QCOMPARE(StreamTransit::listUuidOfUri(tc->playUrl()), uuid);
}

// N55 §2.3 锁失败面：归属提供者销毁后 playUrl() 的 Release 契约是返空串——空串令
// mpv 显式加载失败，绝不静默退回 path()。Debug 下该路径撞 Q_ASSERT（编程错误显影，
// 契约本身），故本用例只在 Release 构建下断言，Debug 构建跳过
void TestPlaylistEntity::ownershipLockFailureReturnsEmptyInRelease() {
#if defined(QT_NO_DEBUG)
    TestEntity e(QStringLiteral("dir/gone.mp4"), testRegistry());
    {
        auto provider = std::make_shared<TransitTestProvider>(testRegistry());
        e.applyOwnership(QUuid::createUuid(), provider);
    } // 提供者出作用域销毁：弱引用过期
    QCOMPARE(e.playUrl(), QString());
#else
    QSKIP("Debug 构建下锁失败撞 Q_ASSERT（编程错误显影）；Release 契约返空串，见 N55 §2.3");
#endif
}

// N55 §二：归属不参与判等——同一身份串盖不同归属（含不同提供者与不同 uuid），
// key() / matches() 结果不变；归属缺席（未盖章）同样不改变判等
void TestPlaylistEntity::ownershipNotInEqualityCriteria() {
    auto registry = testRegistry();
    auto relay = std::make_shared<TestRelay>();
    registry->provide<StreamRelayIf>(relay);
    const QString path = QStringLiteral("https://example.com/dir/same.mp4");
    auto staticProvider = std::make_shared<StaticProvider>(registry);
    auto transitProvider = std::make_shared<TransitTestProvider>(registry);

    TestEntity a(path, registry);
    a.applyOwnership(QUuid::createUuid(), staticProvider);
    TestEntity b(path, registry);
    b.applyOwnership(QUuid::createUuid(), transitProvider);
    QCOMPARE(a.key(), b.key());
    QCOMPARE(a.key(), Util::Url::key(path));
    QVERIFY(a.matches(path));
    QVERIFY(b.matches(path));
    QVERIFY(a.matches(b.path()));

    // 未盖章实体：判等不因归属缺席而变（playUrl() 才守契约，key / matches 无关归属）
    TestEntity plain(path, registry);
    QCOMPARE(plain.key(), a.key());
    QVERIFY(plain.matches(path));
}

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN），无 GUI 依赖
QTEST_GUILESS_MAIN(TestPlaylistEntity)

#include "tst_PlaylistEntity.moc"

// PlaylistEntity 单元测试（N26）：工厂分派与往返幂等、命名派生与修正联动、
// 惰性求值与缓存清空、能力位与静态空回落、clone 深拷贝互不影响。
// 测试无副作用：文件级 case 用 QTemporaryDir 隔离，结束时自动清理。
// 编译进独立可执行 mpvqt_tests（MOC 处理 QObject 测试类），运行：
//   mpvqt_tests                  # 跑全部
//   mpvqt_tests -iterations 1 -matchcase xxx   # 按需筛选

#include <QDir>
#include <QFile>
#include <QIcon>
#include <QObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "src/HttpEntity.h"
#include "src/LocalFileEntity.h"
#include "src/PlaylistEntity.h"
#include "src/ServiceRegistry.h"
#include "src/Util.h"

// 测试桩实体：compute* 带调用计数，能力位可调，保护修正入口透出。
// 计数器 mutable（compute* 为 const 惰性入口）；随拷贝复制，断言前按需归零
namespace {
class TestEntity : public PlaylistEntity {
  public:
    explicit TestEntity(QString playUrl, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(playUrl), registry) {}

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
} // namespace

class TestPlaylistEntity : public QObject {
    Q_OBJECT

  private slots:
    // 工厂分派：本地路径 → LocalFileEntity，非 http 网络/流 URL → UnknownEntity 兜底
    void factoryDispatchesLocalAndNetwork();
    // N35 工厂分派：http(s) + registry → HttpEntity（大小写不敏感前缀）
    void factoryDispatchesHttpWithRegistry();
    // N35 HttpEntity 往返不变式：fromPlayUrl(e->playUrl(), registry)->playUrl() 相等
    void httpEntityRoundtrip();
    // N35 HttpEntity 能力位与元数据回落（不触碰网络：不调 fileMeta()）
    void httpEntityCapabilities();
    // N35 显示名：percent-decode + query/fragment 剥离；空末段回退基类行为
    void httpEntityDisplayName();
    // N35 key：网络路径保留原始大小写（逐字节）
    void httpEntityKeyPreservesCase();
    // 工厂往返幂等：fromPlayUrl(e->playUrl(), registry)->playUrl() == e->playUrl()
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
    // key 默认实现 == normalizePathForMatch(playUrl)
    void keyIsNormalizedPlayUrl();
};

void TestPlaylistEntity::factoryDispatchesLocalAndNetwork() {
    auto registry = std::make_shared<ServiceRegistry>();
    const QString local = QDir::tempPath() + QStringLiteral("/mpvqt_entity_dispatch.mp4");
    auto l = PlaylistEntity::fromPlayUrl(local, registry);
    QVERIFY(dynamic_cast<LocalFileEntity *>(l.get()) != nullptr);
    QVERIFY(dynamic_cast<UnknownEntity *>(l.get()) == nullptr);

    // N35：非 http 网络路径仍落 UnknownEntity 兜底（http 已改分派 HttpEntity）
    auto u =
        PlaylistEntity::fromPlayUrl(QStringLiteral("ftp://example.com/dir/video/a.mp4"), registry);
    QVERIFY(dynamic_cast<UnknownEntity *>(u.get()) != nullptr);
    QVERIFY(dynamic_cast<LocalFileEntity *>(u.get()) == nullptr);
    QVERIFY(dynamic_cast<HttpEntity *>(u.get()) == nullptr);
}

void TestPlaylistEntity::factoryDispatchesHttpWithRegistry() {
    auto registry = std::make_shared<ServiceRegistry>();
    auto h = PlaylistEntity::fromPlayUrl(QStringLiteral("https://example.com/dir/video/a.mp4"),
                                         registry);
    QVERIFY(dynamic_cast<HttpEntity *>(h.get()) != nullptr);
    QVERIFY(dynamic_cast<UnknownEntity *>(h.get()) == nullptr);

    // 前缀大小写不敏感
    auto upper = PlaylistEntity::fromPlayUrl(QStringLiteral("HTTP://example.com/a.mp4"), registry);
    QVERIFY(dynamic_cast<HttpEntity *>(upper.get()) != nullptr);

    // 本地路径忽略 registry（类型不受影响）
    auto l = PlaylistEntity::fromPlayUrl(
        QDir::tempPath() + QStringLiteral("/mpvqt_entity_http_local.mp4"), registry);
    QVERIFY(dynamic_cast<LocalFileEntity *>(l.get()) != nullptr);
}

void TestPlaylistEntity::httpEntityRoundtrip() {
    auto registry = std::make_shared<ServiceRegistry>();
    const QString url = QStringLiteral("https://user:pw@example.com/dir/a.mp4?token=x");
    auto e = PlaylistEntity::fromPlayUrl(url, registry);
    QVERIFY(dynamic_cast<HttpEntity *>(e.get()) != nullptr);
    QCOMPARE(e->playUrl(), url); // 网络路径原样（含 userinfo / query）
    QCOMPARE(PlaylistEntity::fromPlayUrl(e->playUrl(), registry)->playUrl(), e->playUrl());
}

void TestPlaylistEntity::httpEntityCapabilities() {
    auto registry = std::make_shared<ServiceRegistry>();
    auto e = PlaylistEntity::fromPlayUrl(QStringLiteral("https://example.com/a.mp4"), registry);
    QVERIFY(!e->isLocalFile());
    QVERIFY(e->exists()); // 不可判定，恒 true（不参与失效清理）
    QVERIFY(e->supportsFileMeta());
    QVERIFY(!e->supportsMediaMeta());
    // 支持媒体元数据为假：媒体侧静态空回落，cached 恒 nullptr
    QVERIFY(!e->mediaMeta().duration.has_value());
    QVERIFY(e->cachedMediaMeta() == nullptr);
    // 支持文件元数据为真但未求值：cached nullptr（不调 fileMeta()，避免触发 HEAD）
    QVERIFY(e->cachedFileMeta() == nullptr);

    // clone：身份一致，类型不变
    auto c = e->clone();
    QVERIFY(dynamic_cast<HttpEntity *>(c.get()) != nullptr);
    QCOMPARE(c->playUrl(), e->playUrl());
    QVERIFY(!c->isLocalFile());
    QVERIFY(c->exists());
    QVERIFY(c->supportsFileMeta());
    QVERIFY(!c->supportsMediaMeta());
}

void TestPlaylistEntity::httpEntityDisplayName() {
    auto registry = std::make_shared<ServiceRegistry>();

    // percent-encoded 中文与空格正确解码
    auto zh = PlaylistEntity::fromPlayUrl(
        QStringLiteral("https://example.com/dir/%E4%B8%AD%E6%96%87%20name.mp4"), registry);
    QCOMPARE(zh->fileName(), QStringLiteral("中文 name.mp4"));
    QCOMPARE(zh->title(), QStringLiteral("中文 name.mp4"));
    QCOMPARE(zh->suffix(), QStringLiteral("mp4"));

    // query / fragment 剥离：末段不混入 token
    auto q = PlaylistEntity::fromPlayUrl(
        QStringLiteral("https://example.com/v/movie.mkv?token=ab%2Fcd#frag"), registry);
    QCOMPARE(q->fileName(), QStringLiteral("movie.mkv"));
    QCOMPARE(q->title(), QStringLiteral("movie.mkv"));
    QCOMPARE(q->suffix(), QStringLiteral("mkv"));

    // 空末段（以 / 结尾）：解后为空回退基类行为（title = playUrl，fileName 空）
    auto root = PlaylistEntity::fromPlayUrl(QStringLiteral("https://example.com/"), registry);
    QCOMPARE(root->fileName(), QString());
    QCOMPARE(root->title(), QStringLiteral("https://example.com/"));
}

void TestPlaylistEntity::httpEntityKeyPreservesCase() {
    auto registry = std::make_shared<ServiceRegistry>();
    const QString url = QStringLiteral("https://Example.COM/V/a.mp4");
    auto e = PlaylistEntity::fromPlayUrl(url, registry);
    // 网络路径 key 逐字节：主机与路径大小写保留（N35 §2.6）
    QCOMPARE(e->key(), url);
}

void TestPlaylistEntity::factoryRoundtripIdempotent() {
    // 本地路径：resolveLocalPath 加工（相对→绝对 + 原生分隔符）后往返不变
    auto registry = std::make_shared<ServiceRegistry>();
    const QString local = QDir::tempPath() + QStringLiteral("/mpvqt_entity_roundtrip.mp4");
    auto e = PlaylistEntity::fromPlayUrl(local, registry);
    QCOMPARE(PlaylistEntity::fromPlayUrl(e->playUrl(), registry)->playUrl(), e->playUrl());

    // 相对路径：加工为绝对路径
    auto rel = PlaylistEntity::fromPlayUrl(QStringLiteral("relative_dir/c.mp4"), registry);
    QVERIFY(QDir::isAbsolutePath(rel->playUrl()));
    QCOMPARE(PlaylistEntity::fromPlayUrl(rel->playUrl(), registry)->playUrl(), rel->playUrl());

    // 网络 / 流路径：原样保留（N35：用非 http scheme 验证，http 分派见 httpEntity* 用例）
    auto n = PlaylistEntity::fromPlayUrl(QStringLiteral("ftp://example.com/v/a.mp4"), registry);
    QCOMPARE(n->playUrl(), QStringLiteral("ftp://example.com/v/a.mp4"));
    QCOMPARE(PlaylistEntity::fromPlayUrl(n->playUrl(), registry)->playUrl(), n->playUrl());
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
    auto u =
        PlaylistEntity::fromPlayUrl(QStringLiteral("ftp://example.com/v/a.mp4"), testRegistry());
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
    QCOMPARE(c->playUrl(), u->playUrl());
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

    auto e = PlaylistEntity::fromPlayUrl(path, testRegistry());
    QVERIFY(dynamic_cast<LocalFileEntity *>(e.get()) != nullptr);
    QVERIFY(e->supportsFileMeta());
    const FileMeta &fm = e->fileMeta();
    QVERIFY(fm.size.has_value() && *fm.size == 2048);
    QVERIFY(fm.modified.has_value() && fm.modified->isValid());

    // 不存在文件：三字段全空（"值未知"），能力位不变
    auto missing = PlaylistEntity::fromPlayUrl(
        QDir(dir.path()).filePath(QStringLiteral("nope.mp4")), testRegistry());
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

    auto e = PlaylistEntity::fromPlayUrl(path, testRegistry());
    QVERIFY(e->isLocalFile());
    QVERIFY(e->exists());

    auto missing = PlaylistEntity::fromPlayUrl(
        QDir(dir.path()).filePath(QStringLiteral("gone.mp4")), testRegistry());
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
    QCOMPARE(e.key(), Util::normalizePathForMatch(e.playUrl()));
    auto u =
        PlaylistEntity::fromPlayUrl(QStringLiteral("ftp://example.com/v/a.mp4"), testRegistry());
    QCOMPARE(u->key(), Util::normalizePathForMatch(u->playUrl()));
}

// 多套件共用一个可执行文件（N26 起测试文件多于一个）：
// main 集中在 tests/main.cpp，各文件只提供套件工厂
QObject *createTestPlaylistEntity() { return new TestPlaylistEntity; }

#include "tst_PlaylistEntity.moc"

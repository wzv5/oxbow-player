// RcloneCryptProvider 单元测试（N98 §9）：提供者侧纯逻辑——身份编解码往返
// （含空格 / 中文 / @ / # / % / +，铸造守卫免疫的根基）、根目录定形幂等、参数
// 校验、扩展名清单、三个 int 取值的域外降级、saveSettings ↔ load 的逐参往返
// （N44 幂等回显硬要求）、工厂分派（rclonecrypt:// → RcloneCryptEntity）、
// 临时加密目录上的分层 / 平铺 / 递归枚举（混存名跳过与 warnings、strict_names
// 整体失败、扩展名按解密后名字过滤、明文尺寸注入）、端点开流（身份 → 现算密文名
// → FileByteStream → 解密流逐字节还原），以及域外编码取值的配置异常显影。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R RcloneCryptProvider。

#include "pch.h"

#include <QtTest/QtTest>

#include "PlaylistEntity.h"
#include "RcloneCryptEntity.h"
#include "RcloneCryptFixture.h"
#include "RcloneCryptProvider.h"
#include "ServiceRegistry.h"
#include "StreamEndpoint.h"
#include "rclonecrypt/RcloneCrypt.h"

using Bytes = RcloneCrypt::Bytes;

namespace {

// 24 字节文件 nonce（合成密文与解密流共用一条基线）
Bytes fileNonce() {
    Bytes n(24);
    for (std::size_t i = 0; i < n.size(); ++i)
        n[i] = std::uint8_t(0x10 + i);
    return n;
}

QByteArray toQByteArray(const Bytes &b) {
    return QByteArray(reinterpret_cast<const char *>(b.data()), int(b.size()));
}

// 建树用 cipher：与 makeProvider 的参数**逐字相同**（口令 "pw"、salt "salt"、
// standard、base32、目录名加密开）——树与提供者必须同钥，否则解不开全被跳过
std::shared_ptr<const RcloneCrypt> treeCipher() {
    RcloneCrypt::Params p;
    p.password = "pw";
    p.salt = "salt";
    auto c = RcloneCrypt::create(p);
    Q_ASSERT(c);
    return *c;
}

// 全参数提供者
std::shared_ptr<RcloneCryptProvider>
makeProvider(const QString &root,
             RcloneCryptProvider::BrowseMode mode = RcloneCryptProvider::BrowseMode::Layered,
             const QStringList &extensions = {}, bool strictNames = false) {
    return std::make_shared<RcloneCryptProvider>(
        root, QStringLiteral("pw"), QStringLiteral("salt"), RcloneCrypt::NameMode::Standard,
        RcloneCrypt::NameEncoding::Base32, true, QStringLiteral(".bin"), strictNames, false, mode,
        extensions, std::make_shared<ServiceRegistry>());
}

// 在 parent 下写一份假密文文件（本体非真密文，但尺寸取 EncryptedSize(plainLen)
// ——尺寸注入按 decryptedSize 换算，只依赖文件大小）；失败返回空串
QString writeFakeCiphertext(const QDir &parent, const std::string &cipherName,
                            std::size_t plainLen) {
    const std::int64_t enc = RcloneCrypt::encryptedSize(std::int64_t(plainLen));
    QFile f(parent.filePath(QString::fromStdString(cipherName)));
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(QByteArray(int(enc), 'x'));
    f.close();
    return f.fileName();
}

} // namespace

class TestRcloneCryptProvider : public QObject {
    Q_OBJECT

  private slots:
    // N98 §6 要点 4：身份编解码逐字节互逆（空格 / 中文 / @ / # / % / +）
    void identityRoundTrip();
    // '@' 编成 %40：StreamTransit::mint 的 userinfo 守卫对编码形态结构性免疫
    void identityEncodesAtSign();
    // N44：normalizeRoot 幂等（硬要求）与 validateInput
    void rootNormalizeAndValidate();
    // N44：扩展名清单（空白拆分、去点、小写、去重）
    void parseExtensionsIdempotent();
    // N46 先例：三个 int 不透明存储的域外降级
    void intConversionsDomainFallback();
    // N44 硬要求：saveSettings ↔ load 逐参往返
    void saveLoadRoundTrip();
    // N35：工厂分派——rclonecrypt:// → RcloneCryptEntity，path() 保形
    void fromPathDispatch();
    // 分层枚举：容器明文段名、实体身份 / 显示名 / 明文尺寸、混存名跳过、
    // 扩展名按解密后名字过滤
    void layeredEnumeration();
    // strict_names：解不开名 ⇒ 整次失败且文案含 undecryptable
    void strictNamesFailsWholeEnumeration();
    // 平铺与递归形态
    void flatAndRecursiveModes();
    // 端点开流：身份解回明文 → 现算的密文名与盘上名逐字节相同 → 解密流逐字节还原
    void streamEndpointOpensEnumeratedIdentity();
    // 域外编码取值（手改 INI 塞进来的 base32768）按配置异常显影，原值不被洗掉
    void outOfDomainEncodingIsConfigError();
};

void TestRcloneCryptProvider::identityRoundTrip() {
    const QString plains[] = {
        QStringLiteral("Season 03"),
        QStringLiteral("电影 中文名"),
        QStringLiteral("photo@2024.jpg"),
        QStringLiteral("a#b?c"),
        QStringLiteral("50%off"),
        QStringLiteral("c++"),
        QStringLiteral("plain/path with spaces/file.mkv"),
    };
    for (const QString &plain : plains) {
        const QString encoded = RcloneCryptProvider::encodeIdentity(plain);
        QVERIFY(!encoded.contains(QLatin1Char(' ')));
        QCOMPARE(RcloneCryptProvider::decodeIdentity(encoded), plain);
    }
}

void TestRcloneCryptProvider::identityEncodesAtSign() {
    // '@' 与 '+' 非 unreserved，必须编码——authority 段因此恒无字面 '@'，
    // StreamTransit::mint 的 userinfo 守卫不会误伤（N98 §6 要点 4）
    const QString encoded = RcloneCryptProvider::encodeIdentity(QStringLiteral("photo@2024"));
    QVERIFY(encoded.contains(QStringLiteral("%40")));
    QVERIFY(!encoded.contains(QLatin1Char('@')));
    const QString plus = RcloneCryptProvider::encodeIdentity(QStringLiteral("c++"));
    QVERIFY(plus.contains(QStringLiteral("%2B")));
}

void TestRcloneCryptProvider::rootNormalizeAndValidate() {
    // 幂等是硬要求（N44）：已定形态再定形必须结果不变
    const QString inputs[] = {
        QStringLiteral("D:/data"),     QStringLiteral("D:\\data"),     QStringLiteral("D:/data/"),
        QStringLiteral("  D:/data  "), QStringLiteral("relative/dir"),
    };
    for (const QString &input : inputs) {
        const QString once = RcloneCryptProvider::normalizeRoot(input);
        QCOMPARE(RcloneCryptProvider::normalizeRoot(once), once);
    }
    // 空串保持空串
    QVERIFY(RcloneCryptProvider::normalizeRoot(QStringLiteral("  ")).isEmpty());
    // 校验：空根 / 网络 URL 报错，本地目录通过
    QVERIFY(RcloneCryptProvider::validateInput(QStringLiteral("")).has_value());
    QVERIFY(RcloneCryptProvider::validateInput(QStringLiteral("http://x/y")).has_value());
    QVERIFY(!RcloneCryptProvider::validateInput(QStringLiteral("D:/data")).has_value());
}

void TestRcloneCryptProvider::parseExtensionsIdempotent() {
    // 预期清单先落变量——花括号初始化内的逗号会打散 QCOMPARE 宏
    const QStringList expected{"mkv", "mp4", "av1"};
    QCOMPARE(RcloneCryptProvider::parseExtensions(QStringLiteral("  .MKV  mp4 .AV1 mp4  ")),
             expected);
    QVERIFY(RcloneCryptProvider::parseExtensions(QString()).isEmpty());
}

void TestRcloneCryptProvider::intConversionsDomainFallback() {
    QCOMPARE(RcloneCryptProvider::nameModeFromInt(0), RcloneCrypt::NameMode::Standard);
    QCOMPARE(RcloneCryptProvider::nameModeFromInt(1), RcloneCrypt::NameMode::Obfuscate);
    QCOMPARE(RcloneCryptProvider::nameModeFromInt(2), RcloneCrypt::NameMode::Off);
    QCOMPARE(RcloneCryptProvider::nameModeFromInt(3), RcloneCrypt::NameMode::Standard);
    QCOMPARE(RcloneCryptProvider::nameModeFromInt(-1), RcloneCrypt::NameMode::Standard);
    QCOMPARE(RcloneCryptProvider::nameEncodingFromInt(0), RcloneCrypt::NameEncoding::Base32);
    QCOMPARE(RcloneCryptProvider::nameEncodingFromInt(1), RcloneCrypt::NameEncoding::Base64);
    QCOMPARE(RcloneCryptProvider::nameEncodingFromInt(9), RcloneCrypt::NameEncoding::Base32);
    QCOMPARE(RcloneCryptProvider::browseModeFromInt(0), RcloneCryptProvider::BrowseMode::Layered);
    QCOMPARE(RcloneCryptProvider::browseModeFromInt(1), RcloneCryptProvider::BrowseMode::FilesOnly);
    QCOMPARE(RcloneCryptProvider::browseModeFromInt(2),
             RcloneCryptProvider::BrowseMode::FilesRecursive);
    QCOMPARE(RcloneCryptProvider::browseModeFromInt(7), RcloneCryptProvider::BrowseMode::Layered);
}

void TestRcloneCryptProvider::saveLoadRoundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("roundtrip.ini")), QSettings::IniFormat);

    auto provider = std::make_shared<RcloneCryptProvider>(
        QStringLiteral("D:/crypt library"), QStringLiteral("secret pw"),
        QStringLiteral("salt value"), RcloneCrypt::NameMode::Obfuscate,
        RcloneCrypt::NameEncoding::Base64, false, QStringLiteral(".jpg"), true, true,
        RcloneCryptProvider::BrowseMode::FilesRecursive, QStringList{"mkv", "mp4"},
        std::make_shared<ServiceRegistry>());
    provider->setSortRule(SortRule{SortKey::FileName, false});
    provider->saveSettings(settings);
    settings.sync();

    // 类型键落盘（writeProviderKey 的单点口径）
    QCOMPARE(settings.value(QStringLiteral("provider")).toString(),
             QLatin1String(kRcloneCryptProviderKey));

    auto loaded = RcloneCryptProvider::load(settings, std::make_shared<ServiceRegistry>());
    auto asCrypt = std::dynamic_pointer_cast<RcloneCryptProvider>(loaded);
    QVERIFY(asCrypt);
    QCOMPARE(asCrypt->rootPath(), provider->rootPath());
    QCOMPARE(asCrypt->password(), provider->password());
    QCOMPARE(asCrypt->salt(), provider->salt());
    QCOMPARE(asCrypt->nameMode(), provider->nameMode());
    QCOMPARE(asCrypt->nameEncoding(), provider->nameEncoding());
    QCOMPARE(asCrypt->dirNameEncrypt(), provider->dirNameEncrypt());
    QCOMPARE(asCrypt->suffix(), provider->suffix());
    QCOMPARE(asCrypt->strictNames(), provider->strictNames());
    QCOMPARE(asCrypt->passBadBlocks(), provider->passBadBlocks());
    QCOMPARE(asCrypt->browseMode(), provider->browseMode());
    QCOMPARE(asCrypt->extensions(), provider->extensions());
    QCOMPARE(asCrypt->sortRule().key, provider->sortRule().key);
    QCOMPARE(asCrypt->sortRule().ascending, provider->sortRule().ascending);
}

void TestRcloneCryptProvider::fromPathDispatch() {
    auto registry = std::make_shared<ServiceRegistry>();
    const QString identity = QStringLiteral("rclonecrypt://dir1/movie%20one.mkv");
    auto entity = PlaylistEntity::fromPath(identity, registry);
    QVERIFY(std::dynamic_pointer_cast<RcloneCryptEntity>(entity));
    QCOMPARE(entity->path(), identity);
    // 非本地：失效清理不判磁盘（isMissingLocal 恒假路径）；存在性恒真
    QVERIFY(!entity->isLocalFile());
    QVERIFY(entity->exists());
    // 工厂不变式：同串再分派同类型同身份
    auto again = PlaylistEntity::fromPath(entity->path(), registry);
    QVERIFY(std::dynamic_pointer_cast<RcloneCryptEntity>(again));
}

void TestRcloneCryptProvider::layeredEnumeration() {
    const auto cipher = treeCipher();
    QTemporaryDir root;
    QVERIFY(root.isValid());

    // 树：dir1/Season One/{movie one.mkv, notes.txt(混存), archive.zip(过滤)}
    // 子目录逐段单独加密后拼路径（encryptDirName 是逐段语义，含父段前缀会重复）
    const QString cipherDir1 = QString::fromStdString(cipher->encryptDirName("dir1"));
    QVERIFY(QDir(root.path()).mkpath(cipherDir1));
    const QString cipherSeason = QString::fromStdString(cipher->encryptDirName("Season One"));
    QVERIFY(QDir(root.path() + QLatin1Char('/') + cipherDir1).mkpath(cipherSeason));

    QDir childDir(root.path() + QLatin1Char('/') + cipherDir1 + QLatin1Char('/') + cipherSeason);
    // 5 字节明文 ⇒ 密文 53 字节（EncryptedSize(5)）；实体尺寸按 decryptedSize 注入。
    // 文件名按单段加密（枚举侧对每个条目名做单段解密）
    writeFakeCiphertext(childDir, cipher->encryptFileName("movie one.mkv"), 5);
    // 混存：明文名文件照常在盘上，枚举必须跳过
    {
        QFile f(childDir.filePath(QStringLiteral("notes.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("plain");
    }
    // 加密名但扩展名不在清单 ⇒ 解密后过滤
    writeFakeCiphertext(childDir, cipher->encryptFileName("archive.zip"), 5);

    auto provider =
        makeProvider(root.path(), RcloneCryptProvider::BrowseMode::Layered, QStringList{"mkv"});

    // 根层：容器在前，id / title 取明文段名，path 展示位留空
    auto rootLayer = provider->enumerate();
    QVERIFY(rootLayer);
    QCOMPARE(int(rootLayer->containers.size()), 1);
    QCOMPARE(rootLayer->containers.front().id, QStringLiteral("dir1"));
    QCOMPARE(rootLayer->containers.front().title, QStringLiteral("dir1"));
    QVERIFY(rootLayer->containers.front().path.isEmpty());
    QVERIFY(rootLayer->entities.empty());
    // 混存名只出现在子层——根层无警告
    QVERIFY(rootLayer->warnings.isEmpty());

    // 子层 {"dir1"}：只有 "Season One" 容器（电影在两层之下）；容器明文段名、
    // path 展示位留空
    auto childLayer = provider->enumerate({QStringLiteral("dir1")});
    QVERIFY(childLayer);
    QCOMPARE(int(childLayer->entities.size()), 0);
    QCOMPARE(int(childLayer->containers.size()), 1);
    QCOMPARE(childLayer->containers.front().id, QStringLiteral("Season One"));
    QVERIFY(childLayer->containers.front().path.isEmpty());

    // 孙层 {"dir1","Season One"}：实体身份 = 编码 URN，显示名 / 文件名注入明文名，
    // 尺寸 = 明文长度
    auto leafLayer = provider->enumerate({QStringLiteral("dir1"), QStringLiteral("Season One")});
    QVERIFY(leafLayer);
    QCOMPARE(int(leafLayer->entities.size()), 1);
    const auto &entity = leafLayer->entities.front();
    const QString expectedIdentity =
        QStringLiteral("rclonecrypt://") +
        RcloneCryptProvider::encodeIdentity(QStringLiteral("dir1/Season One/movie one.mkv"));
    QCOMPARE(entity->path(), expectedIdentity);
    QCOMPARE(entity->title(), QStringLiteral("movie one.mkv"));
    QCOMPARE(entity->fileName(), QStringLiteral("movie one.mkv"));
    QVERIFY(entity->cachedFileMeta());
    QCOMPARE(entity->cachedFileMeta()->size.value_or(-1), qint64(5));
    // 混存名跳过：解释性文案随**本次枚举**的 warnings 回传（N37 通道——每次调用
    // 各带自己的警告），条目照常产出
    QCOMPARE(int(leafLayer->warnings.size()), 1);
    QVERIFY(leafLayer->warnings.front().contains(QStringLiteral("notes.txt")));
    QVERIFY(
        leafLayer->warnings.front().contains(QStringLiteral("undecryptable"), Qt::CaseInsensitive));
}

void TestRcloneCryptProvider::strictNamesFailsWholeEnumeration() {
    const auto cipher = treeCipher();
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString cipherDir1 = QString::fromStdString(cipher->encryptDirName("dir1"));
    QVERIFY(QDir(root.path()).mkpath(cipherDir1));
    QDir dir1(root.path() + QLatin1Char('/') + cipherDir1);
    writeFakeCiphertext(dir1, cipher->encryptFileName("ok.mkv"), 5);
    // 混存明文名
    {
        QFile f(dir1.filePath(QStringLiteral("notes.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("plain");
    }

    auto provider = makeProvider(root.path(), RcloneCryptProvider::BrowseMode::Layered,
                                 QStringList{"mkv"}, /*strictNames=*/true);
    auto result = provider->enumerate({QStringLiteral("dir1")});
    QVERIFY(!result);
    QVERIFY(result.error().contains(QStringLiteral("undecryptable"), Qt::CaseInsensitive));

    // 非严格：跳过并继续，产出合法条目
    auto lenient =
        makeProvider(root.path(), RcloneCryptProvider::BrowseMode::Layered, QStringList{"mkv"});
    auto ok = lenient->enumerate({QStringLiteral("dir1")});
    QVERIFY(ok);
    QCOMPARE(int(ok->entities.size()), 1);
}

void TestRcloneCryptProvider::flatAndRecursiveModes() {
    const auto cipher = treeCipher();
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString cipherDir1 = QString::fromStdString(cipher->encryptDirName("dir1"));
    QVERIFY(QDir(root.path()).mkpath(cipherDir1));
    const QString cipherDeep = QString::fromStdString(cipher->encryptDirName("deep"));
    QVERIFY(QDir(root.path() + QLatin1Char('/') + cipherDir1).mkpath(cipherDeep));

    QDir rootDir(root.path());
    writeFakeCiphertext(rootDir, cipher->encryptFileName("root.mkv"), 5);
    QDir dir1Dir(root.path() + QLatin1Char('/') + cipherDir1);
    writeFakeCiphertext(dir1Dir, cipher->encryptFileName("inner.mkv"), 5);
    QDir deepDir(root.path() + QLatin1Char('/') + cipherDir1 + QLatin1Char('/') + cipherDeep);
    writeFakeCiphertext(deepDir, cipher->encryptFileName("leaf.mkv"), 5);

    // 仅文件（平铺）：只列根内文件。非空 parent 的调用方违约由 Q_ASSERT 在 Debug
    // 显影（进程级，无法进程内断言降级——契约见基类注释）
    auto flat =
        makeProvider(root.path(), RcloneCryptProvider::BrowseMode::FilesOnly, QStringList{"mkv"});
    auto flatRoot = flat->enumerate();
    QVERIFY(flatRoot);
    QCOMPARE(int(flatRoot->entities.size()), 1);
    QVERIFY(flatRoot->containers.empty());

    // 仅文件（含子目录递归）：整树拉平为 3 条
    auto rec = makeProvider(root.path(), RcloneCryptProvider::BrowseMode::FilesRecursive,
                            QStringList{"mkv"});
    auto tree = rec->enumerate();
    QVERIFY(tree);
    QCOMPARE(int(tree->entities.size()), 3);
    QVERIFY(tree->containers.empty());
    // 递归条目身份含层级相对路径
    const QString deepIdentity =
        QStringLiteral("rclonecrypt://") +
        RcloneCryptProvider::encodeIdentity(QStringLiteral("dir1/deep/leaf.mkv"));
    bool found = false;
    for (const auto &e : tree->entities)
        found |= e->path() == deepIdentity;
    QVERIFY(found);
}

void TestRcloneCryptProvider::streamEndpointOpensEnumeratedIdentity() {
    // 播放链路的恒等式（本方案唯一"双击能不能开出文件"的判据）：枚举铸造的 URN
    // 解回明文相对路径 → 现算的密文名必须与盘上名逐字节相同 → 解密流按明文尺度
    // 逐字节还原。合成密文用 RcloneCryptFixture，两块（64 KiB + 1000）顺带跨一次块界
    const auto cipher = treeCipher();
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString cipherDir1 = QString::fromStdString(cipher->encryptDirName("dir1"));
    QVERIFY(QDir(root.path()).mkpath(cipherDir1));
    QDir dir1(root.path() + QLatin1Char('/') + cipherDir1);

    const std::vector<Bytes> blocks{RcloneCryptFixture::pattern(65536, 0),
                                    RcloneCryptFixture::pattern(1000, 1)};
    const Bytes sealed = RcloneCryptFixture::sealFile(cipher->dataKey(), fileNonce(), blocks);
    const Bytes plain = RcloneCryptFixture::concatBlocks(blocks);
    const std::string cipherName = cipher->encryptFileName("movie one.mkv");
    {
        QFile f(dir1.filePath(QString::fromStdString(cipherName)));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(toQByteArray(sealed));
        f.close();
    }

    auto provider =
        makeProvider(root.path(), RcloneCryptProvider::BrowseMode::Layered, QStringList{"mkv"});
    auto layer = provider->enumerate({QStringLiteral("dir1")});
    QVERIFY(layer);
    QCOMPARE(int(layer->entities.size()), 1);
    const auto entity = layer->entities.front();

    // 身份形态：空格编成 %20，分隔符保留；解回明文后逐段重加密，得到的整条相对
    // 密文路径必须与盘上路径逐字节相同（端点开流走的就是这一步）
    const QString identity = entity->path();
    QVERIFY(identity.startsWith(QLatin1String(kRcloneCryptScheme)));
    const QString encodedRel = identity.mid(int(sizeof(kRcloneCryptScheme)) - 1);
    QVERIFY(encodedRel.contains(QStringLiteral("dir1/movie%20one.mkv")));
    const QString plainRel = RcloneCryptProvider::decodeIdentity(encodedRel);
    QCOMPARE(plainRel, QStringLiteral("dir1/movie one.mkv"));
    QCOMPARE(QString::fromStdString(cipher->encryptFileName(plainRel.toStdString())),
             cipherDir1 + QLatin1Char('/') + QString::fromStdString(cipherName));
    QCOMPARE(entity->fileName(), QStringLiteral("movie one.mkv"));

    auto endpoint = provider->createStreamEndpoint();
    QVERIFY(endpoint);
    auto opened = endpoint->openStream(identity);
    QVERIFY2(bool(opened), "endpoint must open the identity it minted");
    const auto stream = *opened;
    QCOMPARE(stream->size(), qint64(plain.size()));
    QCOMPARE(stream->isSeekable(), true);

    Bytes got;
    QByteArray chunk(4096, Qt::Uninitialized);
    while (true) {
        const qint64 n = stream->read(chunk.data(), chunk.size());
        if (n == ByteStream::kEof)
            break;
        QVERIFY2(n > 0, qPrintable(QStringLiteral("read failed at %1").arg(got.size())));
        got.insert(got.end(), chunk.constData(), chunk.constData() + std::size_t(n));
    }
    QVERIFY(got == plain);

    // 边界 seek 与跨块读同样按明文尺度成立（1 MiB 之外的块界在这里是 65536）
    QCOMPARE(stream->seek(65530), qint64(65530));
    QCOMPARE(stream->read(chunk.data(), 20), qint64(20));
    QVERIFY(std::memcmp(chunk.constData(), plain.data() + 65530, 20) == 0);

    // 身份指向盘上不存在的名字：错误文案可读，不崩不静默
    auto missing = endpoint->openStream(QLatin1String(kRcloneCryptScheme) +
                                        QStringLiteral("dir1/ghost%20file.mkv"));
    QVERIFY(!missing);
    QVERIFY(missing.error().contains(QStringLiteral("cannot open file")));
}

void TestRcloneCryptProvider::outOfDomainEncodingIsConfigError() {
    // base32768（rclone 的取值 2）没有选择入口，出现在 INI 里只可能是手改的——
    // 不静默降成 base32（那会解出一整层错名），也不清空原值
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("bad-encoding.ini"));
    QSettings settings(iniPath, QSettings::IniFormat);
    settings.setValue(QStringLiteral("provider"), QLatin1String(kRcloneCryptProviderKey));
    settings.setValue(QStringLiteral("rclonecrypt_root"), QStringLiteral("D:/crypt library"));
    settings.setValue(QStringLiteral("rclonecrypt_encoding"), 2);
    settings.sync();

    auto loaded = RcloneCryptProvider::load(settings, std::make_shared<ServiceRegistry>());
    auto asCrypt = std::dynamic_pointer_cast<RcloneCryptProvider>(loaded);
    QVERIFY(asCrypt);
    auto result = asCrypt->enumerate();
    QVERIFY(!result);
    QVERIFY(result.error().contains(QStringLiteral("encoding"), Qt::CaseInsensitive));
    QVERIFY(result.error().contains(QStringLiteral("2")));

    // 原值随序列化往返：一次落盘不得把域外取值洗成域内（配置异常要留着看得见）
    QSettings rewritten(dir.filePath(QStringLiteral("rewritten.ini")), QSettings::IniFormat);
    asCrypt->saveSettings(rewritten);
    QCOMPARE(rewritten.value(QStringLiteral("rclonecrypt_encoding")).toInt(), 2);

    // 合法取值不受牵连：0 / 1 各自解回对应编码，2 与负值落空
    QVERIFY(RcloneCryptProvider::nameEncodingFromIni(0) == RcloneCrypt::NameEncoding::Base32);
    QVERIFY(RcloneCryptProvider::nameEncodingFromIni(1) == RcloneCrypt::NameEncoding::Base64);
    QVERIFY(!RcloneCryptProvider::nameEncodingFromIni(2).has_value());
    QVERIFY(!RcloneCryptProvider::nameEncodingFromIni(-1).has_value());
}

QTEST_GUILESS_MAIN(TestRcloneCryptProvider)
#include "tst_RcloneCryptProvider.moc"

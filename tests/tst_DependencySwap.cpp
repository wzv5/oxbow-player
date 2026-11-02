#include "pch.h"

#include <QtTest/QtTest>

#include <QTemporaryDir>

#include "installer/DependencySwap.h"

// N150：落位 / 统一交换 / 卸载 / 启动清理 / 暂存整理的契约测试——全部在 QTemporaryDir
// 里进行，不碰真实插件目录。备份名带时间戳；删 backup 失败不算该项失败；rename 失败
// 旧内容原样保留；新目标落位失败把 backup 换回；启动清理删整个临时根，不做跨启动恢复；
// 安装根只认"存在且是绝对路径的目录"——空、相对、已不在、指向文件一律拒；解压目录清不
// 干净时不往里解。解压本身走系统 tar.exe，不测；只喂解压产物判暂存整理。

using namespace DependencyCatalog;
using namespace DependencySwap;

namespace {

struct Fixture {
    QTemporaryDir dir;
    QString pluginDir() const { return dir.path(); }
    QString target(const QString &relative) const { return dir.filePath(relative); }
};

QString write(const QString &path, const QByteArray &content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    file.write(content);
    file.close();
    return path;
}

QString makeStaged(const Fixture &fx, const QByteArray &content) {
    QDir().mkpath(stageRoot(fx.pluginDir()));
    QDir(stageRoot(fx.pluginDir())).mkpath(QStringLiteral("comp"));
    return write(QDir(QDir(stageRoot(fx.pluginDir())).filePath(QStringLiteral("comp")))
                     .filePath(QStringLiteral("new.bin")),
                 content);
}

// 在 extract/<id> 里造出一份解压产物
QString extracted(const Fixture &fx, const Component &comp, const QString &relative) {
    const QString dir = extractDirFor(fx.pluginDir(), comp);
    QDir().mkpath(QFileInfo(dir + QLatin1Char('/') + relative).absolutePath());
    return write(dir + QLatin1Char('/') + relative, QByteArrayLiteral("payload"));
}

} // namespace

class TestDependencySwap : public QObject {
    Q_OBJECT

  private slots:
    void installNewFile();
    void installCreatesTargetParent();
    void installNewDir();
    void reinstallFileSwapsViaBackup();
    void renameTargetFailureKeepsOld();
    void stagedMissingRollsBack();
    void backupDeleteFailureKept();
    void removeTarget();
    void removeMissingIsOk();
    void backupNameHasTimestamp();
    void cleanupRemovesTempRoot();
    void downloadTargetPaths();
    void prepareDownloadTargetCreatesParents();
    void emptyPluginDirRefused();
    void rootMustBeExistingAbsoluteDir();
    void prepareExtractDirRefusesDirtyDir();
    void stageDirStrip();
    void stageDirRoot();
    void stageSingleFileIgnoresVersionDir();
    void stageMissingRequiredEntry();
    void stageMissingPackageEntry();
    void stageWrongLayout();
    void stageUsesOwnDirNotExtract();
};

void TestDependencySwap::installNewFile() {
    Fixture fx;
    const QString staged = makeStaged(fx, "new");
    const SwapResult result = swapIntoPlace(fx.pluginDir(), QStringLiteral("ffmpeg"),
                                            QStringLiteral("ffmpeg.exe"), staged);
    QVERIFY(result.ok);
    QVERIFY(result.backupPath.isEmpty());
    QFile target(fx.target(QStringLiteral("ffmpeg.exe")));
    QVERIFY(target.open(QIODevice::ReadOnly));
    QCOMPARE(target.readAll(), QByteArray("new"));
    target.close();
    QVERIFY(!QFile::exists(staged));
    // backup 已删：备份目录空
    QVERIFY(QDir(backupRoot(fx.pluginDir())).isEmpty() ||
            !QDir(backupRoot(fx.pluginDir())).exists());
}

void TestDependencySwap::installCreatesTargetParent() {
    Fixture fx;
    const QString staged = makeStaged(fx, "model");
    // 目标父目录 model/ 不存在：落位时先建
    const SwapResult result = swapIntoPlace(fx.pluginDir(), QStringLiteral("hy-mt2-1.8b"),
                                            QStringLiteral("model/x.bin"), staged);
    QVERIFY(result.ok);
    QVERIFY(QFileInfo::exists(fx.target(QStringLiteral("model"))));
    QVERIFY(QFileInfo::exists(fx.target(QStringLiteral("model/x.bin"))));
}

void TestDependencySwap::installNewDir() {
    Fixture fx;
    const QString staged = QDir(stageRoot(fx.pluginDir())).filePath(QStringLiteral("whisper.cpp"));
    QDir().mkpath(stageRoot(fx.pluginDir()));
    QVERIFY(QDir(staged).mkpath(QStringLiteral(".")));
    write(staged + QStringLiteral("/whisper-cli.exe"), "cli");
    const SwapResult result = swapIntoPlace(fx.pluginDir(), QStringLiteral("whisper-cpp"),
                                            QStringLiteral("whisper.cpp"), staged);
    QVERIFY(result.ok);
    QVERIFY(QFileInfo::exists(fx.target(QStringLiteral("whisper.cpp/whisper-cli.exe"))));
}

void TestDependencySwap::reinstallFileSwapsViaBackup() {
    Fixture fx;
    write(fx.target(QStringLiteral("ffmpeg.exe")), "old");
    const QString staged = makeStaged(fx, "new");
    const SwapResult result = swapIntoPlace(fx.pluginDir(), QStringLiteral("ffmpeg"),
                                            QStringLiteral("ffmpeg.exe"), staged);
    QVERIFY(result.ok);
    QFile target(fx.target(QStringLiteral("ffmpeg.exe")));
    QVERIFY(target.open(QIODevice::ReadOnly));
    QCOMPARE(target.readAll(), QByteArray("new"));
    target.close();
    // 旧版随 backup 删除：备份目录不再有内容
    QVERIFY(QDir(backupRoot(fx.pluginDir())).isEmpty() ||
            !QDir(backupRoot(fx.pluginDir())).exists());
}

void TestDependencySwap::renameTargetFailureKeepsOld() {
    Fixture fx;
    const QString targetPath = fx.target(QStringLiteral("ffmpeg.exe"));
    write(targetPath, "old");
    QFile locked(targetPath);
    QVERIFY(locked.open(QIODevice::ReadOnly)); // 占用句柄：rename 必失败
    const QString staged = makeStaged(fx, "new");
    const SwapResult result = swapIntoPlace(fx.pluginDir(), QStringLiteral("ffmpeg"),
                                            QStringLiteral("ffmpeg.exe"), staged);
    QVERIFY(!result.ok);
    QVERIFY(result.backupPath.isEmpty()); // 旧内容原样，没进 backup
    locked.close();
    QFile old(targetPath);
    QVERIFY(old.open(QIODevice::ReadOnly));
    QCOMPARE(old.readAll(), QByteArray("old"));
    old.close();
}

void TestDependencySwap::stagedMissingRollsBack() {
    Fixture fx;
    write(fx.target(QStringLiteral("ffmpeg.exe")), "old");
    const SwapResult result =
        swapIntoPlace(fx.pluginDir(), QStringLiteral("ffmpeg"), QStringLiteral("ffmpeg.exe"),
                      fx.target(QStringLiteral("no-such-stage.bin")));
    QVERIFY(!result.ok);
    // 新目标落位失败：backup 换回，旧内容在原位
    QFile old(fx.target(QStringLiteral("ffmpeg.exe")));
    QVERIFY(old.open(QIODevice::ReadOnly));
    QCOMPARE(old.readAll(), QByteArray("old"));
    old.close();
    QVERIFY(result.backupPath.isEmpty());
}

void TestDependencySwap::backupDeleteFailureKept() {
    Fixture fx;
    // 文件型重装：旧文件带只读属性——rename 进 backup 成功（只读不改名），删只读文件失败，
    // 旧版留在 backup 而该项仍成功（MediaInfo 已加载场景同这一条路，不特殊失败）
    const QString targetPath = fx.target(QStringLiteral("MediaInfo.dll"));
    write(targetPath, "old");
    QFile::setPermissions(targetPath, QFile::ReadOwner | QFile::ReadUser);

    const QString staged = makeStaged(fx, "new");
    const SwapResult result = swapIntoPlace(fx.pluginDir(), QStringLiteral("mediainfo"),
                                            QStringLiteral("MediaInfo.dll"), staged);
    QVERIFY(result.ok);                    // 删 backup 失败不算该项失败
    QVERIFY(!result.backupPath.isEmpty()); // 旧版留在 backup
    QVERIFY(QRegularExpression(QStringLiteral("mediainfo\\.\\d{8}-\\d{6}-\\d{3}"))
                .match(QFileInfo(result.backupPath).fileName())
                .hasMatch());
    QFile backup(result.backupPath);
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QCOMPARE(backup.readAll(), QByteArray("old"));
    backup.close();
    // 新版已在目标位
    QFile target(targetPath);
    QVERIFY(target.open(QIODevice::ReadOnly));
    QCOMPARE(target.readAll(), QByteArray("new"));
    target.close();
    QFile::setPermissions(result.backupPath, QFile::ReadOwner | QFile::WriteOwner); // 便于清理
}

void TestDependencySwap::removeTarget() {
    Fixture fx;
    write(fx.target(QStringLiteral("ffmpeg.exe")), "old");
    const SwapResult result =
        removeFromPluginDir(fx.pluginDir(), QStringLiteral("ffmpeg"), QStringLiteral("ffmpeg.exe"));
    QVERIFY(result.ok);
    QVERIFY(!QFileInfo::exists(fx.target(QStringLiteral("ffmpeg.exe"))));
}

void TestDependencySwap::removeMissingIsOk() {
    Fixture fx;
    const SwapResult result = removeFromPluginDir(fx.pluginDir(), QStringLiteral("ffmpeg"),
                                                  QStringLiteral("no-such.exe"));
    QVERIFY(result.ok);
}

void TestDependencySwap::backupNameHasTimestamp() {
    Fixture fx;
    // 备份名 = <id>.<yyyyMMdd-HHmmss-zzz>：文件名安全、同秒重试不撞名。旧文件带只读
    // 属性：卸载 rename 成功、删 backup 失败，条目才留得下来看
    write(fx.target(QStringLiteral("a.bin")), "x");
    QFile::setPermissions(fx.target(QStringLiteral("a.bin")), QFile::ReadOwner | QFile::ReadUser);
    QVERIFY(
        removeFromPluginDir(fx.pluginDir(), QStringLiteral("comp"), QStringLiteral("a.bin")).ok);
    QVERIFY(!QFileInfo::exists(fx.target(QStringLiteral("a.bin"))));
    const QFileInfoList leftovers =
        QDir(backupRoot(fx.pluginDir()))
            .entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(leftovers.size(), 1);
    QVERIFY(QRegularExpression(QStringLiteral("^comp\\.\\d{8}-\\d{6}-\\d{3}$"))
                .match(leftovers.first().fileName())
                .hasMatch());
    QFile::setPermissions(leftovers.first().absoluteFilePath(),
                          QFile::ReadOwner | QFile::WriteOwner); // 便于清理
}

void TestDependencySwap::cleanupRemovesTempRoot() {
    Fixture fx;
    QVERIFY(QDir(fx.pluginDir()).mkpath(QStringLiteral(".oxbow-install/stage/x")));
    QVERIFY(QDir(fx.pluginDir()).mkpath(QStringLiteral(".oxbow-install/backup")));
    QVERIFY(QDir(fx.pluginDir()).mkpath(QStringLiteral(".oxbow-install/extract/y")));
    write(fx.target(QStringLiteral(".oxbow-install/backup/comp.20260101-000000-000/old.bin")),
          "old");
    write(fx.target(QStringLiteral(".oxbow-install/stale.pkg")), "pkg");
    QVERIFY(cleanupTempRoot(fx.pluginDir()));
    QVERIFY(!QFileInfo::exists(fx.target(QStringLiteral(".oxbow-install"))));
    // 幂等：根不存在也返回 true
    QVERIFY(cleanupTempRoot(fx.pluginDir()));
}

void TestDependencySwap::downloadTargetPaths() {
    Fixture fx;
    // 模型：整包即目标，直接下到 stage/<id>/<目标文件名>
    const QString turbo = downloadTargetPath(fx.pluginDir(), component(Id::WhisperTurbo));
    QVERIFY(turbo.endsWith(QStringLiteral("whisper-large-v3-turbo/ggml-large-v3-turbo.bin")));
    QVERIFY(turbo.startsWith(stageRoot(fx.pluginDir())));
    // 压缩包：<id>.pkg 落临时根
    const QString ffmpeg = downloadTargetPath(fx.pluginDir(), component(Id::Ffmpeg));
    QVERIFY(ffmpeg.endsWith(QStringLiteral("ffmpeg.pkg")));
    QVERIFY(ffmpeg.startsWith(tempRoot(fx.pluginDir())));
    QVERIFY(!ffmpeg.startsWith(stageRoot(fx.pluginDir())));
    QCOMPARE(extractDirFor(fx.pluginDir(), component(Id::LlamaCpp)),
             extractRoot(fx.pluginDir()) + QStringLiteral("/llama-cpp"));
    QCOMPARE(pluginPath(fx.pluginDir(), QStringLiteral("model/x.bin")),
             fx.pluginDir() + QStringLiteral("/model/x.bin"));
}

void TestDependencySwap::prepareDownloadTargetCreatesParents() {
    // 下载件的父目录必须由落位层现建：临时根刚被清过，模型的落点还要多一层 stage/<id>，
    // 缺任何一层都会让首次 write 报"系统找不到指定的路径"（本地写失败，不重试）
    Fixture fx;
    QString error;
    const QString model =
        prepareDownloadTarget(fx.pluginDir(), component(Id::WhisperTurbo), &error);
    QVERIFY(!model.isEmpty());
    QVERIFY(error.isEmpty());
    QVERIFY(QFileInfo::exists(QFileInfo(model).absolutePath()));
    QFile probe(model);
    QVERIFY2(probe.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable(probe.errorString()));
    probe.write("x");
    probe.close();

    Fixture fx2;
    const QString pkg = prepareDownloadTarget(fx2.pluginDir(), component(Id::Ffmpeg), &error);
    QVERIFY2(!pkg.isEmpty(), qPrintable(error));
    QVERIFY(QFileInfo::exists(QFileInfo(pkg).absolutePath()));
    QFile probe2(pkg);
    QVERIFY2(probe2.open(QIODevice::WriteOnly), qPrintable(probe2.errorString()));
    probe2.close();

    QVERIFY(prepareDownloadTarget(QString(), component(Id::Ffmpeg), &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestDependencySwap::emptyPluginDirRefused() {
    // 空安装根不得退化成"相对进程 CWD 的那一份"：三条写入口全部拒
    QVERIFY(!swapIntoPlace(QString(), QStringLiteral("ffmpeg"), QStringLiteral("ffmpeg.exe"),
                           QStringLiteral("stage"))
                 .ok);
    QVERIFY(
        !removeFromPluginDir(QString(), QStringLiteral("ffmpeg"), QStringLiteral("ffmpeg.exe")).ok);
    QVERIFY(cleanupTempRoot(QString()));
    QString staged;
    QString error;
    QVERIFY(!stageExtracted(QString(), component(Id::Ffmpeg), &staged, &error));
    QVERIFY(!prepareExtractDir(QString(), component(Id::Ffmpeg)));
}

void TestDependencySwap::rootMustBeExistingAbsoluteDir() {
    // 取值失败不只有空串一种形态：相对路径落到进程 CWD，不存在的路径会被代它重建，指向
    // 文件的路径更不是安装根。三种都不得产生任何写入或删除
    Fixture fx;
    const QString relative = QStringLiteral("relative-plugin-dir");
    QVERIFY(!QDir::isAbsolutePath(relative));
    QString staged;
    QString error;
    QVERIFY(prepareDownloadTarget(relative, component(Id::Ffmpeg), &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(!swapIntoPlace(relative, QStringLiteral("ffmpeg"), QStringLiteral("ffmpeg.exe"),
                           QStringLiteral("stage"))
                 .ok);
    QVERIFY(
        !removeFromPluginDir(relative, QStringLiteral("ffmpeg"), QStringLiteral("ffmpeg.exe")).ok);
    QVERIFY(!stageExtracted(relative, component(Id::Ffmpeg), &staged, &error));
    QVERIFY(!prepareExtractDir(relative, component(Id::Ffmpeg)));
    QVERIFY(cleanupTempRoot(relative)); // 没有可删的即成功，但一个字节也不动
    QVERIFY(!QDir(relative).exists());  // 也没被顺手建出来

    // 目录已不在：不代它重建
    const QString missing = fx.target(QStringLiteral("gone"));
    QVERIFY(!QDir(missing).exists());
    QVERIFY(prepareDownloadTarget(missing, component(Id::HyMt2), &error).isEmpty());
    QVERIFY(!QDir(missing).exists());
    QVERIFY(!swapIntoPlace(missing, QStringLiteral("ffmpeg"), QStringLiteral("model/x.bin"), staged)
                 .ok);

    // 指向文件：不是安装根
    const QString fileRoot = write(fx.target(QStringLiteral("a-file")), QByteArrayLiteral("x"));
    QVERIFY(!fileRoot.isEmpty());
    QVERIFY(prepareDownloadTarget(fileRoot, component(Id::Ffmpeg), &error).isEmpty());
    QVERIFY(
        !removeFromPluginDir(fileRoot, QStringLiteral("ffmpeg"), QStringLiteral("ffmpeg.exe")).ok);
}

void TestDependencySwap::prepareExtractDirRefusesDirtyDir() {
    // 清不干净就不往里解：DirRoot 形态整目录进暂存，残渣会随交换混进新安装
    Fixture fx;
    const Component &comp = component(Id::LlamaCpp);
    const QString dir = extractDirFor(fx.pluginDir(), comp);
    QVERIFY(QDir().mkpath(dir));
    const QString leftover = dir + QStringLiteral("/leftover.dll");
    QVERIFY(!write(leftover, QByteArrayLiteral("old")).isEmpty());
    QFile locked(leftover);
    QVERIFY(locked.open(QIODevice::ReadOnly)); // 占用句柄：删不掉
    QVERIFY(!prepareExtractDir(fx.pluginDir(), comp));
    QVERIFY(QFileInfo::exists(leftover));
    locked.close();
    QVERIFY(prepareExtractDir(fx.pluginDir(), comp)); // 放开后可清
    QVERIFY(!QFileInfo::exists(leftover));
}

void TestDependencySwap::stageDirStrip() {
    Fixture fx;
    const Component &comp = component(Id::WhisperCpp);
    extracted(fx, comp, QStringLiteral("Release/whisper-cli.exe"));
    extracted(fx, comp, QStringLiteral("Release/ggml-base.dll"));
    extracted(fx, comp, QStringLiteral("tests/other.exe")); // 不要的那一层
    QString staged, error;
    QVERIFY2(stageExtracted(fx.pluginDir(), comp, &staged, &error), qPrintable(error));
    QCOMPARE(staged, stageDirFor(fx.pluginDir(), comp));
    // 去一层前缀：Release/ 的内容即 whisper.cpp/ 的内容，别的层不带
    QVERIFY(QFileInfo::exists(staged + QStringLiteral("/whisper-cli.exe")));
    QVERIFY(QFileInfo::exists(staged + QStringLiteral("/ggml-base.dll")));
    QVERIFY(!QFileInfo::exists(staged + QStringLiteral("/tests")));
}

void TestDependencySwap::stageDirRoot() {
    Fixture fx;
    const Component &comp = component(Id::LlamaCpp);
    extracted(fx, comp, QStringLiteral("llama-server.exe"));
    extracted(fx, comp, QStringLiteral("ggml.dll"));
    QString staged, error;
    QVERIFY2(stageExtracted(fx.pluginDir(), comp, &staged, &error), qPrintable(error));
    QCOMPARE(staged, stageDirFor(fx.pluginDir(), comp));
    QVERIFY(QFileInfo::exists(staged + QStringLiteral("/llama-server.exe")));
    QVERIFY(QFileInfo::exists(staged + QStringLiteral("/ggml.dll")));
}

void TestDependencySwap::stageSingleFileIgnoresVersionDir() {
    Fixture fx;
    // ffmpeg 包带版本号目录：按通配挑唯一条目，不按版本拼路径
    const Component &comp = component(Id::Ffmpeg);
    extracted(fx, comp, QStringLiteral("ffmpeg-9.0.2/bin/ffmpeg.exe"));
    extracted(fx, comp, QStringLiteral("ffmpeg-9.0.2/bin/ffprobe.exe")); // 不要
    extracted(fx, comp, QStringLiteral("ffmpeg-9.0.2/LICENSE"));
    QString staged, error;
    QVERIFY2(stageExtracted(fx.pluginDir(), comp, &staged, &error), qPrintable(error));
    QCOMPARE(staged, stageDirFor(fx.pluginDir(), comp) + QStringLiteral("/ffmpeg.exe"));
    QVERIFY(QFileInfo::exists(staged));
    QVERIFY(!QFile::exists(stageDirFor(fx.pluginDir(), comp) + QStringLiteral("/ffprobe.exe")));
}

void TestDependencySwap::stageMissingRequiredEntry() {
    // 解压"成功"但必需条目不在：判失败，不让它落进错层
    Fixture fx;
    const Component &strip = component(Id::WhisperCpp);
    extracted(fx, strip, QStringLiteral("Release/readme.txt"));
    QString staged, error;
    QVERIFY(!stageExtracted(fx.pluginDir(), strip, &staged, &error));
    QVERIFY(staged.isEmpty());
    QVERIFY(error.contains(QStringLiteral("required entry")));

    Fixture fx2;
    const Component &root = component(Id::LlamaCpp);
    extracted(fx2, root, QStringLiteral("ggml.dll"));
    QVERIFY(!stageExtracted(fx2.pluginDir(), root, &staged, &error));

    Fixture fx3;
    const Component &dll = component(Id::MediaInfo);
    extracted(fx3, dll, QStringLiteral("MediaInfo.exe"));
    QVERIFY(!stageExtracted(fx3.pluginDir(), dll, &staged, &error));
}

void TestDependencySwap::stageMissingPackageEntry() {
    // SingleFile 形态：通配在包里挑不到条目
    Fixture fx;
    const Component &comp = component(Id::Ffmpeg);
    extracted(fx, comp, QStringLiteral("ffmpeg-9.0.2/bin/ffprobe.exe"));
    extracted(fx, comp, QStringLiteral("ffmpeg-9.0.2/bin/ffmpeg.exe.bak"));
    QString staged, error;
    QVERIFY(!stageExtracted(fx.pluginDir(), comp, &staged, &error));
    QVERIFY(staged.isEmpty());
    QVERIFY(error.contains(QStringLiteral("required entry missing in package")));
    // 解压目录压根不存在（未解压）
    Fixture fx2;
    QVERIFY(!stageExtracted(fx2.pluginDir(), component(Id::WhisperCpp), &staged, &error));
}

void TestDependencySwap::stageWrongLayout() {
    // DirStrip 形态而包里没有 Release/ 那一层：判失败，不去猜别的层
    Fixture fx;
    const Component &comp = component(Id::WhisperCpp);
    extracted(fx, comp, QStringLiteral("whisper-cli.exe"));
    extracted(fx, comp, QStringLiteral("bin/whisper-cli.exe"));
    QString staged, error;
    QVERIFY(!stageExtracted(fx.pluginDir(), comp, &staged, &error));
    QVERIFY(error.contains(QStringLiteral("package layout unexpected")));
}

void TestDependencySwap::stageUsesOwnDirNotExtract() {
    // 暂存一律落 stage/<id>：整理完即可随手清解压目录，两者分居两处、清理顺序无关
    Fixture fx;
    const Component &comp = component(Id::LlamaCpp);
    extracted(fx, comp, QStringLiteral("llama-server.exe"));
    QString staged, error;
    QVERIFY(stageExtracted(fx.pluginDir(), comp, &staged, &error));
    const QString extractDir = extractDirFor(fx.pluginDir(), comp);
    QVERIFY(!QFileInfo::exists(extractDir)); // 已 rename 进 stage
    discardExtractDir(fx.pluginDir(), comp); // 清已迁走的那一层不算失败也不伤暂存件
    QVERIFY(QFileInfo::exists(staged + QStringLiteral("/llama-server.exe")));

    // 同名 stage 残留先清掉，不留旧内容混进新形态
    write(staged + QStringLiteral("/leftover.dll"), "old");
    extracted(fx, comp, QStringLiteral("llama-server.exe"));
    QVERIFY(stageExtracted(fx.pluginDir(), comp, &staged, &error));
    QVERIFY(!QFile::exists(stageDirFor(fx.pluginDir(), comp) + QStringLiteral("/leftover.dll")));
}

QTEST_GUILESS_MAIN(TestDependencySwap)

#include "tst_DependencySwap.moc"

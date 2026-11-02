#include "pch.h"

#include "installer/DependencySwap.h"

#include "Logging.h"

namespace DependencySwap {

namespace {

// 安装根只认插件目录：空串与相对路径拼出来的是相对进程 CWD 的那一份，动它等于动别人的
// 目录；目录已不在时也不动手——写进去没有消费者读，删掉的是别人同名位置的东西
bool hasRoot(const QString &pluginDir) {
    if (pluginDir.isEmpty() || !QDir::isAbsolutePath(pluginDir))
        return false;
    return QFileInfo(pluginDir).isDir();
}

QString joinPath(const QString &base, const QString &relative) {
    return QDir(base).filePath(relative);
}

QString backupName(const QString &backupId) {
    // 时间戳进文件名：文件名安全、同秒重试不撞名
    return backupId + QLatin1Char('.') +
           QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
}

// backup 里那份旧内容：目录型整棵删、文件型单删。删不掉不算失败（占用等），留在原地
// 随下一次启动清理
void discardBackup(const QString &backupPath, QString *keptPath) {
    const QFileInfo info(backupPath);
    const bool removed =
        info.isDir() ? QDir(backupPath).removeRecursively() : QFile::remove(backupPath);
    if (removed)
        return;
    *keptPath = backupPath;
    OXBOW_LOG_WARNING().noquote()
        << QStringLiteral("backup could not be deleted, kept for next startup: %1").arg(backupPath);
}

} // namespace

QString tempRoot(const QString &pluginDir) {
    return joinPath(pluginDir, QStringLiteral(".oxbow-install"));
}

QString stageRoot(const QString &pluginDir) { return joinPath(tempRoot(pluginDir), "stage"); }

QString backupRoot(const QString &pluginDir) { return joinPath(tempRoot(pluginDir), "backup"); }

QString extractRoot(const QString &pluginDir) { return joinPath(tempRoot(pluginDir), "extract"); }

QString pluginPath(const QString &pluginDir, const QString &relative) {
    return joinPath(pluginDir, relative);
}

QString downloadTargetPath(const QString &pluginDir, const DependencyCatalog::Component &comp) {
    using namespace DependencyCatalog;
    if (comp.extract.kind == ExtractKind::None) {
        // 整包即目标：直接下到 stage/<id>/<目标文件名>，落位只做起名换
        const QString fileName = QFileInfo(QLatin1String(comp.installTarget)).fileName();
        return joinPath(stageDirFor(pluginDir, comp), fileName);
    }
    return joinPath(tempRoot(pluginDir), QStringLiteral("%1.pkg").arg(comp.installId));
}

QString extractDirFor(const QString &pluginDir, const DependencyCatalog::Component &comp) {
    return joinPath(extractRoot(pluginDir), QLatin1String(comp.installId));
}

QString prepareDownloadTarget(const QString &pluginDir, const DependencyCatalog::Component &comp,
                              QString *error) {
    *error = QString();
    if (!hasRoot(pluginDir)) {
        *error = QStringLiteral("plugin directory unknown");
        return {};
    }
    // 落点的父目录现建：临时根刚被清过，而模型的下载件要落进 stage/<id>
    const QString path = downloadTargetPath(pluginDir, comp);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("could not create the download directory for %1")
                     .arg(QLatin1String(comp.installId));
        return {};
    }
    return path;
}

QString stageDirFor(const QString &pluginDir, const DependencyCatalog::Component &comp) {
    return joinPath(stageRoot(pluginDir), QLatin1String(comp.installId));
}

bool prepareExtractDir(const QString &pluginDir, const DependencyCatalog::Component &comp) {
    if (!hasRoot(pluginDir))
        return false;
    const QString dir = extractDirFor(pluginDir, comp);
    // 清不干净就不往里解：DirRoot 形态把整个解压目录送进暂存，残渣会随交换混进新安装
    if (QFileInfo::exists(dir) && !QDir(dir).removeRecursively())
        return false;
    return QDir().mkpath(dir);
}

void discardExtractDir(const QString &pluginDir, const DependencyCatalog::Component &comp) {
    if (!hasRoot(pluginDir))
        return;
    QDir(extractDirFor(pluginDir, comp)).removeRecursively();
}

bool stageExtracted(const QString &pluginDir, const DependencyCatalog::Component &comp,
                    QString *staged, QString *error) {
    using namespace DependencyCatalog;
    *staged = QString();
    if (!hasRoot(pluginDir)) {
        *error = QStringLiteral("plugin directory unknown");
        return false;
    }
    const ExtractSpec &spec = comp.extract;
    const QString extractDir = extractDirFor(pluginDir, comp);
    const QString stageDir = stageDirFor(pluginDir, comp);
    const bool singleFile = spec.kind == ExtractKind::SingleFile;

    // 待暂存的那一份：目录型是解压目录本身（或要去掉前缀的那一层），文件型是通配命中的条目
    QString source = extractDir;
    if (spec.kind == ExtractKind::DirStrip) {
        source = joinPath(extractDir, QLatin1String(spec.stripPrefix));
        if (!QFileInfo::exists(source)) {
            *error = QStringLiteral("package layout unexpected: %1 not found")
                         .arg(QLatin1String(spec.stripPrefix));
            return false;
        }
    } else if (singleFile) {
        // 包内按通配挑唯一条目（ffmpeg 的包带版本号目录，不按版本拼路径）
        source = QString();
        QDirIterator it(extractDir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            if (QDir::match(QLatin1String(spec.pattern), QDir(extractDir).relativeFilePath(file))) {
                source = file;
                break;
            }
        }
        if (source.isEmpty()) {
            *error = QStringLiteral("required entry missing in package: %1")
                         .arg(QLatin1String(spec.pattern));
            return false;
        }
    }

    // 一律整理进 stage/<id>：解压目录与暂存目录分居两处，谁先清都不影响对方。旧暂存清不掉
    // 就到此为止——目录型与文件型的 rename 都要求目标位为空，硬走下去只会换个名报同一错
    if (QFileInfo::exists(stageDir) && !QDir(stageDir).removeRecursively()) {
        *error = QStringLiteral("could not clear stage directory for %1")
                     .arg(QLatin1String(comp.installId));
        return false;
    }
    if (!QDir().mkpath(singleFile ? stageDir : stageRoot(pluginDir))) {
        *error = QStringLiteral("could not create stage directory");
        return false;
    }
    *staged = singleFile ? joinPath(stageDir, QLatin1String(spec.requiredEntry)) : stageDir;
    if (!QFile::rename(source, *staged)) {
        *error = QStringLiteral("could not stage %1").arg(source);
        *staged = QString();
        return false;
    }
    // 落位前验必需条目：宁可判失败也不让"解压成功"落进错层
    const QString required =
        singleFile ? *staged : joinPath(*staged, QLatin1String(spec.requiredEntry));
    if (!QFileInfo::exists(required)) {
        *error = QStringLiteral("required entry missing after extraction: %1")
                     .arg(QLatin1String(spec.requiredEntry));
        *staged = QString();
        return false;
    }
    return true;
}

SwapResult swapIntoPlace(const QString &pluginDir, const QString &backupId,
                         const QString &relativeTarget, const QString &stagedPath) {
    SwapResult result;
    if (!hasRoot(pluginDir)) {
        result.error = QStringLiteral("plugin directory unknown");
        return result;
    }
    const QString target = pluginPath(pluginDir, relativeTarget);
    // 目标父目录不存在先建（如 model/）
    QDir().mkpath(QFileInfo(target).absolutePath());
    QString backupPath;
    if (QFileInfo::exists(target)) {
        QDir().mkpath(backupRoot(pluginDir));
        backupPath = joinPath(backupRoot(pluginDir), backupName(backupId));
        if (!QFile::rename(target, backupPath)) {
            // 这一步同时是占用探测：失败即旧内容原样、该项失败
            result.error =
                QStringLiteral("rename of existing target into backup failed: %1").arg(target);
            return result;
        }
    }
    if (!QFile::rename(stagedPath, target)) {
        // 新内容落位失败：backup 换回，换不回则旧版留在 backup 并判失败
        if (!backupPath.isEmpty()) {
            if (QFile::rename(backupPath, target)) {
                result.error = QStringLiteral("rename of staged content to target failed, old "
                                              "content restored: %1")
                                   .arg(target);
                return result;
            }
            result.backupPath = backupPath;
        }
        result.error = QStringLiteral("rename of staged content to target failed: %1").arg(target);
        return result;
    }
    if (!backupPath.isEmpty())
        discardBackup(backupPath, &result.backupPath);
    result.ok = true;
    return result;
}

SwapResult removeFromPluginDir(const QString &pluginDir, const QString &backupId,
                               const QString &relativeTarget) {
    SwapResult result;
    if (!hasRoot(pluginDir)) {
        result.error = QStringLiteral("plugin directory unknown");
        return result;
    }
    const QString target = pluginPath(pluginDir, relativeTarget);
    if (!QFileInfo::exists(target)) {
        result.ok = true;
        return result;
    }
    QDir().mkpath(backupRoot(pluginDir));
    const QString backupPath = joinPath(backupRoot(pluginDir), backupName(backupId));
    if (!QFile::rename(target, backupPath)) {
        result.error = QStringLiteral("rename of target into backup failed: %1").arg(target);
        return result;
    }
    // rename 成功即卸载完成（目标已不在）；随后删 backup，删不掉不算该项失败
    discardBackup(backupPath, &result.backupPath);
    result.ok = true;
    return result;
}

bool cleanupTempRoot(const QString &pluginDir) {
    if (!hasRoot(pluginDir)) {
        // 安装根取不到时不做任何删除：说清楚跳过，否则盘上留一份没人认领的临时根也无从查
        OXBOW_LOG_WARNING() << "installer temp root cleanup skipped: plugin directory unavailable";
        return true;
    }
    const QString root = tempRoot(pluginDir);
    if (!QFileInfo::exists(root))
        return true;
    if (QDir(root).removeRecursively())
        return true;
    OXBOW_LOG_WARNING().noquote()
        << QStringLiteral("could not fully remove installer temp root, retrying on next "
                          "startup: %1")
               .arg(root);
    return false;
}

} // namespace DependencySwap

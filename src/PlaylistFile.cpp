#include "pch.h"

#include "PlaylistFile.h"

namespace PlaylistFile {

namespace {

// 两档共用的设备约定：QSaveFile 原子写（MpvConf 先例）。不加 QIODevice::Text
// ——MpvConf 同款理由：Text 会把 \n 翻译成 \r\n，破坏两档统一的 LF 换行。
// QTextStream 为设备写入，Qt6 默认编解码即 UTF-8 且不写 BOM，显式声明只为可读
void setTextStreamOptions(QTextStream &ts) {
    ts.setEncoding(QStringConverter::Utf8);
    ts.setGenerateByteOrderMark(false);
}

// 提交收口：error 有值 = 提交失败原因
std::optional<QString> commitOrError(QSaveFile &sf) {
    if (sf.commit())
        return std::nullopt;
    return QStringLiteral("Commit failed: %1").arg(sf.errorString());
}

// ——PLS（INI 形态，Winamp 谱系）：段头 → NumberOfEntries → 逐条 File/Title/Length
// 三键一组（下标自 1 起）→ Version=2 收尾；Length 恒写 -1（Winamp 惯例，Length 键
// 恒写；时长不导出，N80 变更记录 2）——
std::expected<void, QString> writePls(const QString &path, const QList<Entry> &tracks) {
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly))
        return std::unexpected(QStringLiteral("Cannot open file: %1").arg(sf.errorString()));
    QTextStream ts(&sf);
    setTextStreamOptions(ts);
    ts << QStringLiteral("[playlist]\n");
    ts << QStringLiteral("NumberOfEntries=") << tracks.size() << QLatin1Char('\n');
    for (int i = 0; i < tracks.size(); ++i) {
        const Entry &e = tracks.at(i);
        ts << QStringLiteral("File") << (i + 1) << QLatin1Char('=') << e.url << QLatin1Char('\n');
        ts << QStringLiteral("Title") << (i + 1) << QLatin1Char('=') << e.title
           << QLatin1Char('\n');
        ts << QStringLiteral("Length") << (i + 1) << QLatin1String("=-1\n");
    }
    ts << QStringLiteral("Version=2\n");
    ts.flush();
    if (ts.status() != QTextStream::Ok) {
        sf.cancelWriting();
        return std::unexpected(QStringLiteral("Write failed: %1").arg(sf.errorString()));
    }
    if (auto err = commitOrError(sf))
        return std::unexpected(*err);
    return {};
}

// ——M3U（扩展 M3U，UTF-8）：首行 #EXTM3U，每条两行：先 #EXTINF 后 url；时长位
// 恒写 -1（时长不导出）。m3u8 与此同一份内容（UTF-8 无 BOM 正合 m3u8 定义），
// 后缀只是命名偏好——
std::expected<void, QString> writeM3u(const QString &path, const QList<Entry> &tracks) {
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly))
        return std::unexpected(QStringLiteral("Cannot open file: %1").arg(sf.errorString()));
    QTextStream ts(&sf);
    setTextStreamOptions(ts);
    ts << QStringLiteral("#EXTM3U\n");
    for (const Entry &e : tracks) {
        ts << QStringLiteral("#EXTINF:-1,") << e.title << QLatin1Char('\n');
        ts << e.url << QLatin1Char('\n');
    }
    ts.flush();
    if (ts.status() != QTextStream::Ok) {
        sf.cancelWriting();
        return std::unexpected(QStringLiteral("Write failed: %1").arg(sf.errorString()));
    }
    if (auto err = commitOrError(sf))
        return std::unexpected(*err);
    return {};
}

} // namespace

std::expected<void, QString> writeFile(const QString &path, const QList<Entry> &tracks) {
    // 后缀即格式：分派是模块内部的事（N80 §1），m3u8 并入 m3u 档
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("pls"))
        return writePls(path, tracks);
    if (suffix == QLatin1String("m3u") || suffix == QLatin1String("m3u8"))
        return writeM3u(path, tracks);
    return std::unexpected(QStringLiteral("Unsupported file format: .%1").arg(suffix));
}

} // namespace PlaylistFile

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

// 首行剥 U+FEFF：第三方清单常带 UTF-8 BOM，不剥则 M3U 首行 "#EXTM3U" 带 FEFF
// 前缀、不再是注释行，落成幽灵条目。照 MpvConf::parse / ExternalProvider::
// parseSource 的显式剥除惯例，不赌解码器的初始 BOM 行为（BOM 只在流首出现，
// 仅对首行调用）
void stripLeadingBom(QString &line) {
    if (line.startsWith(QChar(0xFEFF)))
        line.remove(0, 1);
}

// ——PLS 读取（INI 形态）：手写逐行解析，不用 QSettings——它的 INI 值转义会把
// 字面 %XX 序列还原，网络 URL 普遍含字面百分号，静默损坏；且读写模型对只读
// 导入有多余副作用（N81 §4）。逐行取 File<数字>= 键值，键名不区分大小写；条数
// 以扫到的 File 键为准、按键内数字升序输出（NumberOfEntries / Title / Length
// 一律不读——TitleN / LengthN 不产元数据，条数声明不信任第三方文件）。分隔符
// '=' 前后不允许空白（"File1 = x" 视为坏行跳过——宽容口径宁缺勿错，不猜着修）——
std::expected<QStringList, QString> readPls(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::unexpected(QStringLiteral("Cannot open file: %1").arg(f.errorString()));
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    static const QRegularExpression re(QStringLiteral("^File(\\d+)=(.*)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    std::vector<QPair<int, QString>> indexed;
    bool firstLine = true;
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (firstLine) {
            firstLine = false;
            stripLeadingBom(line);
        }
        const auto match = re.match(line);
        if (!match.hasMatch())
            continue;
        const QString value = match.captured(2).trimmed();
        if (value.isEmpty())
            continue; // 空串条目跳过（两档共通）
        indexed.emplace_back(match.captured(1).toInt(), value);
    }
    // 稳定排序：同号键（畸形文件）保文件顺序，正常文件键号唯一
    std::stable_sort(indexed.begin(), indexed.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    QStringList urls;
    urls.reserve(qsizetype(indexed.size()));
    for (const auto &entry : indexed)
        urls.push_back(entry.second);
    return urls;
}

// ——M3U 读取：逐行扫，空行与 # 开头行一律跳过（含 #EXTM3U / #EXTINF，不解析、
// 不配对下一行），其余行剔除首尾空白即 url——
std::expected<QStringList, QString> readM3u(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::unexpected(QStringLiteral("Cannot open file: %1").arg(f.errorString()));
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    QStringList urls;
    bool firstLine = true;
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (firstLine) {
            firstLine = false;
            stripLeadingBom(line);
        }
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        urls.push_back(line);
    }
    return urls;
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

std::expected<QStringList, QString> readFile(const QString &path) {
    // 后缀即格式：分派与 writeFile 同一份口径，m3u8 并入 m3u 档（N81 §3）；
    // 只认后缀不做内容嗅探——经 All Files 选出的生面孔在此显影，不猜着解析
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("pls"))
        return readPls(path);
    if (suffix == QLatin1String("m3u") || suffix == QLatin1String("m3u8"))
        return readM3u(path);
    return std::unexpected(QStringLiteral("Unsupported file format: .%1").arg(suffix));
}

} // namespace PlaylistFile

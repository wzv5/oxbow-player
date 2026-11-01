#include "pch.h"

#include "SrtAssembler.h"

#include "SubtitleExtractor.h"

namespace {

// HH:MM:SS,mmm（负值钳 0；小时位超两位照实展开）
QString formatSrtClock(double seconds) {
    if (seconds < 0)
        seconds = 0;
    const qint64 totalMs = qRound64(seconds * 1000.0);
    const qint64 ms = totalMs % 1000;
    const qint64 totalSeconds = totalMs / 1000;
    const qint64 s = totalSeconds % 60;
    const qint64 m = (totalSeconds / 60) % 60;
    const qint64 h = totalSeconds / 3600;
    return QStringLiteral("%1:%2:%3,%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

} // namespace

void SrtAssembler::feed(const QByteArray &bytes) {
    m_byteBuffer += bytes;

    // 从尾部回扫找 UTF-8 序列边界：起点字节判序列长度，长度未满即为半字尾，
    // 这一段留在缓冲等下一次凑齐（ASCII 与完整序列照常放行）
    qsizetype safe = m_byteBuffer.size();
    const qsizetype n = m_byteBuffer.size();
    for (qsizetype i = n - 1; i >= 0 && i >= n - 4; --i) {
        const uchar b = static_cast<uchar>(m_byteBuffer.at(i));
        if ((b & 0xC0) == 0x80)
            continue; // 多字节序列的中间字节，继续回扫
        const qsizetype len = (b & 0xE0) == 0xC0   ? 2
                              : (b & 0xF0) == 0xE0 ? 3
                              : (b & 0xF8) == 0xF0 ? 4
                                                   : 1;
        if (n - i < len)
            safe = i;
        break;
    }
    QString chunk = QString::fromUtf8(m_byteBuffer.constData(), safe);
    m_byteBuffer.remove(0, safe);

    if (m_bomPending && !chunk.isEmpty()) {
        // 起始 UTF-8 BOM 只剥一次（工具偶尔会带；mpv 读缓存件时同样豁免）
        if (chunk.startsWith(QChar(0xFEFF)))
            chunk.remove(0, 1);
        m_bomPending = false;
    }

    m_lineBuffer += chunk;
    for (int nl = m_lineBuffer.indexOf(u'\n'); nl >= 0; nl = m_lineBuffer.indexOf(u'\n')) {
        const QString line = m_lineBuffer.left(nl);
        m_lineBuffer.remove(0, nl + 1);
        processLine(line);
    }
}

void SrtAssembler::finish() {
    // 无换行的残行再尝试一次（一次给全且不带尾换行的工具形态）
    if (!m_lineBuffer.isEmpty()) {
        const QString line = m_lineBuffer;
        m_lineBuffer.clear();
        processLine(line);
    }
    flushOpenBlock();
}

void SrtAssembler::clear() {
    m_entries.clear();
    m_byteBuffer.clear();
    m_lineBuffer.clear();
    m_blockLines.clear();
    m_bomPending = true;
}

void SrtAssembler::processLine(const QString &line) {
    QString trimmed = line;
    if (trimmed.endsWith(u'\r'))
        trimmed.chop(1); // CRLF 与 LF 都认
    // whisper 行单行一段、无空行封块：命中旁路先封掉挂着的 srt 块再即到即收
    Entry whisper;
    if (parseWhisperLine(trimmed, whisper)) {
        flushOpenBlock();
        appendParsed(whisper);
        return;
    }
    if (trimmed.trimmed().isEmpty()) {
        flushOpenBlock(); // 空行封块；连续空行产生空块，解析侧自然丢弃
        return;
    }
    m_blockLines << trimmed;
}

void SrtAssembler::flushOpenBlock() {
    if (m_blockLines.isEmpty())
        return;
    const QList<Entry> parsed = parseBlock(m_blockLines);
    m_blockLines.clear();
    for (const Entry &entry : parsed)
        appendParsed(entry);
}

void SrtAssembler::appendParsed(const Entry &entry) {
    // 上一块终点缺失：由本条起点回填（N143 的"缺失回落下一块起点"流式版）；
    // 空文本条目不入表（whisper 的 -sns 抑制后可能只剩时间戳行），回填不受影响
    if (!m_entries.isEmpty() && m_entries.constLast().endMissing) {
        m_entries.last().endSeconds = entry.startSeconds;
        m_entries.last().endMissing = false;
    }
    if (!entry.text.trimmed().isEmpty())
        m_entries.append(entry);
}

QList<SrtAssembler::Entry> SrtAssembler::parseBlock(const QStringList &lines) {
    // 解析语义与 N143 同规：单块整段交给 parseSrt——时间行两种毫秒分隔都收、
    // 行号行只当分隔、end 早于 start 的畸形块整块丢、文本清洗与空文本保留
    const QList<SubtitleExtractor::SubtitleEntry> parsed =
        SubtitleExtractor::parseSrt(lines.join(u'\n'));
    QList<Entry> out;
    out.reserve(parsed.size());
    for (const SubtitleExtractor::SubtitleEntry &entry : parsed) {
        Entry block;
        block.startSeconds = entry.startSeconds;
        block.endMissing = std::isinf(entry.endSeconds);
        block.endSeconds = block.endMissing ? entry.startSeconds : entry.endSeconds;
        block.text = entry.text;
        out.append(block);
    }
    return out;
}

bool SrtAssembler::parseWhisperLine(const QString &line, Entry &out) {
    // whisper-cli 的默认 stdout 形态：`[00:00:00.000 --> 00:00:02.100]  文本`，
    // 单行一段。srt 时间行无方括号包裹，两形态无歧义
    static const QRegularExpression pattern(QRegularExpression::anchoredPattern(
        QStringLiteral("\\[\\s*(\\d{2}):(\\d{2}):(\\d{2})\\.(\\d{1,3})\\s*-->\\s*"
                       "(\\d{2}):(\\d{2}):(\\d{2})\\.(\\d{1,3})\\s*\\]\\s*(.*)")));
    const QRegularExpressionMatch match = pattern.match(line);
    if (!match.hasMatch())
        return false;
    const auto seconds = [&match](int h, int m, int s, int ms) {
        return match.captured(h).toInt() * 3600.0 + match.captured(m).toInt() * 60.0 +
               match.captured(s).toInt() + match.captured(ms).toInt() / 1000.0;
    };
    out.startSeconds = seconds(1, 2, 3, 4);
    out.endSeconds = seconds(5, 6, 7, 8);
    out.endMissing = false;
    out.text = match.captured(9).trimmed();
    return true;
}

QString SrtAssembler::renderSrt(const QList<Entry> &entries) {
    QString out;
    for (int i = 0; i < entries.size(); ++i) {
        const Entry &entry = entries.at(i);
        // 终点缺失（流的中断残块）以起点作终点：零时长合法块，mpv 不显示
        const double end = (entry.endMissing || entry.endSeconds < entry.startSeconds)
                               ? entry.startSeconds
                               : entry.endSeconds;
        out += QStringLiteral("%1\n%2 --> %3\n%4\n\n")
                   .arg(QString::number(i + 1), formatSrtClock(entry.startSeconds),
                        formatSrtClock(end), entry.text);
    }
    return out;
}

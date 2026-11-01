#pragma once

#include "pch.h"

// N146：ASR 输出的流式拼装器。与 SubtitleExtractor::parseSrt（N143）分工：那是
// 整份文本的一次性解析，这里是子进程 stdout 的增量摄取——半行／半块都留在缓冲，
// 只有空行（CRLF / LF 都认）封住的完整块才进解析，进程收尾时对残余尾巴再尝试一次。
// 输入双形态：srt 块（空行封块）之外兼收 whisper 行（行首 [HH:MM:SS.mmm -->
// HH:MM:SS.mmm]，单行一段、无空行封块，命中旁路先封掉挂着的 srt 块）；
// trim 后空文本的条目一律不入表。解析语义与 parseSrt 同规（单块由它判定），畸形块
// 整块丢弃不影响后续；只认 UTF-8（带状态增量解码留半字尾，不做本地编码回落），
// 起始 BOM 剥除。曲目标按到达序维护，上一块终点缺失时由下一块起点回填；
// 整表渲染（序号从 1 递增、时间一律逗号毫秒形）供临时 srt 与缓存入库共用。
// 纯内存状态、无线程纪律（调用方在 GUI 线程信号驱动）。
class SrtAssembler {
  public:
    // 终点缺失的块：endSeconds 暂记起点，endMissing 置位；后继块到达即回填，
    // 渲染时仍缺失者以起点作终点（零时长合法块）
    struct Entry {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        bool endMissing = false;
        QString text;
    };

    // 增量喂入（UTF-8 断字安全；可从任意字节边界喂）
    void feed(const QByteArray &bytes);
    // 进程收尾：无换行的残行与未封口的末块各再尝试一次
    void finish();
    // 当前表（到达序；只在 feed / finish 时增长与回填）
    const QList<Entry> &entries() const { return m_entries; }
    // 新一轮复位（含 BOM 状态与全部缓冲）
    void clear();

    // 单块解析（静态纯函数）：块内各行（不含封口空行）按 parseSrt 同规判定，
    // 时间行两种毫秒分隔都收、行号行只当分隔、畸形块返回空表
    static QList<Entry> parseBlock(const QStringList &lines);
    // whisper 行解析（静态纯函数）：行首 [HH:MM:SS.mmm --> HH:MM:SS.mmm] 形态解出
    // 单条目（`.` 毫秒、文本取 `]` 后去首尾空白）；非该形态返回 false、out 不动。
    // 文本为空仍返回 true（out.text 为空，入表侧统一过滤）
    static bool parseWhisperLine(const QString &line, Entry &out);
    // 整表渲染：序号从 1 递增，时间一律 HH:MM:SS,mmm --> HH:MM:SS,mmm
    static QString renderSrt(const QList<Entry> &entries);

  private:
    void processLine(const QString &line);
    void flushOpenBlock();
    void appendParsed(const Entry &entry);

    QList<Entry> m_entries;
    QByteArray m_byteBuffer;  // 尚不能安全解码的 UTF-8 半字尾
    QString m_lineBuffer;     // 尚无换行收尾的半行
    QStringList m_blockLines; // 空行封口前的当前块各行
    bool m_bomPending = true; // 流起始的 UTF-8 BOM 只在首个解码片段剥一次
};

#pragma once

#include "pch.h"

#include "mpvbridge/MpvController.h"

// N143：字幕浏览器的取数与解析单模块。同时承担：本地性判据、直读 / 转取两条路径的
// 选择、ffmpeg 命令行装配、QProcess 异步、结果缓存、取消，以及 srt 文本解析。对外
// 只给"条目表 + 失败原因"两种结果，调用方不知道也不关心走了哪条路径——外部 srt
// 文件直读与 ffmpeg 转出的 srt 是同一种文本，这只是实现细节。
//
// 路径判据：外部轨 codec 为 subrip 且扩展名 .srt 且本地可读时
// 直读（不起进程、不需要 ffmpeg），其余一切（内嵌轨、外部 ass / webvtt、容器型外部
// 字幕）转取。转接形态（oxbow://）一律不支持；正播来源与外部字幕文件都必须是本地
// 可读文件。缓存的键按路径类型分别定：外部轨不含正播文件——同一个字幕文件换一部片
// 挂，条目表不变，不因换了片而重取。
class SubtitleExtractor : public QObject {
    Q_OBJECT
  public:
    struct SubtitleEntry {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        QString text; // 已清洗；空文本合法（清屏 / 占位块保留）
    };

    // 失败原因（面板映射为可翻译原因串；细节一律落日志不进面板）
    enum class Failure { NotLocal, FfmpegMissing, FetchFailed };

    explicit SubtitleExtractor(QObject *parent = nullptr);

    // ffmpeg 可执行文件路径（已由调用方解析；空 = 不可用，仅影响转取路径）
    void setFfmpegPath(const QString &path) { m_ffmpegPath = path; }

    // 发起一次取数：track 为当前主选文本字幕轨，mediaPath 为正播文件身份串。
    // 在飞请求先取消（不排队）；缓存命中与直读路径同步发射 finished。
    void start(const MpvController::TrackInfo &track, const QString &mediaPath);

    // 取消在飞请求，结果静默丢弃；无在飞请求时无操作
    void cancel();

    // 该请求是否已在飞或已出表（身份键与最近一次取数一致）——面板的重取触发点据此
    // 跳过无事可做的重取；失败与取消即作废判据，下一次触发重试
    bool isCurrentRequest(const MpvController::TrackInfo &track, const QString &mediaPath) const;

    // ——判据与解析（static 纯函数，供测试直接覆盖）——

    // 可浏览的文本字幕轨判据：主选（main-selection 0）且非位图轨（image 字段 +
    // select.lua 的 codec 黑名单兜底）
    static bool isBrowsableTextSubtitle(const MpvController::TrackInfo &track);

    // ffmpeg 路径选择：ini 显式值优先，空则取候选表第一项；都没有返回空串（判不可用）
    static QString resolveFfmpegPath(const QString &configured, const QString &configDir);

    // srt 文本 → 条目表：时间行切块（, 与 . 毫秒分隔都接受、终点可缺）、行号只当
    // 分隔不用于定位、按起点稳定排序、畸形块整块丢弃；end 缺失回落下一块起点
    // （末块无上界）；文本已清洗，清洗后为空的条目保留
    static QList<SubtitleEntry> parseSrt(const QString &text);

    // 字节 → 文本：BOM 优先（UTF-8 / UTF-16LE / UTF-16BE）→ UTF-8 严格解码，失败
    // 回落系统本地编码。ok 为假表示两种判据都不成立（本地编码回落恒成功，此分支
    // 当前不可达，结构保留）
    static QString decodeText(const QByteArray &bytes, bool *ok);

    // 文本清洗：剥 HTML 标签与 ASS 花括号覆盖，\h 与 \N / \n 归一为空格
    static QString cleanEntryText(QString text);

  signals:
    void finished(const QList<SubtitleEntry> &entries);
    void failed(SubtitleExtractor::Failure reason);
    // 转取进度（0..1）；ffmpeg 自己报不出总时长（如外部纯文本轨）时不发射
    void progress(double ratio);

  private:
    // 取数请求的身份：键 = 输入文件 + 轨标识 + 修改时间与大小（空 = 本地性判不过）
    struct Resolved {
        QString key;
        QFileInfo input; // 外部轨为外部字幕文件自身，内嵌轨为正播文件
        bool external = false;
    };

    Resolved resolveRequest(const MpvController::TrackInfo &track, const QString &mediaPath) const;
    void finishFetch(qint64 generation, const QList<SubtitleEntry> &entries);
    void failFetch(qint64 generation, Failure reason);
    void storeCache(const QString &key, const QList<SubtitleEntry> &entries);
    void appendErrorTail(const QByteArray &chunk);

    QString m_ffmpegPath;
    QProcess *m_process = nullptr; // 在飞转取进程（同时至多一个）
    qint64 m_generation = 0;       // 取数代数：新请求与取消令旧结果静默丢弃
    QString m_requestKey;          // 在飞 / 最近一次取数的身份键，判据见 isCurrentRequest
    QString m_progressLine;        // -progress 输出的半行缓存（\r / \n 都是分隔）
    QByteArray m_errorTail;        // stderr 末段，失败时落日志
    double m_totalSeconds = 0.0;   // ffmpeg input 头的 Duration；解析不出恒 0（不定态）
    QHash<QString, QList<SubtitleEntry>> m_cache;
    QStringList m_cacheOrder; // LRU：最久未用在首
};

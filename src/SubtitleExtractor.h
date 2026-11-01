#pragma once

#include "pch.h"

#include "mpvbridge/MpvController.h"
#include "streams/StreamEndpoint.h"

class StreamRangeServer;

// 正播条目的版本对：缓存键的版本位，文件被替换即键变、旧表自动失效。两位均 -1 = 不可得
// （键随之退化为不含版本位，陈旧由面板刷新兜底）。crypt 由提供者枚举期注入（size 是
// 明文长度、mtime 是密文文件的），本地实体首次读触发单次 stat——都是实体既有惰性口径。
// 跨层值对象，与 FileMeta / PlayTarget 同风格住类外
struct SubtitleEntryVersion {
    qint64 mtimeSecs = -1;
    qint64 bytes = -1;
};

// 取数请求附带的调用方现读态：版本对由主窗随轨道表推送下传、面板保存转交，
// skipCache 只给刷新路径。住类外与 FileMeta / PlayTarget 同风格（跨层值对象）——类内
// 嵌套结构的默认成员初始化器进不了外层成员函数的默认实参
struct SubtitleRequestOptions {
    SubtitleEntryVersion version;
    bool skipCache = false; // 丢弃同键缓存并完整重取
};

// N143：字幕浏览器的取数与解析单模块。同时承担：输入判据、直读 / 转取两条路径的选择、
// ffmpeg 命令行装配、QProcess 异步、结果缓存、取消，以及 srt 文本解析。对外只给
// "条目表 + 失败原因"两种结果，调用方不知道也不关心走了哪条路径——外部 srt
// 文件直读与 ffmpeg 转出的 srt 是同一种文本，这只是实现细节。
//
// 路径判据：外部轨 codec 为 subrip 且扩展名 .srt 且本地可读时
// 直读（不起进程、不需要 ffmpeg），其余一切（内嵌轨、外部 ass / webvtt、容器型外部
// 字幕）转取。缓存的键按路径类型分别定：外部轨不含正播文件——同一个字幕文件换一部片
// 挂，条目表不变，不因换了片而重取。
//
// N145：正播是转接形态（oxbow://）时读不了文件本身，取数输入换成端点解密流上的回环
// Range 服务，ffmpeg 以服务 URL 作 -i。这一支该不该走由调用方判（认得在播条目的只有
// 它），本模块不吃"允许第二条解码流"这一位；转接键附条目版本对，替换即失效。外部轨
// 只看外部字幕文件自身，与正播来源无关。
class SubtitleExtractor : public QObject {
    Q_OBJECT
  public:
    struct SubtitleEntry {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        QString text; // 已清洗；空文本合法（清屏 / 占位块保留）
    };

    // 失败原因（面板映射为可翻译原因串；细节一律落日志不进面板）。
    // SourceUnavailable：转接形态取不了数——URI 解不出身份、端点取不到、起流失败、
    // 服务启动失败
    enum class Failure { NotLocal, FfmpegMissing, FetchFailed, SourceUnavailable };

    explicit SubtitleExtractor(QObject *parent = nullptr);

    // 端点解析通道：列表身份 → 端点（实现由 MainWindow 接 StreamRelay::endpointFor）
    using EndpointResolver = std::function<std::shared_ptr<StreamEndpoint>(const QUuid &)>;

    // ffmpeg 可执行文件路径（已由调用方解析；空 = 不可用，仅影响转取路径）
    void setFfmpegPath(const QString &path) { m_ffmpegPath = path; }

    // 未注入时转接分支一律 SourceUnavailable
    void setEndpointResolver(EndpointResolver resolver);

    // 发起一次取数：track 为当前主选文本字幕轨，mediaPath 为正播文件身份串。
    // 在飞请求先取消（不排队）；缓存命中与直读路径同步发射 finished。
    void start(const MpvController::TrackInfo &track, const QString &mediaPath,
               const SubtitleRequestOptions &options = {});

    // 取消在飞请求，结果静默丢弃；无在飞请求时无操作
    void cancel();

    // 该请求是否已在飞或已出表（身份键与最近一次取数一致）——面板的重取触发点据此
    // 跳过无事可做的重取；失败与取消即作废判据，下一次触发重试
    bool isCurrentRequest(const MpvController::TrackInfo &track, const QString &mediaPath,
                          const SubtitleRequestOptions &options = {}) const;

    // ——判据与解析（static 纯函数，供测试直接覆盖）——

    // 可浏览的文本字幕轨判据：主选（main-selection 0）且非位图轨（image 字段 +
    // select.lua 的 codec 黑名单兜底）
    static bool isBrowsableTextSubtitle(const MpvController::TrackInfo &track);

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
    // 取数请求的身份：键 = 输入身份 + 轨标识 + 修改时间与大小（空 = 判不可用，
    // 归类见 reason）。N145 起输入身份两种：本地文件路径 / 原始转接 URI
    struct Resolved {
        QString key;
        QFileInfo input; // 外部轨为外部字幕文件自身，内嵌轨为正播文件；转接分支无效
        bool external = false;
        // 转接分支（N145）：transit 为真时取数输入是端点解密流（经回环 Range 服务）
        bool transit = false;
        QUuid listUuid;
        QString entryIdentity;
        Failure reason = Failure::NotLocal; // key 为空时的失败归类
    };

    Resolved resolveRequest(const MpvController::TrackInfo &track, const QString &mediaPath,
                            const SubtitleRequestOptions &options) const;
    // 转接分支的发起（start 的三支分发之一）：解析端点 → 起回环服务 → 就绪后喂 URL 给
    // ffmpeg。服务实例的退役由线程结束信号收口
    void startTransitFetch(qint64 generation, const Resolved &resolved, int ffIndex);
    void launchFfmpeg(qint64 generation, const QString &cacheKey, const QString &inputArg,
                      int ffIndex);
    void stopServer();
    void finishFetch(qint64 generation, const QList<SubtitleEntry> &entries);
    void failFetch(qint64 generation, Failure reason);
    void storeCache(const QString &key, const QList<SubtitleEntry> &entries);
    void appendErrorTail(const QByteArray &chunk);

    QString m_ffmpegPath;
    EndpointResolver m_endpointResolver;
    QProcess *m_process = nullptr;         // 在飞转取进程（同时至多一个）
    StreamRangeServer *m_server = nullptr; // 在役回环服务（N145，同时至多一个）
    qint64 m_generation = 0;               // 取数代数：新请求与取消令旧结果静默丢弃
    QString m_requestKey;                  // 在飞 / 最近一次取数的身份键，判据见 isCurrentRequest
    QString m_progressLine;                // -progress 输出的半行缓存（\r / \n 都是分隔）
    QByteArray m_errorTail;                // stderr 末段，失败时落日志
    double m_totalSeconds = 0.0;           // ffmpeg input 头的 Duration；解析不出恒 0（不定态）
    QHash<QString, QList<SubtitleEntry>> m_cache;
    QStringList m_cacheOrder; // LRU：最久未用在首
};

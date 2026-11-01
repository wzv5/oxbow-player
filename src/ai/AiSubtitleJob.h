#pragma once

#include "pch.h"

#include "ai/AiSubtitleCatalog.h"
#include "ai/SrtAssembler.h"

class MpvController;
class LocalLlmServer;
class AiTranslator;
class StreamEndpoint;
class StreamRangeServer;

// N146：点开始那一刻的现读复核结果（判据由主窗自证）——error 非空即不可发车
// （原因走 catalog 文案），identity 为捕获口 N61 现值，displayName 给当前文件行
struct AiSubtitleStartContext {
    QString error;
    QString identity;
    QString displayName;
};

// N146：AI 字幕的单次生成过程——步骤机（八段）、ffmpeg/ASR/llama-server 三个子进程、
// 转接来源的回环桥、mpv 接线（sub-add / sub-reload + 500ms 合并节流）、缓存入库与
// 临时文件清理。主窗持有，对话框只是视图；一次运行内部串行（识别与翻译不并行）。
//
// 取消与代数（SubtitleExtractor 同款形状）：代数自增令迟到回调静默丢弃；收口是
// 终态与取消共用的同一条路（杀子进程、停回环桥、停工人线程、停节流、删 wav）。
// 终态三选一：succeeded / failed(reason)（reason 为 catalog 文案，key 不进串）/
// canceled。临时 srt 在整个会话里不删（mpv 那条外部字幕轨还指着它）；wav 用完即删。
// 缓存只在相应阶段成功时写（原文在 ASR 成功后、译文在翻译成功后），写失败静默
// （只落日志，不弹提示、不连带播放）。
class AiSubtitleJob : public QObject {
    Q_OBJECT
  public:
    // 过程线（"自检"不在其中——那是对话框打开时的一次性判断）
    enum class Step {
        WaitingToStart,
        ExtractingAudio,
        InitializingSpeechRecognition,
        SpeechRecognition,
        InitializingTranslation,
        Translation,
        Completed,
    };

    // 一次运行的输入：自检时定格的八项设置快照（读法与缺省归 catalog 单点）+
    // 点开始时的"启用翻译"勾选
    struct StartParams {
        AiSubtitleCatalog::Settings settings;
        bool enableTranslation = false;
    };

    explicit AiSubtitleJob(MpvController *controller, QObject *parent = nullptr);

    // 端点解析与 N145 同源（主窗接 StreamRelay::endpointFor）；ffmpeg 路径现取
    // （主窗 resolvedFfmpegPath 的出口，选择逻辑住 ExternalTool）
    void
    setEndpointResolver(std::function<std::shared_ptr<StreamEndpoint>(const QUuid &)> resolver);
    void setFfmpegPathProvider(std::function<QString()> provider);

    // 发车（主窗已复核在播条目；identity 为捕获口 N61 出口的现值）。
    // running 中再调属调用方违约（Debug 断言）
    void start(const StartParams &params, const QString &playingIdentity);
    // 取消（点取消 / 关闭对话框 / 退出哨同一条路）：代数自增 + 全链收口
    void cancel();
    // 终态确认后的复位：回"等待开始"、进度清零；临时 srt 与缓存件都不动
    void reset();

    bool isRunning() const { return m_running; }

  signals:
    void stepChanged(AiSubtitleJob::Step step);
    void progressChanged(double ratio); // -1 = 不定形态（本段无百分比判据）
    void rateLimitWaiting(int waitMs);  // 429 等待期（步骤仍停在翻译）
    void succeeded();
    void failed(const QString &reason);
    void canceled();

  private:
    void setStep(Step step);
    double progressForStep(Step step) const; // 八档进度的唯一判据（视图只照收不猜）
    void startExtraction();
    void launchFfmpeg(const QString &ffmpeg, const QString &inputArg, int ffIndex);
    void startAsr();
    void adoptAssemblerTable();
    void startTranslationStage();
    void runTranslator(const QString &endpoint);
    void markDirty();
    bool stillPlayingCaptured() const; // 在播条目仍是本次捕获那一条（挂接的前提）
    void flushToMpv();
    void reloadByExternalFilename();
    void complete();
    void fail(const QString &reason);
    void teardown();
    void removeWav();
    QString cacheDir() const;
    bool writeFileAtomic(const QString &path, const QString &content);
    void writeCacheFile(const QString &path, const QString &content);
    double asrProgress() const;

    MpvController *m_controller;
    std::function<std::shared_ptr<StreamEndpoint>(const QUuid &)> m_endpointResolver;
    std::function<QString()> m_ffmpegPath;

    qint64 m_generation = 0; // 取消代数：迟到回调按代数丢弃
    Step m_step = Step::WaitingToStart;
    bool m_running = false;

    StartParams m_params;
    QString m_identity; // 本次捕获的在播身份串（比对与缓存基名共用）
    QString m_wavPath;
    QString m_srtPath;
    double m_durationSeconds = 0.0; // 启动时一次读定；≤0 视为不定形态
    int m_totalSentences = 0;       // 翻译段的总句数（ASR 成功时的表长）
    int m_translatedCount = 0;      // 翻译段已完句数（含空句）

    QProcess *m_ffmpeg = nullptr;
    QProcess *m_asr = nullptr;
    StreamRangeServer *m_rangeServer = nullptr; // 提取段的回环桥（N145 形态，一次一个）
    LocalLlmServer *m_llmServer = nullptr;      // 翻译段的内置服务端（一次一个）
    SrtAssembler m_assembler;
    QByteArray m_asrErrorTail;

    QList<SrtAssembler::Entry> m_table; // 唯一权威曲目表（GUI 线程内改）
    QTimer m_mergeTimer;                // 500ms 冷却（单发；前沿立即落地）
    bool m_dirty = false;
    bool m_subAdded = false;         // 已 sub-add 成功；此后一律 sub-reload
    bool m_skipWiringLogged = false; // 在播条目已换、挂接被跳过——日志只留一条

    QThread *m_translateThread = nullptr;
    AiTranslator *m_translator = nullptr;
};

#include "pch.h"

#include "AiSubtitleJob.h"

#include "AiSubtitleCatalog.h"
#include "AiTranslator.h"
#include "AppPaths.h"
#include "LocalLlmServer.h"
#include "Logging.h"
#include "Util.h"
#include "mpvbridge/MpvController.h"
#include "streams/StreamRangeServer.h"
#include "streams/StreamTransit.h"

namespace {

// 失败时落日志的 stderr 末段长度（SubtitleExtractor 同量级）
constexpr int kAsrErrorTailBytes = 2048;

// kill 之后有界等待：本次 wav 的句柄还攥在子进程手里时删不掉（Windows 上子进程
// 不带 FILE_SHARE_DELETE 打开文件），"用完即删"必须落在句柄已关的那一拍
constexpr int kProcessKillWaitMs = 2000;

} // namespace

AiSubtitleJob::AiSubtitleJob(MpvController *controller, QObject *parent)
    : QObject(parent), m_controller(controller) {
    m_mergeTimer.setSingleShot(true);
    connect(&m_mergeTimer, &QTimer::timeout, this, [this] {
        if (!m_running)
            return;
        if (m_dirty) {
            m_dirty = false;
            flushToMpv();
            m_mergeTimer.start(AiSubtitleCatalog::kMergeIntervalMs); // 冷却重启，防长尾饿死
        }
    });
}

void AiSubtitleJob::setEndpointResolver(
    std::function<std::shared_ptr<StreamEndpoint>(const QUuid &)> resolver) {
    m_endpointResolver = std::move(resolver);
}

void AiSubtitleJob::setFfmpegPathProvider(std::function<QString()> provider) {
    m_ffmpegPath = std::move(provider);
}

void AiSubtitleJob::start(const StartParams &params, const QString &playingIdentity) {
    Q_ASSERT(!m_running);
    ++m_generation;
    m_params = params;
    m_identity = playingIdentity;
    m_running = true;
    m_subAdded = false;
    m_dirty = false;
    m_table.clear();
    m_assembler.clear();
    m_asrErrorTail.clear();
    m_durationSeconds = m_controller->propertyDouble(QStringLiteral("duration"), 0.0);
    m_totalSentences = 0;
    m_translatedCount = 0;
    m_skipWiringLogged = false;

    // 临时文件：系统临时目录下不建子目录，随机 token 让同会话重复运行不覆盖仍被
    // mpv 引用的旧 srt
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_wavPath = QDir::tempPath() + QStringLiteral("/%1.wav").arg(token);
    m_srtPath = QDir::tempPath() + QStringLiteral("/%1.srt").arg(token);

    setStep(Step::ExtractingAudio);
    startExtraction();
}

void AiSubtitleJob::cancel() {
    if (!m_running)
        return;
    ++m_generation; // 迟到的回调（含工人线程的 canceled）一律按代数丢弃
    m_running = false;
    teardown();
    emit canceled();
}

void AiSubtitleJob::reset() {
    Q_ASSERT(!m_running);
    m_table.clear();
    setStep(Step::WaitingToStart);
}

void AiSubtitleJob::setStep(Step step) {
    m_step = step;
    emit stepChanged(step);
    emit progressChanged(progressForStep(step));
}

// 八档进度的唯一判据，视图照收不猜：等待开始与完成是定值，提取与
// 两段初始化没有百分比意义（不定形态），识别看已解出曲目的最新终点、翻译看已完句数
double AiSubtitleJob::progressForStep(Step step) const {
    switch (step) {
    case Step::WaitingToStart:
        return 0.0;
    case Step::Completed:
        return 1.0;
    case Step::SpeechRecognition:
        return asrProgress();
    case Step::Translation:
        return m_totalSentences > 0 ? double(m_translatedCount) / m_totalSentences : -1.0;
    case Step::ExtractingAudio:
    case Step::InitializingSpeechRecognition:
    case Step::InitializingTranslation:
        break;
    }
    return -1.0;
}

double AiSubtitleJob::asrProgress() const {
    if (m_durationSeconds <= 0 || m_table.isEmpty())
        return -1.0;
    // 判据：已解出曲目里最新的终点时间戳 / duration（缺失终点按起点计），钳 [0,1]
    const SrtAssembler::Entry &latest = m_table.constLast();
    const double end = latest.endMissing ? latest.startSeconds : latest.endSeconds;
    return qBound(0.0, end / m_durationSeconds, 1.0);
}

void AiSubtitleJob::startExtraction() {
    // 音轨对齐：不依赖 ffmpeg 的缺省选流（多轨时按声道数挑，未必是用户在听的那条）
    int ffIndex = -1;
    const QList<MpvController::TrackInfo> tracks = m_controller->readTracks();
    for (const MpvController::TrackInfo &track : tracks) {
        if (track.type == QLatin1String("audio") && track.selected) {
            ffIndex = track.ffIndex;
            break;
        }
    }
    if (ffIndex < 0) {
        fail(AiSubtitleCatalog::reasonNoAudioTrack());
        return;
    }
    const QString ffmpeg = m_ffmpegPath ? m_ffmpegPath() : QString();
    if (ffmpeg.isEmpty()) {
        fail(AiSubtitleCatalog::reasonFfmpegMissing());
        return;
    }

    if (StreamTransit::isTransitUri(m_identity)) {
        // 转接来源：端点解密流上的回环 Range 桥（N145 同一来源与寿命契约）
        const QUuid listUuid = StreamTransit::listUuidOfUri(m_identity);
        const QString entryIdentity = StreamTransit::unwrap(m_identity);
        if (listUuid.isNull() || entryIdentity.isEmpty() || !m_endpointResolver) {
            OXBOW_LOG_WARNING() << "AiSubtitleJob: transit identity unusable for extraction";
            fail(AiSubtitleCatalog::reasonAudioExtractionNotSupported());
            return;
        }
        const auto endpoint = m_endpointResolver(listUuid);
        if (!endpoint) {
            OXBOW_LOG_WARNING() << "AiSubtitleJob: no endpoint for transit list";
            fail(AiSubtitleCatalog::reasonAudioExtractionNotSupported());
            return;
        }
        const qint64 generation = m_generation;
        auto *server = new StreamRangeServer(this);
        connect(server, &QThread::finished, this, [this, server] {
            if (m_rangeServer == server)
                m_rangeServer = nullptr;
            server->deleteLater();
        });
        connect(server, &StreamRangeServer::ready, this,
                [this, generation, ffmpeg, ffIndex](const QUrl &url, qint64) {
                    if (generation != m_generation)
                        return;
                    launchFfmpeg(ffmpeg, url.toString(), ffIndex);
                });
        connect(server, &StreamRangeServer::failed, this, [this, generation](const QString &why) {
            if (generation != m_generation)
                return;
            OXBOW_LOG_WARNING().noquote() << "AiSubtitleJob: range server failed:" << why;
            fail(AiSubtitleCatalog::reasonAudioExtractionFailed(why));
        });
        m_rangeServer = server;
        server->launch(endpoint, entryIdentity);
        return;
    }
    launchFfmpeg(ffmpeg, m_identity, ffIndex);
}

void AiSubtitleJob::launchFfmpeg(const QString &ffmpeg, const QString &inputArg, int ffIndex) {
    const qint64 generation = m_generation;
    QProcess *process = new QProcess(this);
    m_ffmpeg = process;
    // 16 kHz 单声道 PCM 是两类本地 ASR 的通用进料口径；-map 定向当前在播音轨
    // （桥喂给 ffmpeg 的是整份明文容器，流下标与本地一致）；-nostdin 防止 ffmpeg
    // 偷动 mpv 的标准输入。highpass 去低频隆隆声：低频对语音几乎无信息量却常触发
    // 误识别；不套低通——擦音能量在 4-10 kHz，砍掉是 ASR 负优化
    QStringList args{QStringLiteral("-v"),
                     QStringLiteral("error"),
                     QStringLiteral("-hide_banner"),
                     QStringLiteral("-nostdin"),
                     QStringLiteral("-y"),
                     QStringLiteral("-i"),
                     inputArg,
                     QStringLiteral("-map"),
                     QStringLiteral("0:%1").arg(ffIndex),
                     QStringLiteral("-vn"),
                     QStringLiteral("-ac"),
                     QStringLiteral("1"),
                     QStringLiteral("-ar"),
                     QStringLiteral("16000"),
                     QStringLiteral("-af"),
                     QStringLiteral("highpass=f=80"),
                     QStringLiteral("-c:a"),
                     QStringLiteral("pcm_s16le"),
                     QStringLiteral("-f"),
                     QStringLiteral("wav"),
                     m_wavPath};
    connect(process, &QProcess::errorOccurred, this,
            [this, generation, process](QProcess::ProcessError error) {
                if (generation != m_generation || error != QProcess::FailedToStart)
                    return;
                // 路径已解析非空却起不来，与"路径解析为空"不是同一种失败：原因走
                // 提取段的透传（系统错误文案进日志与串），不借用 FFmpeg not found
                OXBOW_LOG_WARNING()
                    << "AiSubtitleJob: ffmpeg failed to start:" << process->errorString();
                fail(AiSubtitleCatalog::reasonAudioExtractionFailed(process->errorString()));
            });
    connect(process, &QProcess::finished, this,
            [this, generation, process](int exitCode, QProcess::ExitStatus) {
                if (m_ffmpeg == process)
                    m_ffmpeg = nullptr;
                // ffmpeg 退出即成败已定：桥就地收口（成功 / 失败 / 取消同一条），
                // 监听最迟一拍关闭，不留挂着的临时端口
                if (m_rangeServer) {
                    m_rangeServer->stop();
                    m_rangeServer = nullptr;
                }
                const QByteArray tail = process->readAllStandardError();
                process->deleteLater();
                if (generation != m_generation)
                    return;
                if (exitCode != 0) {
                    OXBOW_LOG_WARNING().noquote() << "AiSubtitleJob: ffmpeg exited" << exitCode
                                                  << QString::fromUtf8(tail).simplified();
                    removeWav(); // 未起 ASR：残件立即删
                    fail(AiSubtitleCatalog::reasonAudioExtractionFailed(
                        QString::fromUtf8(tail).simplified()));
                    return;
                }
                startAsr();
            });
    process->start(ffmpeg, args);
}

void AiSubtitleJob::startAsr() {
    // 初始化与识别是同一个 ASR 子进程的两个阶段：start 成功即"初始化"，首个
    // 解析入表的 srt 块到达即切"识别"；内置档的模型加载落在这一段
    setStep(Step::InitializingSpeechRecognition);

    const QString command = AiSubtitleCatalog::selectAsrCommand(m_params.settings.asrMode,
                                                                m_params.settings.asrCommand);
    QStringList args = AiSubtitleCatalog::splitCommandLine(command);
    if (args.isEmpty()) {
        // 拆不出实参即命令行是空串（自定义档空串已被自检挡住，够得到的只有被手改坏
        // 的 ini 档位）；细节只进日志
        OXBOW_LOG_WARNING().noquote() << "AiSubtitleJob: empty ASR command line:" << command;
        fail(AiSubtitleCatalog::reasonAsrCommandInvalid());
        return;
    }
    const QString program =
        AiSubtitleCatalog::resolvePluginProgramPath(App::paths().pluginDir(), args.takeFirst());
    args = AiSubtitleCatalog::expandArg(args, QStringLiteral("${wav_file}"), m_wavPath);

    const qint64 generation = m_generation;
    QProcess *process = new QProcess(this);
    m_asr = process;
    process->setWorkingDirectory(App::paths().pluginDir());
    connect(process, &QProcess::readyReadStandardOutput, this, [this, generation, process] {
        if (generation != m_generation)
            return;
        m_assembler.feed(process->readAllStandardOutput());
        adoptAssemblerTable();
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, generation, process] {
        if (generation != m_generation)
            return;
        m_asrErrorTail += process->readAllStandardError();
        if (m_asrErrorTail.size() > kAsrErrorTailBytes)
            m_asrErrorTail.remove(0, m_asrErrorTail.size() - kAsrErrorTailBytes);
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, generation, process](QProcess::ProcessError error) {
                if (generation != m_generation || error != QProcess::FailedToStart)
                    return;
                OXBOW_LOG_WARNING()
                    << "AiSubtitleJob: ASR failed to start:" << process->errorString();
                removeWav();
                fail(AiSubtitleCatalog::reasonAsrStartFailed(process->errorString()));
            });
    connect(process, &QProcess::finished, this,
            [this, generation, process](int exitCode, QProcess::ExitStatus) {
                if (m_asr == process)
                    m_asr = nullptr;
                m_assembler.finish(); // 残余尾巴（半行 / 未封口末块）再尝试一次
                adoptAssemblerTable();
                const QByteArray tail = process->readAllStandardError();
                process->deleteLater();
                removeWav(); // 用完即删
                if (generation != m_generation)
                    return;
                if (exitCode != 0) {
                    OXBOW_LOG_WARNING().noquote() << "AiSubtitleJob: ASR exited" << exitCode
                                                  << QString::fromUtf8(tail).simplified();
                    fail(AiSubtitleCatalog::reasonAsrFailed(QString::fromUtf8(tail).simplified()));
                    return;
                }
                if (m_table.isEmpty()) {
                    fail(AiSubtitleCatalog::reasonAsrNoOutput());
                    return;
                }
                // ASR 成功：原文缓存立即入库（写失败静默，只落日志）
                writeCacheFile(cacheDir() + QStringLiteral("/%1.srt").arg(
                                                AiSubtitleCatalog::cacheBaseName(m_identity)),
                               SrtAssembler::renderSrt(m_table));
                if (!m_params.enableTranslation) {
                    complete();
                    return;
                }
                startTranslationStage();
            });
    process->start(program, args);
}

void AiSubtitleJob::adoptAssemblerTable() {
    const QList<SrtAssembler::Entry> &entries = m_assembler.entries();
    if (entries.isEmpty())
        return;
    m_table = entries;
    if (m_step == Step::InitializingSpeechRecognition)
        setStep(Step::SpeechRecognition); // 首个块入表即切"识别"（同进程，不重启）
    else
        emit progressChanged(asrProgress());
    markDirty(); // 实时挂上当前字幕：经 500ms 合并节流推给 mpv
}

void AiSubtitleJob::startTranslationStage() {
    m_totalSentences = m_table.size();
    m_translatedCount = 0;
    const qint64 generation = m_generation;
    if (m_params.settings.translationMode == QLatin1String(AiSubtitleCatalog::kModeBuiltin)) {
        // 内置：拉起本地服务端并等它报端口（"初始化翻译"段）；自定义档没有本地进程
        // 可等，直接进"翻译"
        setStep(Step::InitializingTranslation);
        m_llmServer = new LocalLlmServer(this);
        connect(m_llmServer, &LocalLlmServer::ready, this, [this, generation](quint16 port) {
            if (generation != m_generation)
                return;
            runTranslator(QStringLiteral("http://127.0.0.1:%1/v1").arg(port));
        });
        connect(m_llmServer, &LocalLlmServer::failed, this, [this, generation] {
            if (generation == m_generation)
                fail(AiSubtitleCatalog::reasonServerStartFailed());
        });
        connect(m_llmServer, &LocalLlmServer::exited, this, [this, generation] {
            // 翻译期间服务端非预期退出：立即终止整条翻译，不再逐句发请求
            if (generation == m_generation && m_step == Step::Translation)
                fail(AiSubtitleCatalog::reasonServerExitedUnexpectedly());
        });
        m_llmServer->start(AiSubtitleCatalog::builtinServerCommand(), App::paths().pluginDir());
        return;
    }
    runTranslator(m_params.settings.translationApi);
}

void AiSubtitleJob::runTranslator(const QString &endpoint) {
    QStringList sentences;
    sentences.reserve(m_table.size());
    for (const SrtAssembler::Entry &entry : m_table)
        sentences << entry.text;

    setStep(Step::Translation);
    m_translateThread = new QThread(this);
    m_translator = new AiTranslator();
    m_translator->moveToThread(m_translateThread);
    connect(m_translateThread, &QThread::finished, m_translator, &QObject::deleteLater);

    const qint64 generation = m_generation;
    connect(m_translator, &AiTranslator::sentenceTranslated, this,
            [this, generation](int index, const QString &text) {
                if (generation != m_generation || index < 0 || index >= m_table.size())
                    return;
                m_table[index].text = text;
                ++m_translatedCount;
                markDirty(); // 译文逐句推进：同一条节流通道
                emit progressChanged(double(m_translatedCount) / m_totalSentences);
            });
    connect(m_translator, &AiTranslator::rateLimitWaiting, this, [this, generation](int waitMs) {
        if (generation == m_generation)
            emit rateLimitWaiting(waitMs);
    });
    connect(m_translator, &AiTranslator::succeeded, this, [this, generation] {
        if (generation != m_generation)
            return;
        // 翻译成功：译文件入库（短名取长名→短名映射；原文缓存已在 ASR 成功时写）
        writeCacheFile(cacheDir() + QStringLiteral("/%1.%2.srt")
                                        .arg(AiSubtitleCatalog::cacheBaseName(m_identity),
                                             AiSubtitleCatalog::shortLanguageName(
                                                 m_params.settings.translationTargetLang)),
                       SrtAssembler::renderSrt(m_table));
        complete();
    });
    connect(m_translator, &AiTranslator::failed, this, [this, generation](const QString &reason) {
        if (generation != m_generation)
            return;
        // 翻译失败只停翻译步骤：不动 mpv 的字幕配置，临时 srt 保留已译出的部分，
        // 画面上的半译结果就此定格（teardown 停节流、不再推更新），原文缓存保留
        fail(reason);
    });
    // 工人线程的 canceled 不接线：用户取消经 cancel() 已代数自增，迟到回调本就无人收

    m_translateThread->start();
    QMetaObject::invokeMethod(m_translator, "run", Qt::QueuedConnection, Q_ARG(QString, endpoint),
                              Q_ARG(QString, m_params.settings.translationApiKey),
                              Q_ARG(QString, m_params.settings.translationModel),
                              Q_ARG(QString, m_params.settings.translationPrompt),
                              Q_ARG(QString, m_params.settings.translationTargetLang),
                              Q_ARG(QStringList, sentences));
}

void AiSubtitleJob::markDirty() {
    if (!m_running)
        return;
    m_dirty = true;
    if (m_mergeTimer.isActive())
        return;
    // 前沿立即落地 + 500ms 冷却：爆发合并成一次落地，且不至于被连续输出饿死
    m_dirty = false;
    flushToMpv();
    m_mergeTimer.start(AiSubtitleCatalog::kMergeIntervalMs);
}

bool AiSubtitleJob::stillPlayingCaptured() const {
    // 比对按实体身份口径（回显不可信，先整形再比）：本地两侧各落一次
    // Util::Url::key（相对→绝对、分隔符归一、Windows 转小写），吃掉同一文件的不同写法；
    // 转接串不能落 key——它首行就 unwrap，会把列表身份抹平成"另一册的同路径条目"，
    // 而 N61 早判过那种合并是假一致。转接按完整 oxbow:// 串逐字节比（捕获口与本出口
    // 同一条整形链，逐字节即同一条）
    const QString current = m_controller->currentPlayingPath();
    if (StreamTransit::isTransitUri(m_identity))
        return current == m_identity;
    return Util::Url::key(current) == Util::Url::key(m_identity);
}

void AiSubtitleJob::flushToMpv() {
    if (m_table.isEmpty())
        return;
    // 只在播文件仍是本次捕获的那一条时才挂 / 重载：用户中途切了别的文件，
    // sub-add 与 sub-reload 全部跳过，识别与翻译照旧跑完；切回来也不由插件补挂，
    // 挂不挂看 mpv 自己（缓存目录里那份归 sub-auto）
    if (!stillPlayingCaptured()) {
        if (!m_skipWiringLogged) {
            m_skipWiringLogged = true;
            OXBOW_LOG_INFO() << "AiSubtitleJob: playing item changed, subtitle wiring skipped";
        }
        return;
    }
    if (!writeFileAtomic(m_srtPath, SrtAssembler::renderSrt(m_table))) {
        OXBOW_LOG_WARNING() << "AiSubtitleJob: failed to write live srt" << m_srtPath;
        return;
    }
    if (!m_subAdded) {
        // 首次有曲目可呈现：sub-add（默认 select，加完即当前字幕）
        const int err = m_controller->command(
            QStringList{QStringLiteral("sub-add"), QDir::toNativeSeparators(m_srtPath)});
        if (err == 0)
            m_subAdded = true;
        else
            OXBOW_LOG_WARNING() << "AiSubtitleJob: sub-add failed:" << err;
        return;
    }
    reloadByExternalFilename();
}

void AiSubtitleJob::reloadByExternalFilename() {
    // 轨 id 每次现查：sub-reload 对 id 是否保持不变没有任何承诺，按 external-filename
    // 反查是唯一稳的形态；认不到就什么都不做，绝不去猜一条别的轨
    const QString nativeSrt = QDir::toNativeSeparators(m_srtPath);
    const QList<MpvController::TrackInfo> tracks = m_controller->readTracks();
    for (const MpvController::TrackInfo &track : tracks) {
        if (track.type != QLatin1String("sub") || !track.external)
            continue;
        // 回显不可信：正斜杠与原生分隔符两种形态都认（同源同一条路径）
        if (track.externalFilename.compare(nativeSrt, Qt::CaseInsensitive) != 0 &&
            track.externalFilename != m_srtPath)
            continue;
        m_controller->command(QStringList{QStringLiteral("sub-reload"), QString::number(track.id)});
        return;
    }
}

void AiSubtitleJob::complete() {
    if (!m_running)
        return; // 终态只落一次：跨 run 的迟到回调由代数挡，同 run 的二次终态由这道守卫挡
    m_running = false;
    // 最后一拍：仍挂在 500ms 合并窗口里的尾巴在 teardown 之前落地——成功收尾的内容
    // 必须到 mpv（失败与取消维持"定格在最后一次落地"，不补这一拍）
    if (m_dirty) {
        m_dirty = false;
        flushToMpv();
    }
    teardown();
    setStep(Step::Completed); // 只有成功收尾才进入
    emit succeeded();
}

void AiSubtitleJob::fail(const QString &reason) {
    if (!m_running)
        return;
    m_running = false;
    teardown(); // 步骤行停在出事的那一段（m_step 不动）
    emit failed(reason);
}

void AiSubtitleJob::teardown() {
    m_mergeTimer.stop();
    m_dirty = false;
    // 两个子进程同一条收口：kill 之后必须等有界到真退出——本次 wav 的句柄还攥在它
    // 手里时删不掉，只落一个共享冲突、残件留在临时目录
    for (QProcess *process : {std::exchange(m_ffmpeg, nullptr), std::exchange(m_asr, nullptr)}) {
        if (!process)
            continue;
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(kProcessKillWaitMs);
        }
        process->deleteLater();
    }
    if (m_rangeServer) {
        StreamRangeServer *server = m_rangeServer;
        m_rangeServer = nullptr;
        server->disconnect(this);
        server->stop(); // 非阻塞：置取消旗 + 流 cancel；线程退出后经 finished 钩子自毁
    }
    if (m_llmServer) {
        // 服务端不持本次临时文件，收杀照 N143 形态异步（不等退出，GUI 不悬）
        LocalLlmServer *server = m_llmServer;
        m_llmServer = nullptr;
        server->disconnect(this);
        server->stop();
        server->deleteLater();
    }
    if (m_translateThread) {
        QThread *thread = m_translateThread;
        AiTranslator *translator = m_translator;
        m_translateThread = nullptr;
        m_translator = nullptr;
        if (translator)
            translator->requestCancel(); // 直呼：旗 + 在飞 abort（任意线程可调）
        thread->quit();                  // run 返回后事件循环收口；wait 有界，不悬死 GUI
        if (!thread->wait(5000))
            OXBOW_LOG_WARNING() << "AiSubtitleJob: translator thread did not stop in time";
        thread->deleteLater(); // translator 随 finished 钩子自毁
    }
    removeWav();
}

void AiSubtitleJob::removeWav() {
    if (m_wavPath.isEmpty())
        return;
    if (QFile::exists(m_wavPath) && !QFile::remove(m_wavPath))
        OXBOW_LOG_WARNING() << "AiSubtitleJob: failed to remove temp wav" << m_wavPath;
}

QString AiSubtitleJob::cacheDir() const {
    return App::paths().iniDir() + QStringLiteral("/subtitles");
}

bool AiSubtitleJob::writeFileAtomic(const QString &path, const QString &content) {
    // 临时名 + 原子替换（QSaveFile 的 commit 即 rename-replace）：mpv 持有句柄期间
    // （FILE_SHARE_DELETE）同样成立，不让半包落到最终文件名上
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(content.toUtf8()) != content.toUtf8().size())
        return false;
    return file.commit();
}

void AiSubtitleJob::writeCacheFile(const QString &path, const QString &content) {
    // 缓存目录挂在 iniDir 下：只有该子目录本身建不成或写不进时才"本轮不写缓存"，
    // 静默失败（只写日志，不弹提示、不连带播放；不再另找第三处回落）
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        OXBOW_LOG_WARNING() << "AiSubtitleJob: cache directory unavailable, skip" << path;
        return;
    }
    if (!writeFileAtomic(path, content))
        OXBOW_LOG_WARNING() << "AiSubtitleJob: cache write failed, skip" << path;
}

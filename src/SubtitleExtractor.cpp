#include "pch.h"

#include "SubtitleExtractor.h"

#include "ExternalTool.h"
#include "Logging.h"
#include "Util.h"
#include "streams/StreamRangeServer.h"
#include "streams/StreamTransit.h"

namespace {

constexpr int kCacheLimit = 8;        // 缓存份数上限（LRU 淘汰最久未用）
constexpr int kErrorTailBytes = 2048; // 失败时落日志的 stderr 末段长度

// select.lua 同款 codec 黑名单，位图轨判据的兜底——image 字段为主判据，这里只
// 覆盖不报 image 字段的旧情形
bool isBitmapSubtitleCodec(const QString &codec) {
    return codec == QLatin1String("dvd_subtitle") || codec == QLatin1String("hdmv_pgs_subtitle");
}

double parseSrtClock(const QString &h, const QString &m, const QString &s, const QString &frac) {
    // 毫秒段右补齐到三位：",5" 即 500ms
    const int ms = int(frac.rightJustified(3, u'0').toUInt());
    return h.toDouble() * 3600.0 + m.toDouble() * 60.0 + s.toDouble() + ms / 1000.0;
}

// -progress 的 out_time 串（HH:MM:SS.ffffff）→ 秒；不可解析返 -1
double parseProgressClock(const QString &clock) {
    const QStringList parts = clock.split(u':');
    if (parts.size() != 3)
        return -1.0;
    bool ok = false;
    const double seconds = parts.at(2).toDouble(&ok);
    if (!ok)
        return -1.0;
    return parts.at(0).toDouble() * 3600.0 + parts.at(1).toDouble() * 60.0 + seconds;
}

// input 头的 Duration 串（HH:MM:SS.mm，取到逗号前）→ 秒；N/A 等解析不出返 -1
double parseDurationClock(const QString &line) {
    static const QRegularExpression clock(
        QStringLiteral("Duration:\\s*(\\d{1,2}:\\d{2}:\\d{2}[.,]\\d+)"));
    const QRegularExpressionMatch match = clock.match(line);
    if (!match.hasMatch())
        return -1.0;
    QString value = match.captured(1);
    value.replace(u',', u'.');
    return parseProgressClock(value);
}

} // namespace

SubtitleExtractor::SubtitleExtractor(QObject *parent) : QObject(parent) {}

bool SubtitleExtractor::isBrowsableTextSubtitle(const MpvController::TrackInfo &track) {
    return track.type == QLatin1String("sub") && track.mainSelection == 0 && !track.image &&
           !isBitmapSubtitleCodec(track.codec);
}

QString SubtitleExtractor::resolveFfmpegPath(const QString &configured, const QString &configDir) {
    if (!configured.trimmed().isEmpty())
        return configured.trimmed();
    const QStringList candidates =
        externalToolCandidates(externalToolSpec(ExternalTool::Ffmpeg), configDir);
    return candidates.isEmpty() ? QString() : candidates.first();
}

QString SubtitleExtractor::decodeText(const QByteArray &bytes, bool *ok) {
    if (ok)
        *ok = true;
    // BOM 优先，剥掉后不进文本
    if (bytes.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(bytes.mid(3));
    if (bytes.startsWith("\xFF\xFE"))
        return QString::fromUtf16(reinterpret_cast<const char16_t *>(bytes.constData() + 2),
                                  (bytes.size() - 2) / 2);
    if (bytes.startsWith("\xFE\xFF")) {
        QByteArray swapped = bytes.mid(2);
        for (int i = 0; i + 1 < swapped.size(); i += 2)
            std::swap(swapped[i], swapped[i + 1]);
        return QString::fromUtf16(reinterpret_cast<const char16_t *>(swapped.constData()),
                                  swapped.size() / 2);
    }
    // 无 BOM：按 UTF-8 严格解码，失败回落系统本地编码（GB18030 系；非中文系统上
    // 会把 GB18030 解成"成功"的乱码，与 mpv 同病，固有取舍）
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString text = decoder.decode(bytes);
    if (!decoder.hasError())
        return text;
    return QString::fromLocal8Bit(bytes);
}

QString SubtitleExtractor::cleanEntryText(QString text) {
    static const QRegularExpression htmlTag(QStringLiteral("<[^>]*>"));
    static const QRegularExpression assOverride(QStringLiteral("\\{\\\\[^}]*\\}"));
    text.remove(htmlTag);
    text.remove(assOverride);
    text.replace(QLatin1String("\\h"), QLatin1String(" "));
    text.replace(QLatin1String("\\N"), QLatin1String(" "));
    text.replace(QLatin1String("\\n"), QLatin1String(" "));
    return text;
}

QList<SubtitleExtractor::SubtitleEntry> SubtitleExtractor::parseSrt(const QString &text) {
    // 起点必配、终点可缺（缺者回落下一块起点）；, 与 . 毫秒分隔都接受
    static const QRegularExpression timeLine(
        QStringLiteral("^(\\d{1,3}):(\\d{1,2}):(\\d{1,2})[,.](\\d{1,3})\\s*-->\\s*"
                       "(?:(\\d{1,3}):(\\d{1,2}):(\\d{1,2})[,.](\\d{1,3}))?$"));

    struct RawEntry {
        double start = 0.0;
        double end = 0.0;
        bool endMissing = false;
        QStringList textLines;
    };

    // 逐行扫描：时间行自身即块边界（块间漏空行也能切），其前的行号行只当分隔、
    // 一律忽略（真实素材里错号、缺号都常见）；空行封住当前块的文本，封口后的非
    // 时间行视作下一块的行号杂料
    QList<RawEntry> raws;
    RawEntry current;
    bool open = false;
    bool textClosed = false;
    auto flushCurrent = [&raws, &current, &open, &textClosed]() {
        if (open) {
            // 畸形块（end 早于 start）整块丢弃，不半截入表；end == start 合法，
            // end 缺失的块保留（回落下一块起点，见排序后的补齐）
            if (current.endMissing || current.end >= current.start)
                raws.append(current);
        }
        open = false;
        current = RawEntry();
        textClosed = false;
    };
    for (const QString &line : text.split(u'\n')) {
        const QString trimmed = line.trimmed();
        const QRegularExpressionMatch match = timeLine.match(trimmed);
        if (match.hasMatch()) {
            flushCurrent();
            current.start = parseSrtClock(match.captured(1), match.captured(2), match.captured(3),
                                          match.captured(4));
            if (match.captured(5).isEmpty()) {
                current.endMissing = true;
            } else {
                current.end = parseSrtClock(match.captured(5), match.captured(6), match.captured(7),
                                            match.captured(8));
            }
            open = true;
            continue;
        }
        if (!open)
            continue; // 首个时间行之前：行号行或碎料
        if (trimmed.isEmpty()) {
            textClosed = true;
            continue;
        }
        if (textClosed)
            continue;
        const QString cleaned = cleanEntryText(trimmed);
        if (!cleaned.isEmpty())
            current.textLines << cleaned; // 行内换行归一为空格；清洗后空行不占位
    }
    flushCurrent();

    // 按起点稳定排序（源可能乱序）；时间重叠不去重，都列出
    std::stable_sort(raws.begin(), raws.end(),
                     [](const RawEntry &a, const RawEntry &b) { return a.start < b.start; });

    QList<SubtitleEntry> entries;
    entries.reserve(raws.size());
    for (int i = 0; i < raws.size(); ++i) {
        SubtitleEntry entry;
        entry.startSeconds = raws.at(i).start;
        // end 缺失回落下一块起点，末块无上界
        entry.endSeconds = raws.at(i).endMissing
                               ? (i + 1 < raws.size() ? raws.at(i + 1).start
                                                      : std::numeric_limits<double>::infinity())
                               : raws.at(i).end;
        entry.text = raws.at(i).textLines.join(u' ');
        entries.append(entry);
    }
    return entries;
}

SubtitleExtractor::Resolved
SubtitleExtractor::resolveRequest(const MpvController::TrackInfo &track, const QString &mediaPath,
                                  const SubtitleRequestOptions &options) const {
    const bool external = track.external && !track.externalFilename.isEmpty();
    Resolved resolved;
    resolved.external = external;

    // 判据对象是取数的输入文件：内嵌轨是正播文件，外部轨是外部字幕文件自身
    auto localReadable = [](const QString &identity) {
        return Util::Url::isLocal(identity) && !Util::Url::isMissingLocal(identity);
    };

    if (external) {
        // 外部字幕不看正播来源：正播是转接、是远端、还是根本读不了，都不影响读这个
        // 本地文件；但外部串自身是转接形态则仍然读不了
        if (StreamTransit::isTransitUri(track.externalFilename))
            return {};
        const QString externalIdentity = Util::Url::toIdentity(track.externalFilename);
        if (!localReadable(externalIdentity))
            return {};
        const QFileInfo input(externalIdentity);
        resolved.input = input;
        resolved.key =
            QStringLiteral("%1|%2|%3|%4|%5")
                .arg(QStringLiteral("ext"), input.filePath(), QString::number(track.ffIndex),
                     QString::number(quint64(input.lastModified().toSecsSinceEpoch())),
                     QString::number(input.size()));
        return resolved;
    }

    if (StreamTransit::isTransitUri(mediaPath)) {
        // 转接形态的正播读不了文件本身，取数输入改为端点解密流上的回环服务。这一支
        // 是否该走由调用方判过（认得在播条目的只有它），走到这里就当允许
        resolved.transit = true;
        resolved.listUuid = StreamTransit::listUuidOfUri(mediaPath);
        resolved.entryIdentity = StreamTransit::unwrap(mediaPath);
        if (resolved.listUuid.isNull() || resolved.entryIdentity.isEmpty() ||
            resolved.entryIdentity == mediaPath) {
            resolved.reason = Failure::SourceUnavailable;
            return resolved; // 转接形态在，身份却解不出来
        }
        // 键用原始转接 URI 而不是 unwrap 后的身份：两个不同根的 crypt 库可以有同一条
        // 相对路径身份，用身份做键会把两库条目错并进同一份缓存。版本对不可得时退化
        // 为不含版本位的键，陈旧由面板的刷新兜底
        const bool versioned = options.version.mtimeSecs >= 0 && options.version.bytes >= 0;
        resolved.key = QStringLiteral("tr|%1|%2%3")
                           .arg(mediaPath, QString::number(track.ffIndex),
                                versioned ? QStringLiteral("|%1|%2")
                                                .arg(options.version.mtimeSecs)
                                                .arg(options.version.bytes)
                                          : QStringLiteral());
        return resolved;
    }

    const QString mediaIdentity = Util::Url::toIdentity(mediaPath);
    if (!localReadable(mediaIdentity))
        return {};
    const QFileInfo input(mediaIdentity);
    resolved.input = input;
    resolved.key = QStringLiteral("%1|%2|%3|%4|%5")
                       .arg(QStringLiteral("emb"), input.filePath(), QString::number(track.id),
                            QString::number(quint64(input.lastModified().toSecsSinceEpoch())),
                            QString::number(input.size()));
    return resolved;
}

bool SubtitleExtractor::isCurrentRequest(const MpvController::TrackInfo &track,
                                         const QString &mediaPath,
                                         const SubtitleRequestOptions &options) const {
    const Resolved resolved = resolveRequest(track, mediaPath, options);
    return !resolved.key.isEmpty() && resolved.key == m_requestKey;
}

void SubtitleExtractor::setEndpointResolver(EndpointResolver resolver) {
    m_endpointResolver = std::move(resolver);
}

void SubtitleExtractor::start(const MpvController::TrackInfo &track, const QString &mediaPath,
                              const SubtitleRequestOptions &options) {
    cancel();
    const qint64 generation = ++m_generation;

    const Resolved resolved = resolveRequest(track, mediaPath, options);
    if (resolved.key.isEmpty()) {
        failFetch(generation, resolved.reason);
        return;
    }
    m_requestKey = resolved.key;
    if (options.skipCache) {
        // 刷新先丢同键缓存再完整重取：失败后不残留旧表，下一次切轨 / 重开面板也
        // 不会命中陈旧条目
        m_cache.remove(resolved.key);
        m_cacheOrder.removeAll(resolved.key);
    } else if (m_cache.contains(resolved.key)) {
        m_cacheOrder.removeAll(resolved.key);
        m_cacheOrder.append(resolved.key);
        finishFetch(generation, m_cache.value(resolved.key));
        return;
    }

    // 直读只可能是外部轨的事：一份 srt 文本自己就是结果，不起进程、不需要 ffmpeg。
    // 其余一切（内嵌轨、外部 ass / webvtt、容器型外部字幕、转接来源）都要 ffmpeg 转取，
    // 于是转取的前置判据全仓只有这一处——ffmpeg 在不在，以及这条轨的 ff-index 拿没
    // 拿到（内嵌轨缺席即"这条轨抽不出来"，不猜下标；外部轨缺席才允许退回不带 -map）
    const bool directRead =
        resolved.external && track.codec == QLatin1String("subrip") &&
        resolved.input.suffix().compare(QLatin1String("srt"), Qt::CaseInsensitive) == 0;
    if (!directRead) {
        if (m_ffmpegPath.isEmpty()) {
            failFetch(generation, Failure::FfmpegMissing);
            return;
        }
        if (track.ffIndex < 0 && !resolved.external) {
            OXBOW_LOG_WARNING().noquote()
                << "SubtitleExtractor: embedded track without ff-index, id" << track.id;
            failFetch(generation, Failure::FetchFailed);
            return;
        }
    }

    if (resolved.transit) {
        startTransitFetch(generation, resolved, track.ffIndex);
        return;
    }

    const QString inputPath = resolved.input.absoluteFilePath();
    if (directRead) {
        QFile file(inputPath);
        if (!file.open(QIODevice::ReadOnly)) {
            OXBOW_LOG_WARNING().noquote()
                << "SubtitleExtractor: cannot open external srt" << inputPath;
            failFetch(generation, Failure::FetchFailed);
            return;
        }
        bool ok = false;
        const QString text = decodeText(file.readAll(), &ok);
        if (!ok) {
            failFetch(generation, Failure::FetchFailed);
            return;
        }
        const QList<SubtitleEntry> entries = parseSrt(text);
        storeCache(resolved.key, entries);
        finishFetch(generation, entries);
        return;
    }
    launchFfmpeg(generation, resolved.key, inputPath, track.ffIndex);
}

// 转接分支的发起：解析端点 → 起回环服务 → 服务就绪后把 URL 交给 ffmpeg。本函数只接线，
// 起流与协议面全在服务线程；实例退役由线程结束信号收口（摘位 + 排队自毁），不占下一次
// 请求的路。这一支的失败除来源事实（端点取不到 / 起流失败 / 服务启动失败 →
// SourceUnavailable）外，只有 ffmpeg 非零退出码那一条，由 launchFfmpeg 的收口归类
void SubtitleExtractor::startTransitFetch(qint64 generation, const Resolved &resolved,
                                          int ffIndex) {
    if (!m_endpointResolver) {
        failFetch(generation, Failure::SourceUnavailable);
        return;
    }
    const auto endpoint = m_endpointResolver(resolved.listUuid);
    if (!endpoint) {
        OXBOW_LOG_WARNING() << "SubtitleExtractor: no endpoint for transit list";
        failFetch(generation, Failure::SourceUnavailable);
        return;
    }
    auto *server = new StreamRangeServer(this);
    connect(server, &QThread::finished, this, [this, server] {
        if (m_server == server)
            m_server = nullptr; // 已被新请求换上来的那份不动
        server->deleteLater();
    });
    connect(server, &StreamRangeServer::ready, this,
            [this, generation, cacheKey = resolved.key, ffIndex](const QUrl &url, qint64) {
                if (generation != m_generation)
                    return; // 在飞期被取消 / 新请求顶替：旧结果静默丢弃
                launchFfmpeg(generation, cacheKey, url.toString(), ffIndex);
            });
    connect(server, &StreamRangeServer::failed, this, [this, generation](const QString &why) {
        if (generation != m_generation)
            return;
        OXBOW_LOG_WARNING().noquote() << "SubtitleExtractor: range server failed:" << why;
        failFetch(generation, Failure::SourceUnavailable);
    });
    m_server = server;
    server->launch(endpoint, resolved.entryIdentity);
}

void SubtitleExtractor::launchFfmpeg(qint64 generation, const QString &cacheKey,
                                     const QString &inputArg, int ffIndex) {
    QProcess *process = new QProcess(this);
    m_process = process;
    // loglevel 提到 info：input 头的 Duration 行是进度的分母（报 N/A 即落不定态）；
    // hide_banner 只去横幅，header dump 留着。inputArg 为本地路径或回环服务 URL
    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                     QStringLiteral("info"), QStringLiteral("-i"), inputArg};
    if (ffIndex >= 0)
        args << QStringLiteral("-map") << QStringLiteral("0:%1").arg(ffIndex);
    args << QStringLiteral("-f") << QStringLiteral("srt") << QStringLiteral("-map_metadata")
         << QStringLiteral("-1") << QStringLiteral("-fflags") << QStringLiteral("+bitexact")
         << QStringLiteral("-progress") << QStringLiteral("pipe:2") << QStringLiteral("-");

    QObject::connect(process, &QProcess::finished, this,
                     [this, generation, cacheKey](int exitCode, QProcess::ExitStatus) {
                         QProcess *process = m_process;
                         m_process = nullptr;
                         // ffmpeg
                         // 退出即成败已定：服务就地收口，监听最迟一拍关闭，不留挂着的临时端口
                         stopServer();
                         if (generation != m_generation) {
                             if (process)
                                 process->deleteLater();
                             return;
                         }
                         const QByteArray out =
                             process ? process->readAllStandardOutput() : QByteArray();
                         if (process) {
                             appendErrorTail(process->readAllStandardError());
                             process->deleteLater();
                         }
                         if (exitCode != 0) {
                             OXBOW_LOG_WARNING().noquote()
                                 << "SubtitleExtractor: ffmpeg exited" << exitCode
                                 << QString::fromUtf8(m_errorTail).simplified();
                             failFetch(generation, Failure::FetchFailed);
                             return;
                         }
                         bool ok = false;
                         const QString text = decodeText(out, &ok);
                         if (!ok) {
                             failFetch(generation, Failure::FetchFailed);
                             return;
                         }
                         const QList<SubtitleEntry> entries = parseSrt(text);
                         storeCache(cacheKey, entries);
                         finishFetch(generation, entries);
                     });
    QObject::connect(process, &QProcess::readyReadStandardError, this, [this, generation] {
        if (generation != m_generation || !m_process)
            return;
        const QByteArray chunk = m_process->readAllStandardError();
        appendErrorTail(chunk);
        // stderr 混着三样东西：input 头（分母）、-progress 的键值块（分子）、警告。
        // 都按行刷出，\r / \n 都认；末段可能是半行，留缓存
        m_progressLine += QString::fromUtf8(chunk);
        const QStringList lines = m_progressLine.replace(u'\r', u'\n').split(u'\n');
        m_progressLine = lines.last();
        for (int i = 0; i < lines.size() - 1; ++i) {
            const QString line = lines.at(i).trimmed();
            if (m_totalSeconds <= 0 && line.startsWith(QLatin1String("Duration:"))) {
                m_totalSeconds = parseDurationClock(line);
            } else if (m_totalSeconds > 0 && line.startsWith(QLatin1String("out_time="))) {
                const double seconds = parseProgressClock(line.mid(9));
                if (seconds >= 0)
                    emit progress(qBound(0.0, seconds / m_totalSeconds, 1.0));
            }
        }
    });
    QObject::connect(
        process, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError error) {
            // FailedToStart 之后不会再有 finished，由这里收口；其余错误
            // （Crashed 等）由 finished 的非零退出码兜住
            if (error != QProcess::FailedToStart || generation != m_generation)
                return;
            QProcess *process = m_process;
            m_process = nullptr;
            OXBOW_LOG_WARNING().noquote() << "SubtitleExtractor: ffmpeg failed to start"
                                          << (process ? process->errorString() : QString());
            if (process)
                process->deleteLater();
            failFetch(generation, Failure::FetchFailed);
            m_generation = generation + 1;
        });

    process->start(m_ffmpegPath, args);
}

void SubtitleExtractor::stopServer() {
    if (!m_server)
        return;
    m_server->stop(); // 非阻塞：置取消旗 + 流 cancel；线程退出后经 finished 钩子自毁
    m_server = nullptr;
}

void SubtitleExtractor::cancel() {
    ++m_generation;
    m_requestKey.clear();
    m_progressLine.clear();
    m_errorTail.clear();
    m_totalSeconds = 0.0;
    stopServer();
    if (!m_process)
        return;
    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    process->kill();
    process->deleteLater();
}

void SubtitleExtractor::appendErrorTail(const QByteArray &chunk) {
    m_errorTail += chunk;
    if (m_errorTail.size() > kErrorTailBytes)
        m_errorTail.remove(0, m_errorTail.size() - kErrorTailBytes);
}

void SubtitleExtractor::finishFetch(qint64 generation, const QList<SubtitleEntry> &entries) {
    if (generation != m_generation)
        return;
    emit finished(entries);
}

void SubtitleExtractor::failFetch(qint64 generation, Failure reason) {
    if (generation != m_generation)
        return;
    stopServer();         // 失败终局与成功、取消同一条收口
    m_requestKey.clear(); // 终态：下一次触发重试
    emit failed(reason);
}

void SubtitleExtractor::storeCache(const QString &key, const QList<SubtitleEntry> &entries) {
    if (m_cache.contains(key))
        m_cacheOrder.removeAll(key);
    m_cache.insert(key, entries);
    m_cacheOrder.append(key);
    while (m_cacheOrder.size() > kCacheLimit) {
        m_cache.remove(m_cacheOrder.first());
        m_cacheOrder.removeFirst();
    }
}

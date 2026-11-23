#include "pch.h"

#include "ExternalProvider.h"

#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "Util.h"

namespace {

QString trText(const char *source) {
    return QCoreApplication::translate("ExternalProvider", source);
}

// 七个标签都是"这个文件在描述一条流的组成部分"的格式级证据；命中要求标签出现在
// 头部区（ENDLIST 结构上只在末尾，豁免）。#EXT-X-VERSION 刻意不进白名单：频道
// 清单生成器常顺手带上，宽判据会把整张清单压成一行
constexpr const char *kStreamHeaderTags[] = {
    "#EXT-X-MEDIA-SEQUENCE", "#EXT-X-TARGETDURATION",     "#EXT-X-PLAYLIST-TYPE",
    "#EXT-X-STREAM-INF",     "#EXT-X-I-FRAME-STREAM-INF", "#EXT-X-MEDIA:",
};

QStringList splitLines(const QString &text) {
    QStringList lines;
    qsizetype start = 0;
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t u = text.at(i).unicode();
        if (u != u'\n' && u != u'\r')
            continue;
        lines.append(text.mid(start, i - start));
        if (u == u'\r' && i + 1 < n && text.at(i + 1).unicode() == u'\n')
            ++i;
        start = i + 1;
    }
    lines.append(text.mid(start));
    return lines;
}

// 异常档②（各支共用）：行内控制字符或单行超 4096 字节，是"参数填成图片 / 二进制"
// 的表征。单趟按 UTF-8 字节数计（单元数 ≤ 字节数即可提前定性），免整行 toUtf8
bool isAnomalousLine(const QString &line) {
    if (line.size() > 4096)
        return true;
    qsizetype bytes = 0;
    bool pairHigh = false;
    for (const QChar c : line) {
        const char16_t u = c.unicode();
        if (u < 0x20 && u != u'\t')
            return true;
        if (u < 0x80) {
            bytes += 1;
            pairHigh = false;
        } else if (u < 0x800) {
            bytes += 2;
            pairHigh = false;
        } else if (u < 0xD800 || u >= 0xE000) {
            bytes += 3;
            pairHigh = false;
        } else if (u < 0xDC00) {
            bytes += 4; // 代理对在此一次计足
            pairHigh = true;
        } else {
            // 孤立低位代理按 U+FFFD 计（toUtf8 的替换行为），方向保守
            if (!pairHigh)
                bytes += 3;
            pairHigh = false;
        }
        if (bytes > 4096)
            return true;
    }
    return false;
}

// 引号值优先、裸值兜底；模式为函数内 static：只编译一次（2.2 万次调用量级），
// match 是 const 操作，枚举任务线程并发调用安全
QString extinfAttrValue(const QString &attrs, const QString &name) {
    static const QRegularExpression groupTitleRe(
        QStringLiteral("(?:^|[\\s])group-title\\s*=\\s*(?:\"([^\"]*)\"|(\\S+))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tvgNameRe(
        QStringLiteral("(?:^|[\\s])tvg-name\\s*=\\s*(?:\"([^\"]*)\"|(\\S+))"),
        QRegularExpression::CaseInsensitiveOption);
    const bool isGroupTitle = name == QLatin1String("group-title");
    Q_ASSERT(isGroupTitle || name == QLatin1String("tvg-name"));
    const QRegularExpressionMatch m = (isGroupTitle ? groupTitleRe : tvgNameRe).match(attrs);
    if (!m.hasMatch())
        return QString();
    const QString bare = m.captured(2);
    return bare.isNull() ? m.captured(1) : bare;
}

// UA / Referer 几乎必然含空白，裸值支会截断成半个 UA（比不带更坏），故只认引号值
QString extinfAttrQuotedValue(const QString &attrs, const QString &name) {
    static const QRegularExpression userAgentRe(
        QStringLiteral("(?:^|[\\s])http-user-agent\\s*=\\s*\"([^\"]*)\""),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression referrerRe(
        QStringLiteral("(?:^|[\\s])http-referrer\\s*=\\s*\"([^\"]*)\""),
        QRegularExpression::CaseInsensitiveOption);
    const bool isUserAgent =
        name.compare(QLatin1String("http-user-agent"), Qt::CaseInsensitive) == 0;
    Q_ASSERT(isUserAgent || name.compare(QLatin1String("http-referrer"), Qt::CaseInsensitive) == 0);
    const QRegularExpressionMatch m = (isUserAgent ? userAgentRe : referrerRe).match(attrs);
    return m.hasMatch() ? m.captured(1) : QString();
}

// 取值逐字节原样：两键都是 plain String 选项，节点通道实测含逗号 / 空白 / 反斜杠
// 也原样到位。不在白名单返回 nullopt，由调用方置"有被忽略的键"旗标
std::optional<QString> mpvOptionNameOf(const QString &vlcKey) {
    if (vlcKey.compare(QLatin1String("http-user-agent"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("user-agent");
    if (vlcKey.compare(QLatin1String("http-referrer"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("referrer");
    return std::nullopt;
}

QString baseSourceOf(const QString &source) {
    return Util::Url::isHttp(source) ? source : QFileInfo(source).absolutePath();
}

// 网络 / 流目标（含 rtmp://、srt:// 等）经 toNative 收口；file:// 折盘符；相对目标
// 按基址定形（http 基址走 resolved，不剥 userinfo）。产出即喂工厂的身份串
QString normalizeTarget(const QString &rawTarget, const QString &baseSource) {
    const QString t = rawTarget.trimmed();
    if (t.isEmpty())
        return QString();
    if (Util::Url::isNetworkStream(t))
        return Util::Url::toNative(t);
    QString local = t;
    if (local.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
        local = Util::Url::toIdentity(local);
    if (Util::Url::isHttp(baseSource)) {
        const QUrl resolved = QUrl(baseSource, QUrl::ParsingMode::TolerantMode)
                                  .resolved(QUrl(local, QUrl::ParsingMode::TolerantMode));
        if (!resolved.isValid())
            return QString();
        return Util::Url::toNative(
            resolved.toString(QUrl::ComponentFormattingOption::FullyEncoded));
    }
    if (QDir::isAbsolutePath(local))
        return Util::Url::toNative(local);
    if (baseSource.isEmpty())
        return QString();
    return Util::Url::toNative(QDir(baseSource).filePath(local));
}

// 遮挡凭据，不透传 error 原文
QString httpErrorText(const HttpClient::Response &resp, const QString &url) {
    const QString target = Util::Url::stripCredentials(url);
    if (resp.canceled)
        return trText(QT_TRANSLATE_NOOP("ExternalProvider", "Request canceled: %1")).arg(target);
    // overflow 与 timedOut 都只留 error 非空可辨（status 同值），文案不断言原因
    if (!resp.error.isEmpty() && resp.status >= 200 && resp.status < 300)
        return trText(
                   QT_TRANSLATE_NOOP("ExternalProvider", "The playlist could not be fetched: %1"))
            .arg(target);
    if (resp.status == 401)
        return trText(QT_TRANSLATE_NOOP("ExternalProvider", "Authentication failed (401): %1"))
            .arg(target);
    if (resp.status == 403)
        return trText(QT_TRANSLATE_NOOP("ExternalProvider", "Access denied (403): %1")).arg(target);
    if (resp.status == 404)
        return trText(QT_TRANSLATE_NOOP("ExternalProvider", "Not found (404): %1")).arg(target);
    if (resp.status >= 400)
        return trText(QT_TRANSLATE_NOOP("ExternalProvider", "Server error (HTTP %1): %2"))
            .arg(resp.status)
            .arg(target);
    if (resp.status > 0)
        return trText(QT_TRANSLATE_NOOP("ExternalProvider", "Unexpected response (HTTP %1): %2"))
            .arg(resp.status)
            .arg(target);
    return trText(QT_TRANSLATE_NOOP("ExternalProvider", "Request failed (network error): %1"))
        .arg(target);
}

} // namespace

ExternalProvider::ExternalProvider(const QString &sourcePath,
                                   const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_sourcePath(sourcePath) {}

QString ExternalProvider::typeKey() const { return QLatin1String(kExternalProviderKey); }

QString ExternalProvider::displayName() {
    return QCoreApplication::translate("ExternalProvider", kExternalProviderName);
}

bool ExternalProvider::supportsModify() const { return false; }

bool ExternalProvider::supportsSort() const { return true; }

// 一份文件一次取回即全知，逐层再取是重复劳动
bool ExternalProvider::enumeratesWholeTree() const { return true; }

QString ExternalProvider::normalizeSource(const QString &text) {
    QString src = text.trimmed();
    if (src.isEmpty())
        return QString();
    return Util::Url::isNetworkStream(src) ? Util::Url::toNative(src) : Util::Url::toIdentity(src);
}

// 本地 stat 是常数代价，就地拒比让用户点 OK 后猜占位行更诚实；远端不探测
std::optional<QString> ExternalProvider::validateInput(const QString &rawText) {
    const QString src = normalizeSource(rawText);
    if (src.isEmpty())
        return trText(
            QT_TRANSLATE_NOOP("ExternalProvider", "Please enter a playlist path or URL."));
    if (Util::Url::isHttp(src))
        return std::nullopt;
    if (!Util::Url::isLocal(src))
        return trText(QT_TRANSLATE_NOOP("ExternalProvider",
                                        "The path must be a local file path or an http(s) URL."));
    // 本地档必须是磁盘上存在的普通文件；不设扩展名白名单——后缀不能定性正是本篇
    // 立足点，"无后缀 / .txt 而内容确是清单"不该被挡在门外
    const QFileInfo info(src);
    if (info.isDir())
        return trText(QT_TRANSLATE_NOOP(
            "ExternalProvider", "The path must point to a playlist file, not a directory."));
    if (!info.isFile())
        return trText(QT_TRANSLATE_NOOP("ExternalProvider", "The playlist file does not exist: %1"))
            .arg(src);
    return std::nullopt;
}

// iptv-org 以分号作 group-title 的多值分隔，不拆会挂满 "A;B;C" 形态的怪组
QStringList ExternalProvider::splitGroupTitles(const QString &groupTitles) {
    QStringList out;
    for (const QString &part : groupTitles.split(QLatin1Char(';'))) {
        const QString group = part.trimmed();
        if (!group.isEmpty() && !out.contains(group))
            out.append(group);
    }
    return out;
}

ExternalProvider::Parsed ExternalProvider::parseSource(const QString &text,
                                                       const QString &baseSource) {
    Parsed out;
    QString body = text;
    if (body.startsWith(QChar(0xFEFF)))
        body.remove(0, 1);
    if (body.isEmpty())
        return out;
    const QStringList lines = splitLines(body);

    // 第一遍：收集判别旗标。流判据与"全文任一 #EXTINF / #EXT-X-"是全局存在性判据；
    // txt 判据只看首个数据行，先判后解才能落对支
    bool firstNonBlankSeen = false;
    bool firstIsExtm3u = false;
    bool firstDataLineSeen = false;
    bool firstDataIsGenreRow = false;
    bool seenM3uInstruction = false;
    bool headerEnded = false;
    for (const QString &raw : lines) {
        const QString t = raw.trimmed();
        if (t.isEmpty())
            continue;
        if (!firstNonBlankSeen) {
            firstNonBlankSeen = true;
            firstIsExtm3u = t.startsWith(QLatin1String("#EXTM3U"), Qt::CaseInsensitive);
        }
        if (t.startsWith(QLatin1Char('#'))) {
            if (t.startsWith(QLatin1String("#EXT-X-ENDLIST"), Qt::CaseInsensitive)) {
                out.isStreamSource = true;
            } else if (!headerEnded) {
                // 头部区 = 首条非注释目标行之前；混排清单在条目之间保留的分片块
                // （含 TARGETDURATION）被位次限定挡在外面，不再压成一行
                for (const char *tag : kStreamHeaderTags) {
                    if (t.startsWith(QLatin1String(tag), Qt::CaseInsensitive)) {
                        out.isStreamSource = true;
                        break;
                    }
                }
            }
            if (t.startsWith(QLatin1String("#EXTINF"), Qt::CaseInsensitive) ||
                t.startsWith(QLatin1String("#EXT-X-"), Qt::CaseInsensitive))
                seenM3uInstruction = true;
            continue;
        }
        headerEnded = true;
        if (!firstDataLineSeen) {
            firstDataLineSeen = true;
            // 首个数据行按首逗号分两列，第二列 trim 后与 "#genre#" 整字相等即组行
            const qsizetype comma = t.indexOf(QLatin1Char(','));
            firstDataIsGenreRow =
                comma >= 0 && QStringView(t).mid(comma + 1).trimmed() == QLatin1String("#genre#");
        }
    }
    if (out.isStreamSource)
        return out; // 流：解析产出整份丢弃

    // 首个数据行是组行即分组文本支——两套行语法互斥，分组文本没有任何 # 指令；
    // 带 #EXTM3U / #EXT-X-VERSION 装饰头的 tvbox 导出真实存在，装饰头不是数据行、
    // 跳过不计；中段夹带的 #genre# 行不作数。裸清单支兜底：.m3u 原始形态就是逐行表，
    // #EXTINF 属可选方言；HLS 媒体清单已在上面判成流
    enum class Grammar { M3u, GroupedText, Bare };
    const Grammar grammar = firstDataIsGenreRow                     ? Grammar::GroupedText
                            : (firstIsExtm3u || seenM3uInstruction) ? Grammar::M3u
                                                                    : Grammar::Bare;

    // warnings 只留给结构性异常，一类至多一条、串里不带条数
    bool malformed = false;
    // hasVlcOpt 只记"出现白名单外的 #EXTVLCOPT 键"：白名单两键已落地，带头的清单
    // 不再有理由警告
    bool hasVlcOpt = false;
    // "空清单（只有 #EXTM3U）"是作者写就的空表、不带警告；其余任何非空行在零条目时
    // 都说明这份东西大概不是清单
    bool sawContent = false;

    if (grammar == Grammar::M3u) {
        bool pending = false;
        QString pendingTitle;
        QString pendingTvgName;
        QStringList pendingGroups;
        // EXTINF 属性区两键先入，其后 #EXTVLCOPT 行按文件序覆盖同键；目标行消费并
        // 清空。独立于 pending 旗标：无 #EXTINF 的裸目标行同样吃得到
        QVariantMap pendingOptions;
        for (const QString &raw : lines) {
            const QString t = raw.trimmed();
            if (t.isEmpty())
                continue;
            if (t.startsWith(QLatin1Char('#'))) {
                if (t.startsWith(QLatin1String("#EXTINF:"), Qt::CaseInsensitive)) {
                    if (pending)
                        malformed = true; // 上一条 EXTINF 仍无目标行
                    // 属性区与标题区按"引号外的第一个逗号"分界：引号感知使 tvg-logo /
                    // http-user-agent 值内的逗号不误开边界，标题含逗号也不被截断
                    const QString rest = t.mid(8);
                    pendingTitle.clear();
                    pendingTvgName.clear();
                    pendingGroups.clear();
                    pendingOptions.clear(); // 被新 EXTINF 顶替的未消费选项丢弃
                    qsizetype split = -1;
                    bool inQuote = false;
                    for (qsizetype i = 0; i < rest.size(); ++i) {
                        const QChar c = rest.at(i);
                        if (c == QLatin1Char('"'))
                            inQuote = !inQuote;
                        else if (c == QLatin1Char(',') && !inQuote) {
                            split = i;
                            break;
                        }
                    }
                    // 无逗号（如 "#EXTINF:-1 group-title=\"G\""）整段当属性区、标题空
                    QString attrs = split < 0 ? rest : rest.left(split);
                    if (split >= 0)
                        pendingTitle = rest.mid(split + 1).trimmed();
                    qsizetype durEnd = 0;
                    while (durEnd < attrs.size() && !attrs.at(durEnd).isSpace())
                        ++durEnd;
                    attrs = attrs.mid(durEnd).trimmed();
                    pendingGroups =
                        splitGroupTitles(extinfAttrValue(attrs, QStringLiteral("group-title")));
                    pendingTvgName = extinfAttrValue(attrs, QStringLiteral("tvg-name"));
                    // 属性区两键只认引号值形态，空值不注入
                    for (const QString &attr :
                         {QStringLiteral("http-user-agent"), QStringLiteral("http-referrer")}) {
                        const QString value = extinfAttrQuotedValue(attrs, attr);
                        if (!value.isEmpty())
                            if (const std::optional<QString> name = mpvOptionNameOf(attr))
                                pendingOptions.insert(*name, value);
                    }
                    pending = true;
                    sawContent = true;
                } else if (t.startsWith(QLatin1String("#EXTVLCOPT"), Qt::CaseInsensitive)) {
                    // 前缀后必须紧跟 ':'（无冒号按未知指令忽略）；分界取第一个 '='；
                    // 键 trim 后大小写不敏感；值到行尾、两端 trim（内部空格与逗号保留）
                    sawContent = true;
                    const QString rest = t.mid(10);
                    if (!rest.startsWith(QLatin1Char(':')))
                        continue;
                    const QString body = rest.mid(1);
                    const qsizetype eq = body.indexOf(QLatin1Char('='));
                    if (eq < 0)
                        continue;
                    const QString key = body.left(eq).trimmed();
                    const QString value = body.mid(eq + 1).trimmed();
                    if (key.isEmpty())
                        continue;
                    if (const std::optional<QString> name = mpvOptionNameOf(key)) {
                        // 空值不注入：空串 Referer 比不发更糟
                        if (!value.isEmpty())
                            pendingOptions.insert(*name, value);
                    } else {
                        hasVlcOpt = true;
                    }
                } else if (!t.startsWith(QLatin1String("#EXTM3U"), Qt::CaseInsensitive)) {
                    // 其余 # 行静默忽略：未知指令成千上万行，逐行 warn 会淹掉弹窗；
                    // 但它们算"内容"
                    sawContent = true;
                } // #EXTM3U 行（含 x-tvg-url 属性）忽略，且不算内容
                continue;
            }
            if (isAnomalousLine(raw)) {
                // 不收入条目，也不消耗待成形条目（EXTINF 的下一个合法目标行照常配对）
                malformed = true;
                sawContent = true;
                continue;
            }
            // 目标行：与待成形条目配对，无 #EXTINF 时 title 与组名留空
            const QString target = normalizeTarget(raw, baseSource);
            sawContent = true;
            if (target.isEmpty()) {
                malformed = true;
                continue;
            }
            Entry entry;
            entry.target = target;
            if (pending) {
                entry.title = pendingTitle;
                entry.tvgName = pendingTvgName;
                entry.groups = pendingGroups;
                pending = false;
            }
            // 槽独立于 pending 旗标，裸目标行同样吃得到；到文件尾仍无目标行的静默丢弃
            entry.options = pendingOptions;
            pendingOptions.clear();
            // 组只随真实成形的条目登记，被异常档丢掉的行走不到这里
            for (const QString &group : entry.groups)
                if (!out.groupOrder.contains(group))
                    out.groupOrder.append(group);
            out.entries.append(std::move(entry));
        }
        if (pending)
            malformed = true;
    } else if (grammar == Grammar::GroupedText) {
        // 分组文本支（tvbox / my-tv 系）：无任何 # 指令，<组名>,#genre# 行开组、
        // <名>,<URL> 行给条目；产出仍进同一张 Parsed——多一套行语法，不是多一种形态
        QString currentGroup; // 首个组行之前的裸行落根层
        for (const QString &raw : lines) {
            const QString t = raw.trimmed();
            if (t.isEmpty())
                continue;
            if (t.startsWith(QLatin1Char('#'))) {
                if (!t.startsWith(QLatin1String("#EXTM3U"), Qt::CaseInsensitive))
                    sawContent = true;
                continue;
            }
            if (isAnomalousLine(raw)) {
                malformed = true;
                sawContent = true;
                continue;
            }
            sawContent = true;
            const qsizetype comma = t.indexOf(QLatin1Char(','));
            if (comma < 0) {
                // 裸行：base M3U 标准形态照收，组归属按位置
                const QString target = normalizeTarget(raw, baseSource);
                if (target.isEmpty()) {
                    malformed = true;
                    continue;
                }
                Entry entry;
                entry.target = target;
                if (!currentGroup.isEmpty())
                    entry.groups.append(currentGroup);
                out.entries.append(std::move(entry));
                continue;
            }
            const QString col1 = t.left(comma).trimmed();
            const QString col2 = t.mid(comma + 1).trimmed();
            if (col2 == QLatin1String("#genre#")) {
                // 组行：组名 trim 后原样保留 emoji 与全半角，"🕘️更新时间"这类非频道
                // 类别的组不特殊处置
                if (col1.isEmpty()) {
                    malformed = true;
                    continue;
                }
                currentGroup = col1;
                if (!out.groupOrder.contains(col1))
                    out.groupOrder.append(col1);
            } else {
                // 第二列不要求 http(s) 开头——rtmp:// / rtsp:// 等 mpv 直播流 scheme
                // 与 m3u 支一样照收
                const QString target = normalizeTarget(col2, baseSource);
                if (target.isEmpty()) {
                    malformed = true;
                    continue;
                }
                Entry entry;
                entry.target = target;
                entry.title = col1;
                if (!currentGroup.isEmpty())
                    entry.groups.append(currentGroup);
                out.entries.append(std::move(entry));
            }
        }
    } else {
        // 裸清单支：无头无扩展的 base M3U 本体，逐行一条目标，全无标题与组
        for (const QString &raw : lines) {
            const QString t = raw.trimmed();
            if (t.isEmpty())
                continue;
            if (t.startsWith(QLatin1Char('#'))) {
                if (!t.startsWith(QLatin1String("#EXTM3U"), Qt::CaseInsensitive))
                    sawContent = true;
                continue;
            }
            if (isAnomalousLine(raw)) {
                malformed = true;
                sawContent = true;
                continue;
            }
            sawContent = true;
            const QString target = normalizeTarget(raw, baseSource);
            if (target.isEmpty()) {
                malformed = true;
                continue;
            }
            Entry entry;
            entry.target = target;
            out.entries.append(std::move(entry));
        }
    }

    // 结构性异常的三条无计数警告：一类至多一条，弹窗由视图侧封顶机制处理
    if (out.entries.isEmpty() && sawContent)
        out.warnings.append(trText(QT_TRANSLATE_NOOP(
            "ExternalProvider", "No playable entries were found in this playlist.")));
    if (malformed)
        out.warnings.append(trText(QT_TRANSLATE_NOOP(
            "ExternalProvider", "Malformed lines were skipped in this playlist.")));
    // 只报"白名单外的键"，串里不带计数与键名——键名成千上万，进串等于把警告
    // 变成日志
    if (hasVlcOpt)
        out.warnings.append(trText(QT_TRANSLATE_NOOP(
            "ExternalProvider",
            "Some per-entry options in this playlist are not supported and were ignored.")));
    return out;
}

// 工作线程调用；每次重新取源，实例内无状态。根层 = 具名组容器 + 无组名条目，
// 每个具名组的层内容各作一项 descendants（整树）
std::expected<PlaylistProvider::Enumeration, QString>
ExternalProvider::enumerate(const QStringList &parent) const {
    if (!parent.isEmpty()) {
        // 常规通路只以空 parent 调用，收到非空即调用方违约
        Q_ASSERT(false);
        return Enumeration{};
    }
    if (m_sourcePath.isEmpty())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("ExternalProvider", "No path configured.")));

    // 本地整份读入；http(s) 一个 GET 走 exec——枚举无取消需求。时限与体上限随全局配置
    QByteArray bytes;
    if (Util::Url::isHttp(m_sourcePath)) {
        const std::shared_ptr<HttpClient> client =
            registry() ? registry()->resolve<HttpClient>() : nullptr;
        if (!client) {
            Q_ASSERT(false);
            return std::unexpected(
                trText(QT_TRANSLATE_NOOP("ExternalProvider", "HTTP client is not available.")));
        }
        HttpClient::Request request;
        request.url = QUrl(m_sourcePath, QUrl::ParsingMode::TolerantMode);
        request.method = QByteArrayLiteral("GET");
        const HttpClient::Response resp = client->exec(request);
        // error 非空亦判失败：体超限时 status 是真实码、body 为空，只看状态码会把
        // 超限清单当空清单静默吞掉
        const bool ok =
            resp.status >= 200 && resp.status < 300 && !resp.canceled && resp.error.isEmpty();
        if (!ok)
            return std::unexpected(httpErrorText(resp, m_sourcePath));
        bytes = resp.body;
    } else {
        QFile file(m_sourcePath);
        if (!file.open(QIODevice::ReadOnly))
            return std::unexpected(trText(QT_TRANSLATE_NOOP("ExternalProvider",
                                                            "Could not read the playlist file: %1"))
                                       .arg(m_sourcePath));
        bytes = file.readAll();
        // readAll 出错时 bytes 为空，不检查会被当空清单零警告吞掉
        if (file.error() != QFileDevice::NoError)
            return std::unexpected(trText(QT_TRANSLATE_NOOP("ExternalProvider",
                                                            "Could not read the playlist file: %1"))
                                       .arg(m_sourcePath));
    }

    // 非法 UTF-8 按替换字符降级：一处坏字节不该废掉一万条频道。不设 GBK 回退——
    // 猜错的形态是整表频道名乱码且无从报告
    const Parsed parsed = parseSource(QString::fromUtf8(bytes), baseSourceOf(m_sourcePath));

    if (parsed.isStreamSource) {
        // 一行 = 清单自身，双击把整条 URL（或本地 .m3u8）交给 mpv 按 HLS 播放
        Enumeration out;
        out.entities.push_back(PlaylistEntity::fromPath(m_sourcePath, registry()));
        return out;
    }

    Enumeration out;
    out.warnings = parsed.warnings;
    out.containers.reserve(parsed.groupOrder.size());
    for (const QString &group : parsed.groupOrder) {
        Container container;
        // id = title = 组名：id 只要求提供者内稳定、可当 parent 路径段；meta / path 留空
        container.id = group;
        container.title = group;
        out.containers.push_back(std::move(container));
    }
    // 显示名两档回退：title → tvg-name；皆空则不注入，落回实体按身份串派生的末段。
    // fileName 不注入，FileName 排序按真名走；同名不同 URL 天然得到不同实体——
    // 身份是定形后的 path()，不按名判重
    auto makeEntity = [this](const Entry &entry) -> PlaylistEntity::Ptr {
        auto entity = PlaylistEntity::fromPath(entry.target, registry());
        QString name = entry.title;
        if (name.isEmpty())
            name = entry.tvgName;
        if (!name.isEmpty())
            injectDisplayName(*entity, name);
        // 选项解析内已成型（键 = mpv 选项名），本处只搬运；空 map 不注入
        if (!entry.options.isEmpty())
            injectPlayOptions(*entity, entry.options);
        return entity;
    };
    // 同一条目入多组时每组各现造一份实体，互不共享实例
    for (const QString &group : parsed.groupOrder) {
        LevelData level;
        for (const Entry &entry : parsed.entries)
            if (entry.groups.contains(group))
                level.entities.push_back(makeEntity(entry));
        out.descendants.insert(QStringList{group}, std::move(level));
    }
    // 无组名条目摊在根层实体段（容器在前是模型不变式）
    for (const Entry &entry : parsed.entries)
        if (entry.groups.isEmpty())
            out.entities.push_back(makeEntity(entry));
    return out;
}

void ExternalProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings);
    settings.setValue(QStringLiteral("extPath"), m_sourcePath);
    // 含 None：显式记录用户未选择规则
    settings.setValue(QStringLiteral("sortKey"), int(sortRule().key));
    settings.setValue(QStringLiteral("sortAscending"), sortRule().ascending);
}

std::shared_ptr<PlaylistProvider>
ExternalProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    // 读回再定形一次只为兜住手改 INI 的形态漂移（normalizeSource 幂等）；缺键落
    // 空串，enumerate 以 "No path configured." 应答
    auto provider = std::make_shared<ExternalProvider>(
        normalizeSource(settings.value(QStringLiteral("extPath")).toString()), registry);
    const SortRule rule{static_cast<SortKey>(settings.value(QStringLiteral("sortKey"), 0).toInt()),
                        settings.value(QStringLiteral("sortAscending"), true).toBool()};
    provider->setSortRule(rule);
    return provider;
}

// 参数页：Path + 编辑框 + Browse…，HTTP 地址直接键入。过滤器不含全局
// defaultMediaExtensions()——那份清单里没有 m3u，且它是目录枚举的过滤器
class ExternalSettingsPage : public ProviderSettingsPage {
    Q_OBJECT
  public:
    ExternalSettingsPage(const ExternalProvider *seed,
                         const std::shared_ptr<ServiceRegistry> &registry, QWidget *parent)
        : ProviderSettingsPage(parent), m_seed(seed), m_registry(registry) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);

        m_pathEdit = new QLineEdit;
        auto *row = new QHBoxLayout;
        row->addWidget(m_pathEdit, 1);
        auto *browseBtn = new QPushButton(tr("Browse…"));
        row->addWidget(browseBtn);
        form->addRow(tr("Path"), row);
        auto *pathNote = new QLabel(tr("Supports m3u and txt playlist formats, or an http(s) URL"));
        pathNote->setEnabled(false);
        form->addRow(pathNote);
        layout->addLayout(form);
        // 同目录页：把被拉伸出来的余量压到内容之下，避免行间距被均摊撑大
        // 把拉伸出来的余量压到内容之下，避免行间距被均摊撑大
        layout->addStretch(1);

        if (m_seed)
            m_pathEdit->setText(m_seed->sourcePath());

        connect(browseBtn, &QPushButton::clicked, this, [this] {
            const QString file = QFileDialog::getOpenFileName(
                this, tr("Select Playlist"), QString(),
                tr("Playlist files (*.m3u *.m3u8 *.txt);;All files (*)"));
            // Qt 来源的路径，拿到即转原生分隔符
            if (!file.isEmpty())
                m_pathEdit->setText(Util::Url::toNative(file));
        });
    }

    std::optional<QString> validate() const override {
        return ExternalProvider::validateInput(m_pathEdit->text());
    }

    bool hasChanges() const override { return !m_seed || pathValue() != m_seed->sourcePath(); }

    std::shared_ptr<PlaylistProvider> build() const override {
        return std::make_shared<ExternalProvider>(pathValue(), m_registry);
    }

  private:
    QString pathValue() const { return ExternalProvider::normalizeSource(m_pathEdit->text()); }

    const ExternalProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QLineEdit *m_pathEdit = nullptr;
};

ProviderSettingsPage *createExternalSettingsPage(const PlaylistProvider *seed,
                                                 const std::shared_ptr<ServiceRegistry> &registry,
                                                 QWidget *parent) {
    // seed 非空即本类型，由调用方（对话框）保证
    return new ExternalSettingsPage(seed ? static_cast<const ExternalProvider *>(seed) : nullptr,
                                    registry, parent);
}

// 本文件定义了 Q_OBJECT 类（ExternalSettingsPage），CMake AUTOMOC 要求显式包含
// 同名 .moc（否则 configure 期直接报错），按源文件名而非类名
#include "ExternalProvider.moc"

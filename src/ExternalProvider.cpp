#include "pch.h"

#include "ExternalProvider.h"

#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "Util.h"

namespace {

// 非 Q_OBJECT 类的用户可见串：英文源 + QT_TRANSLATE_NOOP 标注翻译上下文
// （供 lupdate 提取），运行期经 QCoreApplication::translate 取值——无译文时即
// 英文原文（WebDavProvider 同款约定）
QString trText(const char *source) {
    return QCoreApplication::translate("ExternalProvider", source);
}

// 流清单标签白名单（§2.2）：七个标签都是"这个文件在描述一条流的组成部分"的
// 格式级证据。命中判据 = 行首命中标签 且 该标签出现在头部区；ENDLIST 因结构上
// 只出现在末尾而豁免位次，单列在扫描器里判。刻意不进白名单的 #EXT-X-VERSION
// 被不少频道清单生成器顺手带上——宽判据会把整张频道清单压成一行，破坏面远大于漏判
constexpr const char *kStreamHeaderTags[] = {
    "#EXT-X-MEDIA-SEQUENCE", "#EXT-X-TARGETDURATION",     "#EXT-X-PLAYLIST-TYPE",
    "#EXT-X-STREAM-INF",     "#EXT-X-I-FRAME-STREAM-INF", "#EXT-X-MEDIA:",
};

// 换行拆分：\r\n / \n / \r 三种通吃（末行无换行符也成行）
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

// 异常档②（§2.3，各支共用）：行内含 NUL 或其它 < 0x20 控制字符（制表符除外），
// 或单行长度超 4096 字节——这是"参数填成了图片 / 二进制"的现实表征；有了它，
// 裸清单支容下裸行才不会顺带把二进制收成一屏垃圾行。
// 单趟实现：UTF-16 单元数 ≤ UTF-8 字节数（合法代理对 4 字节 / 2 单元），单元数
// 已超限即可定性免一次全行编码；控制字符扫描与字节数累计同一趟、超限即提前退出，
// 免整行 toUtf8 的堆分配
bool isAnomalousLine(const QString &line) {
    if (line.size() > 4096)
        return true;
    qsizetype bytes = 0;
    bool pairHigh = false; // 上一字符是高位代理：合法代理对整对合记 4 字节
    for (const QChar c : line) {
        const char16_t u = c.unicode();
        if (u < 0x20 && u != u'\t')
            return true; // 控制字符（制表符除外）
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
            bytes += 4; // 高位代理：代理对（4 字节）在此一次计足
            pairHigh = true;
        } else {
            // 孤立低位代理按 U+FFFD 计（toUtf8 的替换行为）；偏差只出现在本就
            // 该判异常的残行上，方向保守
            if (!pairHigh)
                bytes += 3;
            pairHigh = false;
        }
        if (bytes > 4096)
            return true;
    }
    return false;
}

// #EXTINF 属性区取值：attr="value" 引号值优先，裸值（到空白止）兜底；attr 名按
// 大小写不敏感匹配。模式为函数内 static——只编译一次（index.m3u 量级约 2.2 万次
// 调用，每次现场构造 QRegularExpression 代价可观）；match 是 const 操作，枚举
// 任务线程并发调用安全
QString extinfAttrValue(const QString &attrs, const QString &name) {
    static const QRegularExpression groupTitleRe(
        QStringLiteral("(?:^|[\\s])group-title\\s*=\\s*(?:\"([^\"]*)\"|(\\S+))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tvgNameRe(
        QStringLiteral("(?:^|[\\s])tvg-name\\s*=\\s*(?:\"([^\"]*)\"|(\\S+))"),
        QRegularExpression::CaseInsensitiveOption);
    const bool isGroupTitle = name == QLatin1String("group-title");
    Q_ASSERT(isGroupTitle || name == QLatin1String("tvg-name")); // 仅此两个调用点
    const QRegularExpressionMatch m = (isGroupTitle ? groupTitleRe : tvgNameRe).match(attrs);
    if (!m.hasMatch())
        return QString();
    const QString bare = m.captured(2);
    return bare.isNull() ? m.captured(1) : bare;
}

// 解析基址（§2.3）：http 清单 = 清单 URL（相对目标经 resolved 展开）；本地清单 =
// 清单文件所在目录（相对目标在此拼接）
QString baseSourceOf(const QString &source) {
    return Util::Url::isHttp(source) ? source : QFileInfo(source).absolutePath();
}

// 条目目标定形（解析与枚举共用）：网络 / 流目标（rtmp:// / srt:// 等绝对形态在
// 内）经 Util::Url::toNative 收口——http(s) 收规范形态、其余网络协议原样；file://
// 折本地盘符串；本地绝对目标直接收口；相对目标按基址定形（http 基址走
// QUrl::resolved 展开，与 WebDAV 同一条机制、不剥 userinfo——本提供者不注入也不
// 剥；本地基址按清单文件所在目录拼接）。产出即喂工厂的已定形身份串
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

// HTTP 状态码与网络失败 → 可读文案（英文源 + tr()，照 WebDavProvider::davRequestError
// 的形状，不透传 Response::error 原文）。体超限那句自拟且不带字节数（§2.8 文案纪律）
QString httpErrorText(const HttpClient::Response &resp, const QString &url) {
    const QString target = Util::Url::stripCredentials(url);
    if (resp.canceled)
        return trText(QT_TRANSLATE_NOOP("ExternalProvider", "Request canceled: %1")).arg(target);
    // 响应头已到即 status 为真实状态码——HttpClient 的 overflow 支与 timedOut 支
    // 都只留 error 非空可辨（status 同值），此处无法区分：文案不断言原因，慢链
    // 超时与体超限同落这句（§2.2 原本承诺的正是"报来源整体不可用"，变更记录
    // V7；WebDavProvider::davRequestError 的同形判据歧义归其本篇，不在本篇影响面）
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

bool ExternalProvider::supportsModify() const {
    return false; // 内容以取源为准，不可增删（视图侧整组禁用，失效清理走刷新）
}

bool ExternalProvider::supportsSort() const {
    return true; // 显式 true：不可增删但可按名称排序（§1.3 落的是"可排序"那一档）
}

bool ExternalProvider::enumeratesWholeTree() const {
    return true; // 恒真：一份文件一次取回即全知，逐层再取是重复劳动（§2.7）
}

// 路径定形（§2.1）：trim → 网络形态走 Util::Url::toNative（http(s) 经
// convergeHttpForm 收规范形态），本地形态走 Util::Url::toIdentity（相对→绝对、
// 原生分隔符、file:// 折盘符）。不补尾斜杠——外部路径指向一个文件，不是目录基址。
// 幂等是硬要求：两支对已定形输入均恒等（isNetworkStream / isHttp 判据对定形前后
// 同值，toNative / toIdentity 对各自产出恒等）
QString ExternalProvider::normalizeSource(const QString &text) {
    QString src = text.trimmed();
    if (src.isEmpty())
        return QString();
    return Util::Url::isNetworkStream(src) ? Util::Url::toNative(src) : Util::Url::toIdentity(src);
}

// 参数校验（§2.1）：四条判据见头文件注。第四条是本篇唯一的不对称处——本地路径
// stat 一下是常数代价，就地拒比让用户填完点 OK 再看着占位行猜原因来得诚实；
// 远端不做任何探测（承 N37"不做连通性探测"），"是不是一个合法清单"到枚举期由
// 取源与判别回答
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
    // 本地档：必须是磁盘上存在的普通文件；不设扩展名白名单——后缀不能定性正是
    // 本篇的立足点（§1.2），拿它当参数边界会把"无后缀 / .txt 而内容确是清单"的
    // 文件挡在门外
    const QFileInfo info(src);
    if (info.isDir())
        return trText(QT_TRANSLATE_NOOP(
            "ExternalProvider", "The path must point to a playlist file, not a directory."));
    if (!info.isFile())
        return trText(QT_TRANSLATE_NOOP("ExternalProvider", "The playlist file does not exist: %1"))
            .arg(src);
    return std::nullopt;
}

// group-title 多值拆分（§2.3）：iptv-org 以分号作多值分隔是既定约定——拆开才能
// 组出"Animation"这类真组，不拆则根层挂满 "A;B;C" 形态的怪组
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
    // BOM 剥离（枚举侧解码出的 U+FEFF 与测试直喂的串统一在此处理）
    QString body = text;
    if (body.startsWith(QChar(0xFEFF)))
        body.remove(0, 1);
    if (body.isEmpty())
        return out;
    const QStringList lines = splitLines(body);

    // ——第一遍：流判别（§2.2 白名单 + 头部区位次）与语法判别的旗标收集——
    // 流判据（ENDLIST 全文豁免位次）与"全文任一 #EXTINF / #EXT-X-"是全局存在性
    // 判据；txt 判据按 V10 收紧为只看**首个数据行**（非空且非 # 开头——装饰头
    // 不是数据行）是否为组行。先判后解才能保证解析起步就落对支（分组文本的首个
    // 条目行若按裸清单支整行当目标，判定翻转后无法就地重拆）。两遍都遍历同一份
    // 内存内行表，无额外 I/O，取源代价仍是"一次取源 + 一次解码"
    bool firstNonBlankSeen = false;
    bool firstIsExtm3u = false;
    bool firstDataLineSeen = false;
    bool firstDataIsGenreRow = false; // V10：首个数据行是组行 ⇒ 分组文本支
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
                out.isStreamSource = true; // 收尾标记：全文任意位置命中即算
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
            continue; // 注释 / 指令行不结束头部区
        }
        headerEnded = true; // 首条非注释目标行：头部区到此为止
        if (!firstDataLineSeen) {
            firstDataLineSeen = true;
            // V10：txt 判据只看首个数据行——按首逗号分两列、第二列 trim 后与
            // "#genre#" 逐字相等（非子串包含）即为组行
            const qsizetype comma = t.indexOf(QLatin1Char(','));
            firstDataIsGenreRow =
                comma >= 0 && QStringView(t).mid(comma + 1).trimmed() == QLatin1String("#genre#");
        }
    }
    if (out.isStreamSource)
        return out; // 流：解析产出整份丢弃（entries / groupOrder / warnings 恒空）

    // 语法分支持：V8 反转 + V10 收紧——**首个数据行**是组行即走分组文本支
    // （§2.2 自陈"两套行语法互斥、分组文本一个 # 指令都没有"：带 #EXTM3U /
    // #EXT-X-VERSION 装饰头的 tvbox 导出现实存在，装饰头不是数据行、跳过不计，
    // 其首个数据行仍是组行；中段夹带的 #genre# 形态行不作数）。裸清单支兜底：
    // .m3u 的原始形态就是逐行表，#EXTINF 属可选方言；HLS 媒体清单已在上面被判成
    // "流"、走不到这条支
    enum class Grammar { M3u, GroupedText, Bare };
    const Grammar grammar = firstDataIsGenreRow                     ? Grammar::GroupedText
                            : (firstIsExtm3u || seenM3uInstruction) ? Grammar::M3u
                                                                    : Grammar::Bare;

    // 解析侧旗标：warnings 只留给结构性异常，一类至多一条、串里不带条数（§2.8）
    bool malformed = false;
    bool hasVlcOpt = false;
    // 零条目警告的内容判据："空清单（只有 #EXTM3U）"是作者写就的空表、不带警告；
    // 除此之外的任何非空行（注释、异常行、组行）在零条目时都说明"这份东西大概
    // 不是清单"
    bool sawContent = false;

    if (grammar == Grammar::M3u) {
        // ——m3u 支：#EXTINF 开启待成形条目，目标行配对成 Entry——
        bool pending = false;
        QString pendingTitle;
        QString pendingTvgName;
        QStringList pendingGroups;
        for (const QString &raw : lines) {
            const QString t = raw.trimmed();
            if (t.isEmpty())
                continue;
            if (t.startsWith(QLatin1Char('#'))) {
                if (t.startsWith(QLatin1String("#EXTINF:"), Qt::CaseInsensitive)) {
                    if (pending)
                        malformed = true; // 上一条 EXTINF 到此仍无目标行（悬空）
                    // 属性区与标题区按"引号外的第一个逗号"分界（§2.3 口径）：逗号
                    // 之左 = 时长与属性区，之右 = 标题；引号感知使 tvg-logo /
                    // http-user-agent 值内的逗号不误开边界，标题自身含逗号也由此
                    // 不被截断。左段内时长（首个 token）与属性区以空白分界
                    const QString rest = t.mid(8); // "#EXTINF:" 之长
                    pendingTitle.clear();
                    pendingTvgName.clear();
                    pendingGroups.clear();
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
                    // 无逗号（如 "#EXTINF:-1 group-title=\"G\""）：整段当属性区、标题空
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
                    pending = true;
                    sawContent = true;
                } else if (t.startsWith(QLatin1String("#EXTVLCOPT"), Qt::CaseInsensitive)) {
                    // 取值不落地（§2.3 末条）；警告让缺口可见，沉默会把结构性缺口
                    // 误当成一堆坏链接
                    hasVlcOpt = true;
                    sawContent = true;
                } else if (!t.startsWith(QLatin1String("#EXTM3U"), Qt::CaseInsensitive)) {
                    // 其余 # 行（#EXTGRP、白名单外 #EXT-X-、注释）：静默忽略，
                    // 不记 warning——一份清单里未知指令成千上万行，逐行 warn 会把
                    // 弹窗淹掉；但它们算"内容"（零条目警告的判据）
                    sawContent = true;
                } // #EXTM3U 行（含 x-tvg-url 属性）忽略，且不算内容
                continue;
            }
            if (isAnomalousLine(raw)) {
                // 异常档②（与另两支同位：# 行不参与——一条超长 #EXTM3U 行不该
                // 凭空招 malformed）：不收入条目；不消耗待成形条目（EXTINF 的
                // 下一个合法目标行照常配对）
                malformed = true;
                sawContent = true;
                continue;
            }
            // 目标行：绝对串经 toNative 收口，相对串按基址定形；与待成形条目配对，
            // 无 #EXTINF 时 title 与组名留空、按文件序入列
            const QString target = normalizeTarget(raw, baseSource);
            sawContent = true;
            if (target.isEmpty()) {
                malformed = true; // 异常档③：定形后为空
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
            // 具名组登记（groupOrder = 首次出现序）：文件序即条目序，条目内按
            // group-title 的拆分序；组只随真实成形的条目登记——被异常档丢掉的
            // 行不带组，不产空组容器
            for (const QString &group : entry.groups)
                if (!out.groupOrder.contains(group))
                    out.groupOrder.append(group);
            out.entries.append(std::move(entry));
        }
        if (pending)
            malformed = true; // #EXTINF 到文件尾仍无目标行
    } else if (grammar == Grammar::GroupedText) {
        // ——分组文本支（tvbox / my-tv 系）：一个 # 指令都没有，靠 <组名>,#genre#
        // 行开组、<名>,<URL> 行给条目；产出仍进同一张 Parsed——"多一套行语法"，
        // 不是"多一种列表形态"——
        QString currentGroup; // 首个组行之前的裸行无组、落根层（组归属按位置）
        for (const QString &raw : lines) {
            const QString t = raw.trimmed();
            if (t.isEmpty())
                continue; // 空行与纯空白行：排版不是数据，忽略且不记警告
            if (t.startsWith(QLatin1Char('#'))) {
                // txt 支不解释 # 行：静默忽略；但算"内容"（零条目警告判据）
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
                // 裸行：base M3U 的标准形态照收，组归属按位置（组行之后归该组，
                // 不得被当异常丢弃）
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
                // 组行（判据 = 第二列 trim 后与 "#genre#" 逐字相等，非子串包含，
                // 变更记录 V9）：开一个具名组（组名 trim 后原样保留 emoji 与
                // 全半角；"🕘️更新时间"这类非频道类别的组不特殊处置，它就是一个组）
                if (col1.isEmpty()) {
                    malformed = true;
                    continue;
                }
                currentGroup = col1;
                if (!out.groupOrder.contains(col1))
                    out.groupOrder.append(col1);
            } else {
                // 条目行（V9 放宽：第二列不再要求以 http(s) 开头——rtmp:// /
                // rtsp:// 等 mpv 直播流 scheme 照收，与 m3u 支"原样收口"对齐；
                // 第二列走与全体目标同一条 normalizeTarget 定形支，网络 / 本地
                // 形态由它分派，定形后为空才落异常档③）
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
        // ——裸清单支：无头无扩展的 base M3U 本体，逐行一条路径 / URL，条目全无
        // 标题、无组 ⇒ 根层平铺——
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

    // 结构性异常的三条无计数警告（§2.8）：一类至多一条，弹窗由视图侧封顶机制处理
    if (out.entries.isEmpty() && sawContent)
        out.warnings.append(trText(QT_TRANSLATE_NOOP(
            "ExternalProvider", "No playable entries were found in this playlist.")));
    if (malformed)
        out.warnings.append(trText(
            QT_TRANSLATE_NOOP("ExternalProvider", "Malformed lines were skipped in this list.")));
    if (hasVlcOpt)
        out.warnings.append(trText(QT_TRANSLATE_NOOP(
            "ExternalProvider",
            "Some entries request per-entry HTTP headers that this plugin ignores; whether "
            "they play depends on the origin.")));
    return out;
}

// 枚举（§2.2 / §2.4 / §2.7）：工作线程调用。每次调用都重新取源——实例内无可变
// 状态，"缓存何时作废"在权威整树语义下问题本身消失。交付即这份来源该交付的
// 整棵树：根层 = 具名组容器 + 无组名条目，每个具名组的层内容各作一项 descendants
std::expected<PlaylistProvider::Enumeration, QString>
ExternalProvider::enumerate(const QStringList &parent) const {
    if (!parent.isEmpty()) {
        // 整树形态不响应非空 parent（N64 契约义务 2）：常规通路只以空 parent 调用
        // （整树批次的 parent 由 startLoadAsync 钉为根层）；收到非空即调用方违约，
        // 不解析、不应答该组
        Q_ASSERT(false);
        return Enumeration{};
    }
    if (m_sourcePath.isEmpty())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("ExternalProvider", "No path configured.")));

    // ——取源（唯一出口）——本地整份读入；http(s) 发一个 GET 走 exec：枚举任务的
    // 遮罩 cancelable=false（PlaylistWidget::startEnumAsync），属"无取消需求的
    // 调用点"，正是 HttpClient.h 文件头那段调用方纪律给 exec 留的口子，与
    // WebDavProvider::enumerate 同形。时限与体上限随全局配置，不另设旋钮
    QByteArray bytes;
    if (Util::Url::isHttp(m_sourcePath)) {
        const std::shared_ptr<HttpClient> client =
            registry() ? registry()->resolve<HttpClient>() : nullptr;
        if (!client) {
            // 装配错误：HttpClient 启动期必已 provide（N35 §2.3），永不命中；
            // 断言守候 + fail-safe，不读 INI 兜底（N36 修订 1 / N37 的装配纪律）
            Q_ASSERT(false);
            return std::unexpected(
                trText(QT_TRANSLATE_NOOP("ExternalProvider", "HTTP client is not available.")));
        }
        HttpClient::Request request;
        request.url = QUrl(m_sourcePath, QUrl::ParsingMode::TolerantMode);
        request.method = QByteArrayLiteral("GET");
        const HttpClient::Response resp = client->exec(request);
        // 成功判定：2xx；error 非空亦判失败（体超限——status 为真实状态码、body
        // 丢弃为空，只看状态码会把超限清单当空清单静默吞掉）
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
        // 读失败不静默：readAll 出错（坏扇区 / 共享违规 / 可移动盘被拔）时 bytes
        // 为空，不检查会被 parseSource 当空清单零警告吞掉——§2.8 把"读失败"明列
        // 为 std::unexpected 进占位行
        if (file.error() != QFileDevice::NoError)
            return std::unexpected(trText(QT_TRANSLATE_NOOP("ExternalProvider",
                                                            "Could not read the playlist file: %1"))
                                       .arg(m_sourcePath));
    }

    // 解码：UTF-8 一支——非法字节按替换字符降级，不因此判整份失败（一处坏字节
    // 不该废掉一万条频道，退化成 U+FFFD 可见即如实）；BOM 与换行拆分在 parseSource 内。
    // 不设 GBK 回退——猜错的形态是整表频道名变乱码且无从报告，比个别字符降级更坏
    const Parsed parsed = parseSource(QString::fromUtf8(bytes), baseSourceOf(m_sourcePath));

    if (parsed.isStreamSource) {
        // ——流分支（§2.4）：树只有根层，一行 = 清单自身，双击即把该 URL（或本地
        // .m3u8）整条交给 mpv，由它自己按 HLS 那一套放这条流。经普通工厂出生
        // （http 落 HttpEntity、本地落 LocalFileEntity，本地免费得到 exists() 失效
        // 判定），不注入显示名——title() 按既有派生即文件名
        Enumeration out;
        out.entities.push_back(PlaylistEntity::fromPath(m_sourcePath, registry()));
        return out;
    }

    // ——清单分支：一次整树交付，无索引、无第二份簿记——
    Enumeration out;
    out.warnings = parsed.warnings;
    out.containers.reserve(parsed.groupOrder.size());
    for (const QString &group : parsed.groupOrder) {
        Container container;
        // id = 组名：容器 id 只要求"提供者内稳定、可当 parent 路径段"，QStringList
        // 段序列对组名里的任何字符都无歧义；title = 组名；meta 留空（组没有修改
        // 时间可言）；path 留空（组不是可打开的位置，N47 口径"空 = 未知"）
        container.id = group;
        container.title = group;
        out.containers.push_back(std::move(container));
    }
    // 实体铸造 + 显示名三档回退链（§2.5）：#EXTINF 标题 → tvg-name → URL 末段
    // （剥 query / fragment，避免一排 index.m3u8?token=xx）；三档皆空则原样不动。
    // 同名不同 URL 天然得到不同实体与不同行——显示名不是身份，身份是定形后的
    // path()，本篇绝不做按名判重（§2.3 硬要求）
    auto makeEntity = [this](const Entry &entry) -> PlaylistEntity::Ptr {
        auto entity = PlaylistEntity::fromPath(entry.target, registry());
        QString name = entry.title;
        if (name.isEmpty())
            name = entry.tvgName;
        if (name.isEmpty())
            name = Util::Url::fileName(entry.target);
        if (!name.isEmpty())
            injectDisplayName(*entity, name);
        return entity;
    };
    // 每个具名组的层内容各作一项 descendants，键 = {组名}（长度 1、首段 ∈ 根层
    // containers；同层容器 id 互不相同由 groupOrder 去重天然保证，义务 3 兑现）。
    // 同一条目入多组时在每个组里各现造一份实体、互不共享实例（组-条目归属计数
    // 放大是 iptv-org 多值语义的代价，§2.3）
    for (const QString &group : parsed.groupOrder) {
        LevelData level;
        for (const Entry &entry : parsed.entries)
            if (entry.groups.contains(group))
                level.entities.push_back(makeEntity(entry));
        out.descendants.insert(QStringList{group}, std::move(level));
    }
    // 无组名条目摊在根层实体段（容器在前是模型不变式；不造合成组，§2.4）
    for (const Entry &entry : parsed.entries)
        if (entry.groups.isEmpty())
            out.entities.push_back(makeEntity(entry));
    return out;
}

void ExternalProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // N44：类型键经基类取 typeKey()
    settings.setValue(QStringLiteral("extPath"), m_sourcePath);
    // 排序规则随提供者持久化（与目录 / WebDAV 两族同形，含 None——显式记录用户
    // 未选择规则）
    settings.setValue(QStringLiteral("sortKey"), int(sortRule().key));
    settings.setValue(QStringLiteral("sortAscending"), sortRule().ascending);
}

std::shared_ptr<PlaylistProvider>
ExternalProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    // extPath 读回再定形一次：normalizeSource 幂等，写侧本就存定形串，此处只为
    // 兜住手改 INI 的形态漂移；缺键落空串，enumerate 以 "No path configured." 应答
    auto provider = std::make_shared<ExternalProvider>(
        normalizeSource(settings.value(QStringLiteral("extPath")).toString()), registry);
    const SortRule rule{static_cast<SortKey>(settings.value(QStringLiteral("sortKey"), 0).toInt()),
                        settings.value(QStringLiteral("sortAscending"), true).toBool()};
    provider->setSortRule(rule);
    return provider;
}

// 外部清单的参数页（§2.1）：一行 Path + 编辑框 + Browse…，HTTP 地址由用户直接
// 键入。过滤器只含 m3u 两后缀——不取全局 defaultMediaExtensions()，那份清单里
// 没有 m3u，且它是目录枚举的过滤器，两件事不共用。转接能力为 Unsupported ⇒
// 不调 enableTransitOption，页面无转接行
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
        // 说明文字在编辑框下方单独一行，灰色显示（与本地目录过滤器的备注同格式，
        // 变更记录 V6）：不开 wordWrap——备注按单行计入宽度，由它把表单区（进而
        // 对话框）撑宽；开换行会让 QLabel 变成"用高度换宽度"，布局反而不为它撑宽
        auto *pathNote = new QLabel(tr("Supports m3u and txt playlist formats, or an http(s) URL"));
        pathNote->setEnabled(false); // 禁用态即取调色板的灰，跟随主题不硬编码颜色
        // 备注右侧留 12px：它是本列最宽的一项，不留就会直接贴住窗口右边
        auto *noteRow = new QHBoxLayout;
        noteRow->addWidget(pathNote);
        noteRow->addSpacing(12);
        auto *pathField = new QVBoxLayout;
        pathField->addLayout(row);
        pathField->addLayout(noteRow);
        form->addRow(tr("Path"), pathField);
        layout->addLayout(form);
        // 同目录页：把被拉伸出来的余量压到内容之下，避免行间距被均摊撑大
        layout->addStretch(1);

        // 回填：seed 路径已定形，原样回显；normalizeSource 幂等保证"未改动"判得准
        if (m_seed)
            m_pathEdit->setText(m_seed->sourcePath());

        connect(browseBtn, &QPushButton::clicked, this, [this] {
            const QString file = QFileDialog::getOpenFileName(
                this, tr("Select Playlist"), QString(),
                tr("Playlist files (*.m3u *.m3u8 *.txt);;All files (*)"));
            if (!file.isEmpty())
                m_pathEdit->setText(file);
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
    // 取规范化参数值（hasChanges 与 build 共用同一出口）
    QString pathValue() const { return ExternalProvider::normalizeSource(m_pathEdit->text()); }

    const ExternalProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QLineEdit *m_pathEdit = nullptr;
};

ProviderSettingsPage *createExternalSettingsPage(const PlaylistProvider *seed,
                                                 const std::shared_ptr<ServiceRegistry> &registry,
                                                 QWidget *parent) {
    // 同类型由调用方（对话框）保证：seed 非空即本类型
    return new ExternalSettingsPage(seed ? static_cast<const ExternalProvider *>(seed) : nullptr,
                                    registry, parent);
}

// 本文件定义了 Q_OBJECT 类（ExternalSettingsPage），CMake AUTOMOC 要求显式包含
// 同名 .moc（否则 configure 期直接报错），按源文件名而非类名
#include "ExternalProvider.moc"

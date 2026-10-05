#include "pch.h"

#include "Util.h"

// N52：依赖方向 Util → StreamTransit 单向（StreamTransit 是叶子，不认识 Util）。
// 全仓 unwrap 只在此一处——"来源路径 → 内部形态"的唯一还原折叠点。
#include "StreamTransit.h"

namespace Util {

QString formatFileSize(qint64 bytes) {
    if (bytes < 0)
        return QString();
    // 阈值 1024 进位；KB 及以上保留一位小数，末尾 ".0" 去除
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    const double kb = bytes / 1024.0;
    if (kb < 1024.0)
        return QString::number(kb, 'f', 1) + QStringLiteral(" KB");
    const double mb = kb / 1024.0;
    if (mb < 1024.0)
        return QString::number(mb, 'f', 1) + QStringLiteral(" MB");
    return QString::number(mb / 1024.0, 'f', 2) + QStringLiteral(" GB");
}

namespace {
// 数字段解析：十进制累加并做溢出检测；成功产出数值段（前导零随解析抹平），
// 溢出则返回 false，调用方将该段降级为普通文本段
bool parseDigitRun(QStringView run, qlonglong *out) {
    qlonglong value = 0;
    for (QChar ch : run) {
        const int d = ch.unicode() - u'0';
        // max 加括号：规避 windows.h 的 min/max 宏污染
        if (value > ((std::numeric_limits<qlonglong>::max)() - d) / 10)
            return false;
        value = value * 10 + d;
    }
    *out = value;
    return true;
}
} // namespace

SegmentedName segmentName(QString name) {
    SegmentedName out;
    out.storage = std::move(name);
    const QStringView view(out.storage);
    const qsizetype n = view.size();
    qsizetype i = 0;
    while (i < n) {
        // 仅 ASCII 0-9 视为数字（裁决 1）：显式范围判断而非 isDigit()
        if (view[i] >= u'0' && view[i] <= u'9') {
            qsizetype j = i;
            while (j < n && view[j] >= u'0' && view[j] <= u'9')
                ++j;
            NameSegment seg;
            seg.isNumber = parseDigitRun(view.sliced(i, j - i), &seg.number);
            if (!seg.isNumber)
                seg.text = view.sliced(i, j - i);
            out.segs.append(seg);
            i = j;
        } else {
            qsizetype j = i;
            while (j < n && !(view[j] >= u'0' && view[j] <= u'9'))
                ++j;
            NameSegment seg;
            seg.isNumber = false;
            seg.text = view.sliced(i, j - i);
            out.segs.append(seg);
            i = j;
        }
    }
    return out;
}

int naturalCompare(const SegmentedName &a, const SegmentedName &b) {
    const qsizetype n = qMin(a.segs.size(), b.segs.size());
    for (qsizetype k = 0; k < n; ++k) {
        const NameSegment &sa = a.segs[k];
        const NameSegment &sb = b.segs[k];
        if (sa.isNumber != sb.isNumber)
            return sa.isNumber ? -1 : 1; // 跨类型：数值段恒小于文本段
        if (sa.isNumber) {
            if (sa.number != sb.number)
                return sa.number < sb.number ? -1 : 1;
        } else {
            const int c = sa.text.compare(sb.text, Qt::CaseInsensitive);
            if (c != 0)
                return c;
        }
    }
    // 前缀较短者小；完全相等返回 0（等键）
    return a.segs.size() == b.segs.size() ? 0 : (a.segs.size() < b.segs.size() ? -1 : 1);
}

bool openFileLocation(const QString &path, QWidget *parent) {
    Q_UNUSED(parent);
#ifdef Q_OS_WIN
    // 三步 Shell 调用；不得用 explorer /select 派生子进程（每次调用残留一个进程）
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(native.c_str());
    if (pidl) {
        const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr))
            return true;
    }
#endif
    // 非 Windows 平台或任一 Shell 步骤失败（含 PIDL 创建失败）：降级为打开所在目录
    const QString dir = QFileInfo(path).absolutePath();
    return !dir.isEmpty() && QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

QString formatTime(double seconds) {
    const int total = int(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
}

QString formatTimeHMS(double seconds) {
    const int total = int(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

QString formatTimePercent(double seconds, double duration) {
    if (duration <= 0.0)
        return formatTimeHMS(seconds);
    const int pct = int(seconds / duration * 100.0 + 0.5);
    return QStringLiteral("%1(%2%)").arg(formatTimeHMS(seconds)).arg(pct);
}

QString formatTimeHMSms(double seconds) {
    const qint64 totalMs = qint64(seconds * 1000.0 + 0.5);
    const qint64 ms = totalMs % 1000;
    const qint64 totalS = totalMs / 1000;
    const int h = int(totalS / 3600);
    const int m = int((totalS % 3600) / 60);
    const int s = int(totalS % 60);
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(int(ms), 3, 10, QLatin1Char('0'));
}

bool parseTimeHMSms(const QString &text, double *outSeconds) {
    const QString s = text.trimmed();
    if (s.isEmpty())
        return false;
    const QStringList parts = s.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (parts.size() != 3)
        return false;
    bool okH = false, okM = false, okS = false;
    const double h = parts[0].toDouble(&okH);
    const double m = parts[1].toDouble(&okM);
    const QString &sec = parts[2];
    const int dot = sec.indexOf(QLatin1Char('.'));
    if (dot < 0)
        return false;
    const QString frac = sec.mid(dot + 1);
    if (frac.size() != 3)
        return false;
    const double sVal = sec.toDouble(&okS);
    if (!okH || !okM || !okS || h < 0.0 || m < 0.0 || m >= 60.0 || sVal < 0.0)
        return false;
    if (sVal >= 60.0) // 秒（含小数）须 < 60
        return false;
    if (outSeconds)
        *outSeconds = h * 3600.0 + m * 60.0 + sVal;
    return true;
}

QString trimTrailingZeros(const QString &fixed) {
    QString s = fixed;
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

// 自动命名的单点（播放列表与书签共用，口径见 Util.h 声明处注释）：序号段须整段解析成
// 十进制整数才计入，故 "List 3b"、"List "（空尾段）一类不参与——它们不是本算法生出的
// 名字。另注 toInt 容忍首尾空白，"List 03 " 与 "List 3" 同样按 3 计入；为手改名多出来
// 的空白尾巴加一次 trim 不值得，而前缀比对本身是逐字节的
int nextSequenceNumber(const QString &prefix, const QStringList &existing) {
    int maxN = 0;
    const qsizetype skip = prefix.size();
    for (const QString &name : existing) {
        if (!name.startsWith(prefix))
            continue;
        bool ok = false;
        const int n = name.mid(skip).toInt(&ok);
        if (ok && n > maxN)
            maxN = n;
    }
    return maxN + 1;
}

// ——身份与形态一族（N56 收族：嵌套命名空间即边界，算法与收族前逐字一致）——
namespace Url {

namespace {
// N37：URL 匹配形态的编码收敛内核（约二十行纯字符串函数，不解析 URL 结构、
// 不依赖任何 Qt 版本行为）。扫 %HH：合法 hex 对中 unreserved 字符（ALPHA / DIGIT /
// '-' '.' '_' '~'）解码为字面量，其余统一十六进制为大写；畸形（% 后不足两位或
// 非 hex）原样保留。reserved 字符的转义不解码（%2F 必须原样保留，否则 / 的语义
// 就变）；不合并 . / .. 段、不碰 +。单调性：所有比对都是"两侧各落一次 key"，
// 收敛只可能让原本不等的形态相等，不会让原本相等的不等。
QString convergeUrlEscapes(const QString &s) {
    auto hexVal = [](QChar c) -> int {
        const char ch = c.toLatin1();
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    };
    static constexpr char kUpperHex[] = "0123456789ABCDEF";
    auto isUnreserved = [](int b) {
        return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
               b == '-' || b == '.' || b == '_' || b == '~';
    };
    QString out;
    out.reserve(s.size());
    const int n = s.size();
    int i = 0;
    while (i < n) {
        if (s.at(i) == u'%' && i + 2 < n) {
            const int hi = hexVal(s.at(i + 1));
            const int lo = hexVal(s.at(i + 2));
            if (hi >= 0 && lo >= 0) {
                const int byte = hi * 16 + lo;
                if (isUnreserved(byte)) {
                    out.append(QChar(byte));
                } else {
                    out.append(u'%');
                    out.append(QLatin1Char(kUpperHex[hi]));
                    out.append(QLatin1Char(kUpperHex[lo]));
                }
                i += 3;
                continue;
            }
        }
        out.append(s.at(i));
        ++i;
    }
    return out;
}

// N48：authority 段定位（URL 结构）——"://" 之后至首个 '/' / '?' / '#' 之间即 authority
// （query / fragment 不在此段）。剥离（stripCredentials）/ 插入（insertCredentials）
// 与收敛（convergeHttpForm）共用同一内核：定位规则必须逐字一致，各写一遍必然漂移。
// 入参是裸串、类型上看不出域，故名字带 Url（与仓库既有的"域 + 动作"命名同款）。
// authStart 为 authority 起始下标、authEnd 为 path / query / fragment 起点，
// 两者相等即 authority 为空。找不到 "://" 返回 false（调用方原样返回）
bool locateUrlAuthority(const QString &url, int *authStart, int *authEnd) {
    const int schemeEnd = url.indexOf(QStringLiteral("://"));
    if (schemeEnd < 0)
        return false;
    const int start = schemeEnd + 3;
    int end = start;
    while (end < url.size()) {
        const QChar c = url.at(end);
        if (c == u'/' || c == u'?' || c == u'#')
            break;
        ++end;
    }
    *authStart = start;
    *authEnd = end;
    return true;
}

// N48：authority 段 → userinfo（含尾随 '@'，无 '@' 即空串）与 host[:port]。
// userinfo 内的 '@' 必为编码形态，字面 '@' 是合法分隔符，故取最后一个。
// 入参是裸串（类型上看不出域），故名字带 Url；与 HttpClient 的 userInfoOf 不是一回事——
// 那一个收 QUrl（域由类型自明）、做的是 userinfo 段内部 user:pass 的切分（带解码），
// 本函数做的是 authority 段的切分
QString splitUrlAuthority(const QString &authority, QString *hostPort) {
    const int at = authority.lastIndexOf(u'@');
    *hostPort = at >= 0 ? authority.mid(at + 1) : authority;
    return at >= 0 ? authority.left(at + 1) : QString();
}
} // namespace

// N37：http(s) URL 的规范形态内核（实现见上；N56 收族并公开，原文件内静态
// convergeHttpUrlForm → convergeHttpForm）
QString convergeHttpForm(const QString &url) {
    int authStart = 0;
    int authEnd = 0;
    if (!locateUrlAuthority(url, &authStart, &authEnd))
        return url;
    const QString scheme = url.left(authStart - 3).toLower();
    const QString authority = url.mid(authStart, authEnd - authStart);
    QString hostPort;
    const QString userinfo = splitUrlAuthority(authority, &hostPort);
    // host[:port]：用最后一个 ':' 且其后为纯数字判定端口（IPv6 字面量带方括号，
    // 其 ':' 后非纯数字，不误判；裸 IPv6 authority 本就非法——须方括号，不构成
    // 误判场景）
    QString host = hostPort;
    int port = 0;
    const int colon = hostPort.lastIndexOf(u':');
    if (colon >= 0) {
        bool ok = false;
        const int p = hostPort.mid(colon + 1).toInt(&ok);
        if (ok && p > 0) {
            host = hostPort.left(colon);
            port = p;
        }
    }
    const bool defaultPort = (scheme == QLatin1String("http") && port == 80) ||
                             (scheme == QLatin1String("https") && port == 443);
    QString rebuilt = scheme;
    rebuilt += QStringLiteral("://");
    rebuilt += userinfo;
    rebuilt += host.toLower();
    if (port > 0 && !defaultPort)
        rebuilt += u':' + QString::number(port);
    rebuilt += url.mid(authEnd);
    return convergeUrlEscapes(rebuilt);
}

// N37：URL 形态处理仅对 http(s) 生效——percent / URL 语义只对 URI 路径成立，
// 其余网络协议（rtsp / srt / udp / edl 等）不参与 URL 规范化与编码收敛。
// N48：由匿名命名空间改为公开谓词——判据的持有者已不止本文件的四个调用点（提供者的
// 条目 scheme 白名单、将来的远端来源分派等），多份同口径拷贝必然漂移；它是纯读判定、
// 不参与任何比对产出，公开的代价远小于重复维护
bool isHttp(const QString &path) {
    const int schemeEnd = path.indexOf(QStringLiteral("://"));
    if (schemeEnd < 0)
        return false;
    const QString scheme = path.left(schemeEnd).toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

// N48：剥除 http(s) URL authority 中的 userinfo 段，其余字节逐字不动。纯字符串
// 实现，禁用 QUrl——产出一旦经 QUrl 重序列化，除 userinfo 之外的段也可能变形，
// 而本函数产出要参与跨来源逐字节比对（实体身份 / mpv 回显），多动一个字节就把
// 正常推送误判为外部变更。非 http(s)、无 "://"、authority 为空、无 userinfo 的输入
// 一律恒等返回（幂等，对称剥离会重复施加）。覆盖面不扩（N56 §2.3：非 http 网络
// URL 不剥，将来由该协议自己的实体负责）。禁止用于喂 mpv——喂 mpv 走提供者的
// playUrl（N48 §2.4；N52 起出口为 playUrl，N56 更名）
QString stripCredentials(const QString &url) {
    if (!isHttp(url))
        return url;
    int authStart = 0;
    int authEnd = 0;
    if (!locateUrlAuthority(url, &authStart, &authEnd) || authEnd <= authStart)
        return url;
    const QString authority = url.mid(authStart, authEnd - authStart);
    QString hostPort;
    if (splitUrlAuthority(authority, &hostPort).isEmpty())
        return url;
    return url.left(authStart) + hostPort + url.mid(authEnd);
}

// N48：与 stripCredentials 成对的插入侧——在 URL 的 authority 起点插入 `凭据段@`。
// 插入动作与前者的删除动作共用同一 authority 定位内核（`://` 偏移的规则只此一份），
// 其余字节取自原串，故 strip(insert(u, c)) == u 构造性成立。入参先剥一次凭据：带
// userinfo 的串绝不产出双段。encodedCredentials 的编码形态由调用方决定（约定与 QUrl 的
// userinfo 序列化一致，见 WebDavProvider::directPlayUrl），本函数只做拼接
QString insertCredentials(const QString &url, const QString &encodedCredentials) {
    // 凭据段内不得有字面 '@'——那会与定界符混同（userinfo 内的 '@' 必为编码形态）
    Q_ASSERT(!encodedCredentials.contains(QLatin1Char('@')));
    if (encodedCredentials.isEmpty() || !isHttp(url))
        return url;
    const QString clean = stripCredentials(url);
    int authStart = 0;
    int authEnd = 0;
    if (!locateUrlAuthority(clean, &authStart, &authEnd) || authEnd <= authStart)
        return url; // 无 "://" 或 authority 为空：绝不注入（避免产出 http://u:p@/path）
    return clean.left(authStart) + encodedCredentials + QLatin1Char('@') + clean.mid(authStart);
}

// N48：URL 的 path 段结束下标——首个 '?' 或 '#'（无则 size()）。纯字符串运算，不解析也
// 不校验：query / fragment 的语义由调用方按 URL 语境赋予（本地路径里的 '?' / '#' 是
// 普通字符，调用方不得误用）。两处产出共用同一"分界"口径——显示名取末段
// （fileName）与基 URL 的尾斜杠补位（WebDavProvider::normalizeRootUrl）
qsizetype pathEnd(const QString &url) {
    qsizetype cut = url.size();
    const qsizetype q = url.indexOf(QLatin1Char('?'));
    const qsizetype f = url.indexOf(QLatin1Char('#'));
    if (q >= 0)
        cut = qMin(cut, q);
    if (f >= 0)
        cut = qMin(cut, f);
    return cut;
}

// N56 收族更名 normalizePathForMatch → key："给外部串落键"的算法内核
QString key(const QString &path) {
    // 本地路径先相对→绝对（基于进程 CWD，不访问文件系统），再统一分隔符；
    // Windows 额外转小写。仅用于内部匹配；UI 展示与向 mpv 传递始终保持原样。
    const QString abs = toIdentity(path);
    if (isNetworkStream(abs)) {
        // N37：URL 编码收敛仅对 http(s)（与 fileName 同口径——URL 语义只对
        // URI 路径成立）；其余网络协议维持 N35 既有形态（分隔符归一、不转小写）。
        // 收敛是"两侧各落一次 key"，按协议族划分后每族内单调性依旧成立。
        // N48：http(s) 先剥凭据再收敛——键（书签 / 时长 / 续播 / mpv path 落键）恒为
        // 无凭据形态，改密后既有记忆继续有效；剥离只让原本不等的形态相等，故单调性
        // 保持
        if (isHttp(abs))
            return convergeHttpForm(stripCredentials(abs));
        return QDir::fromNativeSeparators(abs);
    }
    QString p = QDir::fromNativeSeparators(abs);
#ifdef Q_OS_WIN
    p = p.toLower(); // 网络路径已在上面分支返回，此处必为本地路径
#endif
    return p;
}

QString toNative(const QString &path) {
    // 网络分支：http(s) 走 URL 规范化（入口形态），绝不做 toNativeSeparators——
    // 后者会把 '/' 换成 '\'，对 URL 是破坏性的（N37 前的隐患，此前无调用点传
    // URL 未暴露）；解析失败退回原串（不抛错、不判失败）。其余网络协议原样
    // 返回——toNativeSeparators 同样破坏 URL，QUrl 重序列化对非 URI scheme
    // 的语义没有保证
    if (isNetworkStream(path)) {
        if (!isHttp(path))
            return path;
        const QUrl u = QUrl(path, QUrl::ParsingMode::TolerantMode);
        if (!u.isValid())
            return path;
        // Qt 完成 scheme / host 归小写与非 ASCII 的编码；默认端口剥离与转义收敛
        // 和匹配形态共用同一内核——"入口即规范"，等价书写不产出重复条目
        return convergeHttpForm(u.toString(QUrl::ComponentFormattingOption::FullyEncoded));
    }
    return QDir::toNativeSeparators(path);
}

// N52：更名 resolveLocalPath → resolveSourcePath（N56 收族再更名 → toIdentity）。
// 理由：① 它从来不只处理本地路径——网络/流路径在首行即原样返回，入参也包括手工
// 输入的目录根，"Local" 是既有误名；② N52 起本函数还要剥掉传输包装（oxbow:// →
// 条目身份），与 "local" 字面意思正相反。名字取"定形"义：入参是来自某个来源的
// 串（mpv 属性、文件对话框、拖放、用户输入），产出是已定形的条目身份。
// N56 §2.6 起：工厂不再代做本加工——定形是造实体一方的责任，本函数收归
// Util::Url 内部，调用方只经 key（首行折叠）或入口定形（toNative）间接用到它
QString toIdentity(const QString &path) {
    if (path.isEmpty())
        return path;
    // N52：剥传输包装——oxbow:// 转接 URI 还原为条目身份。unwrap 幂等（非本
    // 协议原样返回），故本折叠点可安全常驻首行，比对类（key）读点自动覆盖：
    // 走本函数或 key 的 mpv path 读取点自动还原，无需显式套函数。
    const QString unwrapped = StreamTransit::unwrap(path);
    if (isNetworkStream(unwrapped))
        return unwrapped; // 网络/流：保持原样，不绝对化
    // 本地（含 file://、file:/、盘符路径、相对路径、UNC）：file: 前缀先转本地，
    // 其余相对→绝对（基于进程 CWD 的纯字符串变换，不访问文件系统，文件不存在亦可），
    // 结果转原生分隔符（即显示路径形态；网络/流已在上一步返回原样，不受 toNativeSeparators 影响）
    QString local = unwrapped;
    if (unwrapped.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
        const QUrl u(unwrapped);
        local = u.toLocalFile();
    }
    return QDir::toNativeSeparators(QFileInfo(local).absoluteFilePath());
}

// N61 §2.1：判据与 PlaylistEntity::fromPath 那句断言互为镜像——断言放行的一侧
// （已定形）本函数一律逐字节原样，断言拦下的一侧（未定形）才施加 toIdentity。
// 分支条件不得扩：多认识任何一种"看起来已经是形态"的串都会让判据 R 失去逐字节
// 语义（N56 §2.5），少认一种即把未定形串喂进工厂（回显链上由 fromMpvEcho 落占位
// 兜住，插件自身的四个出口则撞断言显影）。参数与产出的分工见声明处注释
// 补不出形态者原样返回，由工厂的 fromMpvEcho 入口落 UnknownEntity（N61 变更记录第 1 条）
QString identityFromMpvEcho(const QString &echo) {
    return isDefiniteForm(echo) ? echo : toIdentity(echo);
}

// 是否网络/流路径（含 :// 且非 file: 前缀）：本地目录提供者据此拒绝、mpv 源据此保持原样。
// 用串前缀判定，避免 QUrl::scheme() 把 Windows 盘符 "C:\dir" 误判为 scheme、
// 也避免 QUrl::fromUserInput 把相对名（如 "a"）误判为非本地。
bool isNetworkStream(const QString &path) {
    return path.contains(QStringLiteral("://")) &&
           !path.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive);
}

// N54：本地/远端定性的单点——来源串经折叠（含 oxbow://）后不是网络/流形态即为本地。
// 既是 isMissingLocal 的前半，也是"这条来源离开所属列表还能不能播"的分界：本地
// 项就在磁盘上，远端项的播放形态依赖列表（N48 之后 URL 恒无凭据、N52 的转接 URI 依赖
// 列表端点）。空串判"非本地"（无从定性）
bool isLocal(const QString &path) { return !path.isEmpty() && !isNetworkStream(toIdentity(path)); }

// N54：本地来源存在性判据的单点。与实体侧 isLocalFile() && !exists() 同口径——
// 本地路径经 PlaylistEntity::fromPath 必出生 LocalFileEntity，其 exists() 即
// QFileInfo::exists(path())；网络与未知实体 exists() 恒真，故本函数对它们恒假
// （不可判定 ≠ 缺失）。取 QFileInfo::exists 而非 QFile::exists：后者在 Windows 对
// 目录判假，会把"书签指向目录"（mpv 能播）误判为缺失。
bool isMissingLocal(const QString &path) {
    const QString local = toIdentity(path);
    if (!isLocal(local))
        return false;
    return !QFileInfo::exists(local);
}

// N61 变更记录第 1 条：判据单点（见声明处注释）。判"形态"不判"磁盘"——纯字符串
// 谓词，不访问文件系统，故回刷热路径（心跳约 99 Hz 的正播路径出口）可放心直调
bool isDefiniteForm(const QString &path) {
    return !path.isEmpty() &&
           (QFileInfo(path).isAbsolute() || path.contains(QStringLiteral("://")));
}

// N37：播放项显示文件名（自 HttpEntity 构造内的同名逻辑收编，供标题 / OSD
// 取名共用，见头注）。percent-decode 仅对 http(s) 生效（isHttp，与
// toNative / key 的网络分支同口径）。N56 收族更名 playUrlFileName → fileName
QString fileName(const QString &identity) {
    if (isHttp(identity)) {
        // N48：query / fragment 的分界走共用内核（与 WebDavProvider::normalizeRootUrl
        // 的尾斜杠补位是同一口径）
        const QString spec = identity.left(pathEnd(identity));
        const qsizetype slash = spec.lastIndexOf(QLatin1Char('/'));
        const QString lastSeg = slash >= 0 ? spec.mid(slash + 1) : spec;
        return QUrl::fromPercentEncoding(lastSeg.toUtf8());
    }
    return QFileInfo(identity).fileName();
}

} // namespace Url

// ——杂项族（续）——

// N37：HTTP 日期解析（自 HttpEntity 迁入，行为不变——详见头注）
QDateTime parseHttpDate(const QByteArray &raw) {
    QString s = QString::fromLatin1(raw).trimmed();
    if (s.isEmpty())
        return {};

    // 字母时区后缀 → 等价 +0000（数字时区保留原样；无后缀即 0 时区，与
    // Last-Modified 恒为 UTC 墙钟的语义吻合）
    static const char *kZoneNames[] = {"GMT", "UTC", "UT"};
    for (const char *z : kZoneNames) {
        const QString suffix = QStringLiteral(" ") + QString::fromLatin1(z);
        if (s.endsWith(suffix, Qt::CaseInsensitive)) {
            s.chop(suffix.size());
            s += QStringLiteral(" +0000");
            break;
        }
    }

    const QDateTime dt = QDateTime::fromString(s, Qt::RFC2822Date);
    return dt.toLocalTime(); // invalid 时保持 invalid，调用方按"值未知"丢弃
}

// 发行时间解析：ISO 8601 优先，兼容常见格式（yyyy-MM-dd / yyyy/MM/dd / yyyy-MM / yyyy）；
// 全败返回无效 QDateTime
QDateTime parseReleaseDate(const QString &s) {
    if (s.isEmpty())
        return {};
    const QDateTime dt = QDateTime::fromString(s, Qt::ISODate);
    if (dt.isValid())
        return dt;
    const QDate d = QDate::fromString(s.left(10), Qt::ISODate);
    if (d.isValid())
        return {d.startOfDay()};
    const QStringList formats{
        QStringLiteral("yyyy-MM-dd"),
        QStringLiteral("yyyy/MM/dd"),
        QStringLiteral("yyyy-MM"),
        QStringLiteral("yyyy"),
    };
    for (const QString &fmt : formats) {
        const QDate d2 = QDate::fromString(s.left(fmt.length()), fmt);
        if (d2.isValid())
            return {d2.startOfDay()};
    }
    return {};
}

QString resolveMpvConfPath(const QString &pluginDir) {
    // 第一候选：<portable_config>/mpv.conf（插件目录上溯两级即 portable_config，
    // mpv 启动时若不存在会自动创建，必存在）
    const QString portableConf =
        QDir::cleanPath(pluginDir + QStringLiteral("/../..")) + QStringLiteral("/mpv.conf");
    if (QFileInfo::exists(portableConf))
        return portableConf;
    // 第二候选：%APPDATA%/mpv/mpv.conf
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty()) {
        const QString userConf = appdata + QStringLiteral("/mpv/mpv.conf");
        if (QFileInfo::exists(userConf))
            return userConf;
    }
    // 都不存在：返回创建目标（目录必存在，由 commit() 创建文件）
    return portableConf;
}

} // namespace Util

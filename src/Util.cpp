#include "pch.h"

#include "Util.h"

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

QList<NameSegment> segmentName(const QString &name) {
    QList<NameSegment> segments;
    qsizetype i = 0;
    const qsizetype n = name.size();
    while (i < n) {
        // 仅 ASCII 0-9 视为数字（裁决 1）：显式范围判断而非 isDigit()
        if (name[i] >= u'0' && name[i] <= u'9') {
            qsizetype j = i;
            while (j < n && name[j] >= u'0' && name[j] <= u'9')
                ++j;
            NameSegment seg;
            seg.isNumber = parseDigitRun(QStringView(name).sliced(i, j - i), &seg.number);
            if (!seg.isNumber)
                seg.text = name.mid(i, j - i);
            segments.append(seg);
            i = j;
        } else {
            qsizetype j = i;
            while (j < n && !(name[j] >= u'0' && name[j] <= u'9'))
                ++j;
            NameSegment seg;
            seg.isNumber = false;
            seg.text = name.mid(i, j - i);
            segments.append(seg);
            i = j;
        }
    }
    return segments;
}

int naturalCompare(const QList<NameSegment> &a, const QList<NameSegment> &b) {
    const qsizetype n = qMin(a.size(), b.size());
    for (qsizetype k = 0; k < n; ++k) {
        const NameSegment &sa = a[k];
        const NameSegment &sb = b[k];
        if (sa.isNumber != sb.isNumber)
            return sa.isNumber ? -1 : 1; // 跨类型：数值段恒小于文本段
        if (sa.isNumber) {
            if (sa.number != sb.number)
                return sa.number < sb.number ? -1 : 1;
        } else {
            const int c = QString::compare(sa.text, sb.text, Qt::CaseInsensitive);
            if (c != 0)
                return c;
        }
    }
    // 前缀较短者小；完全相等返回 0（等键）
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
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

QString normalizePathForMatch(const QString &path) {
    // 本地路径先相对→绝对（基于进程 CWD，不访问文件系统），再统一分隔符；
    // Windows 额外转小写。仅用于内部匹配；UI 展示与向 mpv 传递始终保持原样。
    const QString abs = Util::resolveLocalPath(path);
    QString p = QDir::fromNativeSeparators(abs);
#ifdef Q_OS_WIN
    p = p.toLower();
#endif
    return p;
}

bool Util::isNetworkStreamPath(const QString &path) {
    // 含 :// 且非 file: 前缀（覆盖 file:// 与 file:/）即视为网络/流。
    // 串前缀判定规避两陷阱：QUrl::scheme() 把 "C:\dir" 误判 scheme "c"；
    // QUrl::fromUserInput 把相对名 "a" 误判非本地。
    return path.contains(QStringLiteral("://")) &&
           !path.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive);
}

QString resolveLocalPath(const QString &path) {
    if (path.isEmpty())
        return path;
    if (Util::isNetworkStreamPath(path))
        return path; // 网络/流：保持原样，不绝对化
    // 本地（含 file://、file:/、盘符路径、相对路径、UNC）：file: 前缀先转本地，
    // 其余相对→绝对（基于进程 CWD 的纯字符串变换，不访问文件系统，文件不存在亦可），
    // 结果转原生分隔符（即显示路径形态；网络/流已在上一步返回原样，不受 toNativeSeparators 影响）
    QString local = path;
    if (path.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
        const QUrl u(path);
        local = u.toLocalFile();
    }
    return QDir::toNativeSeparators(QFileInfo(local).absoluteFilePath());
}

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

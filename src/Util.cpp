#include "Util.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QWidget>

#include <limits>

#ifdef Q_OS_WIN
#include <shlobj.h>
#include <qt_windows.h>
#endif

namespace Util {

QString formatFileSize(qint64 bytes)
{
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
bool parseDigitRun(QStringView run, qlonglong *out)
{
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

QList<NameSegment> segmentName(const QString &name)
{
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

int naturalCompare(const QList<NameSegment> &a, const QList<NameSegment> &b)
{
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

bool openFileLocation(const QString &path, QWidget *parent)
{
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

} // namespace Util

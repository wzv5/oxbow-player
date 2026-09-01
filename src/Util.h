#pragma once

#include "pch.h"

namespace Util {

// 文件大小自适应单位显示（B/KB/MB/GB，按量级就近选择）
QString formatFileSize(qint64 bytes);

// 数字化文件名的分段序列（N4）：文本段或数值段。
// 拆分依据仅为 ASCII 0-9；数值段按十进制解析（前导零不敏感，01 与 1 相等），
// 解析失败（超出 qlonglong 的超长数字串）时该段降级为普通文本段，
// 此时允许相邻出现多个文本段
struct NameSegment {
    bool isNumber;
    QString text;     // isNumber == false 时有效
    qlonglong number; // isNumber == true 时有效
};

// 预分段（装饰-排序：排序前对每项调用一次，比较仅在段序列间进行）
QList<NameSegment> segmentName(const QString &name);

// 段序列比较：逐段对比，同类型按类型内规则（数值比大小 / 文本大小写不敏感字典序），
// 跨类型时数值段恒小于文本段；前缀较短者小；全等返回 0（等键，交由稳定排序保序）
int naturalCompare(const QList<NameSegment> &a, const QList<NameSegment> &b);

// 打开文件所在目录并选中该文件。Windows 经 Shell API（不派生 explorer 进程）；
// 其他平台或失败时降级为打开所在目录。返回是否成功定位选中
bool openFileLocation(const QString &path, QWidget *parent);

// 时间格式化：自适应 mm:ss / hh:mm:ss（进度标签用）
QString formatTime(double seconds);

// OSD 专用：固定 hh:mm:ss（小时补零，00:01:05）
QString formatTimeHMS(double seconds);

// 进度标记统一格式 hh:mm:ss(pct%)（N8）：pct 四舍五入取整。
// duration<=0 时退化为仅时间。三处共用：进度条 tooltip / N7 #6 尾段 / N7 #7 主体
QString formatTimePercent(double seconds, double duration);

// N15：固定 hh:mm:ss.zzz（毫秒三位，四舍五入）
QString formatTimeHMSms(double seconds);

// N16：严格时间解析（书签编辑用），仅接受 hh:mm:ss.zzz（3 段、毫秒恰 3 位、分秒<60）；
// 合法返回 true 并写回秒数，否则 false
bool parseTimeHMSms(const QString &text, double *outSeconds);

// 通用：定点小数去末尾 0（"24.00" -> "24"、"44.10" -> "44.1"）
QString trimTrailingZeros(const QString &fixed);

// 路径规范化（仅内部匹配用：元数据仓键 / 书签键 / 回显比对）。
// 本地路径先相对→绝对（基于进程 CWD，不访问文件系统，文件不存在亦可），再统一分隔符；
// Windows 额外转小写。禁止用于 UI 显示或向 mpv 传递。
QString normalizePathForMatch(const QString &path);

// Qt 来源路径：拿到即转原生分隔符，不做绝对化 / cleanPath / 大小写处理。
// 用于文件对话框返回值、拖放 url、目录枚举等来源处一次性加工（这些来源本就返回绝对路径）。
inline QString nativePath(const QString &path) { return QDir::toNativeSeparators(path); }

// 本地路径相对→绝对（基于进程 CWD，纯字符串变换，不访问文件系统，文件不存在亦可），
// 结果转原生分隔符；网络/流路径（含 :// 或非 file scheme）保持原样。cleanPath 由 absoluteFilePath
// 已含。 既供 mpv 来源路径（propertyString / playlist
// filename），也供手动输入的目录根等来源处加工， 即"来源路径 → 显示路径"的唯一出口。
QString resolveLocalPath(const QString &path);

// 是否网络/流路径（含 :// 且非 file: 前缀）：本地目录提供者据此拒绝、mpv 源据此保持原样。
// 用串前缀判定，避免 QUrl::scheme() 把 Windows 盘符 "C:\dir" 误判为 scheme、
// 也避免 QUrl::fromUserInput 把相对名（如 "a"）误判为非本地。
bool isNetworkStreamPath(const QString &path);

// 发行时间解析：ISO 8601 优先，兼容常见格式（yyyy-MM-dd / yyyy/MM/dd / yyyy-MM / yyyy）；
// 全败返回无效 QDateTime
QDateTime parseReleaseDate(const QString &s);

// R3：mpv.conf 路径解析。从 pluginDir（plugin.ini 的绝对目录）上溯两级即
// portable_config（必存在目录），优先其下 mpv.conf；其次 %APPDATA%/mpv/mpv.conf；
// 都不存在返回 portable_config/mpv.conf 作为创建目标（目录必然存在，无需 mkpath）。
QString resolveMpvConfPath(const QString &pluginDir);

} // namespace Util

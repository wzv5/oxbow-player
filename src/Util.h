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
    QStringView text; // isNumber == false 时有效：引用拥有者文本的切片，不拥有内存
    qlonglong number; // isNumber == true 时有效
};

// 分段结果的拥有者：storage 持有原串（隐式共享浅拷贝），segs 中文本段视图指向 storage。
// 视图仅在拥有者存活期间有效（排序作用域内持有容器即保证）；建成后不得修改 storage
// （视图指向共享数据堆块，QString 移动不搬运堆块，故值返回搬移安全，修改则悬空）
struct SegmentedName {
    QString storage;
    QList<NameSegment> segs;
};

// 预分段（装饰-排序：排序前对每项调用一次，比较仅在段序列间进行）。
// 按值接参：调用方的 const&（如实体 fileName）只做一次隐式共享浅拷贝，
// 临时量则被移动进 storage；两种来源的视图都有效
SegmentedName segmentName(QString name);

// 段序列比较：逐段对比，同类型按类型内规则（数值比大小 / 文本大小写不敏感字典序），
// 跨类型时数值段恒小于文本段；前缀较短者小；全等返回 0（等键，交由稳定排序保序）
int naturalCompare(const SegmentedName &a, const SegmentedName &b);

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
// Windows 额外转小写。N35：网络路径不转小写（URL 大小写敏感，key 逐字节）。
// N37：http(s) 路径改走匹配形态收敛（scheme/host 归小写、去默认端口、unreserved
// 转义解码、其余转义十六进制统一大写），吃掉同一 URL 的等价书写差异；其余网络
// 协议（rtsp / srt / edl 等）维持 N35 既有形态（分隔符归一、不转小写）。
// N48：http(s) 分支先剥凭据（stripUrlCredentials）再收敛——键恒为无凭据形态，
// 凭据只在请求与播放两个使用点注入。
// 禁止用于 UI 显示或向 mpv 传递。
QString normalizePathForMatch(const QString &path);

// 是否 http(s) URL（仅面向前缀串判定，不解析）。串前缀 + scheme 归小写，
// 与 isNetworkStreamPath 同款：不用 QUrl::scheme()——它会把 Windows 盘符 "C:\dir"
// 误判为 scheme。
// "URL 语义只对 http(s) 生效"这一判据的唯一持有者：normalizePathForMatch /
// nativePath / playUrlFileName 的网络分支、stripUrlCredentials、WebDAV 条目的 scheme
// 白名单（N48）共用。N48 由匿名命名空间改为公开——判据持有者已跨文件，多份同口径
// 拷贝必然漂移
bool isHttpUrl(const QString &path);

// N48：剥除 http(s) URL authority 中的 userinfo 段，其余字节逐字不动。
// 非 http(s)、无 "://"、authority 为空、无 userinfo 的输入一律恒等返回（幂等）。
// 纯字符串实现（禁用 QUrl）：产出参与跨来源逐字节比对（实体 playUrl / mpv 回显），
// 经 QUrl 重序列化会在 userinfo 之外的段产生偏差，致比对失配。
// 用于匹配形态、回显比对、书签展示路径与用户可见文案。
// 禁止用于喂 mpv——喂 mpv 走提供者的 playbackUrl。
QString stripUrlCredentials(const QString &url);

// N48：与 stripUrlCredentials 成对的插入侧——在 http(s) URL 的 authority 起点插入
// `凭据段@`（authority 定位内核两处共用，偏移规则只此一份）。encodedCredentials 为**已编码**
// 的 `user` 或 `user:pass`（不含尾随 '@'，不得含字面 '@'；编码形态由调用方决定，约定与
// QUrl 的 userinfo 序列化一致），插入是纯字符串拼接——其余字节逐字不动，故
// strip(insert(u, c)) == u 构造性成立。
// 入参先剥一次凭据（带 userinfo 的串绝不产出双段）；凭据段为空、非 http(s)、无 "://"、
// authority 为空一律原样返回（绝不注入）。
// 只用于"喂 mpv 的播放形态"（提供者 playbackUrl）：实体 playUrl 与持久化恒无凭据。
QString insertUrlCredentials(const QString &url, const QString &encodedCredentials);

// N48：URL 的 path 段结束下标——首个 '?' 或 '#'（无则 size()）。纯字符串运算，不解析
// 也不校验：query / fragment 语义由调用方按 URL 语境赋予（本地路径里的 '?' / '#' 是普通
// 字符，不得误用）。两处产出共用同一分界口径：显示名取末段（playUrlFileName）与基 URL
// 的尾斜杠补位（WebDavProvider::normalizeRootUrl）
qsizetype urlPathEnd(const QString &url);

// Qt 来源路径：本地路径转原生分隔符；http(s) URL 经 QUrl 规范化（FullyEncoded，
// scheme/host 归小写、转义形态定型），解析失败退回原串；其余网络协议原样返回
// ——绝不做 toNativeSeparators（'/'→'\' 对任何 URL 都是破坏性的）；均不做
// 绝对化 / cleanPath / 大小写处理（N37）。
// 用于文件对话框返回值、拖放 url、目录枚举、WebDAV 条目 URL 等来源处一次性加工。
QString nativePath(const QString &path);

// 本地路径相对→绝对（基于进程 CWD，纯字符串变换，不访问文件系统，文件不存在亦可），
// 结果转原生分隔符；网络/流路径（含 :// 或非 file scheme）保持原样。cleanPath 由 absoluteFilePath
// 已含。 既供 mpv 来源路径（propertyString / playlist
// filename），也供手动输入的目录根等来源处加工， 即"来源路径 → 显示路径"的唯一出口。
QString resolveLocalPath(const QString &path);

// 是否网络/流路径（含 :// 且非 file: 前缀）：本地目录提供者据此拒绝、mpv 源据此保持原样。
// 用串前缀判定，避免 QUrl::scheme() 把 Windows 盘符 "C:\dir" 误判为 scheme、
// 也避免 QUrl::fromUserInput 把相对名（如 "a"）误判为非本地。
bool isNetworkStreamPath(const QString &path);

// N25：按序子序列判定——sub 的元素能否在 full 中按原序不跳序地逐一命中（双指针，O(n)）。
// 空序列是任何序列的子序列；两序列相等亦返回 true。
// 用于回刷的本地在途回显：识别"同一变更的在途中间态"（整表载入的部分列表是最终列表的前缀，
// 批量删除的部分列表以最终列表为子序列）
bool isOrderedSubsequence(const QStringList &sub, const QStringList &full);

// N49：图标按给定 alpha 淡化——取素材最大可用尺寸为合成底（源分辨率不低于
// 最终显示尺寸，后续由 QIcon 缩放显示不糊），以 QPainter::setOpacity 合成到
// 透明底新位图后再建 QIcon。透明度与缩放无关，故随后缩放不丢效果。
// 空图标原样返回（不做兜底，与"资源缺失即空图标"同口径）
QIcon fadedIcon(const QIcon &icon, qreal opacity);

// 发行时间解析：ISO 8601 优先，兼容常见格式（yyyy-MM-dd / yyyy/MM/dd / yyyy-MM / yyyy）；
// 全败返回无效 QDateTime
QDateTime parseReleaseDate(const QString &s);

// N37：HTTP 日期（IMF-fixdate，RFC 7231）解析（自 HttpEntity 迁入，行为不变）。
// Qt 的 Qt::RFC2822Date 不支持 GMT/UTC/UT 字母时区后缀（探针实证，N35 修订 3），
// 此处把字母后缀统一替换为等价 +0000 再交原生解析；无时区后缀按 0 时区解释
// （Last-Modified 恒为 UTC 墙钟，正合需求）。失败返回无效 QDateTime（值未知）。
QDateTime parseHttpDate(const QByteArray &raw);

// N37：播放项显示文件名（含后缀不含路径）——列表显示名与标题 / OSD 取名的唯一
// 出口。本地路径取末段（QFileInfo::fileName 口径）；http(s) URL 先剥 query /
// fragment（避免末段混入 "?token=xx"），取末段后 percent-decode（与列表显示名
// 一致，不出现 "%20" 形态）——解码仅限 http(s)：URL 编码语义只对 URI 路径成立，
// 其余协议（rtsp / srt / edl 等）与 UnknownEntity 列表侧同走末段原样。解后为空
// （如以 / 结尾）返回空串，调用方按"未知"处理；无路径且无尾斜杠时末段即主机名。
// 禁止用于匹配（匹配走 normalizePathForMatch）
QString playUrlFileName(const QString &playUrl);

// R3：mpv.conf 路径解析。从 pluginDir（plugin.ini 的绝对目录）上溯两级即
// portable_config（必存在目录），优先其下 mpv.conf；其次 %APPDATA%/mpv/mpv.conf；
// 都不存在返回 portable_config/mpv.conf 作为创建目标（目录必然存在，无需 mkpath）。
QString resolveMpvConfPath(const QString &pluginDir);

} // namespace Util

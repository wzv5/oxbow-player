#pragma once

#include "pch.h"

namespace Util {

// ——杂项族（与 URL / 路径无关的通用工具；N56 起与身份形态一族以命名空间分界）——

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

// 自动命名共用算法：既有名字中形如 "<prefix><十进制整数>" 的最大序号 +1（无匹配即 1）。
// 取"最大 +1"而非"第一个空号"——后者在已有 1 与 3 时会补出 2，删掉中间一条后新名字
// 又能复用其号，序号不随时间单调，名字就读不出先后。播放列表
// （PlaylistWidget::createNewList）与书签（MainWindow::onAddBookmark）共用这一条规则，
// 故单点在此。本函数只答"下一个号是多少"，成品名仍由调用方按自己的模板拼
// （tr("Playlist %1").arg(...)）——不在这里返回整串，是为了不把各家的译文模板吞掉。
// prefix 传**已翻译**的前缀（含其尾随分隔，如 tr("Playlist ")），比对逐字节；
// 序号段解析失败（"List 3b"、"List " 空尾段）不计入，但 toInt 容忍首尾空白，
// 故 "List 03 " 与 "List 3" 同样按 3 计入
int nextSequenceNumber(const QString &prefix, const QStringList &existing);

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

// R3：mpv.conf 路径解析。从 pluginDir（plugin.ini 的绝对目录）上溯两级即
// portable_config（必存在目录），优先其下 mpv.conf；其次 %APPDATA%/mpv/mpv.conf；
// 都不存在返回 portable_config/mpv.conf 作为创建目标（目录必然存在，无需 mkpath）。
QString resolveMpvConfPath(const QString &pluginDir);

// ——身份与形态一族（N56 收族）：嵌套命名空间即这一族的边界——
// 实体与提供者都从这里取内核；调用方只问角色（toIdentity 定形 / key 落键 /
// fileName 取显示名 / 判据定性），不再自己拼加工链。算法本身与收族前逐字一致
// （原 resolveSourcePath / normalizePathForMatch / stripUrlCredentials / … 十二个函数，
// 见 N56 §2.1 收族表）。
namespace Url {

// "来源串 → 已定形身份"的唯一出口（N52 更名 resolveLocalPath → resolveSourcePath、
// N56 收族更名 → toIdentity：它从不只处理本地路径——网络/流路径首行即原样返回；且
// N52 起还要剥掉传输包装（mpvqt:// → 条目身份），与 "local" 字面意思正相反）。
// 首行经 StreamTransit::unwrap 折叠还原（幂等，可重复施加）。
// 本地路径相对→绝对（基于进程 CWD，纯字符串变换，不访问文件系统，文件不存在亦可），
// 结果转原生分隔符；网络/流路径（含 :// 或非 file scheme）保持原样。cleanPath 由
// absoluteFilePath 已含。
// N56 起它不再是显示出口——显示归实体 displayUrl()，本函数只服务定形（造实体前的
// 入口加工与 key 的首行折叠）。
QString toIdentity(const QString &path);

// N61 §2.1：mpv 回显整形——"来源是 mpv 的 playlist 条目 filename 键"这一族的定形出口。
// 判据只有一条：**仅未定形者施加 toIdentity，余者逐字节原样返回**（定形与否问
// isDefiniteForm，本函数与工厂断言共用那一条判据，不各写一遍）。
// mpv 从不改写下发的串（相对进、相对出；绝对化只发生在 path 属性，而它按环境变量
// PWD 拼、可指向不存在的路径），故回显里的相对串必须先补形态才能进判据与工厂。
// 为什么两侧统一施加 toIdentity 不行：它首行做 StreamTransit::unwrap，那会把
// **转接形态与条目身份在判据上合并**——转接开关切换这类真失配将被判成一致，界面
// 永不收编。判据 R 的逐字节语义（N56 §2.5）只在"已定形者原样"这一支上保住。
// 补不出形态者（空串、裸 "file:"、单斜杠伪 URL）按"只补形态、不兜形"原样返回，
// 由工厂的**回显专用入口** PlaylistEntity::fromMpvEcho 落 UnknownEntity 兜底——
// 系统边界上的正确行为是降级，不是终止进程（插件只保证自己产出的路径正确）。
// 唯一例外是盘符相对串（D:a.mkv）：Qt 的 isAbsolute 判它已定形 → 原样返回且真进工厂。
// 消费点两处：PlaylistWidget::setPlaylist 的解析段、MpvController::currentPlayingPath。
QString identityFromMpvEcho(const QString &echo);

// 路径归一化（仅内部匹配用：元数据仓键 / 书签键 / 续播记忆 / 回显定位）。
// 本地路径先相对→绝对（基于进程 CWD，不访问文件系统，文件不存在亦可），再统一分隔符；
// Windows 额外转小写。N35：网络路径不转小写（URL 大小写敏感，key 逐字节）。
// N37：http(s) 路径改走匹配形态收敛（scheme/host 归小写、去默认端口、unreserved
// 转义解码、其余转义十六进制统一大写），吃掉同一 URL 的等价书写差异；其余网络
// 协议（rtsp / srt / edl 等）维持 N35 既有形态（分隔符归一、不转小写）。
// N48：http(s) 分支先剥凭据（stripCredentials）再收敛——键恒为无凭据形态，
// 凭据只在请求与播放两个使用点注入。
// 禁止用于 UI 显示或向 mpv 传递。
// N56 更名 normalizePathForMatch → key："给外部串落键"的算法内核；实体 key() /
// matches(串) 与本函数同一实现，不会漂移。
QString key(const QString &path);

// N37：播放项显示文件名（含后缀不含路径）——列表显示名与标题 / OSD 取名的唯一
// 出口。本地路径取末段（QFileInfo::fileName 口径）；http(s) URL 先剥 query /
// fragment（避免末段混入 "?token=xx"），取末段后 percent-decode（与列表显示名
// 一致，不出现 "%20" 形态）——解码仅限 http(s)：URL 编码语义只对 URI 路径成立，
// 其余协议（rtsp / srt / edl 等）与 UnknownEntity 列表侧同走末段原样。解后为空
// （如以 / 结尾）返回空串，调用方按"未知"处理；无路径且无尾斜杠时末段即主机名。
// 禁止用于匹配（匹配走 key）。
// N56 收族更名 playUrlFileName → fileName。
QString fileName(const QString &identity);

// N48：剥除 http(s) URL authority 中的 userinfo 段，其余字节逐字不动。
// 非 http(s)、无 "://"、authority 为空、无 userinfo 的输入一律恒等返回（幂等）。
// 纯字符串实现（禁用 QUrl）：产出参与跨来源逐字节比对（实体身份 / mpv 回显），
// 经 QUrl 重序列化会在 userinfo 之外的段产生偏差，致比对失配。
// 用于匹配形态、回显比对与实体显示形态（displayUrl）。
// 禁止用于喂 mpv——喂 mpv 走提供者的 playUrl（N52：直发形态经其调 directPlayUrl）。
// N56 收族更名 stripUrlCredentials → stripCredentials；覆盖面不扩（N56 §2.3：
// 非 http 网络 URL 不剥，将来由该协议自己的实体负责）。
QString stripCredentials(const QString &url);

// N48：与 stripCredentials 成对的插入侧——在 http(s) URL 的 authority 起点插入
// `凭据段@`（authority 定位内核两处共用，偏移规则只此一份）。encodedCredentials 为**已编码**
// 的 `user` 或 `user:pass`（不含尾随 '@'，不得含字面 '@'；编码形态由调用方决定，约定与
// QUrl 的 userinfo 序列化一致），插入是纯字符串拼接——其余字节逐字不动，故
// strip(insert(u, c)) == u 构造性成立。
// 入参先剥一次凭据（带 userinfo 的串绝不产出双段）；凭据段为空、非 http(s)、无 "://"、
// authority 为空一律原样返回（绝不注入）。
// 只用于"喂 mpv 的播放形态"（提供者 directPlayUrl，经 playUrl 模板方法调入）：
// 实体身份与持久化恒不含提供者注入的凭据（N56 §2.3：用户显式提供的凭据除外）。
// N56 收族更名 insertUrlCredentials → insertCredentials。
QString insertCredentials(const QString &url, const QString &encodedCredentials);

// N48：URL 的 path 段结束下标——首个 '?' 或 '#'（无则 size()）。纯字符串运算，不解析
// 也不校验：query / fragment 语义由调用方按 URL 语境赋予（本地路径里的 '?' / '#' 是普通
// 字符，不得误用）。两处产出共用同一分界口径：显示名取末段（fileName）与基 URL
// 的尾斜杠补位（WebDavProvider::normalizeRootUrl）。
// N56 收族更名 urlPathEnd → pathEnd。
qsizetype pathEnd(const QString &url);

// Qt 来源路径：本地路径转原生分隔符；http(s) URL 经 QUrl 规范化（FullyEncoded，
// scheme/host 归小写、转义形态定型），解析失败退回原串；其余网络协议原样返回
// ——绝不做 toNativeSeparators（'/'→'\' 对任何 URL 都是破坏性的）；均不做
// 绝对化 / cleanPath / 大小写处理（N37）。
// 用于文件对话框返回值、拖放 url、目录枚举、WebDAV 条目 URL 等来源处一次性加工
// （入口定形：造实体的一方自行保证传给工厂的是已定形串，N56 §2.6）。
// N56 收族更名 nativePath → toNative。
QString toNative(const QString &path);

// N37：http(s) URL 的规范形态内核——scheme / host 归小写、去默认端口
// （http 80 / https 443），userinfo 原样保留（大小写敏感，不解析），其余整段过
// 编码收敛。纯字符串操作，无解析失败路径；找不到 "://" 时原样返回。
// 入口形态（toNative）与匹配形态（key）共用同一内核——"入口即规范"，服务端以等价
// 书写指向同一文件时归一为同一身份，不产出重复条目。
// N48：userinfo 透传段保留——匹配形态在调用本函数之前已剥凭据（此处对匹配路径即
// 死代码），而入口形态不承担去凭据职责；撤掉透传等于让 toNative 也变成剥凭据。
// N56 收族并公开（原文件内静态 convergeHttpUrlForm → convergeHttpForm）：它是
// 形态算法内核的一部分，测试与将来可能的复用都从 Util::Url 取。
QString convergeHttpForm(const QString &url);

// 是否 http(s) URL（仅面向前缀串判定，不解析）。串前缀 + scheme 归小写，
// 与 isNetworkStream 同款：不用 QUrl::scheme()——它会把 Windows 盘符 "C:\dir"
// 误判为 scheme。
// "URL 语义只对 http(s) 生效"这一判据的唯一持有者：key / toNative / fileName 的网络
// 分支、stripCredentials、WebDAV 条目的 scheme 白名单（N48）共用。N48 由匿名命名空间
// 改为公开——判据持有者已跨文件，多份同口径拷贝必然漂移。
// N56 收族更名 isHttpUrl → isHttp。
bool isHttp(const QString &path);

// 是否网络/流路径（含 :// 且非 file: 前缀）：本地目录提供者据此拒绝、mpv 源据此保持原样。
// 用串前缀判定，避免 QUrl::scheme() 把 Windows 盘符 "C:\dir" 误判为 scheme、
// 也避免 QUrl::fromUserInput 把相对名（如 "a"）误判为非本地。
// N56 收族更名 isNetworkStreamPath → isNetworkStream。
bool isNetworkStream(const QString &path);

// 是否本地来源路径：经 toIdentity（含 mpvqt:// 折叠）后**不是**网络/流形态。
// 判据单点在此——isMissingLocal 用它，书签失效清理与书签跳转的本地/远端分流也
// 用它（两处同口径，不各写一遍 !isNetworkStream(toIdentity(...))）。
// N56 收族更名 isLocalSourcePath → isLocal；归属不变（书签层纯字符串，不归实体）。
bool isLocal(const QString &path);

// N54：本地来源是否已从磁盘消失——入参为显示路径，先经 toIdentity（含
// mpvqt:// 折叠），网络/流路径恒假（不可判定 ≠ 缺失），否则按 QFileInfo::exists 判定。
// 与实体侧 isLocalFile() && !exists() 同口径，是"这条来源能不能就地播"的判据单点
// （书签失效清理与书签跳转共用）。目录不算缺失（mpv 能播目录）。
// N56 收族更名 isMissingLocalSource → isMissingLocal。
bool isMissingLocal(const QString &path);

// N61 变更记录第 1 条：串是否**已定形**（能当条目身份用）——非空，且绝对路径或含
// "://"。这条判据的**唯一持有者**：PlaylistEntity::fromPath 那句断言、
// identityFromMpvEcho 的分支条件、fromMpvEcho 的兜底分派三处共用，不各写一遍。
// 盘符相对串（D:a.mkv）按 Qt 判据算已定形——不修，它落在"尽力而为"一侧。
bool isDefiniteForm(const QString &path);

} // namespace Url

} // namespace Util

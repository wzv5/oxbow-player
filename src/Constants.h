#pragma once

#include "pch.h"

// 跨模块共享常量唯一出处（N156 起）：改名只动这里。仅单模块消费的数值落所在
// .cpp 的文件内常量（不因"改名只动一处"的便利污染全局头）；主题派生色函数仍按
// N143 纪律归 AppearanceManager。
// 覆盖：窗口基名（初始/停止态）、关于对话框、applicationName、INI 文件名前缀。
namespace App {

// 窄字符名：QApplication 的 argv 签名要求可写 char*，故为非 const 数组（勿运行期改写）
inline char nameAscii[] = "oxbow";

// 显示名：QString 场景统一入口（标题/关于对话框/applicationName）
inline QString name() { return QStringLiteral("Oxbow"); }

// 项目主页：关于对话框版权行下的超链接，链接标题与 URL 同为此值。
// N151：repo 串单点——更新检查的 latest release API 由它拼出
inline QString projectRepo() { return QStringLiteral("wzv5/oxbow-player"); }
inline QString projectUrl() { return QStringLiteral("https://github.com/%1").arg(projectRepo()); }

// 配置文件名：小写基名（oxbow.ini），与翻译基名（oxbow_<locale>.qm）同族
inline QString iniFileName() { return QStringLiteral("oxbow.ini"); }

// ——扩展名默认清单（无点、小写；三张表 + 一份合并表）——
// 一律以逗号紧拼书写再 split：split 不去空白，逗号后加空格会产出坏过滤器。清单只出
// 扩展名表，对话框过滤器与 QDir 名称过滤器要的 *.ext 形态由 Util::extensionPatterns 加工

// 视频表：照抄 mpv `--video-exts` 的默认表（手册不载具体项，改动前经
// `--help=video-exts` 核对一次），与音频表无交集
inline QStringList defaultVideoExtensions() {
    return QStringLiteral("3g2,3gp,avi,flv,ivf,m2ts,m4v,mj2,mkv,mov,mp4,"
                          "mpeg,mpg,mxf,ogv,rmvb,ts,webm,wmv,y4m")
        .split(u',');
}

// 音频与字幕的回退两张（N96 手动添加流）：正路是每次添加时现读 mpv 的 --sub-auto-exts /
// --audio-exts（跟用户 mpv.conf 的自定义走），这两份只在属性读失败或读到空时兜底。
// 取值照抄 mpv 的默认表（手册不载具体项，改动前经 --help=<选项> 核对一次）
inline QStringList defaultSubtitleExtensions() {
    return QStringLiteral("ass,idx,lrc,mks,pgs,rt,sbv,scc,smi,srt,ssa,sub,"
                          "sup,utf,utf-8,utf8,vtt")
        .split(u',');
}

inline QStringList defaultAudioExtensions() {
    return QStringLiteral("aac,ac3,aiff,ape,au,dts,eac3,flac,m4a,mka,mp3,"
                          "oga,ogg,ogm,opus,thd,wav,wma,wv")
        .split(u',');
}

// 媒体表 = 视频 ∪ 音频：文件对话框过滤器、DirProvider 枚举与视频区拖放判据（N97）的
// 共用默认值。动态列表的扩展名是每 tab 可编辑参数，此处仅为初始预填来源（N9）。
// 两张表当前无交集，仍按拼接后去重保序落——防将来清单重叠产出重复过滤器
inline QStringList defaultMediaExtensions() {
    QStringList all = defaultVideoExtensions();
    for (const QString &ext : defaultAudioExtensions()) {
        if (!all.contains(ext))
            all << ext;
    }
    return all;
}

// ——N49 图标包——
// 素材落在 icons/<包名>/<语义键>.svg，包目录即"一套图"的替换单元。
// 各包按同一批语义键名整理文件——跨包的文件名差异在整理入库时吸收，
// 代码不做"语义键 → 包内文件名"映射表，换包即换目录
inline QString defaultIconPack() { return QStringLiteral("icons8"); }
inline QString iconLocatePlaying() { return QStringLiteral("locate-playing"); }
inline QString iconSearch() { return QStringLiteral("search"); }
inline QString iconPlaylist() { return QStringLiteral("playlist"); }
inline QString iconSettings() { return QStringLiteral("settings"); }
inline QString iconAdd() { return QStringLiteral("add"); }
inline QString iconRemove() { return QStringLiteral("remove"); }
inline QString iconPlay() { return QStringLiteral("play"); }
inline QString iconPause() { return QStringLiteral("pause"); }
inline QString iconStop() { return QStringLiteral("stop"); }
inline QString iconSkipBackward() { return QStringLiteral("skip-backward"); }
inline QString iconSkipForward() { return QStringLiteral("skip-forward"); }
// N143 字幕浏览器工具栏：偏移步进（快进 / 倒带）
inline QString iconFastForward() { return QStringLiteral("fast-forward"); }
inline QString iconRewind() { return QStringLiteral("rewind"); }
inline QString iconStream() { return QStringLiteral("stream"); }
inline QString iconVolume() { return QStringLiteral("volume"); }
inline QString iconVolumeMuted() { return QStringLiteral("volume-muted"); }
inline QString iconMoveUp() { return QStringLiteral("move-up"); }
inline QString iconMoveDown() { return QStringLiteral("move-down"); }
// 设置项逐项重置（单色 24×24，经 Ui::themedIcon 染色）
inline QString iconReset() { return QStringLiteral("reset"); }
// 时间回退语义（N145：字幕偏移重置）——是 time-rollback，不是通用 rollback，
// 不得挪作刷新等其它语义使用
inline QString iconTimeRollback() { return QStringLiteral("time-rollback"); }
// 解析单点：语义键 → 资源路径。将来加"图标包"设置项时只改 defaultIconPack 的取值来源
inline QString iconPath(const QString &key) {
    return QStringLiteral(":/icons/%1/%2.svg").arg(defaultIconPack(), key);
}

// 播放列表展开宽度默认值（客户区像素，A20）：展开时主窗口向右扩展该宽度；用户拖动过 splitter
// 把手则用进程内记忆宽度。 实测 win11 下 336 宽度正好可以让 16:9 的视频在最大化时播放区无黑边。
inline int playlistDefaultWidth() { return 336; }

// 视频区默认尺寸（A20）：启动时视频区恰好为此尺寸；同时作为视频区最小尺寸下限
// （占位符最小尺寸取此值，也是空间不足时压缩视频区的下限）。
// 宽度 480 为实测下限——再窄底部控制栏即显示不全，勿再下调
inline QSize videoDefaultSize() { return QSize(480, 320); }

// ——N35 远端请求设置缺省值——
// iniKey: remoteTimeoutSec（秒）；设置目录 fallback 与 PlaylistWidget 初装配同源。
// 缺省 10 秒：GitHub API 在慢网络下 5 秒常超时，更新检查与远端浏览共用此值（N151）
inline int remoteTimeoutSecDefault() { return 10; }
// iniKey: ignoreHttpsErrors（bool，默认关）
inline bool ignoreHttpsErrorsDefault() { return false; }

// ——N36 递归深度上限——
// iniKey: recursiveDepthLimit；设置目录 fallback / range 与装配侧读值同源
// 取值域：0 = 不递归（只收绑定目录内的文件），n = 最多深入 n 层子目录
// （修订 2 起 0 不再表示"不限"——无上限遍历遇上符号链接环会失控）
inline int recursiveDepthLimitDefault() { return 3; }
// 取值域上界：设置目录 SpinBox 的 range 与装配侧读值钳制共用一处，否则任一
// 单边改动都会让两处静默漂移。下界恒为 0（深度不能为负，0 = 不递归），
// 是语义本身而非配置值，故不抽常量、直接写死
inline int recursiveDepthLimitMax() { return 20; }

// ——N67 视频区光标自动隐藏——
// 播放中光标停在视频区且持续该时长无鼠标活动即隐藏（Qt::BlankCursor）；
// 任何鼠标活动立即恢复并重新计时
inline int cursorAutoHideMs() { return 3000; }

// ——N156 全程无框自绘窗框——
// 迷你模式标题栏高度
inline int miniTitleBarHeight() { return 24; }
// 自绘非客户区的外圈缩放热区带宽
inline int windowResizeBand() { return 6; }
// 自绘窗框的可见边缘宽度
inline int windowBorderWidth() { return 1; }
// 迷你等比缩放的视频区宽下限
inline int miniMinWidth() { return 200; }
// 迷你拖动吸附触发距离
inline int miniSnapDistancePx() { return 16; }

// ——N121 起播预适配——
// on_preloaded hook 内等待 GUI 线程完成窗口预适配的上限（毫秒）。正常适配在
// 几毫秒内完成；上限只兜 GUI 无法处理投递事件的极端情形，超时即放行播放，
// 预适配延后执行或交给 VIDEO_RECONFIG 校正
inline int preloadResizeTimeoutMs() { return 100; }

// ——插件 ini 设置项默认值单点——
// 设置目录 fallback 与全部读取点（plugin.cpp / MainWindow）同引此处；键名与取值语义
// 见 SettingsDialog 设置目录
inline QString languageDefault() { return QString(); }       // 空 = Auto
inline QString interfaceStyleDefault() { return QString(); } // 空 = Auto
inline QString colorSchemeDefault() { return QStringLiteral("system"); }
inline int volumeWheelStepDefault() { return 2; }
inline int volumeKeyStepDefault() { return 5; }
inline QString osdModeDefault() { return QStringLiteral("overlay"); }
inline bool hoverThumbnailDefault() { return true; }
inline bool quadraticVolumeCurveDefault() { return false; }
// N155：Tools 菜单是否前置 mpv menu-data 段（0.42 起 select.lua 会默认填充）
inline bool showMpvMenuDataDefault() { return false; }
// N157：视频区鼠标事件向 mpv 透传（缺省关；开启后视频区鼠标语义归 mpv）
inline bool forwardMouseEventsDefault() { return false; }

// ——N151 检查更新——
// iniKey: updateCheckInterval（整数天）；设置目录 fallback 与 UpdateChecker 读侧同源。
// 取值域：0 = 关；1 = 每天；7 = 每 7 天——读侧只认这三值，其余落 1
inline int updateCheckIntervalDefault() { return 1; }

// ——N154 osd-overlay 通道 id——
// 各通道共用 plugin.cpp 传入的同一个 mpv client：同一 handle 内 id 是唯一键，同 id
// 重发即替换、format=none 同 id 即清除，撞号是互相替换 / 清除而非叠字；跨 client 由
// mpv 按 owner 隔离，只挡外部脚本 / IPC，不构成本仓通道分立的理由。新增通道只在
// 末尾追加一项，靠自增取号、不显式写数值、不插队（插队会让既有项数值与旧构建 /
// 旧日志对不上）；别处不得再写 id 字面量
enum class OsdOverlayId : int {
    Message = 1001, // 基址：OsdController 一次性消息，避开脚本惯用的低位 id
    CacheStatus,    // 自增 = 1002：CacheStatusOsd 缓冲持续态
};

} // namespace App

#pragma once

#include "pch.h"

// 全局程序常量唯一出处：改名只动这里。
// 覆盖：窗口基名（初始/停止态）、关于对话框、applicationName、INI 文件名前缀。
namespace App {

// 窄字符名：QApplication 的 argv 签名要求可写 char*，故为非 const 数组（勿运行期改写）
inline char nameAscii[] = "mpv-qt";

// 显示名：QString 场景统一入口（标题/关于对话框/applicationName）
inline QString name() { return QString::fromLatin1(nameAscii); }

// 配置文件名：由名称派生（mpv-qt.ini）
inline QString iniFileName() { return name() + QStringLiteral(".ini"); }

// 媒体扩展名默认清单（无点、小写）：文件对话框过滤器与 DirProvider 枚举的共用默认值。
// 动态列表的扩展名是每 tab 可编辑参数，此处仅为初始预填来源（N9）
inline QStringList defaultMediaExtensions() {
    return {QStringLiteral("mp4"),  QStringLiteral("mkv"), QStringLiteral("avi"),
            QStringLiteral("mov"),  QStringLiteral("wmv"), QStringLiteral("flv"),
            QStringLiteral("webm"), QStringLiteral("ts"),  QStringLiteral("mp3"),
            QStringLiteral("flac"), QStringLiteral("aac"), QStringLiteral("ogg"),
            QStringLiteral("wav"),  QStringLiteral("m4a")};
}

// ——N49 图标包——
// 素材落在 icons/<包名>/<语义键>.svg，包目录即"一套图"的替换单元。
// 各包按同一批语义键名整理文件——跨包的文件名差异在整理入库时吸收，
// 代码不做"语义键 → 包内文件名"映射表，换包即换目录
inline QString defaultIconPack() { return QStringLiteral("icons8"); }
inline QString iconLocatePlaying() { return QStringLiteral("locate-playing"); }
inline QString iconSearch() { return QStringLiteral("search"); }
inline QString iconPlaylist() { return QStringLiteral("playlist"); }
// 可切换按钮的选中态：素材是矢量，换色只能另出一张（染色会把选中态钉成位图）
inline QString iconPlaylistOn() { return QStringLiteral("playlist-on"); }
inline QString iconSettings() { return QStringLiteral("settings"); }
inline QString iconFilterAdd() { return QStringLiteral("filter-add"); }
inline QString iconFilterRemove() { return QStringLiteral("filter-remove"); }
// 解析单点：语义键 → 资源路径。将来加"图标包"设置项时只改 defaultIconPack 的取值来源
inline QString iconPath(const QString &key) {
    return QStringLiteral(":/icons/%1/%2.svg").arg(defaultIconPack(), key);
}

// 播放列表展开宽度默认值（客户区像素，A20）：展开时主窗口向右扩展该宽度；用户拖动过 splitter
// 把手则用进程内记忆宽度。 实测 win11 下 322 宽度正好可以让 16:9 的视频在最大化时播放区无黑边。
inline int playlistDefaultWidth() { return 322; }

// 视频区默认尺寸（A20）：启动时视频区恰好为此尺寸；同时作为视频区最小尺寸下限
// （占位符最小尺寸取此值，也是空间不足时压缩视频区的下限）。
// 宽度 480 为实测下限——再窄底部控制栏即显示不全，勿再下调
inline QSize videoDefaultSize() { return QSize(480, 320); }

// ——N35 远端请求设置缺省值——
// iniKey: remoteTimeoutSec（秒）；设置目录 fallback 与 PlaylistWidget 初装配同源
inline int remoteTimeoutSecDefault() { return 5; }
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

// ——N66 全屏浮窗——
// 右缘唤出播放列表浮窗的触发带宽（客户区像素）：底区与右区以此为界互斥，
// 右下角 (W−t, H−h) 以下为死区，两个浮窗都不触发
inline int fullscreenEdgeTriggerWidth() { return 16; }
// 播放列表浮窗左缘拖宽热区宽度（客户区像素）
inline int fullscreenResizeHotspotWidth() { return 6; }

// ——N67 视频区光标自动隐藏——
// 播放中光标停在视频区且持续该时长无鼠标活动即隐藏（Qt::BlankCursor）；
// 任何鼠标活动立即恢复并重新计时
inline int cursorAutoHideMs() { return 3000; }

// ——N68 小窗模式——
// 默认客户区宽（逻辑 px）：每次进入恢复默认，不记忆
inline int miniDefaultWidth() { return 400; }
// 等比缩放宽下限（逻辑 px）：WM_SIZING 修正时的客户区宽钳制
inline int miniMinWidth() { return 200; }
// 吸附触发距离（逻辑 px）：拖动中窗口边与工作区边距离不超过此值即贴齐
// （与 N66 边缘触发带宽同风格）。仅 Windows 主路径生效——降级形态不做吸附
inline int miniSnapDistancePx() { return 16; }
// 降级路径收口去抖时长：move/resize 事件静默达此时长即视为松手，触发一次
// refitMiniToAspect（非 Windows 无原生松手信号，以事件静默模拟，见
// MainWindow::scheduleMiniRefit）；进窗后的贴角收口同用这一延时
inline int miniRefitDebounceMs() { return 250; }

} // namespace App

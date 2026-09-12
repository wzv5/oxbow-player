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

// 播放列表展开宽度默认值（客户区像素，A20）：展开时主窗口向右扩展该宽度；
// 用户拖动过 splitter 把手则用进程内记忆宽度
inline int playlistDefaultWidth() { return 310; }

// 视频区默认尺寸（A20）：启动时视频区恰好为此尺寸；同时作为视频区最小尺寸下限
// （占位符最小尺寸取此值，也是空间不足时压缩视频区的下限）。
// 宽度 480 为实测下限——再窄底部控制栏即显示不全，勿再下调
inline QSize videoDefaultSize() { return QSize(480, 320); }

// ——N35 远端请求设置缺省值——
// iniKey: remoteTimeoutSec（秒）；设置目录 fallback 与 PlaylistWidget 初装配同源
inline int remoteTimeoutSecDefault() { return 5; }
// iniKey: ignoreHttpsErrors（bool，默认关）
inline bool ignoreHttpsErrorsDefault() { return false; }

} // namespace App

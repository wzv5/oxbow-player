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
inline QStringList defaultMediaExtensions()
{
    return {QStringLiteral("mp4"), QStringLiteral("mkv"),  QStringLiteral("avi"),
            QStringLiteral("mov"), QStringLiteral("wmv"),  QStringLiteral("flv"),
            QStringLiteral("webm"), QStringLiteral("ts"),  QStringLiteral("mp3"),
            QStringLiteral("flac"), QStringLiteral("aac"), QStringLiteral("ogg"),
            QStringLiteral("wav"), QStringLiteral("m4a")};
}

} // namespace App

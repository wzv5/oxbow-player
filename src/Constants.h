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

} // namespace App

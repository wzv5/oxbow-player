#pragma once

#include "pch.h"

// 版本身份访问口（N84）：取值函数的数据全部来自构建期生成头 revision.h 的
// 对应宏（OXBOW_VERSION_STRING / OXBOW_REVISION_STRING / OXBOW_BUILD_TIME）。
// 本头是其他翻译单元的唯一入口——全仓只有 Version.cpp 允许 include revision.h
// （构建时刻每次构建都变，第二个直接消费者就等于把"每次构建必重编"扩散出去），
// 于是 revision 变化只重编 Version.cpp 一个小 TU，全仓最贵的 MainWindow.cpp 与
// 启动路径 plugin.cpp 永不受牵动。版本号与 hash 不进 Constants.h：那文件是
// 编译期常量单源且被 PCH 波及全部 TU，塞进去等于把"每次提交全量重编"写进设计。
namespace App {

// 完整展示串：0.1.0 (cfaf552-dirty)；hash 不可得（无 git / 无 .git / 源码包）时
// revision 落 unknown，串形不变
QString versionInfo();

// 构建时刻，UTC 指定的 QDateTime（宏形如 2026-10-06T14:30:00Z，Z 即 UTC 指定制）。
// 运行期只是把宏串解析成 QDateTime；本地时区换算与格式化是展示关切，归消费者
QDateTime buildDateTime();

// Qt 运行库版本，运行期经 qVersion() 取（有意不取编译期）：Linux 用户跑的是
// 发行版打包的 Qt，与开发机头文件版本本就不是同一份，报运行库版本才对得上诊断
QString qtVersion();

// 宿主 mpv 版本串（N102）：MpvController 构造期读 mpv-version 后 set 一次（入库即剥
// "mpv " 前缀与 tag 的 `v`），消费者只经 get 取值。未填充即空串
void setMpvVersion(const QString &v);
QString mpvVersion();

} // namespace App

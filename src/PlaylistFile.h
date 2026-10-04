#pragma once

#include "pch.h"

// N80：播放列表清单文件（PLS / M3U）的读写落点。导出（N80）与导入（N81）是一对，
// 共用同一批纯函数：本文件不依赖任何视图状态、不认识提供者，只按 path 后缀认
// 格式；PlaylistWidget 只留菜单与编排。
//
// 取串口径沿用 N79：url 一律原样写下、不做任何加工（本地为原生分隔符绝对路径，
// WebDAV 直发串含凭据）——取到的就是能直接播的那串。
//
// 不设格式枚举、不导出后缀清单：格式就是后缀、已在 path 里；分派是模块内部的
// 事。支持哪些后缀，设置过滤器的调用方自己最清楚——两侧不一致时 writeFile 回
// error 显影，不会静默出错（N80 §1）。ASX 档曾入冻结稿，实测兼容性极差删除
// （变更记录 1）；时长不进条目，导出元数据只有标题（变更记录 2）。
//
// 编进 mpvqtcore 与 mpvqt_testlib 两个源列表（纯函数族按既有归类同编测试库，
// N81 的解析用例与 N80 的写出用例都挂 tests/tst_PlaylistFile.cpp）。
namespace PlaylistFile {

// 单条目：url 恒填；title 恒为有效值（实体 title 未注入显示名时回退文件名）；
// 时长不导出（变更记录 2），两档时长位恒写 -1
struct Entry {
    QString url;
    QString title;
};

// 按 path 后缀分派写出（pls / m3u / m3u8，不区分大小写；m3u8 与 m3u 产出同一份
// 内容，后缀只是命名偏好）。error = 具体原因（打不开 / QSaveFile 提交失败
// errorString / 后缀不受支持），调用方只负责弹窗与日志。
std::expected<void, QString> writeFile(const QString &path, const QList<Entry> &tracks);

} // namespace PlaylistFile

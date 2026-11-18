#pragma once

#include "pch.h"

// N169：Material Symbols 图标字体单点——字体加载与 rlig 连字名清单解析都住这里，
// 全静态函数、不持有 GUI 状态；调用方只见出口，不见解析细节。
namespace MaterialIconCatalog {

// 图标字体 family，进程内解析一次；资源缺失返回空串
QString family();

// 名字是否在连字清单内，解析失败恒为 false
bool contains(const QString &name);

// 全部连字名，已排序；作补全模型与浏览器数据源
const QStringList &names();

// 纯解析：吃 sfnt 字节吐 rlig 连字名（已排序）；越界或表结构异常返回空表
QStringList parseFontLigatures(const QByteArray &data);

} // namespace MaterialIconCatalog

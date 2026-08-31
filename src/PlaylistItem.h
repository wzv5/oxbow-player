#pragma once

#include "pch.h"

// 播放列表条目的元数据对象（N4，N14 重构流存储；N18 精简）：只承载文件固有属性与会话期求得的媒体信息。
// path 字段仅作 UI 显示（原生分隔符形态）；内部匹配键是全局元数据仓的规范化路径。
// 视图状态（如正在播放标记）不入此。
// N18：会话态字段（tracks/chapters/title/album/artist/releaseDate/fileFormat）已从本结构移除，
// 改由各自真正归属的所有者持有（轨道 → MainWindow::m_tracks，章节 → MarkAggregator，
// 标签 / 发行时间 / 文件格式 → MediaInfoHelper 缓存，供属性对话框）。本结构仅保留：
//   path / icon / fileSize / created / modified  — 文件系统固有属性
//   duration                                    — 列表"总时长"绘制快照（mpv 写入）
struct PlaylistItem
{
    QString path;        // 原始路径（UI 显示用，保留 Windows 反斜杠形态）；仓的键是规范化路径
    QIcon icon;          // 系统默认图标；物化时必得（失效文件取通用图标）
    qint64 fileSize;     // 字节；物化时经 QFileInfo 自取
    QDateTime created;   // 创建时间；物化时自取
    QDateTime modified;  // 修改时间；物化时自取

    // ——以下为延迟求值字段：std::optional 为空即"未求值"——
    std::optional<double> duration;             // 总时长，秒（mpv 写入，列表"总时长"绘制用）
};
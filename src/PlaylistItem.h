#pragma once

#include "pch.h"
#include "MpvController.h"

// 播放列表条目的元数据对象（N4，N14 重构流存储）：只承载文件固有属性与会话期求得的媒体信息。
// path 字段仅作 UI 显示（原生分隔符形态）；内部匹配键是全局元数据仓的规范化路径。
// 视图状态（如正在播放标记）不入此。
struct PlaylistItem
{
    QString path;        // 原始路径（UI 显示用，保留 Windows 反斜杠形态）；仓的键是规范化路径
    QIcon icon;          // 系统默认图标；物化时必得（失效文件取通用图标）
    qint64 fileSize;     // 字节；物化时经 QFileInfo 自取
    QDateTime created;   // 创建时间；物化时自取
    QDateTime modified;  // 修改时间；物化时自取

    // ——以下均为延迟求值字段：std::optional 为空即"未求值"——
    std::optional<double> duration;         // 总时长，秒
    std::optional<QString> fileFormat;      // 真实文件格式（file-format，文件属性单设）
    std::optional<QString> title;           // 标题标签
    std::optional<QString> album;           // 专辑标签
    std::optional<QString> artist;          // 作者标签
    std::optional<QDateTime> releaseDate;   // 发行时间，date 标签解析失败即为空

    // N14：全量流存储（按 id 升序），含 selected 标记；属性对话框按 selected 区分当前/其他
    QList<MpvController::TrackInfo> tracks;

    // N15.1：章节列表（chapter-list 快照，原样存储不查重；
    // 严格 == 去重属菜单/命中层语义，此处为文件事实展示）
    QList<MpvController::ChapterInfo> chapters;
};

#pragma once

#include "pch.h"

// 播放列表条目的元数据对象（N4）：只承载文件固有属性与会话期求得的媒体信息。
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
    std::optional<QString> fileFormat;      // 真实文件格式（file-format 探测所得，非后缀推断）
    std::optional<QString> title;           // 标题标签
    std::optional<QString> album;           // 专辑标签
    std::optional<QString> artist;          // 作者标签
    std::optional<QDateTime> releaseDate;   // 发行时间，date 标签解析失败即为空

    // 视频流参数——多轨媒体只记录当前实际播放的轨（mpv 顶层属性语义即为此）。
    // 不缓存像素格式：硬解激活时 mpv 上报的是 GPU 表面格式（如 d3d11），非文件固有值
    std::optional<QString> videoCodec;      // 视频编码格式
    std::optional<QSize> videoResolution;   // 分辨率
    std::optional<double> videoFps;         // 帧率

    // 音频流参数——同上，仅当前实际播放的轨
    std::optional<QString> audioCodec;      // 音频编码格式
    std::optional<int> audioChannels;       // 通道数
    std::optional<int> audioSampleRate;     // 采样率，Hz
    std::optional<int> audioBitrate;        // 码率，bps（容器声明的静态分轨值；推送 0/缺失即无值）
};

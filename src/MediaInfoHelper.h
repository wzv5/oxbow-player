#pragma once

#include "pch.h"

// N18：MediaInfo 运行时动态集成——不依赖链接，头内 shim 自带 LoadLibrary 解析。
// 仅暴露静态接口；缓存键路径规范化由调用方传规范化键实现。
class MediaInfoHelper {
  public:
    struct Stream {
        enum class Kind { Video, Audio, Text, Other };
        Kind kind = Kind::Other;
        bool isDefault;
        QString title;
        QString language;
        QString codec;         // Format 优先，否则 CodecID
        qint64 bitrate = 0;    // bps
        qint64 streamSize = 0; // 字节；用于"大小"行（百分比相对文件总大小）
        double durationSeconds = 0.0;
        int width = 0;
        int height = 0;
        double frameRate = 0.0;
        int sampleRate = 0; // Hz
        int channelCount = 0;
    };

    struct Chapter {
        QString title;
        double timeSeconds = 0.0; // StartTime / TimeStamp_FirstFrame（毫秒 /1000）
    };

    struct Data {
        // ——General——
        QString format;
        std::optional<double> duration; // 秒
        QDateTime recordedDate;         // Recorded_Date，缺失试 Tagged_Date
        // ——标签组（General 同名字段）——
        QString title;
        QString album;
        QString performer;
        // ——多流——
        QList<Stream> streams;
        // ——章节（Stream_Other / Stream_Menu）——
        QList<Chapter> chapters;
    };

    // dll 是否可用：纯读 preload 发布的就绪位（原子量），不在工作线程触发加载
    static bool available();

    // N142：入口预载（仅 GUI 线程、先于任何元数据扫描）。configuredPath 去空白后为空 =
    // 自动态（按候选表发现）；非空 = 显式路径，相对写法以 configDir 为基准规范化为绝对
    // 路径，不存在或基名不符即判不可用、不回落自动加载。句柄永久保留不卸载；已加载后
    // 重复调用不改状态，只有解析出的加载目标与已加载那份不同才记日志、下次启动生效；
    // 未就绪时再次调用可重试
    static void preload(const QString &configuredPath, const QString &configDir);

    // 单文件解析；无 dll / 打开失败返回默认 Data（字段皆空/0）
    static Data parse(const QString &path);
};

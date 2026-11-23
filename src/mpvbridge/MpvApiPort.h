#pragma once

#include "pch.h"

struct FilterEntry;

// mpv 客户端 API 的跨线程面：client.h 除 wait_event 外都允许任意线程同步调用，
// 本接口按 MpvController 的原签名集中列出这一批，供引擎侧（枚举任务线程等）经
// 服务注册表取用——调用方不必另记一份"哪些方法能跨线程"的清单。
// 不收三类：信号（GUI 事件语义）、startEventThread（启动事件线程，启动期独占）、
// 静态 errorString（无实例状态，直接调原类即可）。
// 唯一实现是 MpvController；测试侧按本接口造替身。
class MpvApiPort {
  public:
    virtual ~MpvApiPort() = default;

    // input-bindings 条目；owner / comment 可缺
    struct InputBinding {
        QString key;         // 归一化键名（多键序列以 '-' 连接）
        QString cmd;         // 命令串（与源一致的原文）
        bool isWeak = false; // 真 = 内置（is_builtin）
        QString owner;       // 非空 = 脚本添加（取自区）
        QString section;     // 所属输入区
        QString comment;     // 出厂描述（可缺）
    };

    // input-bindings 的两档筛选结果：内置集与脚本集
    struct InputBindingTiers {
        QList<InputBinding> builtins;
        QList<InputBinding> scripts;
    };

    // menu-data 的单条目；shortcut 键不解析不使用
    struct MenuDataItem {
        QString type; // "separator" / "submenu" / 空
        QString title;
        QString cmd;
        QStringList state; // checked / disabled / hidden 的原值集合，未知值由消费方忽略
        QList<MenuDataItem> submenu;
    };

    // 轨道信息（track-list 解析产物）
    struct TrackInfo {
        qint64 id = 0;
        QString type; // video / audio / sub
        QString title;
        QString lang;
        QString codec;
        bool external = false;
        QString externalFilename;
        bool isDefault = false;
        // 语义是"正在解码"，不是"是不是主轨"——两条字幕可同时解码
        bool selected = false;
        // 同类型内的选择顺序（track-list/N/main-selection）：字幕 0 = sid、1 =
        // secondary-sid；未选中或由 lavfi-complex 选中时该键缺席，取 -1
        int mainSelection = -1;
        // ffmpeg 全局流下标（track-list/N/ff-index）；键缺席取 -1，判不可用，不猜下标
        int ffIndex = -1;
        bool image = false; // 位图字幕轨
        int demuxW = 0;
        int demuxH = 0;
        // on_preloaded 预适配的选轨与尺寸估算（demux 级字段）
        bool albumArt = false; // 内嵌封面
        int demuxCropW = 0;    // 容器 crop 后宽；0 = 无容器 crop
        int demuxCropH = 0;
        int demuxRotation = 0; // 容器旋转（顺时针，度）
        double demuxPar = 0.0; // 像素宽高比；0 = 未声明
        double demuxFps = 0.0;
        int demuxSamplerate = 0;
        int demuxChannelCount = 0;
        qint64 demuxBitrate = 0; // bps，0 表无值
        qint64 metadataBPS = 0;  // bps，0 表无值
    };

    // 章节信息（chapter-list 解析产物）
    struct ChapterInfo {
        double time = 0.0;
        QString title;
    };

    // 命令：单行字符串或参数数组，返回值是 mpv 错误码（0 成功）
    virtual int command(const QString &cmd) = 0;
    virtual int command(const QStringList &args) = 0;
    // 异步命令通道；reply_userdata 恒 0、回执不读
    virtual void commandAsync(const QStringList &args) = 0;
    // 同步命令并取回返回串（只认 MPV_FORMAT_STRING，失败交空串）
    virtual QString commandResultString(const QStringList &args) const = 0;
    // screenshot-raw 取内存原始图像；失败（含空闲无视频）交空 QImage
    virtual QImage screenshotRaw() = 0;
    // loadfile 唯一出口：选项由调用方显式给出，空 map 走字符串通道
    virtual int loadFile(const QString &url, const QString &appendMode,
                         const QVariantMap &options) = 0;
    // 推送会话的屏障读；false = 发射失败（不会产生 reply）
    virtual bool pushBarrier(quint64 gen) = 0;

    // 属性写；返回值 = 是否写入成功
    virtual bool setProperty(const QString &name, const QString &value) = 0;
    virtual bool setProperty(const QString &name, int value) = 0;
    virtual bool setProperty(const QString &name, double value) = 0;
    virtual bool setProperty(const QString &name, bool value) = 0;

    // 属性读：optional 一族区分"读取失败"与"合法空值"
    virtual QString propertyString(const QString &name) const = 0;
    virtual std::optional<QString> propertyStringOpt(const QString &name) const = 0;
    // 选项出厂默认值；可能不可用，失败交 nullopt
    virtual std::optional<QString> optionDefaultValue(const QString &name) const = 0;
    virtual double propertyDouble(const QString &name, double fallback) const = 0;
    virtual bool propertyFlag(const QString &name, bool fallback) const = 0;
    virtual qint64 propertyInt64(const QString &name, qint64 fallback) const = 0;
    // "在播哪一条"的唯一出口：playlist-pos → playlist/<pos>/filename 两步现读
    virtual QString currentPlayingPath() const = 0;

    // 结构化读：每次现读、不观察、不缓存
    virtual QList<FilterEntry> filterEntries(const QString &name) = 0;
    virtual std::optional<QList<FilterEntry>> filterEntriesChecked(const QString &name) = 0;
    virtual InputBindingTiers inputBindings() const = 0;
    virtual QList<MenuDataItem> menuData() const = 0;
    virtual QList<TrackInfo> readTracks() const = 0;

    // 配置路径：构造期求值后只读
    virtual QString mpvConfPath() const = 0;
    virtual QString inputConfPath() const = 0;

    // 视频尺寸：纯读，不写去重状态
    virtual QSize videoSize() const = 0;
    virtual QSize sourceVideoSize() const = 0;
};

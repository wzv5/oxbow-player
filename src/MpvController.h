#pragma once

#include "pch.h"

#include <mpv/client.h>

// 对 mpv client API 的 Qt 化封装：UI 只与本类交互，不直接触碰 mpv_handle。
// 所有方法必须在创建本对象的 GUI 线程中调用。
class MpvController : public QObject
{
    Q_OBJECT
public:
    explicit MpvController(mpv_handle *mpv, QObject *parent = nullptr);

    // 发送命令：单行命令字符串，或参数数组（推荐用于含路径的命令，避免转义问题）
    void command(const QString &cmd);
    void command(const QStringList &args);

    void setProperty(const QString &name, const QString &value);
    void setProperty(const QString &name, int value);
    void setProperty(const QString &name, double value);
    void setProperty(const QString &name, bool value);
    QString propertyString(const QString &name);
    // 实时读属性（N3）：失败返 fallback；pause 等标志属性必须走 FLAG 格式
    double propertyDouble(const QString &name, double fallback);
    bool propertyFlag(const QString &name, bool fallback);

    // N13：轨道信息（track-list 解析产物）
    struct TrackInfo
    {
        qint64 id = 0;
        QString type;               // video / audio / sub
        QString title;
        QString lang;
        QString codec;
        bool external = false;
        QString externalFilename;
        bool isDefault = false;
        bool selected = false;
        int demuxW = 0;
        int demuxH = 0;
        double demuxFps = 0.0;
        int demuxSamplerate = 0;
        int demuxChannelCount = 0;
        qint64 demuxBitrate = 0;    // bps，0 表无值
        qint64 metadataBPS = 0;     // bps，0 表无值
    };

    // N15：章节信息（chapter-list 解析产物）
    struct ChapterInfo
    {
        double time = 0.0;
        QString title;
    };

signals:
    void timePosChanged(double seconds);
    void durationChanged(double seconds);
    void pauseChanged(bool paused);
    void volumeChanged(double volume);
    // items 为 QVariantMap 列表，键：filename / title / current
    void playlistChanged(const QVariantList &items);
    void playlistPosChanged(int pos);
    // mpv 进入/离开空闲状态（列表播完或停止后无当前文件）
    void idleActive(bool active);
    // B9：文件卸载事件。naturalEof=true 表示自然播完（EOF）；
    // false 为 stop/quit/redirect/error 等其他卸载原因
    void endFile(bool naturalEof);
    // 导航器面板（N1）：video-zoom / video-pan-x / video-pan-y / 视频尺寸
    void videoZoomChanged(double zoom);
    void videoPanXChanged(double panX);
    void videoPanYChanged(double panY);
    void videoSizeChanged(int width, int height); // 0,0 表示无视频
    // 媒体元数据字典（N4）：键大小写因文件而异，接收方需做不敏感匹配
    void mediaMetadataChanged(const QVariantMap &metadata);
    // N14：文件格式（file-format，文件属性单设）
    void fileFormatChanged(const QString &format);
    // N13：轨道列表（track-list 全量推送）
    void tracksChanged(const QList<TrackInfo> &tracks);
    // N15：章节列表
    void chaptersChanged(const QList<ChapterInfo> &chapters);
    void mpvShutdown();

public:
    // 当前视频尺寸（N3）：当场重读 width/height 属性，不走缓存；
    // 返回 QSize(0,0) 表示无视频
    QSize videoSize() const;

    // B17：启动事件线程（阻塞 mpv_wait_event(-1)）。必须在 wid 等嵌入相关
    // 属性设置完成后调用：首次 mpv_wait_event 会解除 mpv 核心的启动门控
    // （fuzzy_initialized），核心随即为命令行文件初始化 vo——若此时 wid
    // 尚未生效，mpv 会先自建窗口再重建嵌入，产生窗口闪烁
    void startEventThread();

private:
    class EventThread; // 事件专用线程（定义见 .cpp）

    void handlePropertyChange(const mpv_event *event);

    mpv_handle *m_mpv; // 由 mpv 拥有，插件不销毁
    int m_videoW = 0, m_videoH = 0; // width/height 两属性合并缓存（N1）
    EventThread *m_eventThread = nullptr; // 随进程存活，不经 delete（与宿主同生命周期）
};

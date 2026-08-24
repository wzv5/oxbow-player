#pragma once

#include <QObject>
#include <QSize>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <mpv/client.h>

// 对 mpv client API 的 Qt 化封装：UI 只与本类交互，不直接触碰 mpv_handle。
// 所有方法必须在创建本对象的 GUI 线程中调用。
class MpvController : public QObject
{
    Q_OBJECT
public:
    explicit MpvController(mpv_handle *mpv, QObject *parent = nullptr);
    ~MpvController() override;

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
    // 导航器面板（N1）：video-zoom / video-pan-x / video-pan-y / 视频尺寸
    void videoZoomChanged(double zoom);
    void videoPanXChanged(double panX);
    void videoPanYChanged(double panY);
    void videoSizeChanged(int width, int height); // 0,0 表示无视频
    // 媒体元数据字典（N4）：键大小写因文件而异，接收方需做不敏感匹配
    void mediaMetadataChanged(const QVariantMap &metadata);
    // 当前活动流参数快照（N4）：任一被观察的流/容器属性变化即整体上报。
    // 键：fileFormat/videoCodec/videoW/videoH/fps/audioCodec/channels/sampleRate；
    // 值缺失的键表示当前不可用
    void streamInfoChanged(const QVariantMap &info);
    void mpvShutdown();

public:
    // 当前视频尺寸缓存（N3）：width/height 属性只在变化时推送，
    // 按需创建的导航器画布错过首推，构造时经此播种初始值
    QSize videoSize() const { return QSize(m_videoW, m_videoH); }

protected:
    void customEvent(QEvent *event) override;

private:
    static void wakeupCallback(void *ctx);
    void drainEvents();
    void handlePropertyChange(const mpv_event *event);

    mpv_handle *m_mpv; // 由 mpv 拥有，插件不销毁
    int m_videoW = 0, m_videoH = 0; // width/height 两属性合并缓存（N1）
    QVariantMap m_streamInfo;       // 流参数合并缓存（N4），变化即整体重发
};

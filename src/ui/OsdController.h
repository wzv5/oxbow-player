#pragma once

#include "pch.h"

class MpvController;

// N113 §1：OSD 三形态收口——MainWindow 决定「说什么」，本类决定「怎么显示」。
// 通道：mpv overlay（osd-overlay 自绘 ASS，缺键默认）/ mpv show-text（完全跟随
// 用户 --osd-*）/ Standalone（Qt 顶级透明 Tool 窗自绘），另有 Off 掐断插件自己的
// 全部消息。四态都不改变 mpv 自身的 OSD（seek 条 / 按键提示 / console / osd-msg*）。
class OsdController : public QObject {
    Q_OBJECT
  public:
    enum class Mode { Off, Overlay, ShowText, Standalone };
    // 同一条消息被多路信号触发时的合并取一，高者胜出
    enum class Priority { PlayPause, Stop, NowPlaying };

    OsdController(QWidget *hostWindow, QWidget *videoWidget, MpvController *controller);

    // "off" / "show-text" / "standalone" 精确匹配（先 trim、区分大小写），其余一律
    // Overlay（缺键默认；UI 写入恒小写，手改 INI 用其他写法一律落缺省形态）
    static Mode modeFromIniValue(const QString &raw);

    // 同值零副作用；离开当前形态先做该形态的清除（Overlay 发 format=none、
    // Standalone 隐叠加窗、ShowText 不追撤——已发出的那条由 mpv 按时限自行消失），
    // 三者都停 2s 定时器
    void setMode(Mode mode);
    // 小窗期（N68）：消息照收照丢、不补显——被抑制的消息不排队、退出后不恢复
    void setSuppressed(bool suppressed);

    void show(const QString &text); // 立即显示（overlay 形态的空串按"清除"处理）
    void queue(Priority priority, const QString &text); // 同事件循环内合并，取优先级最高一条
    // move / resize 后调用：独立形态把叠加窗贴到视频区矩形；overlay 形态按新 res
    // 重发正显示的那条（res 在下发那刻定死，尺寸变化不重发即被整条等比缩放）
    void reposition();

  private:
    void ensureOverlay();
    void flush();
    // ——overlay 通道（osd-overlay，N113 §3）——
    void showOverlay(const QString &text); // 组装 ASS 并下发（按视频区逻辑尺寸定 res）
    void sendOverlay(const QString &text, int w, int h); // 下发并记本次 res（重发的比对基准）
    void refreshOverlay();                               // 显示期内 res 变了：按新 res 重发同一条
    void clearOverlay();                                 // format=none 清除（同 id）

    QWidget *m_host = nullptr;             // 主窗：QObject 父 + 独立形态叠加窗父窗 + 字体来源
    QWidget *m_videoWidget = nullptr;      // 视频区：独立形态取全局矩形；overlay 形态取逻辑尺寸
    MpvController *m_controller = nullptr; // 两条 mpv 命令通道（osd-overlay / show-text）
    Mode m_mode = Mode::Overlay;           // 缺键即此形态，与 restoreSettings 的读法同口径
    bool m_suppressed = false;
    // ——独立形态（Qt 叠加窗）——
    QWidget *m_videoOverlay = nullptr; // 顶级浮窗容器，几何始终跟随 m_videoWidget
    QLabel *m_osdLabel = nullptr;      // OSD 文本（容器边距 12 + 标签内绘偏移）
    QTimer *m_osdTimer = nullptr;      // 2s 自动隐藏（各形态分派共用）
    // ——帧合并去重：同事件循环内多信号折叠为一次显示——
    bool m_coalescePending = false;
    QString m_pendingNowPlaying;
    QString m_pendingStop;
    QString m_pendingPlayPause;
    // ——overlay 通道状态：res 在下发那刻定死，显示期内尺寸变化须按新 res 重发——
    bool m_overlayActive = false; // 有正显示的一条（定时器 / 离态清除即落假）
    QString m_overlayText;        // 正显示那条的原文，供重发复用
    int m_overlayResW = 0;        // 上次下发的 res，重发的比对基准
    int m_overlayResH = 0;
    // ASS \fs：按主窗字体的 (ascent + descent) / units_per_EM 折算（\fs 与 pt 不同尺），
    // 构造期算一次缓存——字体度量是常量，主窗字体运行期不变（全仓无改主窗 / 应用字体处）
    double m_overlayFontSize = 0;
};

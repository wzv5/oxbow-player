#pragma once

#include "pch.h"

class MpvController;

// N153：缓冲状态 OSD——mpv 缓冲不足（seeking / demuxer-cache-state/underrun /
// paused-for-cache 三观察任一为真）期间在画面左下角持续显示「正在缓冲… 百分比 /
// 速度」两行。持续态，与 OsdController 的一次性消息通道分立——自持显隐状态机
// （持续满 1s 才显示）、1s 数值刷新与独立 overlay 通道
class CacheStatusOsd : public QObject {
    Q_OBJECT
  public:
    CacheStatusOsd(QWidget *hostWindow, QWidget *videoWidget, MpvController *controller);

    // osdMode != Off；Off 期间静默，正显示的立即清除
    void setEnabled(bool enabled);
    // 小窗期抑制；退出后按当前 paused 重新计满 1s 才恢复
    void setSuppressed(bool suppressed);
    // 显示期内视频区逻辑尺寸变了按新 res 重发（res 在下发那刻定死，同
    // OsdController::reposition）；不重启延迟、不改活跃态
    void reposition();

  public slots:
    void onSeekingChanged(bool seeking);
    void onCacheUnderrunChanged(bool underrun);
    void onPausedForCacheChanged(bool paused);

  private slots:
    void onDelayTimeout();
    void onRefreshTick();

  private:
    bool displayCondition() const;
    void updateState();
    void refreshNow(); // 现读两属性 → 组装 → 重发

    QWidget *m_host = nullptr;        // 主窗：QObject 父 + 字体来源
    QWidget *m_videoWidget = nullptr; // 视频区：overlay res 取其逻辑尺寸
    MpvController *m_controller = nullptr;
    // ——显示条件三标志（N153：三观察任一为真即视为缓冲，NONE 归 false）——
    bool m_seeking = false;  // seek 重启窗口（低层 seek + 重填，underrun 在此恒假）
    bool m_underrun = false; // demuxer 供包断流（起播首填 / seek 重填 / 播放中耗尽）
    bool m_paused = false;   // paused-for-cache（兜底 --demuxer-thread=no，underrun 恒假）
    bool m_enabled = true;
    bool m_suppressed = false;
    bool m_active = false;            // overlay 正显示中
    QTimer *m_delayTimer = nullptr;   // 1s 显示延迟；条件转假即停，不重启
    QTimer *m_refreshTimer = nullptr; // 1s 数值刷新（显示期常转，也是唯一节流手段）
    int m_percent = 0;                // 百分比粘性快照：单次缓冲 episode 内只升不降——
                                      // 低层 seek 会反复重置 ts 信息，duration 在不可用与
                                      // 微小值间抖动，取最大值吸收
    int m_resW = 0;                   // 上次下发的 res（重发的比对基准）
    int m_resH = 0;
};

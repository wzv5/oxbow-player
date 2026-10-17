#pragma once

#include "pch.h"

typedef struct mpv_handle mpv_handle;
class StreamRelay;
class QTimer;

// N107：缩略图第二 mpv core 的引擎。跑在专属 QThread（mpv_command 同步语义会在
// demux/seek 上阻塞，不能落 GUI 线程）；会话生命周期 = 一次悬停会话一套 core，
// 由 GUI 侧 ThumbHoverWatch 决定起止，本类只执行：
//   startSession：建核（wid 指向宿主窗）→ 预设选项 → install 转接协议（早于
//                 initialize，StreamRelay::install 的硬顺序）→ loadfile（带 start）
//   seekTo      ：去抖 + 精确 seek
//   endSession  ：先 mpv_terminate_destroy（阻塞至 core 拆完）——N103 销毁顺序的
//                 前一半，宿主窗的处置（本仓是一套窗复用，即隐藏）由 GUI 侧在
//                 sessionEnded() 之后做
// 任一步失败全程静默（不弹错、不刷屏），以 sessionEnded() 收场，GUI 侧按
// "本次悬停不显示"处理。队列无溢出之忧：低频排空事件队列（client.h 明示队列
// 有限，溢出会让异步请求失败）。
class ThumbEngine : public QObject {
    Q_OBJECT
  public:
    // relay 为主 core 同一实例（端点表不随 handle 分份）；寿命归 MainWindow
    explicit ThumbEngine(std::shared_ptr<StreamRelay> relay, QObject *parent = nullptr);
    ~ThumbEngine() override; // 引擎线程收尾时执行，core 拆解不落 GUI 线程

  public slots:
    // 一次悬停会话的开始。seq 是 GUI 侧的会话代次，原样随 sessionEnded 带回。
    // 硬解恒关（缩略图 seek 场景软解更快）。
    void startSession(quint64 seq, WId hostWid, const QString &path, double startTime);
    // 悬停时刻推进。会话未在飞时为空操作。
    void seekTo(double time);
    // 会话结束。完成后带代次发 sessionEnded()；重复调用安全。
    void endSession();

  signals:
    // core 已拆完，GUI 侧此时才可处置宿主窗口；载荷即该场会话的代次，GUI 侧按号
    // 比对（上一场 core 拆解耗时长，其迟到回执不得误杀新一场）
    void sessionEnded(quint64 seq);

  private:
    void teardownCore();
    bool loadFile(double startTime);

    std::shared_ptr<StreamRelay> m_relay;
    mpv_handle *m_handle = nullptr;
    quint64 m_seq = 0; // 在飞会话的代次，0 = 无
    QString m_path;
    double m_lastSeek = -1.0;       // 去抖基准：本场会话已定格的时刻
    QTimer *m_drainTimer = nullptr; // 事件队列排空节拍
};

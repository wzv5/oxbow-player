#pragma once

#include "pch.h"

#include "PlaybackReporter.h"

// N99：播放态快照——纯值类型 + 一把不外露的锁。它是"上报视图"而非主窗 UI 态的
// 镜像（主窗 C1 进度条 endFile 双双归零，这里只清身份、留 position / duration——
// Stop 的位置来源全靠这份保留；且驱动器工作线程直读 mpv 属性，GUI 被占住不影响
// 上报；position 只能采样，不进折叠也不进推送）。playing 不落存储：改派生
// isPlaying()——"此刻在播"的判据是折叠身份非空；会话态（有活动会话）是驱动器
// 内部事实，不进公开快照（拉取面问的是"在播吗、播到哪"，不是"在上报吗"）
struct PlaybackSnapshot {
    QUuid listUuid; // 来源列表身份（正播实体解析不到 / 判等不符即 nil）
    QString identity;
    double positionSeconds = 0;
    double durationSeconds = 0; // 0 = 未知
    bool paused = false;
    QVariantMap metadata;

    // 派生：在播 ⇔ 折叠身份非空。"线程未起但正播"与"Gated 但正播"两态自动答对
    // （折叠在这两态都照跑）；权威限于收集期（稳态 Unbooted 折叠未收敛，快照是
    // 全默认值——收集期权威的固有边界，需要全时域权威的消费者问主窗）
    bool isPlaying() const { return !identity.isEmpty(); }

    // 事件构造的唯一出口，Start / Stop / Progress 三相全从这份值出生——"事件构造
    // 一律读快照"从注释纪律变成类型上的唯一出口。唯一例外：Start 的 position 由
    // startSession 显式置 0，不经快照（转开路径此刻快照的 position 是 Gated 前的
    // 旧值，取它会抬报告者的节流基线、延后首个纠偏 Progress）
    PlaybackEvent toEvent(PlaybackEvent::Phase phase, const QString &sessionToken) const;
};

// 值 + 一把互斥锁，锁不外露：驱动器的七个散件成员与手动加锁纪律收成这一枚——
// 写侧 update（锁内改，并把改后的值一并交出）、读侧 copy（锁内取字段副本），
// 任意线程可读（拉取出口）。四条推送与采样在驱动器线程写，锁内只做字段搬运（µs 级），
// 无争用热点
class GuardedPlaybackSnapshot {
  public:
    PlaybackSnapshot copy() const;
    // 一次加锁完成"判—写—取"：取值规则要在写穿之后立刻用同一份值出生事件，void 出口
    // 只能写成 update 再 copy——两次加锁两遍搬运，把一次事务拆成两步就不再是类型上的
    // 单点（比对与写穿之间是否可能插入别的写者，也退回靠线程模型推理而非代码保证）
    PlaybackSnapshot update(const std::function<void(PlaybackSnapshot &)> &mutate);

  private:
    PlaybackSnapshot m_value;
    mutable std::mutex m_mutex;
};

#pragma once

#include "pch.h"

// N74：播放进度上报的事件与报告者抽象。值对象 + 单一虚入口，纯头。
//
// 契约（逐条即接口的一部分）：
//
// 1. 实现类必须：构造后不可变、不含 QObject、不持实体 / 视图指针（与 N52 §2.3
//    的 StreamEndpoint 契约同族）；只可持有构造期快照与服务注册表的 shared_ptr。
// 2. playbackEvent 在驱动器线程上被调用，按注册顺序逐个调用——不并发、不重入。
//    任一报告者的阻塞时长原样计入同一轮里其后所有报告者的延迟（head-of-line），
//    不是各报各的。
// 3. 允许阻塞：同步 SDK、同步 HTTP、只给同步入口的不透明第三方调用直接可用
//    （这是驱动器不驻 GUI 线程的第一理由）。但实现必须自带阻塞上限——HTTP 走
//    HttpClient 的 submit + wait(id, N) 切片上界（写法见 HttpClient.h 文件头）；
//    上界同时是退出尾巴的长度（GUI 只等 200ms 就软停，驱动器线程等当前调用返回）。
// 4. 允许抛：驱动器逐调用 try / catch 兜住，单个报告者失败不影响后续报告者与
//    播放。不得声明 noexcept——抛穿 noexcept 即 terminate，与本条意图相反。
// 5. 自过滤：不相关的事件（协议前缀不符 / 需要的 metadata 字段不在）便宜地自行
//    判断并返回，驱动器不做认领判定。**投递路由不在此列**：一条事件该投给哪一册
//    报告者由驱动器按来源列表判（`PlaybackReporterRegistry::claimsSource`）——报告者
//    只在自己被投到的范围里认领，它不认识列表身份，也不必认识。
// 6. 自节流：驱动器按节拍采样并逐次调用，报告者自己决定哪些采样要真正动作。
//    节拍实长含扇出阻塞，故节流判据取事件里的事实（positionSeconds 差值一类），
//    不取采样计数——计数会随服务器快慢一起漂。
struct PlaybackEvent {
    enum class Phase { Start, Progress, Stop };

    Phase phase;
    QString sessionToken; // 驱动器铸造的一次播放段 QUuid（WithoutBraces），跨事件关联键
    QString identity;     // N61 唯一出口产物：整形 + unwrap 后的身份串，形态原样携带，不代做拆解
    double positionSeconds = 0;
    double durationSeconds = 0; // 0 = 未知
    bool paused = false;
    QVariantMap metadata; // mpv media-metadata 快照（Title / Artist / Album…键随文件而异）
};

// 单一入口：未来新增 Phase（Pause / Seek…）只扩枚举，虚函数签名不动
class PlaybackReporter {
  public:
    virtual ~PlaybackReporter() = default;
    virtual void playbackEvent(const PlaybackEvent &) = 0;
};

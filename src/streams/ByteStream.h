#pragma once

#include "pch.h"

// N52 L2：抽象字节流——一条已打开、可顺序消费、按能力可寻位的源。
// 纯头，无 .cpp；接口层不含任何传输概念（无地址、无请求描述、无协议词汇），
// 这是"货币是流，不是请求"的落点：加一种传输 = 新增一个 L3 文件对，本头与
// mpv 适配层、协议层零改动。
//
// 实现类纪律（接口的一部分）：
//   - 构造后不可变（除流自身消费状态）；不含 QObject；
//   - 一条流一个实例，只被单一消费者线程按序驱动 read / seek / size，
//     外加另一条线程可调 cancel——cancel 与析构必须能与阻塞中的 read 安全并发；
//   - 任意线程可析构（最后一份 shared_ptr 可能落在消费者线程）。
class ByteStream {
  public:
    // 观测计数（仅供日志与统计）。reopens 单一计数，不分"重开/重连"两个词；
    // skip* 为跳读命中次数与累计丢弃字节数；pos / total 是当前位置与声明总长。
    struct Stats {
        qint64 pos = 0;
        qint64 total = -1;
        int reopens = 0;
        qint64 skipHits = 0;
        qint64 skipBytes = 0;
    };

    // read 哨兵（与传输实现内部哨兵值同形；实现侧以 static_assert 钉等，防漂移）。
    // > 0：交付字节数（短读合法）；kEof：按声明的总长取完（终态诚实，见下）；
    // kError：错误或撤销；kNoData：窗口内无字节、仍在途（仅作防御返回，实现
    // 通常以切片循环内部消化，不外抛）。
    static constexpr qint64 kEof = 0;
    static constexpr qint64 kError = -1;
    static constexpr qint64 kNoData = -2;

    // seek 哨兵：kSeekUnsupported = 实锤不可寻位（永久结论）；kSeekError =
    // 偶发失败（可重试）。成功即返回目标偏移（>= 0）。
    static constexpr qint64 kSeekUnsupported = -1;
    static constexpr qint64 kSeekError = -2;

    virtual ~ByteStream() = default;

    // 顺序取数。只在消费者线程调用。
    virtual qint64 read(char *out, qint64 maxBytes) = 0;

    // 能力申报只有两项：可寻位与总长。不可寻位 + 总长未知的源（直播、转码、
    // 会话型）由此完整表达，不需要任何新类型或新枚举——"这台服务器吃不吃
    // 按偏移重启"是传输的运行期事实，不属于本接口。
    virtual bool isSeekable() const { return false; }

    // 默认实现恒返 kSeekUnsupported（实锤不可寻位）；仅可寻位流覆写。
    virtual qint64 seek(qint64 to) {
        Q_UNUSED(to);
        return kSeekUnsupported;
    }

    // 声明总长；-1 = 总长未知。
    virtual qint64 size() const { return -1; }

    // 请求撤销：任意线程、不阻塞、必须能打断阻塞中的 read 与等头中的作业。
    // 撤销后的 read 按错误收尾，且撤销不算触发自愈重开。
    virtual void cancel() {}

    // 观测快照（纯虚：本层为纯头，无 .cpp）。
    virtual Stats stats() const = 0;
};

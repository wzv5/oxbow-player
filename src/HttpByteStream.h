#pragma once

#include "ByteStream.h"
#include "pch.h"

class HttpClient; // 仅前向声明：HttpClient.h 只出现在 HttpByteStream.cpp

// N52 L3：第一个传输实现——HTTP 取数与其全部策略（请求装配、按偏移重启、
// 206 校验、总长取定、背压水位、位置守恒、跳读与重开分岔、断连自愈、
// 无进展看门狗）。本层定位是"第一个传输"，不是"流转接状态机"：跳读、重启
// 与校验策略全都长在 HTTP 身上，故与传输无关的重启骨架虽在本文件内分节，
// 却不升入 L2。
//
// 文件内两节（D3 已裁决：不抽基类）：节 A 传输无关的重启策略（位置守恒、
// seek 分岔、断连自愈、cancel、Stats）；节 B HTTP 专有（Range 装配、206 硬
// 校验、总长取自 Content-Range / Content-Length、判 2xx、水位）。两节之间
// 只经 restartAt / pumpRead 两个私有钩子相连。第二个"可偏移重启"的传输
// 出现时再做机械剥离。

// HTTP 请求描述：只活在 L3 文件对内，不是接口货币——请求描述一旦升入 L2，
// 就等于宣布"本插件只支持 http"。
struct HttpStreamTarget {
    QString url;                            // 已含鉴权装配的完整地址
    QList<QPair<QString, QString>> headers; // 附加头（鉴权头等）
    qint64 bufferBytes = 0;                 // 流式背压水位；<= 0 取 HttpClient 内置默认
};

// 窄取数接口：HttpByteStream 的全部 HTTP 触点（fake 可得；真实实现 HttpFeed
// 是本文件对内的私有类，落 HttpClient 的五个静态面）。语义要点：
//   - submit 只提交不等头，立即返回；startOffset > 0 时装配按偏移重启的
//     请求头，== 0 时为首播请求（不带该头）。
//   - awaitHeaders 真=响应头已到达或作业已终态；假=窗口耗尽仍在途（调用方
//     负责 abort 收尾）。
//   - read 返回 ByteStream::kEof / kError / kNoData 同值哨兵（实现侧以
//     static_assert 与 HttpClient::kRead* 钉等）。
//   - totalBytesOf：206 取分段总长；无声明返回 -1。只认最终落点响应。
//   - abort 任意线程可调、不阻塞、只认句柄不认流状态——等头阶段的撤销同样可达。
class IHttpFeed {
  public:
    // 不透明作业句柄基类：实现各自携带私有状态，调用方只持有不窥探。
    struct Job {
        virtual ~Job() = default;
    };
    using JobPtr = std::shared_ptr<Job>;

    virtual ~IHttpFeed() = default;

    virtual JobPtr submit(const HttpStreamTarget &target, qint64 startOffset) = 0;
    virtual bool awaitHeaders(const JobPtr &job, int timeoutMs) = 0;
    virtual qint64 read(const JobPtr &job, char *out, qint64 maxBytes, int timeoutMs) = 0;
    virtual int statusCodeOf(const JobPtr &job) const = 0;
    virtual qint64 totalBytesOf(const JobPtr &job) const = 0;
    virtual void abort(const JobPtr &job) = 0;
};

class HttpByteStream final : public ByteStream {
  public:
    // 测试缝（只传值、不定值）：0 即取文件内常量（D9：阈值不进头文件公共面，
    // 数值字面量只出现在 .cpp 常量处）。产品调用一律不传。
    struct Timeouts {
        int startIdleMs = 0; // 等响应头零进展上限
        int readIdleMs = 0;  // read 连续零字节上限
    };

    // 起流工厂（open 期、消费者线程一次性调用）：提交首播作业、等响应头
    // （有界）、判 2xx、取声明总长并完成能力申报。失败即整条流失败。
    static std::expected<std::shared_ptr<HttpByteStream>, QString>
    open(std::shared_ptr<IHttpFeed> feed, const HttpStreamTarget &target);
    static std::expected<std::shared_ptr<HttpByteStream>, QString>
    open(std::shared_ptr<IHttpFeed> feed, const HttpStreamTarget &target, const Timeouts &timeouts);

    // 产品路径的 feed 装配点：把注册表 resolve 到的 HttpClient 包成 IHttpFeed
    // （真实 HttpFeed 定义在本文件对内）。测试注入 fake。
    static std::shared_ptr<IHttpFeed> makeFeed(const std::shared_ptr<HttpClient> &client);

    ~HttpByteStream() override;

    // ByteStream 接口
    qint64 read(char *out, qint64 maxBytes) override;
    bool isSeekable() const override;
    qint64 seek(qint64 to) override;
    qint64 size() const override;
    void cancel() override;
    Stats stats() const override;

  private:
    HttpByteStream(std::shared_ptr<IHttpFeed> feed, const HttpStreamTarget &target,
                   const Timeouts &timeouts);

    // —— 节 A / 节 B 的公共骨架 ——
    // 探测式重启：旧作业原位不动，新作业按目标偏移提交并登记探测槽（登记即
    // 可被 cancel 打到），等头有界，206 校验通过才晋升并拆旧作业。
    enum class RestartResult { Promoted, RangeUnsupported, Failed, Canceled };
    RestartResult restartAt(qint64 to);

    // read 侧骨架：切片循环等待、cancel 检查、看门狗、早断三态与自愈。
    qint64 pumpRead(char *out, qint64 maxBytes);
    bool healRestart();

    bool awaitHeadersSliced(const IHttpFeed::JobPtr &job, int budgetMs);
    int startIdleTimeout() const;
    int readIdleTimeout() const;

    std::shared_ptr<IHttpFeed> m_feed;
    HttpStreamTarget m_target;
    Timeouts m_timeouts;

    // 一把 mutex 只保护"取/换作业槽、pos、旗"的瞬间；feed 调用一律在锁外
    // （否则 cancel 拿不到锁，read 挂死无从解救）。stats() 是 const 读口，故
    // mutex 与统计块均 mutable。
    mutable QMutex m_mutex;
    IHttpFeed::JobPtr m_current; // 当前取数作业
    IHttpFeed::JobPtr m_probe;   // 探测作业（重启验证期短暂存在）
    qint64 m_pos = 0;
    qint64 m_total = -1; // 响应头声明的总长；-1 = 无声明
    bool m_seekable = false;
    bool m_canceled = false;
    bool m_rangeUnsupported = false; // 实锤不吃按偏移重启：粘旗，后续 seek 快返
    bool m_dead = false;             // 同一 pos 反复早断达收敛上限：判死
    int m_reopenFailStreak = 0;      // 同一 pos 上连续早断计数
    qint64 m_lastReopenPos = -1;
    mutable Stats m_stats; // reopens / skipHits / skipBytes 累计
};

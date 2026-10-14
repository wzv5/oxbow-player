#include "pch.h"

#include "HttpByteStream.h"
#include "HttpClient.h"

// —— 文件内常量节（D9 / D10：阈值不进头文件公共面、不做 INI 参数，数值字面量
// 只出现在此处；产品调参位）——
namespace {

// 等响应头零进展上限：等头是一次往返，超时即判死（起流失败 / 重开按错误上抛）
constexpr int kStreamStartIdleTimeoutMs = 10000;
// read 连续零字节上限：链路停（不是慢）——判据只看"有没有字节到达"
constexpr int kStreamReadIdleTimeoutMs = 30000;
// 跳读阈值：阈值内前向 seek 丢弃不发请求（推导上限见 N51 §2.5）
constexpr qint64 kSkipThresholdBytes = 64 * 1024;
// 收敛上限：同一 pos 上连续早断触发的自愈重开达此数仍早断即判死上抛（D10）
constexpr int kStreamMaxReopens = 3;
// 等待切片时长：实现细节，只求 ≤1000ms 使 cancel 检查与看门狗有界
constexpr int kSliceMs = 250;

// 哨兵同值钉等：L2 单点定义，L3 不造依赖也不直写字面量（N50 口径），防漂移
static_assert(ByteStream::kEof == HttpClient::kReadEof);
static_assert(ByteStream::kError == HttpClient::kReadError);
static_assert(ByteStream::kNoData == HttpClient::kReadNoData);

// —— 节 B：HTTP 专有的取数适配器（真实 feed；fake 由测试注入）——

struct FeedJob final : IHttpFeed::Job {
    HttpClient::RequestId id;
};

class HttpFeed final : public IHttpFeed {
  public:
    explicit HttpFeed(std::shared_ptr<HttpClient> client) : m_client(std::move(client)) {
        Q_ASSERT(m_client);
    }

    JobPtr submit(const HttpStreamTarget &target, qint64 startOffset) override {
        HttpClient::Request req;
        req.url = QUrl(target.url);
        req.method = QByteArrayLiteral("GET");
        for (const auto &h : target.headers)
            req.headers.append({h.first.toUtf8(), h.second.toUtf8()});
        if (startOffset > 0) // 首播请求不带按偏移重启头；重开/seek 才带
            req.headers.append({QByteArrayLiteral("Range"), QByteArrayLiteral("bytes=") +
                                                                QByteArray::number(startOffset) +
                                                                QByteArrayLiteral("-")});
        req.streaming = true;
        if (target.bufferBytes > 0)
            req.streamBufferBytes = target.bufferBytes;
        auto job = std::make_shared<FeedJob>();
        job->id = m_client->submit(req);
        return job;
    }

    bool awaitHeaders(const JobPtr &job, int timeoutMs) override {
        return HttpClient::waitHeaders(feedId(job), timeoutMs);
    }

    qint64 read(const JobPtr &job, char *out, qint64 maxBytes, int timeoutMs) override {
        return HttpClient::readBlocking(feedId(job), out, maxBytes, timeoutMs);
    }

    int statusCodeOf(const JobPtr &job) const override {
        return HttpClient::statusOf(feedId(job)).status;
    }

    qint64 totalBytesOf(const JobPtr &job) const override {
        // 只认最终落点响应：流式作业由 QNAM 自管跟随跳转，statusOf 已是终点；
        // 204 / 304 一类无体响应即便带声明也不在此参与早断判定（总长取定只认
        // 2xx/206 的声明，见 open / restartAt 的调用侧）
        const HttpClient::Response resp = HttpClient::statusOf(feedId(job));
        if (resp.status == 206) {
            // 分段总长取自 "bytes 123-456/789" 的 '/' 之后；"*" 或不可解 = 无声明
            const QByteArray cr = resp.headers.value(QByteArrayLiteral("content-range"));
            const qsizetype slash = cr.lastIndexOf('/');
            if (slash >= 0) {
                bool ok = false;
                const qint64 total = cr.mid(slash + 1).trimmed().toLongLong(&ok);
                if (ok && total >= 0)
                    return total;
            }
            return -1;
        }
        const QByteArray cl = resp.headers.value(QByteArrayLiteral("content-length"));
        if (cl.isEmpty())
            return -1;
        bool ok = false;
        const qint64 total = cl.toLongLong(&ok);
        return (ok && total >= 0) ? total : -1;
    }

    // 任意线程、不阻塞、只认句柄——等头阶段的 cancel 同样可达（§1.1 第 10 条）
    void abort(const JobPtr &job) override { HttpClient::abort(feedId(job)); }

  private:
    static HttpClient::RequestId feedId(const JobPtr &job) {
        return std::static_pointer_cast<FeedJob>(job)->id;
    }
    std::shared_ptr<HttpClient> m_client;
};

} // namespace

std::shared_ptr<IHttpFeed> HttpByteStream::makeFeed(const std::shared_ptr<HttpClient> &client) {
    return std::make_shared<HttpFeed>(client);
}

// ============================================================================
// 节 A：传输无关的重启策略骨架（位置守恒 / seek 分岔 / 断连自愈 / cancel）
// ============================================================================

HttpByteStream::HttpByteStream(std::shared_ptr<IHttpFeed> feed, const HttpStreamTarget &target,
                               const Timeouts &timeouts)
    : m_feed(std::move(feed)), m_target(target), m_timeouts(timeouts) {
    Q_ASSERT(m_feed);
}

HttpByteStream::~HttpByteStream() {
    // close = 析构（L4 释放最后一份 shared_ptr 即触发）：撤销旗置位 + 回收两槽
    // 在途作业；reply 回收仍由 HttpClient 既有通路负责
    cancel();
}

int HttpByteStream::startIdleTimeout() const {
    return m_timeouts.startIdleMs > 0 ? m_timeouts.startIdleMs : kStreamStartIdleTimeoutMs;
}

int HttpByteStream::readIdleTimeout() const {
    return m_timeouts.readIdleMs > 0 ? m_timeouts.readIdleMs : kStreamReadIdleTimeoutMs;
}

bool HttpByteStream::awaitHeadersSliced(const IHttpFeed::JobPtr &job, int budgetMs) {
    // 禁止向等头传永久等：切片循环，每片检查撤销旗，预算耗尽即放弃
    int waited = 0;
    while (waited < budgetMs) {
        {
            QMutexLocker l(&m_mutex);
            if (m_canceled)
                return false;
        }
        if (m_feed->awaitHeaders(job, kSliceMs))
            return true;
        waited += kSliceMs;
    }
    return false;
}

std::expected<std::shared_ptr<HttpByteStream>, QString>
HttpByteStream::open(std::shared_ptr<IHttpFeed> feed, const HttpStreamTarget &target) {
    return open(std::move(feed), target, Timeouts{});
}

std::expected<std::shared_ptr<HttpByteStream>, QString>
HttpByteStream::open(std::shared_ptr<IHttpFeed> feed, const HttpStreamTarget &target,
                     const Timeouts &timeouts) {
    if (!feed)
        return std::unexpected(QStringLiteral("no feed installed"));

    auto s = std::shared_ptr<HttpByteStream>(new HttpByteStream(std::move(feed), target, timeouts));

    // 首播作业：提交即登记当前槽（不等头）——登记即可被 cancel 打到
    IHttpFeed::JobPtr job = s->m_feed->submit(target, 0);
    {
        QMutexLocker l(&s->m_mutex);
        s->m_current = job;
        if (s->m_canceled) { // 槽空期到达的撤销：登记后补 abort 一次
            s->m_current.reset();
            l.unlock();
            s->m_feed->abort(job);
            return std::unexpected(QStringLiteral("canceled before stream start"));
        }
    }

    if (!s->awaitHeadersSliced(job, s->startIdleTimeout())) {
        s->m_feed->abort(job); // 调用方必须自行 abort 收尾，否则 reply 一直挂着
        bool canceled = false;
        {
            QMutexLocker l(&s->m_mutex);
            canceled = s->m_canceled;
            s->m_current.reset();
        }
        return std::unexpected(canceled ? QStringLiteral("stream start canceled")
                                        : QStringLiteral("timed out waiting for response headers"));
    }

    const int status = s->m_feed->statusCodeOf(job);
    if (status < 200 || status >= 300)
        return std::unexpected(status == 0
                                   ? QStringLiteral("connection failed before response headers")
                                   : QStringLiteral("HTTP status %1").arg(status));

    const qint64 total = s->m_feed->totalBytesOf(job);
    {
        QMutexLocker l(&s->m_mutex);
        s->m_total = total;
        // 可寻位申报口径（D11）：首包有声明总长即申报可寻位；无声明总长
        // （分段缺失/直播类）按不可寻位申报，适配层不注册 seek/size
        s->m_seekable = total >= 0;
        s->m_pos = 0;
    }
    return s;
}

qint64 HttpByteStream::pumpRead(char *out, qint64 maxBytes) {
    if (maxBytes <= 0)
        return ByteStream::kNoData; // 防御：无请求即无进展

    bool healed = false; // 断连自愈：单次 read 至多重开一次
    qint64 idleMs = 0;   // 连续零字节时长（链路慢不清零不算错，链路停才算）
    const int readIdle = readIdleTimeout();

    for (;;) {
        IHttpFeed::JobPtr job;
        {
            QMutexLocker l(&m_mutex);
            if (m_canceled || m_dead)
                return ByteStream::kError; // 撤销按既有语义上抛；判死不再重开
            job = m_current;
        }
        if (!job)
            return ByteStream::kError; // 无当前作业（上轮自愈失败后的空槽）

        const qint64 n = m_feed->read(job, out, maxBytes, kSliceMs);

        if (n > 0) {
            // 位置守恒：交付推进 pos；任一次成功交付即清零收敛计数
            QMutexLocker l(&m_mutex);
            m_pos += n;
            m_reopenFailStreak = 0;
            m_lastReopenPos = -1;
            idleMs = 0;
            return n;
        }

        {
            QMutexLocker l(&m_mutex);
            if (m_canceled)
                return ByteStream::kError;
        }

        if (n == ByteStream::kNoData) {
            // 仍在途：累计零字节时长，看门狗只认"零进展"
            idleMs += kSliceMs;
            if (idleMs >= readIdle) {
                m_feed->abort(job); // 主动 abort 当前作业，按错误走自愈重开
                {
                    QMutexLocker l(&m_mutex);
                    if (m_current == job)
                        m_current.reset();
                }
                if (healed)
                    return ByteStream::kError;
                healed = true;
                if (!healRestart())
                    return ByteStream::kError;
                idleMs = 0;
            }
            continue;
        }

        if (n == ByteStream::kEof) {
            // 终态诚实（§2.3 契约 8 的 L3 落点）：未到声明总长而供数通道已
            // 终止 = 早断，判 kError 走自愈重开；无声明总长（EOF 就是 EOF）
            // 或已按声明取完不参与判定——四条排除项中的后两条。
            bool early = false;
            {
                QMutexLocker l(&m_mutex);
                early = m_total >= 0 && m_pos < m_total;
            }
            if (!early)
                return ByteStream::kEof;
            if (healed)
                return ByteStream::kError;
            healed = true;
            if (!healRestart())
                return ByteStream::kError;
            idleMs = 0;
            continue;
        }

        // kError：断连——按 pos 自愈重开一次
        if (healed)
            return ByteStream::kError;
        healed = true;
        if (!healRestart())
            return ByteStream::kError;
        idleMs = 0;
    }
}

// 自愈重开：同一 pos 上连续早断达收敛上限即判死（kStreamMaxReopens；任一次
// 成功交付清零；seek 主动重开与鉴权往返不计数——前者不经本函数，后者对
// 等头透明、永不落到 read 路径）。看门狗触发的重开同走本计数：同一 pos 反复
// 无进展与反复早断是同一种致死回路，都不得无限循环。
bool HttpByteStream::healRestart() {
    {
        QMutexLocker l(&m_mutex);
        if (m_pos == m_lastReopenPos) {
            if (++m_reopenFailStreak > kStreamMaxReopens) {
                m_dead = true;
                return false;
            }
        } else {
            m_reopenFailStreak = 1;
            m_lastReopenPos = m_pos;
        }
    }
    if (restartAt(m_pos) != RestartResult::Promoted)
        return false; // 起不来就明确上抛：mpv 得知"这条流死了"而不是无限等
    QMutexLocker l(&m_mutex);
    m_stats.reopens++; // 只计 read 路径成功落位的自愈重开
    return true;
}

qint64 HttpByteStream::read(char *out, qint64 maxBytes) { return pumpRead(out, maxBytes); }

bool HttpByteStream::isSeekable() const {
    QMutexLocker l(&m_mutex);
    return m_seekable;
}

qint64 HttpByteStream::size() const {
    QMutexLocker l(&m_mutex);
    return m_total;
}

qint64 HttpByteStream::seek(qint64 to) {
    if (to < 0)
        return ByteStream::kSeekError;
    {
        QMutexLocker l(&m_mutex);
        if (m_canceled || m_dead)
            return ByteStream::kSeekError;
        if (m_rangeUnsupported || !m_seekable)
            return ByteStream::kSeekUnsupported; // 粘旗快返：不再开探测
        if (to == m_pos) {
            // 原位 seek（含起流期的结构性 seek(0)）：零成本，不发请求
            m_stats.skipHits++;
            return to;
        }
    }

    qint64 pos = 0;
    {
        QMutexLocker l(&m_mutex);
        pos = m_pos;
    }

    if (to > pos && to - pos <= kSkipThresholdBytes) {
        // 阈值内前向：跳读——丢弃不发请求（不吃按偏移重启的服务器前向微距照常可行）
        QMutexLocker l(&m_mutex);
        const qint64 drop = to - m_pos;
        m_pos = to;
        m_stats.skipHits++;
        m_stats.skipBytes += drop;
        return to;
    }

    // 后向或超阈值前向：探测式重启
    switch (restartAt(to)) {
    case RestartResult::Promoted:
        return to;
    case RestartResult::RangeUnsupported:
        return ByteStream::kSeekUnsupported;
    case RestartResult::Failed:
    case RestartResult::Canceled:
        return ByteStream::kSeekError;
    }
    return ByteStream::kSeekError;
}

void HttpByteStream::cancel() {
    IHttpFeed::JobPtr cur, probe;
    {
        QMutexLocker l(&m_mutex);
        m_canceled = true;
        cur = m_current;
        probe = m_probe;
    }
    // abort 两槽内全部作业；槽为空时只置旗，submit 后检查旗即补 abort 一次
    if (cur)
        m_feed->abort(cur);
    if (probe)
        m_feed->abort(probe);
}

ByteStream::Stats HttpByteStream::stats() const {
    QMutexLocker l(&m_mutex);
    m_stats.pos = m_pos;
    m_stats.total = m_total;
    return m_stats;
}

// ============================================================================
// 探测式重启（seek 与自愈共用）：全程不断开旧作业，新作业验证通过才以新换旧
// ============================================================================

HttpByteStream::RestartResult HttpByteStream::restartAt(qint64 to) {
    // 新作业只提交不等头，立即返回；旧连接原位保留
    IHttpFeed::JobPtr job = m_feed->submit(m_target, to);
    bool flagCanceled = false;
    {
        QMutexLocker l(&m_mutex);
        m_probe = job; // 登记即新纪元：此后 cancel() 必能 abort 到它
        if (m_canceled) {
            // 槽空期到达的撤销：登记后补 abort 一次
            flagCanceled = true;
            m_probe.reset();
        } else {
            m_canceled = false; // 撤销旗随新作业登记作废（新纪元清除）
        }
    }
    if (flagCanceled) {
        m_feed->abort(job);
        return RestartResult::Canceled;
    }

    if (!awaitHeadersSliced(job, startIdleTimeout())) {
        m_feed->abort(job);
        {
            QMutexLocker l(&m_mutex);
            if (m_probe == job)
                m_probe.reset();
        }
        return RestartResult::Failed; // 等不到响应头（黑洞由看门狗收场）
    }

    const int status = m_feed->statusCodeOf(job);
    if (status == 206) {
        const qint64 total = m_feed->totalBytesOf(job);
        IHttpFeed::JobPtr old;
        {
            QMutexLocker l(&m_mutex);
            m_probe.reset();
            old = m_current;
            m_current = job;
            m_pos = to; // 位置守恒：晋升与 pos 推进同一瞬间落定
            if (total >= 0)
                m_total = total;
        }
        if (old && old != job)
            m_feed->abort(old); // 晋升后再拆旧作业
        return RestartResult::Promoted;
    }

    m_feed->abort(job);
    {
        QMutexLocker l(&m_mutex);
        if (m_probe == job)
            m_probe.reset();
    }
    if (status >= 200 && status < 300) {
        // 200 全量：服务端实锤不按偏移重启（永久结论）——粘旗 + 快返，旧连接
        // 原位保留，绝不"从头重播"；Stats::reopens 不计数
        QMutexLocker l(&m_mutex);
        m_rangeUnsupported = true;
        return RestartResult::RangeUnsupported;
    }
    return RestartResult::Failed; // 偶发（探测超时、建连失败、4xx/5xx）：下次照常再探
}

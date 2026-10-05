#include "pch.h"

#include "HttpClient.h"

// N34 实现。分两块，均在 .cpp 内、不上头文件：
// 1. 共享传输（匿名命名空间）：专用网络线程 + 进程内唯一 QNAM + 缺陷表 + 等待原语。
//    client 对象只是不可变缺省值的持有者，构造与析构均不碰传输层；
// 2. 策略执行：总时限、自管重定向、Basic 预置 + authenticationRequired 兜底、
//    证书、headersOnly 截断、体上限、缺陷登记、池化快照、停机。
// 调用方不认识 QNAM；公开入口含策略，内部单跳 sendHop 为裸机制。

#define OXBOW_ASSERT_TASK_THREAD()                                                                 \
    Q_ASSERT(!QCoreApplication::instance() ||                                                      \
             QThread::currentThread() != QCoreApplication::instance()->thread())

// ——作业状态：完整定义在 .cpp 内，头文件只留前置声明。
// 类键与定义同用 struct（MSVC 对前后类键不一致告警 C4099）。
struct HttpClient::HttpJobState {
    // ——以下字段由 g_mutex 保护——
    bool finished = false;
    bool canceled = false;
    Response response;
    QNetworkReply *reply = nullptr; // 网络线程在锁内写入；abort 在锁内读以投递中止

    // ——流式字段（N50），同由 g_mutex 保护——
    bool headReady = false; // 响应头已捕获：waitHeaders 的唤醒条件
    int headStatus = 0;
    QHash<QByteArray, QByteArray> headHeaders;
    std::deque<QByteArray> chunks; // 块队列；仅单个消费者线程经 readBlocking 摘取
    qint64 chunksBytes = 0;

    // ——以下字段 submit 时初始化，发布后只读（重定向更新 url 系除外，见 prepareRedirect）——
    QElapsedTimer submitElapsed;  // 总时限起点：submit 时启动，含排队，见规格 §2.5
    qint64 remainingMs = 0;       // 出队时换算的剩余时长（总时限 - 已流逝），sendHop 用它启动 timer
    Config snapshot;              // submit 时的实例缺省快照
    bool streaming = false;       // N50：增量交付；为真时下方 deadline / maxBodyBytes 均不适用
    qint64 streamBufferBytes = 0; // N50：背压水位（submit 时定，> 0）
    QString initialOrigin;        // N50：本次作业初始 origin（authenticationRequired 校验用）
    QString baseOrigin;           // N50：Config::baseUrl 的 origin，可空

    // ——以下字段仅网络线程访问，无锁——
    QTimer *deadline = nullptr; // 总时限计时（出队时按剩余时长启动，贯穿全部重定向跳）；
                                // 流式作业不建（见规格 §2.5）
    QUrl url;                   // 当前跳的 URL（重定向逐跳更新）
    QString user;               // 当前跳生效的凭据（重定向逐跳更新）
    QString password;
    QByteArray presetAuth; // 当前跳预置的 Authorization；流式作业恒空（见规格 §2.8）
    QByteArray method;
    QByteArray body;
    QByteArray contentType;
    QList<QPair<QByteArray, QByteArray>> extraHeaders;
    bool headersOnly = false;
    bool headersCaptured = false; // 已取到响应头（headersOnly 截断 / 涌 body）
    bool timedOut = false;
    bool overflow = false;
    int redirectsUsed = 0;
    int capturedStatus = 0;
    QHash<QByteArray, QByteArray> capturedHeaders;
    QByteArray bodyBuf;
};

namespace {

// 全局一份互斥 + 条件变量：含义是"作业有【可观测量】变化"——终态落定（N34），以及
// N50 流式新增的两处：响应头捕获、字节入队。每作业的状态字段由它保护，等待者按限时
// 换算 deadline，只在变化时醒来查一次。
QMutex g_mutex;
QWaitCondition g_cond;

// HEAD 缺陷判据阈值：已收到响应头却仍持续涌入 body 超 64KB 即判定（405 / 501 之外
// 的另一判据）。metaDataChanged 是异步派发的，该阈值同时覆盖缓冲先到的极端情形。
constexpr qint64 kHeadFlowDefectThreshold = 64 * 1024;

constexpr qint64 kDefaultMaxBodyBytes = 16 * 1024 * 1024;

// N50 流式：块队列的默认背压水位，与 Qt 侧读缓冲零头。
// 后者只用于吸收两次 readyRead 之间的突发——正常路径上每次 readyRead 都被 readAll
// 清空，故不需要大；限制它的意义在于「我们不取数」时 Qt 真的会节流（默认 0 无界）。
constexpr qint64 kDefaultStreamBufferBytes = 4 * 1024 * 1024;
constexpr qint64 kStreamReplyReadBufferBytes = 512 * 1024;

// 进程级共享传输，惰性创建；由 g_mutex 保护。
struct Transport {
    QThread *thread = nullptr;
    QObject *worker = nullptr;            // 队列投递上下文（亲和网络线程）
    QNetworkAccessManager *nam = nullptr; // 亲和网络线程
    QSet<QString> headDefects;            // 不支持 HEAD 的 origin（scheme://host:port）
    bool ready = false;
    bool quitting = false;
};

Transport *g_transport = nullptr;
bool g_appQuitting = false; // aboutToQuit 后置位；此后不再创建传输

QString originOf(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    int port = url.port();
    if (port == -1)
        port = scheme == "https" ? 443 : (scheme == "http" ? 80 : 0);
    return scheme + "://" + url.host().toLower() + ":" + QString::number(port);
}

// 口令为原始串，由本篇负责编码（不二次编码 URL 串）
QByteArray basicAuthHeader(const QString &user, const QString &password) {
    return "Basic " + (user.toUtf8() + ':' + password.toUtf8()).toBase64();
}

// URL userinfo 优先；返回是否取到（user / password 原始串，已解码）。
// userInfo(FullyDecoded) 为禁用入口；此处取 FullyEncoded 原始串、先按 ':' 切分
// 再逐段解码——口令里被编码的冒号（%3A）因此不会干扰切分
bool userInfoOf(const QUrl &url, QString &user, QString &password) {
    const QByteArray raw = url.userInfo(QUrl::ComponentFormattingOption::FullyEncoded).toUtf8();
    if (raw.isEmpty())
        return false;
    const int colon = raw.indexOf(':');
    const QByteArray userPart = colon < 0 ? raw : raw.left(colon);
    const QByteArray passPart = colon < 0 ? QByteArray() : raw.mid(colon + 1);
    user = QUrl::fromPercentEncoding(userPart);
    password = QUrl::fromPercentEncoding(passPart);
    return true;
}

HttpClient::Response canceledResponse(const QString &reason) {
    HttpClient::Response r;
    r.error = reason;
    r.canceled = true;
    return r;
}

// 非 QObject 类的翻译标注：文案用英文书写并经 QT_TRANSLATE_NOOP 标注翻译上下文
// （供 lupdate 提取），运行期经 QCoreApplication::translate 取值——无译文时即英文原文
QString trText(const char *source) { return QCoreApplication::translate("HttpClient", source); }

// 总时限耗尽的统一文案（含排队期；排队期满未轮到与在途超时同文案）
QString timeoutMessage(qint64 timeoutMs) {
    return trText(QT_TRANSLATE_NOOP("HttpClient", "Request timed out (total limit %1 ms)"))
        .arg(timeoutMs);
}

// 响应头归一化：键一律转小写；同名重复头保留末次（rawHeaderPairs 按出现序给出）
void fillHeaders(QNetworkReply *reply, QHash<QByteArray, QByteArray> &out) {
    const auto pairs = reply->rawHeaderPairs();
    for (const auto &p : pairs)
        out.insert(p.first.toLower(), p.second);
}

void captureHeaders(HttpClient::HttpJobState &job, QNetworkReply *reply) {
    job.capturedStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    job.capturedHeaders.clear();
    fillHeaders(reply, job.capturedHeaders);
}

// N50 流式写侧：把 reply 上的数据搬进本作业的块队列。仅网络线程调用（readyRead 或
// pumpStream 的 queued 投递）。达水位即不再取数——数据留在 Qt 侧缓冲，由已设的
// setReadBufferSize 向 socket 施压；消费方取走后经 pumpStream 再来一次。
// 响应头在这里兜一道：头总在体之前，故首次入队时必然已可读。
void streamingReadyRead(const std::shared_ptr<HttpClient::HttpJobState> &job,
                        QNetworkReply *reply) {
    {
        QMutexLocker l(&g_mutex);
        if (job->finished)
            return;
        if (!job->headReady) {
            captureHeaders(*job, reply);
            job->headStatus = job->capturedStatus;
            job->headHeaders = job->capturedHeaders;
            job->headReady = true;
            g_cond.wakeAll(); // 唤醒 waitHeaders 的等待者
        }
        if (job->chunksBytes >= job->streamBufferBytes) {
            // N52 探针（临时）：水位打满后的 Qt 侧滞留观测——若整文件灌在 Qt 缓冲，
            // bytesAvailable 会无界增长；每 256 次命中采样一行（每次命中 = 一次
            // readyRead 且我方不再取数）。与 readBufferSize() 的实际值同放一行，
            // 直接验证该限流是否兑现
            static std::atomic<int> s_gateHits{0};
            if (s_gateHits.fetch_add(1, std::memory_order_relaxed) % 256 == 0)
                qDebug() << "[oxbow] probe watermark-full queue=" << job->chunksBytes
                         << "qtBuf=" << reply->bytesAvailable()
                         << "qtLimit=" << reply->readBufferSize();
            return; // 已到水位：留着让消费方取走后再续泵
        }
    }
    // 锁外取数：readAll 不碰作业状态，容量以 Qt 侧读缓冲为界
    QByteArray d = reply->readAll();
    if (d.isEmpty())
        return;
    QMutexLocker l(&g_mutex);
    if (job->finished)
        return; // 取数期间被落定：丢弃本块（终态先落定者为准）
    job->chunksBytes += d.size();
    job->chunks.push_back(std::move(d));
    g_cond.wakeAll(); // 唤醒 readBlocking 的等待者
}

// N50 续泵：队列由「满」降到「未满」后，由消费方投递到此。不能只依赖 readyRead 再来
// ——Qt 恢复读之后若没有新数据到达，readyRead 不会重新发射，留在 reply 缓冲里的那块
// 就永远取不到。
void pumpStream(const std::shared_ptr<HttpClient::HttpJobState> &job) {
    QNetworkReply *reply = nullptr;
    {
        QMutexLocker l(&g_mutex);
        if (job->finished)
            return;
        reply = job->reply; // finished / 撤销时已置空；非空即本跳尚在，且删除走 deleteLater
    }
    if (reply && reply->bytesAvailable() > 0)
        streamingReadyRead(job, reply);
}

QString readableNetworkError(QNetworkReply *reply) {
    switch (reply->error()) {
    case QNetworkReply::ConnectionRefusedError:
        return trText(QT_TRANSLATE_NOOP("HttpClient", "Connection refused"));
    case QNetworkReply::RemoteHostClosedError:
        return trText(QT_TRANSLATE_NOOP("HttpClient", "Remote host closed the connection"));
    case QNetworkReply::HostNotFoundError:
        return trText(QT_TRANSLATE_NOOP("HttpClient", "Host not found"));
    case QNetworkReply::TimeoutError:
        return trText(QT_TRANSLATE_NOOP("HttpClient", "Connection timed out"));
    case QNetworkReply::AuthenticationRequiredError:
        return trText(
            QT_TRANSLATE_NOOP("HttpClient", "Authentication failed (credentials rejected)"));
    case QNetworkReply::ContentNotFoundError:
        return trText(QT_TRANSLATE_NOOP("HttpClient", "Resource not found (404)"));
    case QNetworkReply::ProtocolUnknownError:
        return trText(QT_TRANSLATE_NOOP("HttpClient", "Unsupported protocol"));
    case QNetworkReply::SslHandshakeFailedError:
        return trText(QT_TRANSLATE_NOOP("HttpClient", "TLS handshake failed"));
    default:
        break;
    }
    return trText(QT_TRANSLATE_NOOP("HttpClient", "Network error (%1)")).arg(reply->errorString());
}

void registerHeadDefect(const QUrl &url) {
    QMutexLocker l(&g_mutex);
    if (g_transport)
        g_transport->headDefects.insert(originOf(url));
}

// 终态落定：唯一出口。撤销 / 完成竞态时以先落定者为准，后到者直接返回。
void finishTerminal(const std::shared_ptr<HttpClient::HttpJobState> &job,
                    HttpClient::Response resp) {
    QMutexLocker l(&g_mutex);
    if (job->finished)
        return;
    job->canceled = resp.canceled;
    job->finished = true;
    job->response = std::move(resp);
    job->reply = nullptr;
    if (job->deadline) {
        job->deadline->stop();
        job->deadline->deleteLater();
        job->deadline = nullptr;
    }
    g_cond.wakeAll();
}

// 跨 origin 跳转时剥掉预置 Authorization，按新 URL 重取凭据
// （新 URL userinfo 优先，否则仅当新 origin 等于实例 baseUrl 的 origin 时复用实例凭据），
// 不把旧 host 的凭据泄漏到新 host。同 origin 的各跳原样保留。
void prepareRedirect(HttpClient::HttpJobState &job, const QUrl &target) {
    const bool crossOrigin = originOf(job.url) != originOf(target);
    job.url = target;
    job.bodyBuf.clear();
    // 截断状态逐跳重置：headersOnly 只取最终落点的响应头，中间跳的 301 头不算数
    job.headersCaptured = false;
    job.capturedStatus = 0;
    job.capturedHeaders.clear();
    if (!crossOrigin)
        return;
    job.presetAuth.clear();
    job.user.clear();
    job.password.clear();
    if (!userInfoOf(target, job.user, job.password)) {
        if (originOf(target) == originOf(job.snapshot.baseUrl)) {
            job.user = job.snapshot.user;
            job.password = job.snapshot.password;
        }
    }
    if (!job.user.isEmpty() || !job.password.isEmpty())
        job.presetAuth = basicAuthHeader(job.user, job.password);
}

void sendHop(const std::shared_ptr<HttpClient::HttpJobState> &job, QNetworkAccessManager *nam);
void finalizeJob(const std::shared_ptr<HttpClient::HttpJobState> &job, QNetworkReply *reply,
                 QNetworkAccessManager *nam);

// 单跳发送：裸机制，只收已装配好的参数，不判缺陷、不看重定向、不自建排队。
// 计时器先于请求建立：黑洞地址（DNS 无响应 / SYN 石沉大海）在发出前即已被总时限覆盖，
// 到点 abort 即可回收。计时器只在首次进入时建立，后续重定向跳复用同一枚，
// 故总 deadline 贯穿全部跳、不重置（见规格 §2.5；重定向循环在 finalizeJob）。
void sendHop(const std::shared_ptr<HttpClient::HttpJobState> &job, QNetworkAccessManager *nam) {
    {
        QMutexLocker l(&g_mutex);
        if (job->finished)
            return; // abort 抢先落定：本跳不再发出，也不建计时器
    }
    // N50：流式作业不建任何计时器——消费方可能长时间不取数（mpv 暂停即停止 read），
    // 任何「无数据即超时」的作业内计时器都会把健康的流判死；超时改由调用侧窗口承担
    // （waitHeaders / readBlocking 的入参），见规格 §2.5。
    if (!job->streaming && !job->deadline) {
        auto *timer = new QTimer(nam);
        timer->setSingleShot(true);
        // context 取 nam（寿命等同传输，随传输销毁自动断开）：每跳的 reply 都会重建，
        // 拿 reply 做 context 会让重定向之后的跳失去总时限保护
        QObject::connect(timer, &QTimer::timeout, nam, [job] {
            QNetworkReply *r = nullptr;
            {
                QMutexLocker ll(&g_mutex);
                if (job->finished)
                    return; // 已落定（完成 / 撤销 / 停机）：不再动手，也不碰可能已销毁的 reply
                r = job->reply;
                job->timedOut = true; // 超时不登记缺陷（网络抖动不应永久禁用一个 origin）
            }
            if (r)
                r->abort();
        });
        {
            QMutexLocker l(&g_mutex);
            if (job->finished) { // 与 abort 的竞态：计时器交给 abort 一侧回收
                l.unlock();
                timer->deleteLater();
                return;
            }
            job->deadline = timer; // 与 abort / finishTerminal 同锁读写
        }
        timer->start(job->remainingMs); // 剩余时长在出队时（runJob）换算完毕
    }

    QNetworkRequest req(job->url);
    // 重定向策略按作业分岔（N50）：非流式自管（为保 PROPFIND 的方法与 body，见
    // finalizeJob）；流式交 QNAM 自动跟随且允许跨 origin，跳数用 Qt 自己的默认上限
    // ——不使用 Config::maxRedirects（该字段的语义是「自管」的跳数上限），见规格 §2.8。
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     job->streaming ? QNetworkRequest::NoLessSafeRedirectPolicy
                                    : QNetworkRequest::ManualRedirectPolicy);
    // 连接复用一律是 QNAM 内建行为（同 host 6 连接，闲置缓存 120s），本篇不调
    // 任何池参数（保 Qt 6.2 兼容）；调用方纪律见规格 §2.4。
    for (const auto &h : job->extraHeaders)
        req.setRawHeader(h.first, h.second);
    if (!job->contentType.isEmpty() && !job->body.isEmpty())
        req.setRawHeader("Content-Type", job->contentType);
    if (!job->presetAuth.isEmpty())
        req.setRawHeader("Authorization", job->presetAuth);

    // 流式作业的限流依赖 setReadBufferSize 的下游停读；但 CustomOperation 一旦
    // 携带上传设备（含空 body 的 QByteArray 重载内部创建的 QBuffer），该限流即
    // 完全失效——Qt 把整个响应无节制吸入 reply 缓冲（_probe-n52/qnam-buf 定向
    // 对照：rb=512KB 从不读取，body=QByteArray 10 秒灌入 4.98GB / RSS 5.2GB；
    // body=nullptr 精确停在 512KB；慢消费 drain 模式背压逐字节兑现）。插件整
    // 文件内存增长即此。空 body 一律不挂上传设备；非空 body 照旧
    QNetworkReply *reply =
        job->body.isEmpty()
            ? nam->sendCustomRequest(req, job->method, static_cast<QIODevice *>(nullptr))
            : nam->sendCustomRequest(req, job->method, job->body);
    {
        QMutexLocker l(&g_mutex);
        if (job->finished) { // abort 抢先落定：立刻中止本跳，不产生更多流量
            l.unlock();
            reply->abort();
            reply->deleteLater(); // 本跳未及接 finished，自行回收
            return;
        }
        job->reply = reply;
    }

    if (job->streaming) {
        // N50：把 Qt 侧读缓冲限制在一个零头上，「我们不取数」才真的能压到 socket
        // ——默认 readBufferSize 为 0 即无界，此时 Qt 不会节流，数据只是从本作业的
        // 队列挪进 Qt 的队列（见规格 §2.3）。重定向时 Qt 走 followRedirect 重发，
        // 代理对象可能重建，故在 redirected 上再补设一次。此处与规格里已取消的
        // 「跨 origin 重定向守卫」无关，只是重设读缓冲。
        reply->setReadBufferSize(kStreamReplyReadBufferBytes);
        QObject::connect(reply, &QNetworkReply::redirected, reply,
                         [reply] { reply->setReadBufferSize(kStreamReplyReadBufferBytes); });
    }

    // 截断（headersOnly）：响应头到达即断；随后的 finished（OperationCanceledError）
    // 在已取到头时按成功处理。metaDataChanged 异步派发可能多读少量字节，可接受——
    // 要防的是"读完整个文件"。重定向中间跳（301 / 302 / 303 / 307 / 308）不截断，
    // 由 finished 分支跟随，headersOnly 只取最终落点的头。
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [job, reply] {
        if (job->streaming) {
            // N50：头到达即记下（waitHeaders / statusOf 的确定窗口）。必须在这里兜住，
            // 不能只靠写侧——无响应体的响应不会触发 readyRead。
            QMutexLocker l(&g_mutex);
            if (job->headReady || job->finished)
                return;
            const int st = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308)
                return; // 中间跳的响应头不算数（自动跟随时不应出现，仍守一道）
            captureHeaders(*job, reply);
            job->headStatus = job->capturedStatus;
            job->headHeaders = job->capturedHeaders;
            job->headReady = true;
            g_cond.wakeAll(); // 唤醒 waitHeaders 的等待者
            return;
        }
        if (!job->headersOnly || job->headersCaptured)
            return;
        const int st = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308)
            return;
        captureHeaders(*job, reply);
        job->headersCaptured = true;
        reply->abort();
    });

    QObject::connect(reply, &QNetworkReply::readyRead, reply, [job, reply] {
        if (job->streaming) {
            streamingReadyRead(job, reply); // N50：入队与背压，不走下方的体累积
            return;
        }
        if (job->headersCaptured)
            return;
        // headersOnly：调用方只读头，体越界后一律丢弃（不占内存、也不受体上限约束）。
        // 但保留到缺陷判据阈值之上，以免 HEAD 涌 body 判据（>64KB）在 headersOnly
        // 形态下永久失效——阈值以下照常累积，判据仍可命中
        if (job->headersOnly && job->bodyBuf.size() > kHeadFlowDefectThreshold) {
            reply->readAll();
            return;
        }
        job->bodyBuf += reply->readAll();
        // 缺陷登记：HEAD 请求收到响应头后仍持续涌入 body（超 64KB）→ 该 origin 不支持 HEAD，
        // 按 headersOnly 截断成功返回并登记；GET / PROPFIND 的 body 属正常响应，不判缺陷
        if (job->method == "HEAD" && job->bodyBuf.size() > kHeadFlowDefectThreshold) {
            registerHeadDefect(job->url);
            captureHeaders(*job, reply);
            job->headersCaptured = true;
            reply->abort();
            return;
        }
        const qint64 maxBody =
            job->snapshot.maxBodyBytes > 0 ? job->snapshot.maxBodyBytes : kDefaultMaxBodyBytes;
        if (!job->headersOnly && job->bodyBuf.size() > maxBody) {
            // 超限即 abort，按失败返回：body 丢弃不返回半截数据；超限是调用方 guard，
            // 非服务端缺陷，不登记
            captureHeaders(*job, reply);
            job->overflow = true;
            reply->abort();
        }
    });

    // 凭据兜底：服务器索要时才经 authenticationRequired 应答（预置 Basic 已省掉一次
    // 401 往返）。信号在 QNAM 上，按 reply 归属过滤到本作业；认证重试由 Qt 以
    // authenticator 生成的凭据覆盖同名 Authorization 字段，预置 Basic 不会双发
    // （公开 API 无法移除已发出的预置头，QNetworkReply::setRawHeader 为 protected）
    QObject::connect(nam, &QNetworkAccessManager::authenticationRequired, reply,
                     [job](QNetworkReply *r, QAuthenticator *auth) {
                         if (r != job->reply)
                             return; // 其他作业的挑战
                         if (job->user.isEmpty() && job->password.isEmpty())
                             return;
                         if (job->streaming) {
                             // N50：流式作业不预置 Authorization——预置头会被 QNAM 原样
                             // 带到重定向目标（createRedirectRequest 整份复制原请求），
                             // 故凭据只从这里注入，origin 就得自己守：只对本次作业的初始
                             // origin、或实例 baseUrl 的 origin 回填，其余一律不应答
                             // （请求以 AuthenticationRequired 落错误终态）。非流式仍预置，
                             // 跨 origin 由 prepareRedirect 剥头，见规格 §2.8。
                             const QString cur = originOf(r->url());
                             if (cur != job->initialOrigin &&
                                 (job->baseOrigin.isEmpty() || cur != job->baseOrigin))
                                 return;
                         }
                         auth->setUser(job->user);
                         auth->setPassword(job->password);
                     });

    if (job->snapshot.ignoreSslErrors)
        QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                         [reply](const QList<QSslError> &) { reply->ignoreSslErrors(); });

    QObject::connect(reply, &QNetworkReply::finished, reply,
                     [job, reply, nam] { finalizeJob(job, reply, nam); });

    // 停机：QNAM 销毁时未结束作业记为 canceled 并唤醒全部等待者。
    // 仅在传输 quitting 时生效——重定向换发对旧 reply 的常规 deleteLater 不触发取消
    QObject::connect(reply, &QObject::destroyed, [job] {
        QMutexLocker l(&g_mutex);
        if (job->finished)
            return;
        if (!g_transport || !g_transport->quitting)
            return;
        job->canceled = true;
        job->finished = true;
        job->response = canceledResponse(
            trText(QT_TRANSLATE_NOOP("HttpClient", "Application quitting; request canceled")));
        g_cond.wakeAll();
    });
}

// 完成策略（单点收口）：重定向循环、超时 / 超限 / 截断 / 缺陷登记的出口都在这里。
void finalizeJob(const std::shared_ptr<HttpClient::HttpJobState> &job, QNetworkReply *reply,
                 QNetworkAccessManager *nam) {
    {
        QMutexLocker l(&g_mutex);
        if (job->finished) {
            // 撤销 / 停机已先落定：结果已是 canceled，仅回收本跳 reply；
            // 摘除 job->reply——排队中的 abort lambda（QPointer 守护）与本回收
            // 存在竞态，指针不得以悬垂形态残留
            job->reply = nullptr;
            reply->deleteLater();
            return;
        }
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 重定向（ManualRedirectPolicy 自管）：301 / 302 / 303 / 307 / 308 且带 Location 时，
    // 以同方法同 body 同头重发（仅改 URL）。与 fetch 对 303 转 GET 的规定故意偏离：
    // WebDAV 保方法优先，PROPFIND 被降格即变义。相对 Location 按当前 URL 解析；
    // 循环跳转计入跳数；总 deadline 自 submit 起算贯穿全部跳，不重置。
    // headersOnly 的中间跳不截断（metaDataChanged 已跳过），此处照样跟随。
    // N50：流式作业的重定向由 QNAM 自动跟随（见规格 §2.8），落不到这里——本分支是
    // 非流式自管专用。
    if (!job->streaming && status > 0 && !job->timedOut && !job->overflow &&
        reply->error() == QNetworkReply::NoError) {
        // 读原始头而非已知头：header(LocationHeader) 对相对路径的 Location 不可靠
        const QByteArray locRaw = reply->rawHeader("Location");
        const bool isRedirect =
            status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (isRedirect && !locRaw.isEmpty()) {
            bool follow = false;
            {
                // 先在锁内摘除、锁外回收：abort 若落在同一窗口只会读到空指针，
                // 不会向一个已排入 DeferredDelete 的 reply 投递 abort（跨线程 UAF）
                QMutexLocker l(&g_mutex);
                job->reply = nullptr;
                follow = !job->finished;
            }
            reply->deleteLater();
            if (!follow)
                return; // abort 恰好落在重定向间隙：不再跟随，结果已是 canceled
            if (job->redirectsUsed >= job->snapshot.maxRedirects) {
                finishTerminal(job, HttpClient::Response{
                                        status,
                                        {},
                                        {},
                                        trText(QT_TRANSLATE_NOOP("HttpClient",
                                                                 "Too many redirects (limit %1)"))
                                            .arg(job->snapshot.maxRedirects),
                                        false});
                return;
            }
            prepareRedirect(*job, job->url.resolved(QUrl::fromEncoded(locRaw)));
            job->redirectsUsed++;
            sendHop(job, nam);
            return;
        }
    }

    HttpClient::Response resp;
    if (job->overflow) {
        // 超限：status 为实际状态码、headers 为已收头、body 为空（超限部分丢弃）
        resp.status = job->capturedStatus;
        resp.headers = job->capturedHeaders;
        const qint64 maxBody =
            job->snapshot.maxBodyBytes > 0 ? job->snapshot.maxBodyBytes : kDefaultMaxBodyBytes;
        resp.error = trText(QT_TRANSLATE_NOOP("HttpClient",
                                              "Response body exceeds limit (%1 bytes); aborted"))
                         .arg(maxBody);
    } else if (job->timedOut) {
        resp.status = job->capturedStatus;
        resp.headers = job->capturedHeaders;
        const qint64 timeout = job->snapshot.timeoutMs > 0 ? job->snapshot.timeoutMs : 5000;
        resp.error = timeoutMessage(timeout);
    } else if (job->headersOnly && reply->error() == QNetworkReply::NoError) {
        // headersOnly 且正常收完（无 Location 的 3xx 等未截断即走完的边沿）：
        // 按成功返回已收头，body 照常丢弃（调用方只读头）。
        captureHeaders(*job, reply);
        resp.status = job->capturedStatus;
        resp.headers = job->capturedHeaders;
    } else if (job->headersCaptured) {
        // headersOnly 截断成功（含 HEAD 涌 body，缺陷已在 readyRead 登记）：
        // body 为截断前已到的少量字节，无意义，调用方只读头
        resp.status = job->capturedStatus;
        resp.headers = job->capturedHeaders;
        resp.body = job->bodyBuf;
    } else if (reply->error() != QNetworkReply::NoError) {
        resp.status = status;
        fillHeaders(reply, resp.headers);
        resp.error = readableNetworkError(reply);
    } else {
        resp.status = status;
        fillHeaders(reply, resp.headers);
        // 补齐尾部：个别 Qt 版本可能在 finished 前合并派发 readyRead，
        // 收尾时取回剩余字节，避免 body 静默丢尾部数据。
        // N50：流式作业的尾部另有去处（终态前并入块队列，见下），不塞进 body。
        if (!job->streaming && reply->bytesAvailable() > 0)
            job->bodyBuf += reply->readAll();
        resp.body = job->bodyBuf;
    }

    // 缺陷登记：HEAD 响应 405 / 501 → 登记"该 origin 不支持 HEAD"，同 origin 后续
    // HEAD 由 submit / exec 入口短路；GET / PROPFIND 不受影响；超时 / 撤销不登记
    if (job->method == "HEAD" && (status == 405 || status == 501)) {
        registerHeadDefect(job->url);
        resp.error =
            trText(QT_TRANSLATE_NOOP("HttpClient",
                                     "Server does not support HEAD (%1); registered, "
                                     "subsequent HEAD requests to this origin will not be sent"))
                .arg(originOf(job->url));
    }

    // N50：流式作业落终态前，把 reply 里尚未取走的字节并入块队列。此处不受水位约束
    // ——这是数据尾部，丢掉就等于截断；容量以 Qt 侧读缓冲为界，是个有界的零头。
    // 若连响应头都没来得及记下（如空响应体），在此一并补上，免得 waitHeaders 空等。
    if (job->streaming) {
        QMutexLocker l(&g_mutex);
        if (!job->finished) {
            if (!job->headReady) {
                captureHeaders(*job, reply);
                job->headStatus = job->capturedStatus;
                job->headHeaders = job->capturedHeaders;
                job->headReady = true;
            }
            const QByteArray d = reply->readAll();
            if (!d.isEmpty()) {
                job->chunksBytes += d.size();
                job->chunks.push_back(d);
            }
            g_cond.wakeAll(); // 头到达 / 尾部入队都可能解锁等待者
        }
    }

    reply->deleteLater();
    finishTerminal(job, std::move(resp));
}

// 作业在网络线程的入口：停机后不再执行新作业；排队期耗尽总时限的不再发出。
void runJob(const std::shared_ptr<HttpClient::HttpJobState> &job, QNetworkAccessManager *nam) {
    {
        QMutexLocker l(&g_mutex);
        if (job->finished)
            return; // abort 先落定，作业已取消
        // g_transport 非空：作业只由存活的传输投递，且该指针仅在锁内置空，
        // 置空发生在网络线程退出之后（停机顺序见 shutdownTransport）
        if (g_transport->quitting) {
            job->canceled = true;
            job->finished = true;
            job->response = canceledResponse(
                trText(QT_TRANSLATE_NOOP("HttpClient", "Application quitting; request not sent")));
            g_cond.wakeAll();
            return;
        }
    }
    // 总时限自 submit 起算：出队时换算剩余时长；排队已耗尽的不再发出，
    // 直接按超时失败落定（黑洞地址的 DNS / 建连 hanging 同理，timer 已在跑）。
    // N50：流式作业没有总时限（见规格 §2.5），跳过换算与排队超时判定——启动窗口
    // 由调用侧 waitHeaders 的入参承担。
    if (!job->streaming) {
        const qint64 timeout = job->snapshot.timeoutMs > 0 ? job->snapshot.timeoutMs : 5000;
        const qint64 remaining = timeout - job->submitElapsed.elapsed();
        if (remaining <= 0) {
            job->timedOut = true;
            HttpClient::Response resp;
            resp.error = timeoutMessage(timeout);
            finishTerminal(job, std::move(resp));
            return;
        }
        job->remainingMs = remaining;
    }
    sendHop(job, nam);
}

// 共享传输停机：不再创建、未结束作业记为 canceled 并唤醒、销毁 QNAM、停止网络线程。
// 对外不暴露停机接口。
void shutdownTransport() {
    QMutexLocker l(&g_mutex);
    g_appQuitting = true;
    if (!g_transport) {
        g_cond.wakeAll(); // 唤醒可能卡在创建等待的调用方
        return;
    }
    if (g_transport->quitting)
        return;
    g_transport->quitting = true;
    auto *t = g_transport;
    g_cond.wakeAll();
    // 收尾任务在本轮投递序中位于既有提交之后（FIFO）：先全部执行到（各自 canceled
    // 落定），再在网络线程内销毁 QNAM（reply destroyed 把在途作业记为 canceled），
    // 最后退出事件循环
    QMetaObject::invokeMethod(
        t->worker,
        [t] {
            delete t->nam; // reply destroyed 处理器自行加锁，避免本线程重入 g_mutex
            {
                QMutexLocker ll(&g_mutex);
                t->nam = nullptr;
            }
            QThread::currentThread()->quit();
        },
        Qt::QueuedConnection);
    l.unlock();
    t->thread->wait(); // 等待退出后，队列中不再有未执行的提交
    l.relock();
    delete t->worker;
    delete t->thread;
    delete t;
    g_transport = nullptr;
    g_cond.wakeAll();
}

// 共享传输惰性创建：首次构造 client 时起一条专用网络线程，
// 并在该线程内创建唯一一个 QNAM；创建过程同步等到"QNAM 就绪"后才对外可用。
// 前提：调用方已持 g_mutex。返回 QNAM 或 nullptr（停机后）。
QNetworkAccessManager *ensureTransportLocked() {
    if (g_appQuitting)
        return nullptr;
    if (g_transport && g_transport->quitting)
        return nullptr;
    if (!g_transport) {
        auto *thread = new QThread;
        auto *worker = new QObject; // 空对象仅作队列投递上下文
        worker->moveToThread(thread);
        g_transport = new Transport{thread, worker, nullptr, {}, false, false};
        thread->start();
        QMetaObject::invokeMethod(
            worker,
            [] {
                QMutexLocker l(&g_mutex);
                g_transport->nam = new QNetworkAccessManager(g_transport->worker);
                qRegisterMetaType<HttpClient::RequestId>("HttpClient::RequestId");
                g_transport->ready = true;
                g_cond.wakeAll();
            },
            Qt::QueuedConnection);
        // 停机接线：传输首次创建时自行接 qApp 的 aboutToQuit
        QCoreApplication *application = QCoreApplication::instance();
        if (application)
            QObject::connect(application, &QCoreApplication::aboutToQuit, application,
                             [] { shutdownTransport(); });
        // 循环条件每轮重查 g_transport：停机一侧会在 wakeAll 之后把它置空，
        // 只看 ready 会在唤醒后解引用空指针
        while (g_transport && !g_transport->ready && !g_appQuitting)
            g_cond.wait(&g_mutex);
    }
    if (g_appQuitting || !g_transport || g_transport->quitting)
        return nullptr;
    return g_transport->nam;
}

} // namespace

HttpClient::HttpClient(const Config &config) : m_config(config) {
    QMutexLocker l(&g_mutex);
    ensureTransportLocked(); // 惰性创建共享传输
}

HttpClient::Response HttpClient::exec(const Request &request) const {
    OXBOW_ASSERT_TASK_THREAD();
    const RequestId id = submit(request);
    wait(id, -1);
    return take(id);
}

HttpClient::RequestId HttpClient::submit(const Request &request) const {
    auto job = std::make_shared<HttpJobState>();
    job->snapshot = m_config;
    job->submitElapsed.start(); // 总时限起点：含排队，见规格 §2.5
    job->method = request.method;
    job->body = request.body;
    job->contentType = request.contentType;
    job->extraHeaders = request.headers;
    job->headersOnly = request.headersOnly;

    // 空方法为调用错误：返回可读失败，不发请求
    if (request.method.trimmed().isEmpty()) {
        QMutexLocker l(&g_mutex);
        job->finished = true;
        job->response.error = trText(QT_TRANSLATE_NOOP("HttpClient", "Request method is empty"));
        return job;
    }

    // N50：streaming 与 headersOnly 互斥——一个要「持续交付响应体」，一个要「取到响应头
    // 即断」。同时置位按调用错误处理，不发请求。
    if (request.streaming && request.headersOnly) {
        QMutexLocker l(&g_mutex);
        job->finished = true;
        job->response.error = trText(
            QT_TRANSLATE_NOOP("HttpClient", "Request cannot be both streaming and headersOnly"));
        return job;
    }

    // URL 解析：相对→按 baseUrl 解析；绝对→直接用。baseUrl 为空且相对为调用错误，
    // 返回可读失败，不发请求
    QUrl url = request.url;
    if (url.scheme().isEmpty()) {
        if (m_config.baseUrl.isEmpty()) {
            QMutexLocker l(&g_mutex);
            job->finished = true;
            job->response.error =
                trText(QT_TRANSLATE_NOOP(
                           "HttpClient",
                           "HttpClient has no baseUrl bound; cannot resolve relative URL: %1"))
                    .arg(request.url.toString(QUrl::FullyEncoded));
            return job;
        }
        url = m_config.baseUrl.resolved(url);
    }
    job->url = url;

    // N50：流式作业级参数与 origin 基线（authenticationRequired 的校验依据）
    job->streaming = request.streaming;
    job->streamBufferBytes =
        request.streamBufferBytes > 0 ? request.streamBufferBytes : kDefaultStreamBufferBytes;
    job->initialOrigin = originOf(url);
    job->baseOrigin = m_config.baseUrl.isEmpty() ? QString() : originOf(m_config.baseUrl);

    // 凭据：URL userinfo 优先，空则用实例缺省
    if (!userInfoOf(url, job->user, job->password)) {
        job->user = m_config.user;
        job->password = m_config.password;
    }
    // 预置 Basic 省一次 401 往返；调用方已自带 Authorization 头则不预置。
    // N50：流式作业不预置——QNAM 重发重定向请求时整份复制原请求（createRedirectRequest），
    // 预置头必然被带到重定向目标，而公开 API 无法在重发前改头（QNetworkReply::setRawHeader
    // 为 protected），只要预置了就挡不住跨 origin。凭据改由 authenticationRequired 注入、
    // origin 在那一侧校验；代价是首次请求多一次 401 往返，见规格 §2.8。
    const bool hasExplicitAuth =
        std::any_of(request.headers.cbegin(), request.headers.cend(),
                    [](const QPair<QByteArray, QByteArray> &h) {
                        return h.first.compare("Authorization", Qt::CaseInsensitive) == 0;
                    });
    if ((!job->user.isEmpty() || !job->password.isEmpty()) && !hasExplicitAuth && !job->streaming)
        job->presetAuth = basicAuthHeader(job->user, job->password);

    QMutexLocker l(&g_mutex);
    // HEAD 缺陷短路：判定与执行同处一地，命中即返回已结束作业，不发请求
    if (job->method == "HEAD" && g_transport && g_transport->headDefects.contains(originOf(url))) {
        job->finished = true;
        job->response.error =
            trText(QT_TRANSLATE_NOOP("HttpClient",
                                     "Server does not support HEAD (%1); request not sent"))
                .arg(originOf(url));
        return job;
    }
    QNetworkAccessManager *nam = ensureTransportLocked();
    if (!nam) {
        job->finished = true;
        job->response = canceledResponse(
            trText(QT_TRANSLATE_NOOP("HttpClient", "Application quitting; request not sent")));
        return job;
    }
    // 经队列连接投递到网络线程执行；限流与排队一律交给 QNAM，不自建闸门与队列
    QMetaObject::invokeMethod(nam, [job, nam] { runJob(job, nam); }, Qt::QueuedConnection);
    return job;
}

bool HttpClient::wait(RequestId id, int timeoutMs) {
    OXBOW_ASSERT_TASK_THREAD();
    if (!id)
        return false;
    if (timeoutMs != -1 && timeoutMs < 0)
        timeoutMs = -1; // 其他负数按 -1 计
    QDeadlineTimer deadline(timeoutMs < 0 ? QDeadlineTimer::Forever : QDeadlineTimer(timeoutMs));
    QMutexLocker l(&g_mutex);
    while (!id->finished) {
        if (timeoutMs == 0)
            return false; // 立即返回只做状态检测
        if (!g_cond.wait(&g_mutex, deadline))
            return false; // 到 deadline 仍未结束，作业仍在途
    }
    return true;
}

HttpClient::Response HttpClient::take(const RequestId &id) {
    OXBOW_ASSERT_TASK_THREAD();
    if (!id)
        return Response{
            0, {}, {}, trText(QT_TRANSLATE_NOOP("HttpClient", "Invalid job handle")), false};
    QMutexLocker l(&g_mutex);
    if (!id->finished)
        return Response{
            0,
            {},
            {},
            trText(QT_TRANSLATE_NOOP(
                "HttpClient", "Job not finished yet (contract: take after wait returns true)")),
            false};
    return id->response; // 副本、幂等，不留"取过就不能再取"的隐状态
}

void HttpClient::abort(const RequestId &id) {
    if (!id)
        return;
    QNetworkReply *reply = nullptr;
    QTimer *deadline = nullptr;
    QNetworkAccessManager *nam = nullptr;
    {
        // 先落定作业状态再投递中止消息：与完成的竞态以先落定者为准，
        // 保证 take 紧随 abort 也读到已落定的 canceled 响应
        QMutexLocker l(&g_mutex);
        if (id->finished)
            return; // 已完成 / 已撤销 / 已停机：abort 无效果，结果仍在
        id->canceled = true;
        id->finished = true;
        id->response = canceledResponse(trText(QT_TRANSLATE_NOOP("HttpClient", "Request aborted")));
        // 落定即摘除：计时器到期与后续重定向跳都不再指向本 reply，
        // 回收动作统一由本次投递在网络线程完成（QTimer::stop 非线程安全）
        reply = id->reply;
        id->reply = nullptr;
        deadline = id->deadline;
        id->deadline = nullptr;
        if (g_transport && g_transport->nam && !g_transport->quitting)
            nam = g_transport->nam;
        g_cond.wakeAll();
    }
    if (!nam)
        return; // 传输已停：QNAM 与其子对象（reply / timer）随传输一并销毁
    // QPointer 守护（N52 实测加固）：reply 的自然终态（如服务端断开触发
    // RemoteHostClosed）在本投递排队期间走 finalizeJob「abort 先落定」分支，
    // 该分支只回收 reply 不等本 lambda——排队执行到此处时 reply 可能已被
    // deleteLater 销毁，裸指针虚调用 abort() 即 0xC0000005 执行冲突。
    // reply 的析构与本 lambda 同在网络线程，QPointer 读写同线程，安全。
    QPointer<QNetworkReply> replyGuard(reply);
    QMetaObject::invokeMethod(
        nam,
        [replyGuard, deadline] {
            if (deadline) {
                deadline->stop();
                deadline->deleteLater();
            }
            // 尚未发出的作业正排在 QNAM 队列中，abort 把它摘掉且不产生网络流量
            if (replyGuard)
                replyGuard->abort();
        },
        Qt::QueuedConnection);
}

// ——N50 流式消费面。三条唤醒源供这些等待者共用：字节入队 / 响应头捕获 / 终态落定。

bool HttpClient::waitHeaders(RequestId id, int timeoutMs) {
    OXBOW_ASSERT_TASK_THREAD();
    if (!id)
        return false;
    if (timeoutMs != -1 && timeoutMs < 0)
        timeoutMs = -1; // 其他负数按 -1 计（与 wait 同款口径）
    QDeadlineTimer deadline(timeoutMs < 0 ? QDeadlineTimer::Forever : QDeadlineTimer(timeoutMs));
    QMutexLocker l(&g_mutex);
    while (!id->headReady && !id->finished) {
        if (timeoutMs == 0)
            return false; // 立即返回只做状态检测
        if (!g_cond.wait(&g_mutex, deadline))
            return false; // 窗口耗尽而作业仍在途：调用方必须自行 abort 收尾
    }
    return id->headReady || id->finished;
}

qint64 HttpClient::readBlocking(const RequestId &id, char *out, qint64 maxBytes, int timeoutMs) {
    OXBOW_ASSERT_TASK_THREAD();
    if (!id || !out || maxBytes <= 0)
        return kReadError;
    if (timeoutMs != -1 && timeoutMs < 0)
        timeoutMs = -1;
    QDeadlineTimer deadline(timeoutMs < 0 ? QDeadlineTimer::Forever : QDeadlineTimer(timeoutMs));

    // 锁内整块摘出（O(1)，不做搬运），锁外再拷进调用方缓冲——g_mutex 是全部作业共用的，
    // 不在这把锁里做大块 memcpy，见规格 §2.4。
    std::deque<QByteArray> local;
    qint64 localBytes = 0;
    bool wasFull = false;
    {
        QMutexLocker l(&g_mutex);
        for (;;) {
            if (id->chunksBytes > 0) {
                wasFull = id->chunksBytes >= id->streamBufferBytes;
                while (!id->chunks.empty() && localBytes < maxBytes) {
                    localBytes += id->chunks.front().size();
                    local.push_back(std::move(id->chunks.front()));
                    id->chunks.pop_front();
                }
                id->chunksBytes -= localBytes;
                break;
            }
            // 缓冲已空，此时才看终态——缓冲先于终态，否则会丢尾部数据（文件头纪律）
            if (id->finished)
                return (id->response.canceled || !id->response.error.isEmpty()) ? kReadError
                                                                                : kReadEof;
            if (timeoutMs == 0)
                return kReadNoData;
            if (!g_cond.wait(&g_mutex, deadline))
                return kReadNoData; // 窗口内没等到，不是失败：作业仍在途，下次接着等
        }
    }

    qint64 taken = 0;
    while (!local.empty() && taken < maxBytes) {
        QByteArray &head = local.front();
        const qint64 n = qMin<qint64>(maxBytes - taken, head.size());
        std::memcpy(out + taken, head.constData(), static_cast<size_t>(n));
        taken += n;
        if (n == head.size())
            local.pop_front();
        else
            head.remove(0, n); // 部分消费（短读）：余量留待下面归还
    }

    bool needPump = false;
    QNetworkAccessManager *nam = nullptr;
    {
        QMutexLocker l(&g_mutex);
        if (!local.empty()) {
            // 逆序 push_front 还原原有次序；网络线程只往尾部追加，前端无竞争者
            qint64 rest = 0;
            for (const QByteArray &c : local)
                rest += c.size();
            while (!local.empty()) {
                id->chunks.push_front(std::move(local.back()));
                local.pop_back();
            }
            id->chunksBytes += rest;
        }
        if (wasFull && id->chunksBytes < id->streamBufferBytes) {
            needPump = true;
            if (g_transport && g_transport->nam && !g_transport->quitting)
                nam = g_transport->nam;
        }
    }
    if (needPump && nam) {
        // 队列由满降到未满：投递一次，让网络线程把留在 reply 缓冲里的那块取回来
        // （不能只等 readyRead——没有新数据到达时它不会再来）
        QMetaObject::invokeMethod(nam, [id] { pumpStream(id); }, Qt::QueuedConnection);
    }
    return taken;
}

HttpClient::Response HttpClient::statusOf(const RequestId &id) {
    OXBOW_ASSERT_TASK_THREAD();
    if (!id)
        return Response{
            0, {}, {}, trText(QT_TRANSLATE_NOOP("HttpClient", "Invalid job handle")), false};
    QMutexLocker l(&g_mutex);
    if (id->finished)
        return id->response; // 已终态：终态响应（流式作业 body 恒空）
    Response r;              // 未终态：给已到达的响应头与状态码，body 恒空
    r.status = id->headStatus;
    r.headers = id->headHeaders;
    return r;
}

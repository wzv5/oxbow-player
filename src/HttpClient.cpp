#include "pch.h"

#include "HttpClient.h"

// N34 实现。分两块，均在 .cpp 内、不上头文件：
// 1. 共享传输（匿名命名空间）：专用网络线程 + 进程内唯一 QNAM + 缺陷表 + 等待原语。
//    client 对象只是不可变缺省值的持有者，构造与析构均不碰传输层；
// 2. 策略执行：总时限、自管重定向、Basic 预置 + authenticationRequired 兜底、
//    证书、headersOnly 截断、体上限、缺陷登记、池化快照、停机。
// 调用方不认识 QNAM；公开入口含策略，内部单跳 sendHop 为裸机制。

#define MPVQT_ASSERT_TASK_THREAD()                                                                 \
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

    // ——以下字段 submit 时初始化，发布后只读（重定向更新 url 系除外，见 prepareRedirect）——
    QElapsedTimer submitElapsed; // 总时限起点：submit 时启动，含排队，见规格 §2.5
    qint64 remainingMs = 0;      // 出队时换算的剩余时长（总时限 - 已流逝），sendHop 用它启动 timer
    Config snapshot;             // submit 时的实例缺省快照

    // ——以下字段仅网络线程访问，无锁——
    QTimer *deadline = nullptr; // 总时限计时（出队时按剩余时长启动，贯穿全部重定向跳）
    QUrl url;                   // 当前跳的 URL（重定向逐跳更新）
    QString user;               // 当前跳生效的凭据（重定向逐跳更新）
    QString password;
    QByteArray presetAuth; // 当前跳预置的 Authorization
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

// 全局一份互斥 + 条件变量：含义是"有作业状态发生变化"。每作业的状态字段由它保护，
// wait 按限时换算 deadline 等待，只在状态变化时醒来查一次。
QMutex g_mutex;
QWaitCondition g_cond;

// HEAD 缺陷判据阈值：已收到响应头却仍持续涌入 body 超 64KB 即判定（405 / 501 之外
// 的另一判据）。metaDataChanged 是异步派发的，该阈值同时覆盖缓冲先到的极端情形。
constexpr qint64 kHeadFlowDefectThreshold = 64 * 1024;

constexpr qint64 kDefaultMaxBodyBytes = 16 * 1024 * 1024;

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
    if (!job->deadline) {
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
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    // 连接复用一律是 QNAM 内建行为（同 host 6 连接，闲置缓存 120s），本篇不调
    // 任何池参数（保 Qt 6.2 兼容）；调用方纪律见规格 §2.4。
    for (const auto &h : job->extraHeaders)
        req.setRawHeader(h.first, h.second);
    if (!job->contentType.isEmpty() && !job->body.isEmpty())
        req.setRawHeader("Content-Type", job->contentType);
    if (!job->presetAuth.isEmpty())
        req.setRawHeader("Authorization", job->presetAuth);

    QNetworkReply *reply = nam->sendCustomRequest(req, job->method, job->body);
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

    // 截断（headersOnly）：响应头到达即断；随后的 finished（OperationCanceledError）
    // 在已取到头时按成功处理。metaDataChanged 异步派发可能多读少量字节，可接受——
    // 要防的是"读完整个文件"。重定向中间跳（301 / 302 / 303 / 307 / 308）不截断，
    // 由 finished 分支跟随，headersOnly 只取最终落点的头。
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [job, reply] {
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
            // 撤销 / 停机已先落定：结果已是 canceled，仅回收本跳 reply
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
    if (status > 0 && !job->timedOut && !job->overflow &&
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
        // 收尾时取回剩余字节，避免 body 静默丢尾部数据
        if (reply->bytesAvailable() > 0)
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
    MPVQT_ASSERT_TASK_THREAD();
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

    // 凭据：URL userinfo 优先，空则用实例缺省
    if (!userInfoOf(url, job->user, job->password)) {
        job->user = m_config.user;
        job->password = m_config.password;
    }
    // 预置 Basic 省一次 401 往返；调用方已自带 Authorization 头则不预置
    const bool hasExplicitAuth =
        std::any_of(request.headers.cbegin(), request.headers.cend(),
                    [](const QPair<QByteArray, QByteArray> &h) {
                        return h.first.compare("Authorization", Qt::CaseInsensitive) == 0;
                    });
    if ((!job->user.isEmpty() || !job->password.isEmpty()) && !hasExplicitAuth)
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
    MPVQT_ASSERT_TASK_THREAD();
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
    MPVQT_ASSERT_TASK_THREAD();
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
    QMetaObject::invokeMethod(
        nam,
        [reply, deadline] {
            if (deadline) {
                deadline->stop();
                deadline->deleteLater();
            }
            // 尚未发出的作业正排在 QNAM 队列中，abort 把它摘掉且不产生网络流量
            if (reply)
                reply->abort();
        },
        Qt::QueuedConnection);
}

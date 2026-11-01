#include "pch.h"

#include "AiTranslator.h"

#include "AiSubtitleCatalog.h"

AiTranslator::AiTranslator(QObject *parent) : QObject(parent) {}

void AiTranslator::requestCancel() {
    // GUI 线程直呼（见头注释）：旗一落，run 的轮询切片必然读到；在飞请求立即打断
    m_cancelRequested.store(true, std::memory_order_relaxed);
    if (const HttpClient::RequestId current = m_currentId.load())
        HttpClient::abort(current);
}

void AiTranslator::run(QString endpoint, QString apiKey, QString model, QString promptTemplate,
                       QString targetLang, QStringList sentences) {
    HttpClient::Config config;
    config.timeoutMs = AiSubtitleCatalog::kRequestTimeoutMs;
    const HttpClient client(config);

    const QUrl url = AiSubtitleCatalog::normalizeEndpoint(endpoint);
    // key 与 model 都按"非空才参与"处理：归一后空串即不带鉴权头、不进请求体
    const QString key = AiSubtitleCatalog::normalizeApiKey(apiKey);
    const QString modelValue = model.trimmed();

    for (int index = 0; index < sentences.size(); ++index) {
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            emit canceled();
            return;
        }
        // 先 trim 后判空：空句不发请求，直接视为完成（进度照常推进）
        const QString sentence = AiSubtitleCatalog::normalizedSentence(sentences.at(index));
        if (sentence.isEmpty()) {
            emit sentenceTranslated(index, QString());
            continue;
        }
        const QString content =
            AiSubtitleCatalog::renderPrompt(promptTemplate, targetLang, sentence);

        // 一句两份预算，各算各的：429 的等待重试按句计（上限 kRateLimitMaxRetries），
        // 不占本句失败的重试位；其余失败（网络错 / 非 2xx / 结构不符 / 空译文）同一句
        // 最多重试一次，仍失败即终止整条翻译，不逐句兜底、不保留原文继续
        int rateLimitRetries = 0;
        int attempts = 0; // 只数"本句失败重试"那一档；限速等待在 continue 里不进位
        for (;;) {
            const HttpClient::Response response = sendOne(client, url, key, modelValue, content);
            if (m_cancelRequested.load(std::memory_order_relaxed) || response.canceled) {
                emit canceled();
                return;
            }

            if (response.status == 429) {
                if (rateLimitRetries >= AiSubtitleCatalog::kRateLimitMaxRetries) {
                    emit failed(AiSubtitleCatalog::reasonRateLimitRetriesExhausted());
                    return;
                }
                const int waitMs = AiSubtitleCatalog::rateLimitWaitMs(
                    QString::fromUtf8(response.headers.value(QByteArrayLiteral("retry-after"))),
                    rateLimitRetries);
                ++rateLimitRetries;
                emit rateLimitWaiting(waitMs);
                if (!sleepInterruptible(waitMs)) {
                    emit canceled();
                    return;
                }
                continue; // 等待不算本句失败，同一句再来一次（attempts 不进位）
            }

            // 鉴权失败不重试：立即终止整条翻译（原因串不带 key）
            if (response.status == 401 || response.status == 403) {
                emit failed(AiSubtitleCatalog::reasonAuthenticationFailed());
                return;
            }

            const bool httpOk = response.status >= 200 && response.status < 300;
            QString translated;
            if (httpOk && AiSubtitleCatalog::parseTranslationReply(response.body, translated) &&
                !translated.isEmpty()) {
                emit sentenceTranslated(index, translated);
                break;
            }

            if (attempts >= 1) {
                const QString detail = response.error.isEmpty()
                                           ? AiSubtitleCatalog::reasonMalformedResponse()
                                           : response.error;
                emit failed(AiSubtitleCatalog::reasonTranslationFailed(detail));
                return;
            }
            ++attempts;
        }
    }
    emit succeeded();
}

HttpClient::Response AiTranslator::sendOne(const HttpClient &client, const QUrl &url,
                                           const QString &apiKey, const QString &model,
                                           const QString &content) {
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), content);
    QJsonObject body;
    body.insert(QStringLiteral("messages"), QJsonArray{message});
    body.insert(QStringLiteral("stream"), false);
    if (!model.isEmpty())
        body.insert(QStringLiteral("model"), model);

    HttpClient::Request request;
    request.url = url;
    request.method = QByteArrayLiteral("POST");
    request.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    request.contentType = QByteArrayLiteral("application/json");
    if (!apiKey.isEmpty())
        request.headers.append(
            {QByteArrayLiteral("Authorization"), QByteArray("Bearer ") + apiKey.toUtf8()});

    const HttpClient::RequestId id = client.submit(request);
    m_currentId.store(id); // 只给取消侧（GUI 线程）当 abort 的靶子，本线程一律用局部 id
    // N34 的可取消上下文纪律：wait 切片 ≤100ms，旗落即 abort（take 得 canceled 响应）
    while (!HttpClient::wait(id, 100)) {
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            HttpClient::abort(id);
            break;
        }
    }
    const HttpClient::Response response = HttpClient::take(id);
    m_currentId.store(nullptr);
    return response;
}

bool AiTranslator::sleepInterruptible(int totalMs) {
    const QDeadlineTimer deadline(totalMs);
    while (!deadline.hasExpired()) {
        if (m_cancelRequested.load(std::memory_order_relaxed))
            return false;
        QThread::msleep(qMin<qint64>(100, deadline.remainingTime()));
    }
    return !m_cancelRequested.load(std::memory_order_relaxed);
}

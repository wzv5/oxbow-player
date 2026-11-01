#pragma once

#include "pch.h"

#include "HttpClient.h"

// N146：翻译的工人线程侧——AI 链路唯一的自建线程（HttpClient 的 wait / exec 不许在
// GUI 线程调用，内部有断言，而本场景是可取消上下文）。逐句串行、无上下文、不并发；
// 429 有独立的重试预算（不算本句失败），其余失败同一句最多重试一次，仍失败即终止
// 整条。每句结果经信号排队回 GUI，由任务对象写回曲目表。
//
// 取消通路有两段，都必须绕开"run 长期占用工人线程事件循环"这一事实：
//   - 轮询段：requestCancel() 由 GUI 线程**直呼**（不走队列——排队永远轮不到），
//     原子置旗；run 的 ≤100ms 切片轮询读到即收口；
//   - 在飞请求：HttpClient::abort(id) 任意线程可调，请求立即落定，等待者必然醒来。
// 取消旗只由 requestCancel 置位、run 全程只读：对象一次运行一个，无跨 run 残留，
// 故不得在 run 开头清旗——那会吞掉恰在 run 出队之前落下的取消。
class AiTranslator : public QObject {
    Q_OBJECT
  public:
    explicit AiTranslator(QObject *parent = nullptr);

    // GUI 线程直呼：置取消旗并打断在飞请求（幂等；无在跑 run 时无效果）
    void requestCancel();

  public slots:
    // 一条翻译会话：在工人线程串行跑完全部句子。endpoint 自定义档传用户原文
    // （归一在此做），内置档传 http://127.0.0.1:<端口>/v1；apiKey/model 仅自定义档
    // 参与（鉴权头非空才带，model 非空才进请求体）。终态三选一：
    // succeeded / failed(reason) / canceled，reason 已是可展示文案（不含 key）
    void run(QString endpoint, QString apiKey, QString model, QString promptTemplate,
             QString targetLang, QStringList sentences);

  signals:
    void sentenceTranslated(int index, const QString &text); // 空句也发（进度照常推进）
    void rateLimitWaiting(int waitMs);                       // 429 等待期（步骤仍停在翻译）
    void succeeded();
    void failed(const QString &reason);
    void canceled();

  private:
    HttpClient::Response sendOne(const HttpClient &client, const QUrl &url, const QString &apiKey,
                                 const QString &model, const QString &content);
    // 可打断的等待：100ms 切片轮询取消旗；假 = 被取消
    bool sleepInterruptible(int totalMs);

    std::atomic_bool m_cancelRequested{false};
    std::atomic<HttpClient::RequestId> m_currentId{nullptr}; // 仅工人线程写，取消侧只读打断
};

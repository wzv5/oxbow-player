#pragma once

#include "pch.h"

// N34：HttpClient 封装——调用方持有轻量 client 对象，构造时绑定缺省 host、
// 用户信息与缺省参数，之后请求只需传 path；对象寿命归调用方，构造后不可变、
// 可随处持有与析构，在途作业独立于对象寿命。传输层（QNAM + 专用网络线程 +
// 缺陷表 + 等待原语）进程内全局共享保连接复用，首次构造 client 时惰性创建
// （GUI 线程首触阻塞约毫秒级，一次性）。无池级配置接口：连接复用与并发约束
// 一律是 QNAM 内建行为（同 host 6 连接），调用方按小批量纪律使用。
//
// 调用方纪律（规格 N34 §2.6）：凡所在上下文可取消（N32 任务体内），禁用 wait(-1)
// 与 exec（exec 即 submit + wait(-1) + take），必须以 ≤100ms 切片轮询自身取消旗，
// 命中即 abort。标准写法（各调用点照抄同一形状）：
//
//   auto id = client.submit(req);
//   while (!HttpClient::wait(id, 100)) {
//       if (shouldAbort()) {          // 调用方自己的取消旗
//           HttpClient::abort(id);
//           break;
//       }
//   }
//   return HttpClient::take(id);      // 撤销后得 canceled 响应
//
// 无取消需求的调用点（属性对话框等同步路径）用 exec 即可。

class HttpClient {
  public:
    struct Config {
        QUrl baseUrl;         // 可空；空即未绑定，请求须带完整 URL
        QString user;         // 原始串（非编码）；空则从请求 URL 的 userinfo 取
        QString password;     // 同上
        int timeoutMs = 5000; // 总时限（毫秒），自 submit 起算（含排队、建连、
                              // 传输与全部重定向跳）；非正数按 5000 计，见规格 §2.5
        bool ignoreSslErrors = false;
        int maxRedirects = 5;                   // 自管重定向的跳数上限
        qint64 maxBodyBytes = 16 * 1024 * 1024; // 正常体上限（字节），超限按失败返回
    };

    struct Request {
        QUrl url;                                     // 相对→按 baseUrl 解析；绝对→直接用
        QByteArray method;                            // "HEAD" / "GET" / "PROPFIND"
        QByteArray body;                              // 空即无请求体
        QByteArray contentType;                       // 有 body 时用（PROPFIND 需要）
        QList<QPair<QByteArray, QByteArray>> headers; // 额外头（Depth 等）
        bool headersOnly = false;                     // 取到响应头即断
    };

    struct Response {
        int status = 0;
        QHash<QByteArray, QByteArray> headers; // 键一律小写，调用方以小写查
        QByteArray body;                       // 上限见 Config::maxBodyBytes，超限按失败返回
        QString error;         // 英文文案（QT_TRANSLATE_NOOP 标注 + translate 取值），
                               // 空即无错；调用方原样透传给 UI 显示
        bool canceled = false; // 调用方撤销；为真时其余字段无意义
    };

    // 不透明作业句柄：只持有、不窥探、不 reset；不依附任何 client 对象寿命，
    // 析构后作业照常跑完，wait / take 不受影响。
    struct HttpJobState;
    using RequestId = std::shared_ptr<HttpJobState>;

    // 值语义，构造后不可变，可拷贝；任意线程构造。
    explicit HttpClient(const Config &config);

    // 阻塞形态（= submit + wait(-1) + take），仅任务线程（GUI 线程禁用，断言守候），
    // 仅限无取消需求的调用点；可取消上下文禁用，见文件头调用方纪律。
    Response exec(const Request &request) const;

    // 含 HEAD 缺陷短路：命中即返回已结束作业，不发请求。可任意线程。
    RequestId submit(const Request &request) const;

    // 等待系为 static：纯作业级。仅任务线程（GUI 线程禁用，断言守候）。
    // 真=作业已结束（成功 / 失败 / 撤销都算结束，调 take 取结果）；
    // 假=超时，作业仍在途。timeoutMs：-1=永远等；0=立即返回只做状态检测；
    // 正数=毫秒；其他负数按 -1 计。可取消上下文禁传 -1（调用方纪律，见文件头）。
    static bool wait(RequestId id, int timeoutMs = -1);

    // 副本、幂等；撤销后 take 得 canceled 响应。
    static Response take(const RequestId &id);

    // 任意线程可调；已完成作业的 abort 无效果，结果仍在（先落定者为准）。
    static void abort(const RequestId &id);

  private:
    Config m_config; // 构造后不可变的缺省值持有者
};

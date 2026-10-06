// HttpClient 单元测试（N34）：外部 Python 桩服务器（tests/httpserver_stub.py，
// 纯 stdlib）提供真实 HTTP/HTTPS 端点——initTestCase 启动（QProcess，随机端口，
// stdout 一行 STUB-READY JSON 携带端口与现场签发的自签 CA PEM），cleanupTestCase
// 自动结束。凭据 / 重定向 / 缺陷登记的核对以桩侧请求日志为准（/__log、/__reset）。
// exec / wait / take 禁止在应用主线程调用（断言守候），一律经 std::thread 包裹调用。
// TLS：先用未受信证书验证握手失败与 ignoreSslErrors 两态，再把 CA 加进进程内
// 默认 QSslConfiguration 验证"加入可信根后通过"。

#include "pch.h"

#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QtTest/QtTest>

#include <atomic>
#include <thread>
#include <utility>

#include "HttpClient.h"

namespace {

// exec / wait / take 的应用主线程禁用断言：经 std::thread 包裹调用
HttpClient::Response execInThread(const HttpClient &c, const HttpClient::Request &r) {
    HttpClient::Response out;
    std::thread th([&] { out = c.exec(r); });
    th.join();
    return out;
}

bool waitInThread(const HttpClient::RequestId &id, int timeoutMs) {
    bool ok = false;
    std::thread th([&] { ok = HttpClient::wait(id, timeoutMs); });
    th.join();
    return ok;
}

HttpClient::Response takeInThread(const HttpClient::RequestId &id) {
    HttpClient::Response out;
    std::thread th([&] { out = HttpClient::take(id); });
    th.join();
    return out;
}

// ——N50 流式消费面同样禁止在应用主线程调用，一律经 std::thread 包裹——

bool waitHeadersInThread(const HttpClient::RequestId &id, int timeoutMs) {
    bool ok = false;
    std::thread th([&] { ok = HttpClient::waitHeaders(id, timeoutMs); });
    th.join();
    return ok;
}

// 取一次：> 0 为字节数（并入 out），其余为 kRead* 哨兵
qint64 readOnceInThread(const HttpClient::RequestId &id, QByteArray &out, qint64 maxBytes,
                        int timeoutMs = -1) {
    char buf[64 * 1024];
    const qint64 want = qMin<qint64>(maxBytes, static_cast<qint64>(sizeof(buf)));
    qint64 n = HttpClient::kReadNoData;
    std::thread th([&] {
        n = HttpClient::readBlocking(id, buf, want, timeoutMs);
        if (n > 0)
            out.append(buf, static_cast<int>(n));
    });
    th.join();
    return n;
}

struct Drain {
    qint64 bytes = 0;
    qint64 terminal = HttpClient::kReadNoData; // kReadEof / kReadError；预算耗尽则维持 kReadNoData
};

// 反复读到终态。带预算上限：作业若卡住，用例断言失败而不是挂死整个测试进程
Drain drainInThread(const HttpClient::RequestId &id, QByteArray &out, int budgetMs = 30000) {
    Drain res;
    std::thread th([&] {
        QElapsedTimer clock;
        clock.start();
        char buf[64 * 1024];
        for (;;) {
            const qint64 n =
                HttpClient::readBlocking(id, buf, static_cast<qint64>(sizeof(buf)), 500);
            if (n > 0) {
                out.append(buf, static_cast<int>(n));
                res.bytes += n;
                continue;
            }
            if (n == HttpClient::kReadNoData) {
                if (clock.elapsed() > budgetMs)
                    return;
                continue;
            }
            res.terminal = n;
            return;
        }
    });
    th.join();
    return res;
}

// 流式 GET 请求的构造快捷方式（watermark <= 0 即取内置缺省水位）
HttpClient::Request streamingGet(const QUrl &url, qint64 watermark = 0) {
    HttpClient::Request r;
    r.url = url;
    r.method = "GET";
    r.streaming = true;
    r.streamBufferBytes = watermark;
    return r;
}

HttpClient::Response statusOfInThread(const HttpClient::RequestId &id) {
    HttpClient::Response out;
    std::thread th([&] { out = HttpClient::statusOf(id); });
    th.join();
    return out;
}

// ——Python 桩的生命周期管理——
class PythonStub {
  public:
    bool start() {
        QString py = QStandardPaths::findExecutable(QStringLiteral("python3.13"));
        if (py.isEmpty())
            py = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (py.isEmpty())
            py = QStandardPaths::findExecutable(QStringLiteral("python"));
        if (py.isEmpty()) {
            m_error = QStringLiteral("PATH 上找不到 python（桩服务器需要）");
            return false;
        }
        const QString script =
            QString::fromUtf8(QT_TESTCASE_SOURCEDIR) + QStringLiteral("/tests/httpserver_stub.py");
        m_proc.setProgram(py);
        m_proc.setArguments({script});
        QObject::connect(&m_proc, &QProcess::readyReadStandardOutput, [&] {
            m_out += m_proc.readAllStandardOutput();
            if (m_ready)
                return;
            const int nl = m_out.indexOf('\n');
            if (nl < 0 || !m_out.startsWith("STUB-READY "))
                return;
            const auto doc = QJsonDocument::fromJson(m_out.mid(11, nl - 11));
            m_http = quint16(doc.object().value("http").toInt());
            m_defect = quint16(doc.object().value("defect").toInt());
            m_flow = quint16(doc.object().value("flow").toInt());
            m_auth = quint16(doc.object().value("auth").toInt());
            m_auth2 = quint16(doc.object().value("auth2").toInt());
            m_https = quint16(doc.object().value("https").toInt());
            m_tls2 = quint16(doc.object().value("tls2").toInt());
            m_ca = doc.object().value("ca").toString().toUtf8();
            m_ready = m_http && m_defect && m_flow && m_auth && m_auth2 && m_https && m_tls2 &&
                      !m_ca.isEmpty();
        });
        QObject::connect(&m_proc, &QProcess::readyReadStandardError,
                         [&] { m_err += m_proc.readAllStandardError(); });
        QObject::connect(&m_proc, &QProcess::finished, [&](int code) {
            m_exitCode = code;
            m_exited = true;
        });
        m_proc.start();
        // 失败判据不得用「state() != Running」：start() 之后 started() 尚经事件循环投递，
        // 这一刻 state() 仍是 NotRunning，旧判据把「还没起来」当成「起来又死了」立刻返回
        // ——桩其实正常在跑（Linux 实测：0.03s 报未就绪、stderr 空、进程退出时才被销毁）
        if (!m_proc.waitForStarted(10000)) {
            m_error = QStringLiteral("桩进程未能启动：%1（%2）").arg(m_proc.errorString(), py);
            return false;
        }
        const bool settled = QTest::qWaitFor([&] { return m_ready || m_exited; }, 30000);
        if (m_ready)
            return true;
        m_out += m_proc.readAllStandardOutput(); // 收尾再排一次，末行可能刚到
        m_err += m_proc.readAllStandardError();
        QStringList missing;
        if (!m_http)
            missing << QStringLiteral("http");
        if (!m_defect)
            missing << QStringLiteral("defect");
        if (!m_flow)
            missing << QStringLiteral("flow");
        if (!m_auth)
            missing << QStringLiteral("auth");
        if (!m_auth2)
            missing << QStringLiteral("auth2");
        if (!m_https)
            missing << QStringLiteral("https");
        if (!m_tls2)
            missing << QStringLiteral("tls2");
        if (m_ca.isEmpty())
            missing << QStringLiteral("ca");
        m_error =
            QStringLiteral("桩服务器未就绪（timeout=%1 readyLine=%2 exited=%3 "
                           "exitCode=%4 error=%5 缺项=%6）：%7")
                .arg(settled ? QStringLiteral("no") : QStringLiteral("yes"))
                .arg(m_out.contains("STUB-READY") ? QStringLiteral("yes") : QStringLiteral("no"))
                .arg(m_exited ? 1 : 0)
                .arg(m_exitCode)
                .arg(m_proc.errorString(),
                     missing.isEmpty() ? QStringLiteral("无") : missing.join(QLatin1Char(',')),
                     QString::fromUtf8(m_err).simplified());
        return false;
    }

    void stop() {
        if (m_proc.state() == QProcess::Running) {
            m_proc.kill();
            m_proc.waitForFinished(5000);
        }
    }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_http)); }
    QUrl defectUrl() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_defect)); }
    QUrl flowUrl() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_flow)); }
    QUrl authUrl() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_auth)); }
    // 跨 origin 重定向落点专用：须缓存干净（见 stub 文件头 D 端口注释）
    QUrl auth2Url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_auth2)); }
    QUrl tlsUrl() const { return QUrl(QStringLiteral("https://127.0.0.1:%1").arg(m_https)); }
    QUrl tls2Url() const { return QUrl(QStringLiteral("https://127.0.0.1:%1").arg(m_tls2)); }
    QByteArray caPem() const { return m_ca; }
    const QString &error() const { return m_error; }

    // ——请求日志（经 HttpClient 访问 /__log、/__reset，二者自身不记日志）——
    struct Entry {
        QByteArray method;
        QByteArray path;
        bool auth = false;
        QByteArray scheme; // 认证方案（"Basic" / "Digest" / ""），挑战往返诊断用
        QByteArray authv;  // 完整 Authorization，失败深查（手算摘要）用
    };
    struct Snapshot {
        QList<Entry> requests;
        int connTotal = 0; // 累计连接数（keep-alive 下多请求共享一条）
        int connPeak = 0;  // 峰值并发连接数
    };

    bool resetLogs() {
        HttpClient::Config cfg; // 空 baseUrl，绝对 URL 直用
        HttpClient c{cfg};
        HttpClient::Request r;
        r.method = "POST";
        r.url = QUrl(QStringLiteral("http://127.0.0.1:%1/__reset").arg(m_http));
        if (execInThread(c, r).status != 200)
            return false;
        r.url = QUrl(QStringLiteral("http://127.0.0.1:%1/__reset").arg(m_defect));
        if (execInThread(c, r).status != 200)
            return false;
        r.url = QUrl(QStringLiteral("http://127.0.0.1:%1/__reset").arg(m_flow));
        if (execInThread(c, r).status != 200)
            return false;
        r.url = QUrl(QStringLiteral("http://127.0.0.1:%1/__reset").arg(m_auth));
        if (execInThread(c, r).status != 200)
            return false;
        r.url = QUrl(QStringLiteral("http://127.0.0.1:%1/__reset").arg(m_auth2));
        return execInThread(c, r).status == 200;
    }

    Snapshot snapshot(quint16 port) {
        HttpClient::Config cfg;
        HttpClient c{cfg};
        HttpClient::Request r;
        r.method = "GET";
        r.url = QUrl(QStringLiteral("http://127.0.0.1:%1/__log").arg(port));
        Snapshot out;
        const auto resp = execInThread(c, r);
        if (resp.status != 200)
            return out;
        const auto obj = QJsonDocument::fromJson(resp.body).object();
        const auto arr = obj.value("requests").toArray();
        for (const auto &v : arr) {
            const auto o = v.toObject();
            out.requests.append({o.value("method").toString().toUtf8(),
                                 o.value("path").toString().toUtf8(), o.value("auth").toBool(),
                                 o.value("scheme").toString().toUtf8(),
                                 o.value("authv").toString().toUtf8()});
        }
        const auto conn = obj.value("connections").toObject();
        out.connTotal = conn.value("total").toInt();
        out.connPeak = conn.value("peak").toInt();
        return out;
    }

    static int count(const QList<Entry> &log, const QByteArray &method, const QByteArray &path) {
        int n = 0;
        for (const auto &e : log)
            if (e.method == method && e.path == path)
                ++n;
        return n;
    }

  private:
    QProcess m_proc;
    QByteArray m_out;
    QByteArray m_err;
    bool m_ready = false;
    bool m_exited = false;
    int m_exitCode = 0;
    quint16 m_http = 0, m_defect = 0, m_flow = 0, m_auth = 0, m_auth2 = 0, m_https = 0, m_tls2 = 0;
    QByteArray m_ca;
    QString m_error;
};

} // namespace

class TestHttpClient : public QObject {
    Q_OBJECT

  private slots:
    // 启动 Python 桩（随机端口，现场签发自签证书）
    void initTestCase() { QVERIFY2(m_stub.start(), qPrintable(m_stub.error())); }

    void cleanupTestCase() { m_stub.stop(); }

    // baseUrl 为空且相对 URL → 可读失败、不发请求
    void relativeUrlWithoutBaseFailsWithoutRequest() {
        HttpClient c{HttpClient::Config{}};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/x"));
        r.method = "GET";
        const auto resp = execInThread(c, r);
        QVERIFY(!resp.error.isEmpty());
        QCOMPARE(resp.status, 0);
        QVERIFY(!resp.canceled);
    }

    // 空方法为调用错误：返回可读失败，不发请求
    void emptyMethodFailsWithoutSending() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/b"));
        r.method = "";
        const auto resp = execInThread(c, r);
        QVERIFY(resp.error.contains(QStringLiteral("method")));
        QCOMPARE(resp.status, 0);
        QVERIFY(!resp.canceled);
        const auto log = m_stub.snapshot(m_stub.url().port()).requests;
        QVERIFY(log.isEmpty()); // 未发出
    }

    // 响应头归一化：键一律小写，调用方以小写查
    void responseHeadersAreLowercased() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/b"));
        r.method = "GET";
        const auto resp = execInThread(c, r);
        QCOMPARE(resp.status, 200);
        QVERIFY(!resp.headers.isEmpty());
        // 桩对每个响应都发 Content-Length（驼峰头），归一化后以小写可见
        QVERIFY(resp.headers.contains("content-length"));
        const auto keys = resp.headers.keys();
        QVERIFY(std::all_of(keys.cbegin(), keys.cend(),
                            [](const QByteArray &k) { return k == k.toLower(); }));
    }

    // 缺省绑定：只传 path 即可打到正确地址且免 401（预置 Basic 头，由服务端授权即证）；
    // 错误凭据得 401 且错误文案可读
    void baseUrlBindingCarriesBasicAuth() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.user = QStringLiteral("u1");
        cfg.password = QStringLiteral("p1");
        HttpClient c{cfg};
        HttpClient::Request ok;
        ok.url = QUrl(QStringLiteral("/basic/file.bin"));
        ok.method = "GET";
        const auto good = execInThread(c, ok);
        QVERIFY(good.error.isEmpty());
        QCOMPARE(good.status, 200);
        QCOMPARE(good.body, QByteArray("basic ok"));

        HttpClient::Config bad = cfg;
        bad.user = QStringLiteral("uX");
        bad.password = QStringLiteral("pX");
        HttpClient c2{bad};
        const auto denied = execInThread(c2, ok);
        QCOMPARE(denied.status, 401);
        QVERIFY(!denied.error.isEmpty());
        QVERIFY(!denied.canceled);
    }

    // 绝对 URL 直接用；URL userinfo 优先于实例凭据：
    // 正确 userinfo 通过；错误 userinfo 得 401（若实例凭据被误用会得 200）
    void absoluteUrlAndUserinfoPriority() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.user = QStringLiteral("u1");
        cfg.password = QStringLiteral("p1");
        HttpClient c{cfg};
        HttpClient::Request r;
        r.method = "GET";
        r.url = QUrl(QStringLiteral("http://u1:p1@127.0.0.1:%1/basic/a").arg(m_stub.url().port()));
        QCOMPARE(execInThread(c, r).status, 200);
        r.url =
            QUrl(QStringLiteral("http://alice:sec@127.0.0.1:%1/basic/b").arg(m_stub.url().port()));
        const auto denied = execInThread(c, r);
        QCOMPARE(denied.status, 401);
    }

    // Digest 回退：预置 Basic 的首打被 Digest 挑战（Basic 头不被接受），经
    // authenticationRequired 带 Digest 重试后通过。独占 auth origin：
    // QNAM 按连接缓存旧 authenticator，同 origin 换凭据会多一次旧凭据尝试，
    // 计数断言与用例顺序耦合，故隔离（见桩注释）。
    void digestAuthViaChallenge() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.authUrl();
        cfg.user = QStringLiteral("u2");
        cfg.password = QStringLiteral("p2");
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/digest/f"));
        r.method = "GET";
        const auto resp = execInThread(c, r);
        QVERIFY2(resp.error.isEmpty(),
                 qPrintable(QStringLiteral("status=%1 error=%2").arg(resp.status).arg(resp.error)));
        QCOMPARE(resp.status, 200);
        QCOMPARE(resp.body, QByteArray("digest ok"));
        const auto log = m_stub.snapshot(m_stub.authUrl().port()).requests;
        // 挑战往返形状一次钉死：首打预置 Basic 被挑战，重试走 Digest 通过。
        // 失配时打印完整请求头供手算摘要定位（QCOMPARE 只打印 size，不够用）。
        QStringList schemes;
        QStringList authvs;
        for (const auto &e : log)
            if (e.path == "/digest/f") {
                schemes << QString::fromUtf8(e.scheme);
                authvs << QString::fromUtf8(e.authv);
            }
        QVERIFY2(schemes == QStringList({"Basic", "Digest"}),
                 qPrintable(QStringLiteral("schemes=[%1]\nauthvs:\n%2")
                                .arg(schemes.join(","), authvs.join("\n"))));

        HttpClient::Config bad = cfg;
        bad.password = QStringLiteral("wrong");
        HttpClient c2{bad};
        QCOMPARE(execInThread(c2, r).status, 401);
    }

    // 重定向：301 / 303 后仍以 PROPFIND（同方法同 body 同头）打到新地址，
    // 不被降格成 GET；循环跳转超上限失败且错误可读
    void redirectKeepsMethodAndBody() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request r;
        r.method = "PROPFIND";
        r.body = "<propfind/>";
        r.contentType = "application/xml";

        r.url = QUrl(QStringLiteral("/redir-rel/a"));
        const auto rel = execInThread(c, r);
        QVERIFY(rel.error.isEmpty());
        QCOMPARE(rel.status, 207);
        QCOMPARE(rel.body, QByteArray("MS:<propfind/>")); // 请求体原样带到落点

        r.url = QUrl(QStringLiteral("/redir-303/a"));
        const auto see = execInThread(c, r);
        QCOMPARE(see.status, 207);
        QCOMPARE(see.body, QByteArray("MS:<propfind/>")); // 303 同样保方法保 body

        r.url = QUrl(QStringLiteral("/redir-loop/a"));
        const auto loop = execInThread(c, r);
        QVERIFY(loop.error.contains(QStringLiteral("Too many redirects")));

        const auto mainLog = m_stub.snapshot(m_stub.url().port()).requests;
        QCOMPARE(PythonStub::count(mainLog, "PROPFIND", "/redir-rel/a"), 1);
        QCOMPARE(PythonStub::count(mainLog, "PROPFIND", "/b"), 2);            // 相对与 303 各到一次
        QCOMPARE(PythonStub::count(mainLog, "GET", "/b"), 0);                 // 不出现被降格的 GET
        QCOMPARE(PythonStub::count(mainLog, "PROPFIND", "/redir-loop/a"), 6); // 1 + 5 跳
    }

    // 跨 origin 跳转不泄漏旧凭据：新 origin 不等实例 baseUrl origin、新 URL 无 userinfo
    // → 不带 Authorization（以桩侧日志的 auth 标志为准）
    void redirectAcrossOriginsDropsCredentials() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.user = QStringLiteral("uA");
        cfg.password = QStringLiteral("pA");
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/redir-abs/a"));
        r.method = "PROPFIND";
        const auto resp = execInThread(c, r);
        QCOMPARE(resp.status, 207);
        const auto mainLog = m_stub.snapshot(m_stub.url().port()).requests;
        const auto defectLog = m_stub.snapshot(m_stub.defectUrl().port()).requests;
        QCOMPARE(PythonStub::count(mainLog, "PROPFIND", "/redir-abs/a"), 1);
        QCOMPARE(PythonStub::count(defectLog, "PROPFIND", "/b"), 1);
        QVERIFY(std::any_of(mainLog.cbegin(), mainLog.cend(), [](const PythonStub::Entry &e) {
            return e.path == "/redir-abs/a" && e.auth;
        }));
        QVERIFY(std::none_of(defectLog.cbegin(), defectLog.cend(), [](const PythonStub::Entry &e) {
            return e.path == "/b" && e.auth; // 新 host 未泄漏凭据
        }));
    }

    // 重定向后的跳仍受总时限约束：deadline 自 submit 起算、贯穿全部跳且不重置，
    // 计时器不得随中间跳 reply 的销毁一并失效（落点 /slow 睡 8s，须在总时限处断开）
    void redirectedHopStillHitsTotalDeadline() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.timeoutMs = 800;
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/redir-slow/a"));
        r.method = "GET";
        QElapsedTimer clock;
        clock.start();
        const auto resp = execInThread(c, r);
        const qint64 elapsed = clock.elapsed();
        QVERIFY2(resp.error.contains(QStringLiteral("timed out")),
                 qPrintable(QStringLiteral("status=%1 error=%2").arg(resp.status).arg(resp.error)));
        QVERIFY(!resp.canceled);
        QVERIFY(elapsed >= 700);
        QVERIFY(elapsed < 3000); // 未悬挂到服务端的 8s
    }

    // 超时：慢服务器在总时限处返回失败，不悬挂、不撤销
    void totalTimeoutOnSilentServer() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.timeoutMs = 300;
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/slow"));
        r.method = "GET";
        QElapsedTimer clock;
        clock.start();
        const auto resp = execInThread(c, r);
        const qint64 elapsed = clock.elapsed();
        QVERIFY(!resp.error.isEmpty());
        QVERIFY(!resp.canceled);
        QCOMPARE(resp.status, 0);
        QVERIFY(elapsed >= 250); // 不早于时限
        QVERIFY(elapsed < 5000); // 且不悬挂到缺省总时限
    }

    // 总时限非闲置超时：慢速但持续有字节的传输同样在总时限处断开
    // （trickle 全程 ~5s、总时限 800ms；若误用闲置超时则永不断开）
    void trickleStillHitsTotalDeadline() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.timeoutMs = 800;
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/trickle"));
        r.method = "GET";
        QElapsedTimer clock;
        clock.start();
        const auto resp = execInThread(c, r);
        const qint64 elapsed = clock.elapsed();
        QVERIFY(resp.error.contains(QStringLiteral("timed out")));
        QVERIFY(!resp.canceled);
        QVERIFY(elapsed >= 700);
        QVERIFY(elapsed < 3000);
    }

    // headersOnly：响应头到达即断，不读完整个"文件"
    void headersOnlyTruncatesAtHeaders() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/big"));
        r.method = "GET";
        r.headersOnly = true;
        const auto resp = execInThread(c, r);
        QVERIFY(resp.error.isEmpty()); // 截断按成功处理
        QCOMPARE(resp.status, 200);
        // 调用方只读头：体不进缓冲区（仅保留到缺陷判据阈值，超出即丢弃）
        QVERIFY2(resp.body.size() < 256 * 1024,
                 qPrintable(QStringLiteral("body=%1").arg(resp.body.size())));
    }

    // headersOnly 跟随重定向：取的是最终落点的响应头（200），而非中间跳的 301
    void headersOnlyFollowsRedirect() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/redir-rel/a"));
        r.method = "GET";
        r.headersOnly = true;
        const auto resp = execInThread(c, r);
        QVERIFY2(resp.error.isEmpty(),
                 qPrintable(QStringLiteral("status=%1 error=%2").arg(resp.status).arg(resp.error)));
        QCOMPARE(resp.status, 200);
    }

    // 响应体上限：超上限按失败返回（status/headers 有效、body 空、error 可读、不撤销）
    void bodyLimitFailure() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.maxBodyBytes = 1000;
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/big"));
        r.method = "GET";
        const auto resp = execInThread(c, r);
        QVERIFY(resp.error.contains(QStringLiteral("exceeds limit")));
        QCOMPARE(resp.status, 200); // status / headers 保留
        QVERIFY(!resp.headers.isEmpty());
        QVERIFY(resp.body.isEmpty()); // 超限部分丢弃
        QVERIFY(!resp.canceled);
    }

    // 超限不登记缺陷：超限是调用方 guard 而非服务端缺陷，同 origin 后续 HEAD 照常发出
    // （若被误登记，第二个 HEAD 得 status=0 且 error 含 "not sent"）
    void bodyOverflowDoesNotRegisterDefect() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.maxBodyBytes = 1000;
        HttpClient c{cfg};
        HttpClient::Request big;
        big.url = QUrl(QStringLiteral("/big"));
        big.method = "GET";
        const auto overflow = execInThread(c, big);
        QVERIFY(overflow.error.contains(QStringLiteral("exceeds limit")));

        HttpClient::Request head;
        head.url = QUrl(QStringLiteral("/b"));
        head.method = "HEAD";
        const auto after = execInThread(c, head);
        QCOMPARE(after.status, 200);
        QVERIFY(!after.error.contains(QStringLiteral("not sent")));
        const auto log = m_stub.snapshot(m_stub.url().port()).requests;
        QCOMPARE(PythonStub::count(log, "HEAD", "/b"), 1); // HEAD 确实发出
    }

    // 正确 HEAD 不登记缺陷：成功 HEAD（200、无流错误）不得污染缺陷表，
    // 同 origin 后续 HEAD 照常发出（桩侧两次均有记录），不受短路影响
    void successfulHeadDoesNotRegisterDefect() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request head;
        head.url = QUrl(QStringLiteral("/b"));
        head.method = "HEAD";
        const auto first = execInThread(c, head);
        QVERIFY2(
            first.error.isEmpty(),
            qPrintable(QStringLiteral("status=%1 error=%2").arg(first.status).arg(first.error)));
        QCOMPARE(first.status, 200);
        const auto second = execInThread(c, head);
        QCOMPARE(second.status, 200); // 未被短路（短路返回 status=0）
        QVERIFY(second.error.isEmpty());
        const auto log = m_stub.snapshot(m_stub.url().port()).requests;
        QCOMPARE(PythonStub::count(log, "HEAD", "/b"), 2); // 两次均发出
    }

    // 缺陷登记：HEAD 405 → 登记后同 origin 的后续 HEAD 由 submit 短路（换新 client 亦然），
    // GET 不受影响；短路未发请求以桩侧日志为准
    void headDefectRegisteredAndShortCircuits() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.defectUrl();
        HttpClient c1{cfg};
        HttpClient::Request head;
        head.url = QUrl(QStringLiteral("/x"));
        head.method = "HEAD";
        const auto first = execInThread(c1, head);
        QCOMPARE(first.status, 405);
        QVERIFY(first.error.contains(QStringLiteral("does not support HEAD")));

        HttpClient c2{cfg}; // 换一台新 client 同 origin 仍短路
        const auto id = c2.submit(head);
        QVERIFY(id != nullptr);
        const auto second = takeInThread(id);
        QCOMPARE(second.status, 0);
        QVERIFY(second.error.contains(QStringLiteral("not sent")));
        QVERIFY(!second.canceled);

        HttpClient::Request get;
        get.url = QUrl(QStringLiteral("/y"));
        get.method = "GET";
        const auto third = execInThread(c2, get); // GET / PROPFIND 不受缺陷影响
        QVERIFY(third.error.isEmpty());
        QCOMPARE(third.status, 200);

        const auto log = m_stub.snapshot(m_stub.defectUrl().port()).requests;
        QCOMPARE(PythonStub::count(log, "HEAD", "/x"), 1); // 第二次 HEAD 未发出
        QCOMPARE(PythonStub::count(log, "GET", "/y"), 1);
    }

    // 涌 body 不断开即不登记：keep-alive 下直传 1MiB 不断开的 HEAD 以干净 200 完成。
    // 第二个 HEAD 的形态随 Qt 版本分岔（6.11 / 6.6.2 实测均见下），故本例断的是
    // **自洽性**而非某一版本的结果：桩侧收到过第二个 HEAD，就必须得 405 的明示；桩侧
    // 没收到，就必须是"已登记缺陷 → 短路不发"。第三种形态（桩侧收到了却报 status=0，
    // 即连接被残余体失步）由分支内的状态断言拦住，正是本例要防的污染。
    // 分岔机制：QNAM 是否把 HEAD 响应里涌来的体派发给 readyRead。6.11 不派发 → 首条
    // HEAD 干净 200、不登记，第二个 HEAD 照常发出得 405；6.6.2 派发 → HttpClient 的
    // 64KB 涌 body 判据（kHeadFlowDefectThreshold）命中并登记缺陷，首条 HEAD 仍按
    // headersOnly 截断成功返回 200，同 origin 的后续 HEAD 短路不发。放宽是给分岔让的
    // 路，Windows（发布平台）侧另钉一条强断言，N34 结论不因放宽而失守
    void headFlowWithoutCloseIsNotRegistered() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.flowUrl(); // 涌 body 专用 origin，与 405 登记隔离
        HttpClient c{cfg};
        HttpClient::Request flow;
        flow.url = QUrl(QStringLiteral("/flow"));
        flow.method = "HEAD"; // headersOnly=false：不断开即干净完成，不触发任何判据
        const auto resp = execInThread(c, flow);
        QVERIFY2(resp.error.isEmpty(),
                 qPrintable(QStringLiteral("status=%1 error=%2 body=%3")
                                .arg(resp.status)
                                .arg(resp.error, QString::fromUtf8(resp.body.toBase64()))));
        QCOMPARE(resp.status, 200);
        QVERIFY(!resp.headers.isEmpty());
        const qint64 flowed = resp.body.size(); // 派发到 readyRead 的体字节数

        HttpClient::Request head;
        head.url = QUrl(QStringLiteral("/x"));
        head.method = "HEAD";
        const auto second = execInThread(c, head);
        const auto log = m_stub.snapshot(m_stub.flowUrl().port()).requests;
        const int sentSecond = PythonStub::count(log, "HEAD", "/x");
        const QString shape = QStringLiteral("第二条 HEAD：status=%1 error=%2 桩侧 HEAD /x 计数=%3 "
                                             "HEAD /flow 计数=%4 首条已派发体字节=%5")
                                  .arg(second.status)
                                  .arg(second.error)
                                  .arg(sentSecond)
                                  .arg(PythonStub::count(log, "HEAD", "/flow"))
                                  .arg(flowed);
        if (sentSecond == 1) {
            QVERIFY2(second.status == 405, qPrintable(shape)); // 发出即须得 405
            QVERIFY2(second.error.contains(QStringLiteral("does not support HEAD")),
                     qPrintable(shape));
        } else {
            QVERIFY2(second.status == 0, qPrintable(shape)); // 未发出即须短路的 0
            QVERIFY2(second.error.contains(QStringLiteral("request not sent")), qPrintable(shape));
        }
        QCOMPARE(PythonStub::count(log, "HEAD", "/flow"), 1); // 首条恒发出
#ifdef Q_OS_WIN
        // 发布平台的既有结论钉死（N34）：涌 body 的 HEAD **不得**登记缺陷——第二条必须
        // 发出并得 flow 端口的 405 明示。上面两分支为容住 QNAM 的版本分岔而放宽，这条不
        // 放宽：Windows 上若改走短路支，正是放宽后会静默放过的回归，须显影
        QCOMPARE(sentSecond, 1);
        QVERIFY2(second.status == 405, qPrintable(shape));
#endif
    }

    // 连接复用：顺序请求复用连接（20 条请求、连接数远少于请求数；
    // 两台不同 client 交替使用，共享同一传输池）
    void connectionReuseAcrossClients() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c1{cfg};
        HttpClient c2{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/b"));
        r.method = "GET";
        for (int i = 0; i < 20; ++i)
            QCOMPARE(execInThread(i % 2 ? c1 : c2, r).status, 200);
        const auto reuse = m_stub.snapshot(m_stub.url().port());
        QCOMPARE(PythonStub::count(reuse.requests, "GET", "/b"), 20);
        QVERIFY(reuse.connTotal <= 6); // 20 条请求 6 条以内连接：池化生效
    }

    // 作业独立于 client 对象寿命：client 先析构，在途作业照常跑完，wait / take 不受影响
    void jobSurvivesClientDestruction() {
        HttpClient::RequestId id;
        {
            HttpClient::Config cfg;
            cfg.baseUrl = m_stub.url();
            HttpClient temporary{cfg};
            HttpClient::Request r;
            r.url = QUrl(QStringLiteral("/hold"));
            r.method = "GET";
            id = temporary.submit(r);
        } // client 先死，作业仍在途
        QVERIFY(waitInThread(id, -1));
        const auto resp = takeInThread(id);
        QCOMPARE(resp.status, 200);
        QVERIFY(!resp.canceled);
    }

    // 总时限含排队：占满 QNAM 内建 6 连接后，队尾作业在排队期耗尽总时限→
    // 按超时失败，且从未发出（桩侧确认）。并发上限功能已删除（保旧发行版
    // 兼容）：实际约束即 QNAM 内建值，调用方按小批量纪律使用，见规格 §2.4。
    void queueTimeCountsTowardTotalDeadline() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.timeoutMs = 2000;
        HttpClient c{cfg};
        HttpClient::Request slow;
        slow.url = QUrl(QStringLiteral("/slow"));
        slow.method = "GET";
        std::vector<HttpClient::RequestId> ids;
        for (int i = 0; i < 6; ++i)
            ids.push_back(c.submit(slow)); // 占满 6 连接
        QTest::qWait(200);                 // 等 6 条全部发出、在途
        HttpClient::Config tailCfg = cfg;
        tailCfg.timeoutMs = 300;
        HttpClient tailClient{tailCfg};
        const auto tail = tailClient.submit(slow); // 无空位，QNAM 排队
        QElapsedTimer clock;
        clock.start();
        QVERIFY(waitInThread(tail, -1));
        const auto tailResp = takeInThread(tail);
        QVERIFY(!tailResp.canceled);
        QVERIFY(tailResp.error.contains(QStringLiteral("timed out")));
        QVERIFY(clock.elapsed() < 1500); // 远早于首批的 2000ms 到期
        for (const auto &id : ids) {
            QVERIFY(waitInThread(id, -1));
            const auto resp = takeInThread(id);
            QVERIFY(!resp.canceled);
            QVERIFY(!resp.error.isEmpty()); // 首批同样超时（服务端睡 8s）
        }
        const auto snap = m_stub.snapshot(m_stub.url().port());
        QCOMPARE(PythonStub::count(snap.requests, "GET", "/slow"), 6); // 队尾从未发出
        QCOMPARE(snap.connPeak, 6); // 池化生效：6 并发恰开 6 连接
    }

    // 排队中撤销：批量提交后撤销仍在 QNAM 队列中的尾部作业——桩侧确认从未发出
    void queuedAbortNeverSends() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/hold"));
        r.method = "GET";
        std::vector<HttpClient::RequestId> ids;
        for (int i = 0; i < 12; ++i)
            ids.push_back(c.submit(r));
        for (size_t i = 8; i < ids.size(); ++i) // 后 4 条仍在队列中，撤销之
            HttpClient::abort(ids[i]);
        for (size_t i = 0; i < 8; ++i) { // 前 8 条正常完成
            QVERIFY(waitInThread(ids[i], -1));
            QCOMPARE(takeInThread(ids[i]).status, 200);
        }
        for (size_t i = 8; i < ids.size(); ++i) {
            QVERIFY(waitInThread(ids[i], -1));
            QVERIFY(takeInThread(ids[i]).canceled);
        }
        const auto snap = m_stub.snapshot(m_stub.url().port());
        QCOMPARE(PythonStub::count(snap.requests, "GET", "/hold"), 8); // 撤销者从未发出
    }

    // 撤销：在途作业 abort 后 take 得 canceled 响应；已完成作业 abort 无效果、结果仍在
    void abortCanceledAndFinishedNoop() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request slow;
        slow.url = QUrl(QStringLiteral("/slow"));
        slow.method = "GET";
        const auto id = c.submit(slow);
        QVERIFY(id != nullptr);
        QVERIFY(!waitInThread(id, 0)); // wait(0) 立即返回只做状态检测
        HttpClient::abort(id);
        const auto canceled = takeInThread(id);
        QVERIFY(canceled.canceled);
        QCOMPARE(canceled.status, 0);
        QVERIFY(waitInThread(id, -1)); // 撤销算结束

        HttpClient::Request fast;
        fast.url = QUrl(QStringLiteral("/b"));
        fast.method = "GET";
        const auto id2 = c.submit(fast);
        QVERIFY(waitInThread(id2, -1));
        const auto done = takeInThread(id2);
        QCOMPARE(done.status, 200);
        HttpClient::abort(id2); // 已完成：abort 无效果
        const auto still = takeInThread(id2);
        QCOMPARE(still.status, 200);
        QVERIFY(!still.canceled);
    }

    // 撤销后原 deadline 到期不得误伤：作业已落定，计时器不再触碰已回收的 reply，
    // 后续作业照常成功（锁住"abort 未回收计时器 + 悬垂 reply"这一组合）
    void abortedJobDeadlineExpiryIsHarmless() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.timeoutMs = 600;
        HttpClient c{cfg};
        HttpClient::Request slow;
        slow.url = QUrl(QStringLiteral("/slow"));
        slow.method = "GET";
        const auto id = c.submit(slow);
        QVERIFY(!waitInThread(id, 0));
        HttpClient::abort(id);
        QVERIFY(waitInThread(id, -1));
        QVERIFY(takeInThread(id).canceled);
        QTest::qWait(900); // 越过原 deadline：此时在途 reply 已被回收
        HttpClient::Request fast;
        fast.url = QUrl(QStringLiteral("/b"));
        fast.method = "GET";
        const auto after = execInThread(c, fast);
        QVERIFY2(
            after.error.isEmpty(),
            qPrintable(QStringLiteral("status=%1 error=%2").arg(after.status).arg(after.error)));
        QCOMPARE(after.status, 200);
    }

    // 跨跳撤销：重定向跟随到慢落点后撤销，在途作业得 canceled 响应。
    // 重定向间隙（finalize→sendHop 微秒窗口）的精确命中不可单元测试，
    // 此处覆盖邻近行为，窗口本身靠 pre-check 代码审查保证。
    void abortDuringRedirectedHopYieldsCanceled() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/redir-slow/a"));
        r.method = "GET";
        const auto id = c.submit(r);
        QVERIFY(id != nullptr);
        // 等跟随到达慢落点（桩侧见 /slow）再撤销；落点睡 8s，不会先完成
        bool arrived = false;
        for (int i = 0; i < 50 && !arrived; ++i) {
            QTest::qWait(100);
            arrived = PythonStub::count(m_stub.snapshot(m_stub.url().port()).requests, "GET",
                                        "/slow") >= 1;
        }
        QVERIFY(arrived);
        HttpClient::abort(id);
        QVERIFY(waitInThread(id, -1));
        QVERIFY(takeInThread(id).canceled);
    }

    // 切片等待：wait(限时) 超时返假且作业仍在途，之后再等仍能等到结束（撤销落定）
    void waitSliceSemantics() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/slow"));
        r.method = "GET";
        const auto id = c.submit(r);
        auto early = std::make_shared<bool>(true);
        auto finalDone = std::make_shared<bool>(false);
        auto resp = std::make_shared<HttpClient::Response>();
        std::thread worker([id, early, finalDone, resp] {
            *early = HttpClient::wait(id, 150); // 限时到仍未结束
            *finalDone = HttpClient::wait(id, -1);
            *resp = HttpClient::take(id);
        });
        QTest::qWait(400); // 先让 worker 的首个 150ms 切片到期，再撤销（否则 abort
                           // 落在切片窗口内，early=true 属合法结果，竞态）
        HttpClient::abort(id);
        worker.join();
        QVERIFY(!*early);
        QVERIFY(*finalDone);
        QVERIFY(resp->canceled);
    }

    // TLS：未加信任 → 握手失败；ignoreSslErrors=true → 通过；
    // 进程内加入桩 CA 为可信根后，忽略开关关掉也能通过（按是否加入区分信任两态）
    void tlsTrustDistinguishesCa() {
        HttpClient::Config strict;
        strict.baseUrl = m_stub.tls2Url(); // 独立 origin：握手失败不污染 (b)(c) 的连接缓存
        HttpClient::Request r;
        r.url = QUrl(QStringLiteral("/tls"));
        r.method = "GET";

        // a) 未加信任且不忽略 → 握手失败、无响应。具体文案因 TLS 后端与失败呈现方式
        //    而异（"TLS handshake failed" 或对端握手期关闭），故只断言可观测语义
        HttpClient cStrict{strict};
        const auto untrusted = execInThread(cStrict, r);
        QVERIFY2(
            untrusted.status == 0 && !untrusted.error.isEmpty() && !untrusted.canceled,
            qPrintable(
                QStringLiteral("status=%1 error=%2").arg(untrusted.status).arg(untrusted.error)));

        // b) ignoreSslErrors=true → 通过
        HttpClient::Config lenient = strict;
        lenient.ignoreSslErrors = true;
        HttpClient cLenient{lenient};
        const auto ignored = execInThread(cLenient, r);
        QVERIFY2(ignored.error.isEmpty(),
                 qPrintable(
                     QStringLiteral("status=%1 error=%2").arg(ignored.status).arg(ignored.error)));
        QCOMPARE(ignored.status, 200);

        // c) 进程内可信根：CA 加入默认 QSslConfiguration 后，严格校验也通过
        QSslConfiguration def = QSslConfiguration::defaultConfiguration();
        def.addCaCertificate(QSslCertificate(m_stub.caPem(), QSsl::Pem));
        QSslConfiguration::setDefaultConfiguration(def);
        const auto trusted = execInThread(cStrict, r);
        QVERIFY2(trusted.error.isEmpty(),
                 qPrintable(
                     QStringLiteral("status=%1 error=%2").arg(trusted.status).arg(trusted.error)));
        QCOMPARE(trusted.status, 200);
    }

    // ———— N50 流式：增量交付与背压（以下用例由维护者手动运行）————

    // 流式读满大响应体：内容与非流式一次取到的逐字节一致，EOF 走 kReadEof 而非 kReadError
    void streamingDeliversFullBodyAndEof() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};

        HttpClient::Request plain;
        plain.url = QUrl(QStringLiteral("/big"));
        plain.method = "GET";
        const auto whole = execInThread(c, plain);
        QCOMPARE(whole.status, 200);
        QCOMPARE(whole.body.size(), 2 * 1024 * 1024);

        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/big"))));
        QByteArray got;
        const Drain d = drainInThread(id, got);
        QCOMPARE(d.terminal, HttpClient::kReadEof);
        QCOMPARE(d.bytes, qint64(whole.body.size()));
        QCOMPARE(got, whole.body);
    }

    // 短读合法、分批读满且逐字节一致。两趟分别压住两侧边界：
    // a) maxBytes 远小于队首块 → 走「部分消费 + 余量归还」（每次只从块里取走一段）；
    //    该路径不可从外部观测，判据是重建体逐字节一致（2MiB 全程两千余次部分消费）
    // b) maxBytes 大于水位 → 队列永远填不满一次请求，**中途**短读可观测
    void streamingShortReadsReassemble() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};

        HttpClient::Request plain;
        plain.url = QUrl(QStringLiteral("/big"));
        plain.method = "GET";
        const auto whole = execInThread(c, plain);
        QCOMPARE(whole.status, 200);

        // a) 部分消费 + 余量归还
        {
            const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/big"))));
            QByteArray got;
            qint64 total = 0;
            qint64 terminal = HttpClient::kReadNoData;
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 30000) {
                const qint64 n = readOnceInThread(id, got, 1000, 500);
                if (n > 0) {
                    total += n;
                    continue;
                }
                if (n == HttpClient::kReadNoData)
                    continue;
                terminal = n;
                break;
            }
            QCOMPARE(terminal, HttpClient::kReadEof);
            QCOMPARE(total, qint64(whole.body.size()));
            QCOMPARE(got, whole.body);
        }

        // b) 中途短读（可观测）
        {
            const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/big")), 64 * 1024));
            QByteArray got;
            qint64 total = 0;
            qint64 shortMidStream = 0;
            qint64 terminal = HttpClient::kReadNoData;
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 30000) {
                const qint64 n = readOnceInThread(id, got, 1024 * 1024, 500);
                if (n > 0) {
                    total += n;
                    if (total < qint64(whole.body.size()))
                        ++shortMidStream; // 未读完就到手的一段：中途短读
                    continue;
                }
                if (n == HttpClient::kReadNoData)
                    continue;
                terminal = n;
                break;
            }
            QCOMPARE(terminal, HttpClient::kReadEof);
            QCOMPARE(total, qint64(whole.body.size()));
            QCOMPARE(got, whole.body);
            QVERIFY2(shortMidStream >= 2,
                     qPrintable(QStringLiteral("中途短读次数=%1").arg(shortMidStream)));
        }
    }

    // waitHeaders 窗口耗尽而作业仍在途 → false；调用方须自行 abort 收尾
    void waitHeadersTimesOutWhileJobInFlight() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/slow"))));
        QVERIFY(!waitHeadersInThread(id, 500));     // /slow 头要等 8s
        QVERIFY(!takeInThread(id).error.isEmpty()); // 作业仍在途：take 拒绝给结果
        HttpClient::abort(id);
        QByteArray out;
        QCOMPARE(drainInThread(id, out).terminal, HttpClient::kReadError);
    }

    // waitHeaders 命中 + statusOf 读出状态码与响应头（流式作业的启动判定窗口）
    void waitHeadersSucceedsAndStatusOfReadsHead() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/big"))));
        QVERIFY(waitHeadersInThread(id, 5000));
        const auto head = statusOfInThread(id);
        QCOMPARE(head.status, 200);
        QVERIFY(head.headers.contains("content-length"));
        QVERIFY(head.body.isEmpty()); // 未终态：body 恒空
        QByteArray got;
        QCOMPARE(drainInThread(id, got).terminal, HttpClient::kReadEof);
    }

    // Config::timeoutMs 对流式作业不生效：同一端点上非流式必在总时限处断开，流式须读满
    void streamingIgnoresTotalTimeout() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url(); // timeoutMs 缺省 5000；/trickle-long 全程 ~8s
        HttpClient c{cfg};

        HttpClient::Request plain;
        plain.url = QUrl(QStringLiteral("/trickle-long"));
        plain.method = "GET";
        QVERIFY(execInThread(c, plain).error.contains(QStringLiteral("timed out")));

        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/trickle-long"))));
        QByteArray got;
        const Drain d = drainInThread(id, got, 20000);
        QCOMPARE(d.terminal, HttpClient::kReadEof);
        QCOMPARE(d.bytes, qint64(80)); // 一个字节都没少
        QCOMPARE(got, QByteArray(80, 'y'));
    }

    // 背压：水位压到 64KB 后缓慢消费 2MiB，仍读满且全程无错误终态
    // （水位远小于响应体，必然反复 park / 续泵，泵停续三态都被走到）
    void streamingBackpressureWithSmallWatermark() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/big")), 64 * 1024));

        QByteArray got;
        qint64 total = 0;
        qint64 terminal = HttpClient::kReadNoData;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 30000) {
            const qint64 n = readOnceInThread(id, got, 16 * 1024, 500);
            if (n > 0) {
                total += n;
                QTest::qWait(1); // 让消费慢于网络，逼出水位
                continue;
            }
            if (n == HttpClient::kReadNoData)
                continue;
            terminal = n;
            break;
        }
        QCOMPARE(terminal, HttpClient::kReadEof);
        QCOMPARE(total, qint64(2 * 1024 * 1024));
    }

    // 错误终态：非 2xx（404）与建连失败各自返 kReadError，原因可从 statusOf 取
    void streamingErrorTerminal() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};

        const auto notFound = c.submit(streamingGet(QUrl(QStringLiteral("/notfound"))));
        QByteArray got;
        QCOMPARE(drainInThread(notFound, got).terminal, HttpClient::kReadError);
        const auto detail = statusOfInThread(notFound);
        QCOMPARE(detail.status, 404);
        QVERIFY(!detail.error.isEmpty());

        HttpClient::Config dead; // 未监听端口 → 建连失败
        dead.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:9"));
        HttpClient cd{dead};
        const auto refused = cd.submit(streamingGet(QUrl(QStringLiteral("/x"))));
        QByteArray none;
        QCOMPARE(drainInThread(refused, none, 10000).terminal, HttpClient::kReadError);
        QVERIFY(!statusOfInThread(refused).error.isEmpty());
    }

    // timeoutMs = 0 即「立即取一次、不阻塞」：在途且缓冲空返 kReadNoData；
    // 终态且缓冲已空返 kReadEof——终态判定因此不需要额外方法
    void streamingTimeoutZeroReturnsImmediately() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};

        const auto slow = c.submit(streamingGet(QUrl(QStringLiteral("/slow"))));
        QByteArray none;
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(readOnceInThread(slow, none, 4096, 0), HttpClient::kReadNoData);
        QVERIFY(clock.elapsed() < 1000); // 立即返回，没有等 8s
        HttpClient::abort(slow);

        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/big"))));
        QByteArray got;
        QCOMPARE(drainInThread(id, got).terminal, HttpClient::kReadEof);
        QByteArray tail;
        QCOMPARE(readOnceInThread(id, tail, 4096, 0), HttpClient::kReadEof);
        QVERIFY(tail.isEmpty());

        // 错误终态 + 缓冲已空 + 传 0 → 同样立即返回 kReadError
        const auto bad = c.submit(streamingGet(QUrl(QStringLiteral("/notfound"))));
        QByteArray none2;
        QCOMPARE(drainInThread(bad, none2).terminal, HttpClient::kReadError);
        QByteArray tail2;
        QCOMPARE(readOnceInThread(bad, tail2, 4096, 0), HttpClient::kReadError);
    }

    // 取消是事件而非轮询：abort 把阻塞中的 readBlocking 叫醒，不必等窗口耗尽
    void streamingAbortWakesBlockedRead() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/slow")))); // 8s 不吐任何字节
        QVERIFY(!waitHeadersInThread(id, 300));

        std::atomic<qint64> blocked{HttpClient::kReadNoData};
        QElapsedTimer clock;
        clock.start();
        std::thread reader([&] {
            char buf[4096];
            blocked = HttpClient::readBlocking(id, buf, sizeof(buf), -1); // 永久等
        });
        QTest::qWait(300); // 确认它已经阻塞住
        HttpClient::abort(id);
        reader.join();
        QCOMPARE(blocked.load(), HttpClient::kReadError);
        QVERIFY(clock.elapsed() < 5000); // 是被叫醒的，不是等够了
        // 撤销要在 statusOf 上如实体现
        const auto canceled = statusOfInThread(id);
        QVERIFY(canceled.canceled);
        QVERIFY(!canceled.error.isEmpty());
    }

    // 缓冲先于终态：撤销前已到手的字节照常交付，交付完才返 kReadError
    void streamingBufferedBytesSurviveAbort() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/big"))));

        QByteArray one;
        QCOMPARE(readOnceInThread(id, one, 1, 5000), qint64(1)); // 只取 1 字节，余下留在队列
        HttpClient::abort(id);

        QByteArray rest;
        qint64 terminal = HttpClient::kReadNoData;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 10000) {
            const qint64 n = readOnceInThread(id, rest, 64 * 1024, 0);
            if (n > 0)
                continue; // 仍在交付缓冲
            terminal = n;
            break;
        }
        QCOMPARE(terminal, HttpClient::kReadError);
        QVERIFY(!rest.isEmpty()); // 撤销没有把已到手的字节丢掉
    }

    // 缓冲先于终态（传输中断一路）：/truncated 声明 100000 字节却只发 4096 就断连，
    // 已到的 4096 仍须先交付，交付完才落 kReadError——不得截断丢弃
    void streamingBufferedBytesSurviveTransportError() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/truncated"))));
        QByteArray got;
        const Drain d = drainInThread(id, got);
        QCOMPARE(d.terminal, HttpClient::kReadError);
        QVERIFY(!got.isEmpty());                    // 中断前到手的字节交付了
        QVERIFY(got.size() <= 4096);                // 且不超过服务端实际发出的量
        QCOMPARE(got, QByteArray(got.size(), 'z')); // 内容完好，未被截断或错位
        QVERIFY(!statusOfInThread(id).error.isEmpty());
    }

    // 重定向全部交 QNAM 自动跟随：同 origin 与跨 origin 都照常读到最终响应体
    void streamingFollowsRedirectsAcrossOrigins() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};

        const auto same = c.submit(streamingGet(QUrl(QStringLiteral("/redir-rel/a"))));
        QByteArray sameBody;
        QCOMPARE(drainInThread(same, sameBody).terminal, HttpClient::kReadEof);
        QCOMPARE(sameBody, QByteArray("ok /b"));

        // 跨 origin（主端口 → 缺陷端口）：端口不同即不同 origin，仍须跟随
        const auto cross = c.submit(streamingGet(QUrl(QStringLiteral("/redir-abs/a"))));
        QByteArray crossBody;
        QCOMPARE(drainInThread(cross, crossBody).terminal, HttpClient::kReadEof);
        QCOMPARE(crossBody, QByteArray("ok /b"));
    }

    // 凭据不越界：流式不预置 Authorization（首跳日志 auth=false），跨 origin 后也不带
    void streamingDoesNotPresetCredentialsAcrossOrigins() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.user = QStringLiteral("uA");
        cfg.password = QStringLiteral("pA");
        HttpClient c{cfg};
        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/redir-abs/a"))));
        QByteArray got;
        QCOMPARE(drainInThread(id, got).terminal, HttpClient::kReadEof);
        QCOMPARE(got, QByteArray("ok /b"));

        const auto mainLog = m_stub.snapshot(m_stub.url().port()).requests;
        const auto defectLog = m_stub.snapshot(m_stub.defectUrl().port()).requests;
        QCOMPARE(PythonStub::count(mainLog, "GET", "/redir-abs/a"), 1);
        QCOMPARE(PythonStub::count(defectLog, "GET", "/b"), 1);
        // 流式不预置：首跳就没带 Authorization（非流式此处为 true）
        QVERIFY(std::none_of(mainLog.cbegin(), mainLog.cend(), [](const PythonStub::Entry &e) {
            return e.path == "/redir-abs/a" && e.auth;
        }));
        // 跨 origin 的落点更不该带——预置头会被 QNAM 整份复制到重定向请求上
        QVERIFY(std::none_of(defectLog.cbegin(), defectLog.cend(),
                             [](const PythonStub::Entry &e) { return e.path == "/b" && e.auth; }));
    }

    // 凭据不越界（判据落在「需认证的落点」上）：跨 origin 后落点索要认证时不得回填原
    // origin 的凭据，请求须以认证失败落错误终态；认证端口的日志可证落点压根没收到
    // Authorization——N51 建在这条性质上，这里必须有测试背书
    void streamingCrossOriginRedirectToAuthRouteFails() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        cfg.user = QStringLiteral("uA");
        cfg.password = QStringLiteral("pA");
        HttpClient c{cfg};
        // 主端口 301 → 【缓存干净的专用认证 origin】的 /basic/x（跨 origin 且落点需认证）。
        // 落点刻意不复用认证端口(D)：QNAM 按 host:port 缓存已成功的 authenticator，D 被
        // Digest 用例认证过，同 origin 的新请求会先回放那份缓存凭据——日志上的 auth 就不再
        // 只反映我们的行为（这正是本用例第一版误报的成因）。
        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/redir-cross-basic/a"))));
        QByteArray got;
        const Drain d = drainInThread(id, got, 15000);
        QCOMPARE(d.terminal, HttpClient::kReadError);
        QVERIFY(!statusOfInThread(id).error.isEmpty());

        const auto authLog = m_stub.snapshot(m_stub.auth2Url().port()).requests;
        QVERIFY(PythonStub::count(authLog, "GET", "/basic/x") >= 1); // 确实打了这一跳
        // 落点不得收到任何 Authorization：预置头回归、或 authenticationRequired 的 origin
        // 校验失守，都会让这一跳带上凭据。失败时打印实际值，一眼区分是"我们的凭据越界"
        // 还是其它来源
        for (const auto &e : authLog) {
            if (e.path != "/basic/x" || !e.auth)
                continue;
            QFAIL(qPrintable(
                QStringLiteral("落点收到了 Authorization：%1").arg(QString::fromUtf8(e.authv))));
        }
    }

    // 重定向循环：由 Qt 自己的默认跳数上限兜住，不会无限转
    void streamingRedirectLoopFails() {
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        const auto id = c.submit(streamingGet(QUrl(QStringLiteral("/redir-loop/a"))));
        QByteArray got;
        QCOMPARE(drainInThread(id, got, 20000).terminal, HttpClient::kReadError);
    }

    // streaming 与 headersOnly 互斥：按调用错误处理且不发请求
    void streamingAndHeadersOnlyAreMutuallyExclusive() {
        QVERIFY(m_stub.resetLogs());
        HttpClient::Config cfg;
        cfg.baseUrl = m_stub.url();
        HttpClient c{cfg};
        HttpClient::Request r = streamingGet(QUrl(QStringLiteral("/big")));
        r.headersOnly = true;
        const auto resp = takeInThread(c.submit(r));
        QVERIFY(resp.error.contains(QStringLiteral("streaming")));
        QCOMPARE(resp.status, 0);
        const auto log = m_stub.snapshot(m_stub.url().port()).requests;
        QVERIFY(log.isEmpty()); // 未发出
    }

  private:
    PythonStub m_stub;
};

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN），无 GUI 依赖
QTEST_GUILESS_MAIN(TestHttpClient)

#include "tst_HttpClient.moc"

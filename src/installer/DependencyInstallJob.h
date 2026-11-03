#pragma once

#include "pch.h"

#include "HttpClient.h"
#include "Logging.h"
#include "installer/DependencyCatalog.h"

// N150：安装器作业对象——归主窗（应用级寿命），向导只订阅信号。单个 QThread 承载
// "查询 + 探测"与"执行"两种任务，两种任务各排一次队、在同一条工作线程上串行，故同一
// 时刻只有一个作业在动；所有阻塞等待可中断（HttpClient abort、子进程切片等待 + 取消旗）。
// 线程亲和的入口经 queued invoke 进工作线程，cancel / shutdown 可从任意线程调用。
class DependencyInstallJob : public QObject {
    Q_OBJECT
  public:
    // 向导据此判"附着还是另起一轮"：只有执行中的作业要附着，查询中的作业让新会话排队
    enum class Phase { Idle, Querying, Executing };

    explicit DependencyInstallJob(QObject *parent = nullptr);
    ~DependencyInstallJob() override;

    DependencyInstallJob(const DependencyInstallJob &) = delete;
    DependencyInstallJob &operator=(const DependencyInstallJob &) = delete;

    void startThread();
    Phase phase() const;

    // 向导会话开始：清本次会话的 host 失败记录（成功查询的进程内缓存保留）
    void beginWizardSession();

    // 查询 + 探测七项；已成功的查询直接回填缓存
    void startQueries(const QString &pluginDir);

    // 执行计划；plan 里的动作由第一页算定，进页不再重算
    void startExecute(const QString &pluginDir, const QVector<DependencyCatalog::PlanEntry> &plan);

    // 任意线程可调：置取消旗并 abort 在途 HTTP 作业（进程 kill 由工作线程的等待循环做）
    void cancel();

    // 退出路径：置取消旗、abort、对线程做有界等待；超时故意泄漏线程对象（返回 false）
    bool shutdown(int waitMs);

  signals:
    void probeReady(int id, const QString &version, const QString &rawOutput);
    void queryReady(int id, bool queryOk, const QString &version, qint64 downloadBytes,
                    const QString &error);
    void stepText(const QString &text);
    void progress(qint64 done, qint64 total);
    void logLine(const QString &line);
    void finished(int installed, int removed, int failed, int skipped);

  private:
    // 一项的成败计数：安装 / 卸载 / 失败 / 跳过
    struct Outcome {
        int installed = 0;
        int removed = 0;
        int failed = 0;
        int skipped = 0;
    };

    // —— 以下全部跑在工作线程 ——
    void runQueries(const QString &pluginDir);
    void runExecute(const QString &pluginDir, const QVector<DependencyCatalog::PlanEntry> &plan);

    void probeAll(const QString &pluginDir);
    void probeExecutable(const QString &pluginDir, const DependencyCatalog::Component &comp);

    DependencyCatalog::SourceQuery queryComponent(DependencyCatalog::Id id);
    DependencyCatalog::SourceQuery queryGitHub(DependencyCatalog::Id id);
    DependencyCatalog::SourceQuery queryMediaArea();
    DependencyCatalog::SourceQuery queryModel(DependencyCatalog::Id id);
    DependencyCatalog::SourceQuery queryModelHost(DependencyCatalog::Id id,
                                                  DependencyCatalog::ModelHost host);
    bool hostFailedThisSession(const QString &host) const;
    QString hostFailureReason(const QString &host) const;
    void markHostFailed(const QString &host, const QString &reason);

    HttpClient::Response blockingRequest(const HttpClient &client,
                                         const HttpClient::Request &request);

    void executeEntry(const QString &pluginDir, const DependencyCatalog::PlanEntry &entry,
                      Outcome &outcome);
    void executeRemove(const QString &pluginDir, const DependencyCatalog::Component &comp,
                       Outcome &outcome);
    void executeInstall(const QString &pluginDir, const DependencyCatalog::PlanEntry &entry,
                        const DependencyCatalog::Component &comp, Outcome &outcome);
    bool downloadWithRetry(DependencyCatalog::Id id, const DependencyCatalog::SourceQuery &query,
                           const QString &destFile);
    // 单次下载尝试；received 返回本次已收字节（重试与换 host 时据此回退进度）
    bool downloadOnce(DependencyCatalog::Id id, const QUrl &url, qint64 expectedBytes,
                      const QByteArray &expectedSha256, const QString &destFile,
                      DependencyCatalog::DownloadFailure *failure, QString *error, int *httpStatus,
                      qint64 *received);
    bool extractPackage(const QString &pluginDir, DependencyCatalog::Id id,
                        const QString &packageFile);
    QString findTar() const;
    void sleepCancellable(int ms);
    qint64 packageBytes(DependencyCatalog::Id id) const;

    // 日志框与文件日志的唯一出口：同一批源串、一次调用两处都落
    void report(Logging::Level level, const QString &line);
    // 进度按字节加权：一项开跑前定好全部权重，收口时补齐未走到的那一份，故失败项也不会
    // 把进度条留在半路；重试把当前项已收字节打回起点，已完成权重不回退
    void startItem(const DependencyCatalog::PlanEntry &entry, qint64 packageBytes);
    void advanceProgress(qint64 weightBytes);
    void rewindProgress(qint64 weightBytes);
    void advanceStep();
    void emitProgress();
    void finishItem();

    void setPhase(Phase phase);
    void markInFlight(const HttpClient::RequestId &id);

    QThread *m_thread = nullptr;
    const HttpClient m_queryClient;    // GitHub / mediaarea：15 秒总时限
    const HttpClient m_modelClient;    // 模型 host 查询：10 秒
    const HttpClient m_downloadClient; // 流式下载
    std::atomic_int m_phase{static_cast<int>(Phase::Idle)};
    std::atomic_bool m_cancel{false};
    QMutex m_jobMutex; // 保护 m_inFlight（cancel 与工作线程两侧都要碰）
    HttpClient::RequestId m_inFlight;
    QHash<QString, QString> m_failedHosts; // host → 查询失败原因（本次会话，关窗即清）
    // 每项最近一次查询结论：只复用成功项——一次进程会话内结果稳定，重开向导不重复请求
    // （GitHub 未认证限流 60 次/小时/IP）；失败项重开时重查
    QMap<int, DependencyCatalog::SourceQuery> m_queryCache;
    qint64 m_progressDone = 0;
    qint64 m_progressTotal = 0;
    qint64 m_itemWeight = 0; // 当前项的全部权重
    qint64 m_itemDone = 0;   // 当前项已计的权重
};

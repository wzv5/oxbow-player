#include "pch.h"

#include "installer/DependencyInstallJob.h"

#include "Util.h"
#include "installer/DependencySwap.h"
#include "installer/InstallerStrings.h"

using namespace DependencyCatalog;
using namespace DependencySwap;
using namespace InstallerText;

namespace {

constexpr int kQuerySliceMs = 100;
constexpr qint64 kDownloadStallMs = 60000; // 响应头与读侧的无字节上限
constexpr qint64 kDownloadChunk = 64 * 1024;
constexpr int kProbeTimeoutMs = 5000;
constexpr int kProcessSliceMs = 100;
constexpr int kExtractTimeoutMs = 15 * 60 * 1000;
constexpr int kProgressEmitMs = 100;
constexpr int kGithubFallbackPerPage = 30;

enum class ProcessExit { Finished, TimedOut, Canceled };

// 子进程一律切片等：取消旗随时能赢，不许 waitForFinished(-1)
ProcessExit waitProcess(QProcess &process, int timeoutMs, const std::atomic_bool &cancel) {
    QElapsedTimer deadline;
    deadline.start();
    while (process.state() != QProcess::NotRunning) {
        const bool expired = deadline.hasExpired(timeoutMs);
        if (!expired && !cancel.load()) {
            process.waitForFinished(kProcessSliceMs);
            continue;
        }
        process.kill();
        process.waitForFinished(kProcessSliceMs);
        return expired ? ProcessExit::TimedOut : ProcessExit::Canceled;
    }
    return ProcessExit::Finished;
}

QString failureText(DownloadFailure failure, const QString &detail) {
    switch (failure) {
    case DownloadFailure::Network:
        return QStringLiteral("network error: %1").arg(detail);
    case DownloadFailure::HttpServerError:
        return QStringLiteral("HTTP server error: %1").arg(detail);
    case DownloadFailure::LocalWrite:
        return QStringLiteral("local write failed: %1").arg(detail);
    case DownloadFailure::HttpClientError:
        return QStringLiteral("HTTP client error: %1").arg(detail);
    case DownloadFailure::Canceled:
        return QStringLiteral("canceled");
    case DownloadFailure::HashMismatch:
        return QStringLiteral("sha256 mismatch");
    case DownloadFailure::SizeMismatch:
        return QStringLiteral("size mismatch");
    case DownloadFailure::None:
        break;
    }
    return detail;
}

DownloadFailure classifyStatus(int status) {
    if (status == 0)
        return DownloadFailure::Network;
    if (status >= 500 || status == 408 || status == 429)
        return DownloadFailure::HttpServerError;
    return DownloadFailure::HttpClientError;
}

SourceQuery failedQuery(const QString &reason) {
    SourceQuery query;
    query.error = reason;
    return query;
}

SourceQuery canceledQuery() { return failedQuery(QStringLiteral("canceled")); }

QString httpErrorText(const QString &what, int status) {
    return QStringLiteral("%1 returned HTTP %2").arg(what).arg(status);
}

// 挑中的资产 → 查询结论。资产不在或没有 size 都判不可用（大小是第一步的硬门槛）
SourceQuery fromAsset(const GitHubAsset &asset, const QString &version) {
    if (!asset.isValid() || asset.size < 0)
        return failedQuery(QStringLiteral("target asset not found in release"));
    SourceQuery query;
    query.ok = true;
    query.version = version;
    query.downloadBytes = asset.size;
    query.sha256 = asset.sha256;
    query.downloadUrl = QUrl(asset.browserUrl);
    return query;
}

QString readDllFileVersion(const QString &dllPath) {
    const wchar_t *native = reinterpret_cast<const wchar_t *>(dllPath.utf16());
    const DWORD handle = GetFileVersionInfoSizeW(native, nullptr);
    if (handle == 0)
        return {};
    std::vector<char> data(static_cast<size_t>(handle));
    if (!GetFileVersionInfoW(native, 0, handle, data.data()))
        return {};
    VS_FIXEDFILEINFO *info = nullptr;
    UINT size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID *>(&info), &size) || !info)
        return {};
    return QStringLiteral("%1.%2.%3.%4")
        .arg(HIWORD(info->dwFileVersionMS))
        .arg(LOWORD(info->dwFileVersionMS))
        .arg(HIWORD(info->dwFileVersionLS))
        .arg(LOWORD(info->dwFileVersionLS));
}

HttpClient::Request plainGetRequest(const QUrl &url) {
    HttpClient::Request request;
    request.url = url;
    request.method = "GET";
    return request;
}

HttpClient::Request githubRequest(const QUrl &url) {
    HttpClient::Request request = plainGetRequest(url);
    request.headers.append({"User-Agent", githubUserAgent()}); // 缺了 GitHub 直接 403
    return request;
}

} // namespace

DependencyInstallJob::DependencyInstallJob(QObject *parent)
    : QObject(parent), m_queryClient([] {
          HttpClient::Config config;
          config.timeoutMs = 15000; // GitHub / mediaarea 总时限
          return config;
      }()),
      m_modelClient([] {
          HttpClient::Config config;
          config.timeoutMs = 10000; // 模型 host 查询总时限
          return config;
      }()),
      m_downloadClient(HttpClient::Config{}) {}

DependencyInstallJob::~DependencyInstallJob() { shutdown(2000); }

void DependencyInstallJob::startThread() {
    if (m_thread)
        return;
    // moveToThread 的前提是本对象无父；QThread 自身也不能做本对象的子对象（随搬家会
    // 试图把线程对象搬进它自己的目标线程）。作业从此无父：寿命到进程退出
    setParent(nullptr);
    m_thread = new QThread;
    moveToThread(m_thread);
    m_thread->start();
}

DependencyInstallJob::Phase DependencyInstallJob::phase() const {
    return static_cast<Phase>(m_phase.load(std::memory_order_acquire));
}

void DependencyInstallJob::setPhase(Phase phase) {
    m_phase.store(static_cast<int>(phase), std::memory_order_release);
}

void DependencyInstallJob::beginWizardSession() {
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_failedHosts.clear(); // 会话级失败记录关窗即清；成功查询的缓存保留
        },
        Qt::QueuedConnection);
}

void DependencyInstallJob::startQueries(const QString &pluginDir) {
    // 排队进工作线程：上一轮没跑完就排在它后面，两轮在同一条线程上串行、不交叉
    QMetaObject::invokeMethod(
        this, [this, pluginDir] { runQueries(pluginDir); }, Qt::QueuedConnection);
}

void DependencyInstallJob::startExecute(const QString &pluginDir, const QVector<PlanEntry> &plan) {
    QMetaObject::invokeMethod(
        this,
        [this, pluginDir, plan] {
            if (phase() == Phase::Executing)
                return; // 同一时刻只有一个作业：执行不另起第二轮
            runExecute(pluginDir, plan);
        },
        Qt::QueuedConnection);
}

void DependencyInstallJob::cancel() {
    m_cancel.store(true);
    QMutexLocker locker(&m_jobMutex);
    // 句柄只持有不 reset（HttpClient 的调用纪律）；已完成作业的 abort 无副作用，
    // 所以不必在每次请求收口时清空它
    if (m_inFlight)
        HttpClient::abort(m_inFlight);
}

bool DependencyInstallJob::shutdown(int waitMs) {
    if (!m_thread)
        return true;
    cancel();
    m_thread->quit();
    const bool stopped = m_thread->wait(waitMs);
    if (stopped)
        m_thread = nullptr;
    // 超时即故意泄漏：QThread 没有父对象（见 startThread），进程随后结束，不在
    // QApplication 收尾时销毁仍在跑的线程
    return stopped;
}

void DependencyInstallJob::report(Logging::Level level, const QString &line) {
    Logging::Line(level).noquote() << line;
    emit logLine(line);
}

void DependencyInstallJob::markInFlight(const HttpClient::RequestId &id) {
    QMutexLocker locker(&m_jobMutex);
    m_inFlight = id;
}

// —— 查询与探测 ——

void DependencyInstallJob::runQueries(const QString &pluginDir) {
    m_cancel.store(false);
    setPhase(Phase::Querying);
    probeAll(pluginDir);
    for (int order = 0; order < kComponentCount && !m_cancel.load(); ++order) {
        const Id id = idAt(order);
        const Component &comp = component(id);
        const SourceQuery query = queryComponent(id);
        emit queryReady(order, query.ok, query.version, query.downloadBytes, query.error);
        if (isAvailable(query, comp)) {
            const QString size = displayDownloadSize(query.downloadBytes);
            report(Logging::Level::Info,
                   query.version.isEmpty()
                       ? QStringLiteral("checked %1: %2 available").arg(comp.name, size)
                       : QStringLiteral("checked %1: version %2, %3 available")
                             .arg(comp.name, query.version, size));
            continue;
        }
        report(Logging::Level::Warning,
               QStringLiteral("checked %1: unavailable (%2)").arg(comp.name, query.error));
    }
    setPhase(Phase::Idle);
}

void DependencyInstallJob::probeAll(const QString &pluginDir) {
    for (int order = 0; order < kComponentCount && !m_cancel.load(); ++order) {
        const Component &comp = component(idAt(order));
        if (comp.probe.kind == ProbeKind::None)
            continue; // 模型没有版本可探：已安装列由本地事实现算
        if (comp.probe.kind == ProbeKind::DllVersion) {
            const QString marker = DependencySwap::pluginPath(pluginDir, installedMarkerOf(comp));
            const QString raw = QFileInfo::exists(marker) ? readDllFileVersion(marker) : QString();
            emit probeReady(static_cast<int>(comp.id), normalizeDllFileVersion(raw), raw);
            continue;
        }
        probeExecutable(pluginDir, comp);
    }
}

void DependencyInstallJob::probeExecutable(const QString &pluginDir, const Component &comp) {
    const int order = static_cast<int>(comp.id);
    const QString marker = DependencySwap::pluginPath(pluginDir, installedMarkerOf(comp));
    if (!QFileInfo::exists(marker)) {
        emit probeReady(order, {}, {});
        return;
    }
    const QStringList arguments{QString::fromLatin1(comp.probe.arg)};

    QProcess process;
    process.setProgram(marker);
    process.setArguments(arguments);
    process.setWorkingDirectory(pluginDir); // 内置命令按插件目录定形，探测同基准
    process.start();
    const ProcessExit exit = waitProcess(process, kProbeTimeoutMs, m_cancel);
    if (exit == ProcessExit::Canceled)
        return;
    if (exit == ProcessExit::TimedOut) {
        emit probeReady(order, {},
                        QStringLiteral("probe timed out after %1 ms").arg(kProbeTimeoutMs));
        return;
    }
    // stdout 与 stderr 合流：两类工具的版本行不同源
    const QString raw =
        QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
    emit probeReady(order, parseProbeVersion(comp.id, raw), raw);
}

SourceQuery DependencyInstallJob::queryComponent(Id id) {
    const auto cached = m_queryCache.constFind(static_cast<int>(id));
    if (cached != m_queryCache.constEnd() && cached->ok) {
        report(Logging::Level::Info,
               QStringLiteral("cache hit for %1").arg(QLatin1String(component(id).name)));
        return cached.value();
    }
    const Component &comp = component(id);
    SourceQuery query;
    switch (comp.sourceKind) {
    case SourceKind::Model:
        query = queryModel(id);
        break;
    case SourceKind::MediaAreaPage:
        query = queryMediaArea();
        break;
    case SourceKind::GitHubRelease:
    case SourceKind::GitHubNightlyRelease:
        query = queryGitHub(id);
        break;
    }
    // 只缓存成功结论（GitHub 未认证限流 60 次/小时/IP）；失败项下次打开重查
    m_queryCache.insert(static_cast<int>(id), query);
    return query;
}

HttpClient::Response DependencyInstallJob::blockingRequest(const HttpClient &client,
                                                           const HttpClient::Request &request) {
    const HttpClient::RequestId id = client.submit(request);
    markInFlight(id);
    while (!HttpClient::wait(id, kQuerySliceMs)) {
        if (m_cancel.load()) {
            HttpClient::abort(id);
            break;
        }
    }
    return HttpClient::take(id);
}

SourceQuery DependencyInstallJob::queryGitHub(Id id) {
    const Component &comp = component(id);
    const QString repo = QLatin1String(comp.githubRepo);
    const QRegularExpression pattern = githubAssetPattern(comp);

    const HttpClient::Response latest =
        blockingRequest(m_queryClient, githubRequest(githubLatestReleaseUrl(repo)));
    if (latest.canceled)
        return canceledQuery();
    if (latest.status != 200)
        return failedQuery(httpErrorText(QStringLiteral("GitHub latest release"), latest.status));
    const QJsonDocument latestDoc = QJsonDocument::fromJson(latest.body);
    const QString version =
        githubReleaseVersion(latestDoc.object()[QLatin1String("tag_name")].toString());
    if (version.isEmpty())
        return failedQuery(QStringLiteral("GitHub latest response missing tag_name"));
    if (comp.sourceKind == SourceKind::GitHubRelease)
        return fromAsset(githubPickAsset(latestDoc, pattern), version);

    // 二进制挂在 nightly tag 上：先从发布说明取构建号，缺行再退 /releases 列表
    const QString buildNumber =
        githubNightlyBuildNumber(latestDoc.object()[QLatin1String("body")].toString());
    if (!buildNumber.isEmpty()) {
        const HttpClient::Response nightly = blockingRequest(
            m_queryClient,
            githubRequest(githubReleaseByTagUrl(repo, githubNightlyTag(buildNumber))));
        if (nightly.canceled)
            return canceledQuery();
        if (nightly.status != 200)
            return failedQuery(
                httpErrorText(QStringLiteral("GitHub nightly release"), nightly.status));
        return fromAsset(githubPickAsset(QJsonDocument::fromJson(nightly.body), pattern), version);
    }

    const HttpClient::Response releases = blockingRequest(
        m_queryClient, githubRequest(githubReleasesListUrl(repo, kGithubFallbackPerPage)));
    if (releases.canceled)
        return canceledQuery();
    if (releases.status != 200)
        return failedQuery(
            httpErrorText(QStringLiteral("GitHub releases fallback"), releases.status));
    const QJsonArray list = QJsonDocument::fromJson(releases.body).array();
    const int index = githubFallbackReleaseIndex(list, pattern);
    if (index < 0)
        return failedQuery(QStringLiteral("no recent nightly release carries the target asset"));
    report(Logging::Level::Info,
           QStringLiteral("nightly line missing for %1, fell back to release %2")
               .arg(QLatin1String(comp.name),
                    list.at(index).toObject()[QLatin1String("tag_name")].toString()));
    return fromAsset(githubPickAsset(QJsonDocument(list.at(index).toObject()), pattern), version);
}

SourceQuery DependencyInstallJob::queryMediaArea() {
    const HttpClient::Response page = blockingRequest(
        m_queryClient, plainGetRequest(QUrl(
                           QStringLiteral("https://mediaarea.net/en/MediaInfo/Download/Windows"))));
    if (page.canceled)
        return canceledQuery();
    if (page.status != 200)
        return failedQuery(httpErrorText(QStringLiteral("mediaarea download page"), page.status));
    const QString version = mediaAreaVersion(QString::fromUtf8(page.body));
    if (version.isEmpty())
        return failedQuery(QStringLiteral("download page has no package link"));
    const QUrl url = mediaAreaDownloadUrl(version);

    // 下载页不标体积：先 HEAD 取 Content-Length，取不到再用 Range 读 Content-Range 总长
    HttpClient::Request head;
    head.url = url;
    head.method = "HEAD";
    head.headersOnly = true;
    qint64 size = headerContentLength(blockingRequest(m_queryClient, head).headers);
    if (size < 0) {
        HttpClient::Request range = plainGetRequest(url);
        range.headers.append(qMakePair(QByteArray("Range"), QByteArray("bytes=0-0")));
        const HttpClient::Response ranged = blockingRequest(m_queryClient, range);
        if (ranged.status == 206 || ranged.status == 200)
            size = headerContentRangeTotal(ranged.headers);
    }
    if (size < 0)
        return failedQuery(QStringLiteral("package size unavailable"));
    SourceQuery query;
    query.ok = true;
    query.version = version;
    query.downloadBytes = size;
    query.downloadUrl = url;
    return query;
}

SourceQuery DependencyInstallJob::queryModel(Id id) {
    const QVector<ModelHost> chain = modelHostChain(component(id));
    for (int i = 0; i < chain.size(); ++i) {
        if (m_cancel.load())
            break;
        if (hostFailedThisSession(modelHostName(chain.at(i)))) {
            // 本次向导会话里已经失败过的 host 不再试；原因一并带出，免得只剩一句"跳过"
            const QString host = modelHostName(chain.at(i));
            report(Logging::Level::Info,
                   QStringLiteral("skipping model host %1 (failed this session: %2)")
                       .arg(host, hostFailureReason(host)));
            continue;
        }
        SourceQuery query = queryModelHost(id, chain.at(i));
        if (!query.ok) {
            markHostFailed(modelHostName(chain.at(i)), query.error);
            continue;
        }
        query.hostIndex = i; // 零字节换 host 从这一位的下一家起试
        return query;
    }
    return failedQuery(QStringLiteral("no model host provided the file"));
}

SourceQuery DependencyInstallJob::queryModelHost(Id id, ModelHost host) {
    const Component &comp = component(id);
    const QString hostName = modelHostName(host);
    const HttpClient::Response response =
        blockingRequest(m_modelClient, plainGetRequest(modelQueryUrl(host, comp)));
    if (response.canceled)
        return canceledQuery();
    if (response.status != 200)
        return failedQuery(httpErrorText(hostName, response.status));
    qint64 size = -1;
    QByteArray hash;
    if (!modelLookup(host, QJsonDocument::fromJson(response.body), comp, &size, &hash) || size < 0)
        return failedQuery(
            QStringLiteral("%1 does not list %2").arg(hostName, QLatin1String(comp.modelPath)));
    if (hash.isEmpty())
        report(
            Logging::Level::Warning,
            QStringLiteral("model host %1 provides no hash, verification disabled").arg(hostName));
    SourceQuery query;
    query.ok = true;
    query.downloadBytes = size;
    query.sha256 = hash;
    query.downloadUrl = modelDownloadUrl(host, comp);
    return query;
}

bool DependencyInstallJob::hostFailedThisSession(const QString &host) const {
    return m_failedHosts.contains(host);
}

QString DependencyInstallJob::hostFailureReason(const QString &host) const {
    return m_failedHosts.value(host);
}

void DependencyInstallJob::markHostFailed(const QString &host, const QString &reason) {
    m_failedHosts.insert(host, reason);
    report(Logging::Level::Warning,
           QStringLiteral("model host %1 failed this session: %2").arg(host, reason));
}

// —— 执行 ——

void DependencyInstallJob::runExecute(const QString &pluginDir, const QVector<PlanEntry> &plan) {
    m_cancel.store(false);
    setPhase(Phase::Executing);
    cleanupTempRoot(pluginDir); // 进执行页先把临时根清干净，落位与下载都从空根起
    m_progressDone = 0;
    m_progressTotal = progressDenominator(plan, [this](Id id) { return packageBytes(id); });
    emitProgress();

    Outcome outcome;
    for (const PlanEntry &entry : plan) {
        if (m_cancel.load())
            break;
        executeEntry(pluginDir, entry, outcome);
    }
    setPhase(Phase::Idle);
    emit finished(outcome.installed, outcome.removed, outcome.failed, outcome.skipped);
    cleanupTempRoot(pluginDir);
}

qint64 DependencyInstallJob::packageBytes(Id id) const {
    return m_queryCache.value(static_cast<int>(id)).downloadBytes;
}

void DependencyInstallJob::startItem(const PlanEntry &entry, qint64 packageBytes) {
    m_itemWeight = itemProgressWeight(entry, packageBytes);
    m_itemDone = 0;
}

void DependencyInstallJob::advanceProgress(qint64 weightBytes) {
    m_progressDone += weightBytes;
    m_itemDone += weightBytes;
}

void DependencyInstallJob::rewindProgress(qint64 weightBytes) {
    m_progressDone -= weightBytes;
    m_itemDone -= weightBytes;
}

void DependencyInstallJob::emitProgress() { emit progress(m_progressDone, m_progressTotal); }

void DependencyInstallJob::advanceStep() {
    advanceProgress(kProgressStepWeightBytes);
    emitProgress();
}

void DependencyInstallJob::finishItem() {
    // 失败项也走完自己的权重，否则进度条收口时永远差一截
    if (m_itemDone < m_itemWeight) {
        advanceProgress(m_itemWeight - m_itemDone);
        emitProgress();
    }
    m_itemWeight = 0;
    m_itemDone = 0;
}

void DependencyInstallJob::executeEntry(const QString &pluginDir, const PlanEntry &entry,
                                        Outcome &outcome) {
    const Component &comp = component(entry.id);
    startItem(entry, packageBytes(entry.id));
    switch (entry.action) {
    case Action::Skip:
        report(Logging::Level::Info, wizText(kLogSkipped, QLatin1String(comp.installId)));
        ++outcome.skipped;
        break;
    case Action::Remove:
        executeRemove(pluginDir, comp, outcome);
        break;
    case Action::Install:
    case Action::Reinstall:
        executeInstall(pluginDir, entry, comp, outcome);
        break;
    case Action::Keep:
        break;
    case Action::None:
        // "方块 + 未安装"界面不允许出现，真遇到按无动作处理并留一行
        report(Logging::Level::Warning,
               QStringLiteral("no action for %1 (half-checked but not installed)")
                   .arg(QLatin1String(comp.installId)));
        break;
    }
    finishItem();
}

void DependencyInstallJob::executeRemove(const QString &pluginDir, const Component &comp,
                                         Outcome &outcome) {
    emit stepText(wizText(kStepRemoving, QLatin1String(comp.name)));
    const SwapResult result = removeFromPluginDir(pluginDir, QLatin1String(comp.installId),
                                                  QLatin1String(comp.installTarget));
    advanceStep();
    if (!result.ok) {
        report(Logging::Level::Error, result.error);
        ++outcome.failed;
        return;
    }
    report(Logging::Level::Info,
           QStringLiteral("removed %1")
               .arg(DependencySwap::pluginPath(pluginDir, QLatin1String(comp.installTarget))));
    ++outcome.removed;
}

void DependencyInstallJob::executeInstall(const QString &pluginDir, const PlanEntry &entry,
                                          const Component &comp, Outcome &outcome) {
    const QString name = QLatin1String(comp.name);
    const QString target = DependencySwap::pluginPath(pluginDir, QLatin1String(comp.installTarget));
    QString download;
    // 每一项的下载件与解压残渣当场清（模型成功落位后下载件已随交换迁走，此处空转）
    const auto discardAttempt = [&] {
        QFile::remove(download);
        DependencySwap::discardExtractDir(pluginDir, comp);
    };

    const auto cached = m_queryCache.constFind(static_cast<int>(comp.id));
    if (cached == m_queryCache.constEnd() || !cached->ok || cached->downloadBytes < 0) {
        // 大小是第一步的硬门槛：进得来的执行项必有成功结论，缺即按跳过记
        report(
            Logging::Level::Warning,
            QStringLiteral("no cached size for %1, skipping").arg(QLatin1String(comp.installId)));
        report(Logging::Level::Info, wizText(kLogSkipped, QLatin1String(comp.installId)));
        ++outcome.skipped;
        return;
    }

    // 落点的父目录现建：临时根刚被清过，整包即目标的形态还要落进 stage/<id>
    QString downloadError;
    download = DependencySwap::prepareDownloadTarget(pluginDir, comp, &downloadError);
    if (download.isEmpty()) {
        report(Logging::Level::Error, downloadError);
        ++outcome.failed;
        return;
    }

    emit stepText(wizText(kStepDownloading, name));
    report(Logging::Level::Info, QStringLiteral("downloading %1 (%2) from %3")
                                     .arg(name, displayDownloadSize(cached->downloadBytes),
                                          cached->downloadUrl.toString()));
    if (!downloadWithRetry(comp.id, cached.value(), download)) {
        discardAttempt();
        ++outcome.failed;
        return;
    }

    QString staged = download; // 整包即目标的形态（模型）：下载件本身就是暂存件
    if (comp.extract.kind != ExtractKind::None) {
        emit stepText(wizText(kStepExtracting, name));
        if (!extractPackage(pluginDir, comp.id, download)) {
            discardAttempt();
            ++outcome.failed;
            return;
        }
        advanceStep();
        QString stageError;
        if (!DependencySwap::stageExtracted(pluginDir, comp, &staged, &stageError)) {
            report(Logging::Level::Error, stageError);
            discardAttempt();
            ++outcome.failed;
            return;
        }
        report(Logging::Level::Info, QStringLiteral("staged %1").arg(staged));
    }

    emit stepText(
        wizText(entry.action == Action::Reinstall ? kStepReinstalling : kStepInstalling, name));
    report(Logging::Level::Info, QStringLiteral("placing %1 -> %2").arg(staged, target));
    const SwapResult result = swapIntoPlace(pluginDir, QLatin1String(comp.installId),
                                            QLatin1String(comp.installTarget), staged);
    advanceStep();
    discardAttempt();
    if (!result.ok) {
        // 换回失败时旧版落在 backup 里：路径必须进日志，否则用户只看到"失败"找不到原件
        report(Logging::Level::Error, result.backupPath.isEmpty()
                                          ? result.error
                                          : QStringLiteral("%1 (old content kept in %2)")
                                                .arg(result.error, result.backupPath));
        ++outcome.failed;
        return;
    }
    report(Logging::Level::Info,
           result.backupPath.isEmpty()
               ? QStringLiteral("placed %1 (old version deleted)").arg(target)
               : QStringLiteral("old version kept in backup: %1").arg(result.backupPath));
    ++outcome.installed;
}

bool DependencyInstallJob::downloadWithRetry(Id id, const SourceQuery &query,
                                             const QString &destFile) {
    const Component &comp = component(id);
    const QVector<ModelHost> chain = modelHostChain(comp);
    QUrl url = query.downloadUrl;
    int hostIndex = query.hostIndex;
    RetryBudget budget; // 额度按项独立：每一项各自 3 次
    while (true) {
        if (m_cancel.load())
            return false;
        DownloadFailure failure = DownloadFailure::None;
        QString error;
        int status = 0;
        qint64 received = 0;
        if (downloadOnce(id, url, query.downloadBytes, query.sha256, destFile, &failure, &error,
                         &status, &received)) {
            report(Logging::Level::Info,
                   query.sha256.isEmpty()
                       ? QStringLiteral("downloaded %1 (%2 bytes, no hash available)")
                             .arg(QLatin1String(comp.installId))
                             .arg(received)
                       : QStringLiteral("downloaded %1 (%2 bytes, sha256 verified)")
                             .arg(QLatin1String(comp.installId))
                             .arg(received));
            return true;
        }
        if (m_cancel.load())
            return false;

        const RetryBudget::Verdict verdict =
            budget.decide(failure, status, received, hostIndex + 1 < chain.size());
        if (verdict.move == RetryBudget::Move::SwitchHost) {
            // 零字节才换家；重查该家的大小与哈希，与第一步不一致即判该项失败
            const ModelHost next = chain.at(static_cast<size_t>(hostIndex + 1));
            const SourceQuery alt = queryModelHost(id, next);
            if (!alt.ok) {
                report(Logging::Level::Error,
                       QStringLiteral("host switch failed: %1").arg(alt.error));
                return false;
            }
            if (alt.downloadBytes != query.downloadBytes ||
                (!query.sha256.isEmpty() && alt.sha256 != query.sha256)) {
                report(Logging::Level::Error,
                       QStringLiteral("host switch failed: metadata mismatch on %1 (size %2 vs %3)")
                           .arg(modelHostName(next))
                           .arg(alt.downloadBytes)
                           .arg(query.downloadBytes));
                return false;
            }
            report(Logging::Level::Info,
                   wizText(kLogHostSwitch)
                       .arg(modelHostName(chain.at(static_cast<size_t>(hostIndex))),
                            failureText(failure, error), modelHostName(next),
                            alt.downloadUrl.toString()));
            url = alt.downloadUrl;
            ++hostIndex;
            continue; // 不消耗额度、不退避
        }
        if (verdict.move == RetryBudget::Move::Retry) {
            if (received > 0) {
                rewindProgress(received); // 从头重下，当前项已收字节打回起点
                emitProgress();
            }
            report(Logging::Level::Warning, wizText(kLogDownloadRetry)
                                                .arg(budget.attemptsUsed())
                                                .arg(budget.limit())
                                                .arg(failureText(failure, error)));
            sleepCancellable(verdict.backoffMs);
            continue;
        }
        report(Logging::Level::Error, failureText(failure, error));
        return false;
    }
}

bool DependencyInstallJob::downloadOnce(Id id, const QUrl &url, qint64 expectedBytes,
                                        const QByteArray &expectedSha256, const QString &destFile,
                                        DownloadFailure *failure, QString *error, int *httpStatus,
                                        qint64 *received) {
    *failure = DownloadFailure::None;
    *httpStatus = 0;
    *received = 0;

    QFile file(destFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *failure = DownloadFailure::LocalWrite;
        *error = file.errorString();
        return false;
    }
    const auto abortJob = [&](const HttpClient::RequestId &jobId) {
        HttpClient::abort(jobId);
        file.close();
    };

    HttpClient::Request request;
    request.url = url;
    request.method = "GET";
    request.streaming = true;
    const HttpClient::RequestId jobId = m_downloadClient.submit(request);
    markInFlight(jobId);

    // 响应头窗口：启动窗口由 waitHeaders 的入参承担（流式作业不吃 Config::timeoutMs）
    QElapsedTimer headerTimer;
    headerTimer.start();
    while (!HttpClient::waitHeaders(jobId, kQuerySliceMs)) {
        if (m_cancel.load()) {
            abortJob(jobId);
            *failure = DownloadFailure::Canceled;
            *error = QStringLiteral("canceled");
            return false;
        }
        if (headerTimer.hasExpired(kDownloadStallMs)) {
            abortJob(jobId);
            *failure = DownloadFailure::Network;
            *error = QStringLiteral("no response headers in 60 s");
            return false;
        }
    }
    const HttpClient::Response head = HttpClient::statusOf(jobId);
    if (head.canceled) {
        file.close();
        *failure = DownloadFailure::Canceled;
        *error = QStringLiteral("canceled");
        return false;
    }
    *httpStatus = head.status;
    if (head.status < 200 || head.status >= 300) {
        abortJob(jobId);
        *failure = classifyStatus(head.status);
        *error = QStringLiteral("HTTP %1").arg(head.status);
        return false;
    }
    const qint64 contentLength = headerContentLength(head.headers);
    if (contentLength >= 0 && contentLength != expectedBytes) {
        abortJob(jobId);
        *failure = DownloadFailure::SizeMismatch;
        *error = QStringLiteral("Content-Length %1 differs from queried size %2")
                     .arg(contentLength)
                     .arg(expectedBytes);
        return false;
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    char buffer[kDownloadChunk];
    QElapsedTimer emitTimer;
    emitTimer.start();
    while (true) {
        const qint64 n = HttpClient::readBlocking(jobId, buffer, kDownloadChunk, kDownloadStallMs);
        if (n > 0) {
            if (file.write(buffer, n) != n) {
                abortJob(jobId);
                *failure = DownloadFailure::LocalWrite;
                *error = file.errorString();
                return false;
            }
            hasher.addData(QByteArrayView(buffer, static_cast<qsizetype>(n)));
            *received += n;
            advanceProgress(n);
            if (emitTimer.hasExpired(kProgressEmitMs)) {
                emitTimer.restart();
                emitProgress(); // 记账逐块、发信号每 100 ms 一次
            }
            continue;
        }
        if (n == HttpClient::kReadEof)
            break;
        abortJob(jobId);
        if (n == HttpClient::kReadNoData) {
            *failure = DownloadFailure::Network;
            *error = QStringLiteral("no bytes for 60 s");
            return false;
        }
        // kReadError：终态错误或撤销，原因向 statusOf 取
        const HttpClient::Response tail = HttpClient::statusOf(jobId);
        if (tail.canceled) {
            *failure = DownloadFailure::Canceled;
            *error = QStringLiteral("canceled");
            return false;
        }
        *httpStatus = tail.status;
        *failure = classifyStatus(tail.status);
        *error = tail.error.isEmpty() ? QStringLiteral("HTTP %1").arg(tail.status) : tail.error;
        return false;
    }
    file.close();

    if (*received != expectedBytes) {
        *failure = DownloadFailure::SizeMismatch;
        *error = QStringLiteral("received %1 bytes, expected %2").arg(*received).arg(expectedBytes);
        return false;
    }
    if (expectedSha256.isEmpty()) {
        report(Logging::Level::Warning, QStringLiteral("no hash available for %1, size check only")
                                            .arg(QLatin1String(component(id).installId)));
        return true;
    }
    if (hasher.result().toHex() != expectedSha256) {
        report(
            Logging::Level::Error,
            QStringLiteral("sha256 mismatch for %1").arg(QLatin1String(component(id).installId)));
        *failure = DownloadFailure::HashMismatch;
        *error = QStringLiteral("sha256 mismatch");
        return false;
    }
    return true;
}

bool DependencyInstallJob::extractPackage(const QString &pluginDir, Id id,
                                          const QString &packageFile) {
    const Component &comp = component(id);
    const QString tar = findTar();
    if (tar.isEmpty()) {
        report(Logging::Level::Error,
               QStringLiteral("extraction tool unavailable: no System32 tar.exe or bsdtar on "
                              "PATH"));
        return false;
    }
    // PATH 上可能有 GNU tar（它不认 7z）：起解压前先验 bsdtar，版本随日志留痕
    QProcess versionProbe;
    versionProbe.setProgram(tar);
    versionProbe.setArguments({QStringLiteral("--version")});
    versionProbe.start();
    const ProcessExit probeExit = waitProcess(versionProbe, kProbeTimeoutMs, m_cancel);
    const QString versionText =
        QString::fromUtf8(versionProbe.readAllStandardOutput()).simplified();
    if (probeExit != ProcessExit::Finished || versionProbe.exitStatus() != QProcess::NormalExit ||
        !versionText.contains(QLatin1String("bsdtar"))) {
        report(Logging::Level::Error,
               QStringLiteral("extraction tool unavailable: %1 is not bsdtar").arg(tar));
        return false;
    }
    report(Logging::Level::Info, QStringLiteral("extracting via %1 (%2)").arg(tar, versionText));

    if (!prepareExtractDir(pluginDir, comp)) {
        report(Logging::Level::Error, QStringLiteral("could not prepare extraction directory "
                                                     "for %1")
                                          .arg(QLatin1String(comp.installId)));
        return false;
    }
    const QString extractDir = DependencySwap::extractDirFor(pluginDir, comp);
    QStringList arguments;
    arguments << QStringLiteral("-xf") << packageFile << QStringLiteral("-C") << extractDir;
    if (comp.extract.kind == ExtractKind::SingleFile)
        arguments << QString::fromLatin1(comp.extract.pattern);

    QProcess process;
    process.setProgram(tar);
    process.setArguments(arguments);
    process.start();
    const ProcessExit exit = waitProcess(process, kExtractTimeoutMs, m_cancel);
    if (exit == ProcessExit::Canceled)
        return false;
    if (exit == ProcessExit::TimedOut) {
        report(Logging::Level::Error, QStringLiteral("extraction timed out"));
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        // 解压器不可用已在上方判掉，走到这里只剩包本身的问题
        report(Logging::Level::Error,
               QStringLiteral("package corrupted or extraction failed (tar exit %1)")
                   .arg(process.exitCode()));
        return false;
    }
    int files = 0;
    QDirIterator it(extractDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++files;
    }
    report(Logging::Level::Info, QStringLiteral("extracted %1 files").arg(files));
    return true;
}

QString DependencyInstallJob::findTar() const {
    // 优先系统 bsdtar；PATH 上可能是 GNU tar（不认 7z），回落那一条还要再验版本
    const QString systemRoot = QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot"));
    if (!systemRoot.isEmpty()) {
        const QString systemTar = systemRoot + QStringLiteral("/System32/tar.exe");
        if (QFileInfo::exists(systemTar))
            return systemTar;
    }
    return QStandardPaths::findExecutable(QStringLiteral("tar"));
}

void DependencyInstallJob::sleepCancellable(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (!m_cancel.load() && !timer.hasExpired(ms)) {
        const qint64 remaining = ms - timer.elapsed();
        QThread::msleep(static_cast<unsigned long>(qMin<qint64>(remaining, 200)));
    }
}

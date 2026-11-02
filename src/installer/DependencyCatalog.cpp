#include "pch.h"

#include "installer/DependencyCatalog.h"

#include "Util.h"
#include "Version.h"
#include "installer/InstallerStrings.h"

using namespace InstallerText;

namespace DependencyCatalog {

namespace {

// 组件目录表：一个组件的全部知识面只在这一处。顺序即 UI 顺序；照 SettingsDialog 的
// SettingItem 写法——必填七项打底，其余按来源形态点到为止，省下的格子取默认值
const Component kComponents[] = {
    {
        .id = Id::MediaInfo,
        .installId = "mediainfo",
        .name = "MediaInfo",
        .category = Category::Basic,
        .descriptionText =
            QT_TRANSLATE_NOOP("DependencyCatalog", "Media information for media files"),
        .installTarget = "MediaInfo.dll",
        .sourceKind = SourceKind::MediaAreaPage,
        .extract = {.kind = ExtractKind::SingleFile,
                    .pattern = "MediaInfo.dll",
                    .requiredEntry = "MediaInfo.dll"},
        .probe = {.kind = ProbeKind::DllVersion},
    },
    {
        .id = Id::Ffmpeg,
        .installId = "ffmpeg",
        .name = "FFmpeg",
        .category = Category::Basic,
        .descriptionText = QT_TRANSLATE_NOOP("DependencyCatalog", "Extracts subtitles and audio"),
        .installTarget = "ffmpeg.exe",
        .sourceKind = SourceKind::GitHubRelease,
        .githubRepo = "GyanD/codexffmpeg",
        .assetPattern = R"(^ffmpeg-.*-essentials_build\.7z$)",
        .extract = {.kind = ExtractKind::SingleFile,
                    .pattern = R"(*/bin/ffmpeg.exe)",
                    .requiredEntry = "ffmpeg.exe"},
        .probe = {.kind = ProbeKind::Process,
                  .arg = "-version",
                  .versionPattern = R"(^ffmpeg version (\S+?)(?:[- ].*)?$)"},
    },
    {
        .id = Id::WhisperCpp,
        .installId = "whisper-cpp",
        .name = "whisper.cpp",
        .category = Category::SpeechRecognition,
        .descriptionText = QT_TRANSLATE_NOOP("DependencyCatalog", "ASR engine (CPU build)"),
        .installTarget = "whisper.cpp",
        .sourceKind = SourceKind::GitHubNightlyRelease,
        .installedMarker = "whisper.cpp/whisper-cli.exe",
        .isDirectory = true,
        .githubRepo = "ggerganov/whisper.cpp",
        .assetPattern = R"(^whisper-bin-x64\.zip$)",
        .extract = {.kind = ExtractKind::DirStrip,
                    .stripPrefix = "Release/",
                    .requiredEntry = "whisper-cli.exe"},
        .probe = {.kind = ProbeKind::Process,
                  .arg = "--version",
                  .versionPattern = R"rx(^whisper\.cpp version: (\S+))rx"},
    },
    {
        .id = Id::WhisperTurbo,
        .installId = "whisper-large-v3-turbo",
        .name = "Whisper large-v3-turbo",
        .category = Category::SpeechRecognition,
        .descriptionText = QT_TRANSLATE_NOOP("DependencyCatalog", "ASR model"),
        .installTarget = "model/ggml-large-v3-turbo.bin",
        .sourceKind = SourceKind::Model,
        .modelRepo = "ggerganov/whisper.cpp",
        .modelPath = "ggml-large-v3-turbo.bin",
        .hosts = {ModelHost::HuggingFace, ModelHost::HfMirror},
    },
    {
        .id = Id::WhisperVad,
        .installId = "whisper-silero-vad",
        .name = "Silero VAD",
        .category = Category::SpeechRecognition,
        .descriptionText = QT_TRANSLATE_NOOP("DependencyCatalog", "Voice activity detection model"),
        .installTarget = "model/ggml-silero-v6.2.0.bin",
        .sourceKind = SourceKind::Model,
        .modelRepo = "ggml-org/whisper-vad",
        .modelPath = "ggml-silero-v6.2.0.bin",
        .hosts = {ModelHost::HuggingFace, ModelHost::HfMirror},
    },
    {
        .id = Id::LlamaCpp,
        .installId = "llama-cpp",
        .name = "llama.cpp",
        .category = Category::Translation,
        .descriptionText =
            QT_TRANSLATE_NOOP("DependencyCatalog", "Local translation server (CPU build)"),
        .installTarget = "llama.cpp",
        .sourceKind = SourceKind::GitHubNightlyRelease,
        .installedMarker = "llama.cpp/llama-server.exe",
        .isDirectory = true,
        .githubRepo = "ggml-org/llama.cpp",
        .assetPattern = R"(^llama-b\d+-bin-win-cpu-x64\.zip$)",
        .extract = {.kind = ExtractKind::DirRoot, .requiredEntry = "llama-server.exe"},
        .probe = {.kind = ProbeKind::Process,
                  .arg = "--version",
                  .versionPattern = R"(^version: (\S+?)(?:[- ].*)?$)"},
    },
    {
        .id = Id::HyMt2,
        .installId = "hy-mt2-1.8b",
        .name = "Hy-MT2 1.8B",
        .category = Category::Translation,
        .descriptionText = QT_TRANSLATE_NOOP("DependencyCatalog", "Translation model (Q6_K)"),
        .installTarget = "model/Hy-MT2-1.8B-Q6_K.gguf",
        .sourceKind = SourceKind::Model,
        .modelRepo = "tencent/Hy-MT2-1.8B-GGUF",
        .modelPath = "Hy-MT2-1.8B-Q6_K.gguf",
        .hosts = {ModelHost::HuggingFace, ModelHost::ModelScope, ModelHost::HfMirror},
    },
};

constexpr const char *kGithubApi = "https://api.github.com/repos/";
constexpr const char *kHfHost = "huggingface.co";
constexpr const char *kHfMirrorHost = "hf-mirror.com";
constexpr const char *kModelScopeHost = "www.modelscope.cn";

} // namespace

Id idAt(int uiOrder) { return static_cast<Id>(uiOrder); }

const Component &component(Id id) { return kComponents[static_cast<int>(id)]; }

const CategoryGroup *categoryGroups(int &count) {
    static const CategoryGroup kGroups[] = {
        {.category = Category::Basic,
         .title = QT_TRANSLATE_NOOP("DependencyCatalog", "Basic"),
         .items = {Id::MediaInfo, Id::Ffmpeg}},
        {.category = Category::SpeechRecognition,
         .title = QT_TRANSLATE_NOOP("DependencyCatalog", "ASR"),
         .items = {Id::WhisperCpp, Id::WhisperTurbo, Id::WhisperVad}},
        {.category = Category::Translation,
         .title = QT_TRANSLATE_NOOP("DependencyCatalog", "Translation"),
         .items = {Id::LlamaCpp, Id::HyMt2}},
    };
    count = 3;
    return kGroups;
}

QRegularExpression githubAssetPattern(const Component &comp) {
    if (!comp.assetPattern || !*comp.assetPattern)
        return {};
    return QRegularExpression(QString::fromLatin1(comp.assetPattern));
}

Qt::CheckState nextCheckState(Qt::CheckState current, bool installed, bool) {
    if (installed) {
        switch (current) {
        case Qt::PartiallyChecked:
            return Qt::Checked;
        case Qt::Checked:
            return Qt::Unchecked;
        case Qt::Unchecked:
        default:
            return Qt::PartiallyChecked;
        }
    }
    // 未装项跳过方块：空 → 对钩 → 空
    return current == Qt::Checked ? Qt::Unchecked : Qt::Checked;
}

PlanEntry planAction(Id id, Qt::CheckState state, bool installed, bool available) {
    if (state == Qt::Checked)
        return {id, available ? (installed ? Action::Reinstall : Action::Install) : Action::Skip};
    if (state == Qt::Unchecked)
        return {id, installed ? Action::Remove : Action::None};
    return {id, installed ? Action::Keep : Action::None};
}

bool isAvailable(const SourceQuery &query, const Component &comp) {
    if (isModelComponent(comp))
        return query.ok && query.downloadBytes >= 0;
    return query.ok && !query.version.isEmpty() && query.downloadBytes >= 0;
}

bool isAvailable(const Component &comp, const RowFacts &row) {
    return isAvailable(row.query, comp);
}

bool rowSettled(const RowFacts &row) { return row.querySettled && row.probeSettled; }

bool allRowsSettled(const QVector<RowFacts> &rows) {
    return std::all_of(rows.cbegin(), rows.cend(),
                       [](const RowFacts &row) { return rowSettled(row); });
}

bool hasNewerVersion(const Component &comp, const RowFacts &row) {
    if (isModelComponent(comp) || !row.installed || !isAvailable(comp, row))
        return false;
    if (row.probeVersion.isEmpty() || row.query.version.isEmpty())
        return false;
    return compareDottedVersions(row.query.version, row.probeVersion) > 0;
}

QString statusText(const QVector<RowFacts> &rows) {
    if (!allRowsSettled(rows))
        return wizText(kStatusChecking);
    for (int i = 0; i < rows.size() && i < kComponentCount; ++i) {
        if (hasNewerVersion(component(idAt(i)), rows.at(i)))
            return wizText(kStatusNewVersions);
    }
    return {};
}

QVector<PlanEntry> executablePlan(const std::function<Qt::CheckState(Id)> &checkState,
                                  const QVector<RowFacts> &rows) {
    QVector<PlanEntry> plan;
    for (int i = 0; i < rows.size() && i < kComponentCount; ++i) {
        const Id id = idAt(i);
        const PlanEntry entry = planAction(id, checkState(id), rows.at(i).installed,
                                           isAvailable(component(id), rows.at(i)));
        if (entry.action != Action::None && entry.action != Action::Keep)
            plan.append(entry);
    }
    return plan;
}

bool planChangesFiles(const QVector<PlanEntry> &plan) {
    for (const PlanEntry &entry : plan) {
        if (entry.action == Action::Install || entry.action == Action::Reinstall ||
            entry.action == Action::Remove)
            return true;
    }
    return false;
}

QUrl githubLatestReleaseUrl(const QString &repo) {
    return QUrl(QString::fromLatin1(kGithubApi) + repo + QStringLiteral("/releases/latest"));
}

QUrl githubReleaseByTagUrl(const QString &repo, const QString &tag) {
    return QUrl(QString::fromLatin1(kGithubApi) + repo + QStringLiteral("/releases/tags/") + tag);
}

QUrl githubReleasesListUrl(const QString &repo, int perPage) {
    return QUrl(QString::fromLatin1(kGithubApi) + repo +
                QStringLiteral("/releases?per_page=%1").arg(perPage));
}

QByteArray githubUserAgent() { return QStringLiteral("oxbow/%1").arg(App::version()).toUtf8(); }

QString githubReleaseVersion(const QString &tagName) {
    QString tag = tagName;
    if (tag.size() > 1 && tag.startsWith(QLatin1Char('v')) && tag.at(1).isDigit())
        tag.remove(0, 1);
    return tag;
}

QString githubNightlyBuildNumber(const QString &releaseBody) {
    static const QRegularExpression nightlyLine(QStringLiteral(R"(\*\*Nightly build:\*\*)"));
    const auto lineMatch = nightlyLine.match(releaseBody);
    if (!lineMatch.hasMatch())
        return {};
    static const QRegularExpression buildNumber(QStringLiteral(R"(\bb(\d+)\b)"));
    const auto numberMatch = buildNumber.match(releaseBody, lineMatch.capturedEnd());
    return numberMatch.hasMatch() ? numberMatch.captured(1) : QString();
}

QString githubNightlyTag(const QString &buildNumber) {
    return buildNumber.isEmpty() ? QString() : QStringLiteral("b%1").arg(buildNumber);
}

GitHubAsset githubPickAsset(const QJsonDocument &release, const QRegularExpression &namePattern) {
    GitHubAsset result;
    // 空模式（非 GitHub 来源）不参与挑选：Qt6 里默认构造的正则视为有效且匹配任意前缀
    if (!release.isObject() || !namePattern.isValid() || namePattern.pattern().isEmpty())
        return result;
    const QJsonArray assets = release.object()[QLatin1String("assets")].toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset[QLatin1String("name")].toString();
        if (!namePattern.match(name).hasMatch())
            continue;
        result.name = name;
        result.browserUrl = asset[QLatin1String("browser_download_url")].toString();
        result.size = static_cast<qint64>(asset[QLatin1String("size")].toDouble(-1));
        const QString digest = asset[QLatin1String("digest")].toString();
        if (digest.startsWith(QLatin1String("sha256:")))
            result.sha256 = digest.mid(7).toLatin1();
        return result;
    }
    return result;
}

int githubFallbackReleaseIndex(const QJsonArray &releases, const QRegularExpression &namePattern) {
    static const QRegularExpression buildTag(QStringLiteral(R"(^b\d+$)"));
    for (int i = 0; i < releases.size(); ++i) {
        const QJsonObject release = releases.at(i).toObject();
        if (!buildTag.match(release[QLatin1String("tag_name")].toString()).hasMatch())
            continue;
        if (githubPickAsset(QJsonDocument(release), namePattern).isValid())
            return i;
    }
    return -1;
}

QString mediaAreaVersion(const QString &downloadPageHtml) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(MediaInfo_DLL_(\d+\.\d+)_Windows_x64_WithoutInstaller\.7z)"));
    const auto match = pattern.match(downloadPageHtml);
    return match.hasMatch() ? match.captured(1) : QString();
}

QUrl mediaAreaDownloadUrl(const QString &version) {
    return QUrl(QStringLiteral("https://mediaarea.net/download/binary/libmediainfo0/%1/"
                               "MediaInfo_DLL_%1_Windows_x64_WithoutInstaller.7z")
                    .arg(version));
}

qint64 headerContentLength(const QHash<QByteArray, QByteArray> &headers) {
    bool ok = false;
    const qint64 length = headers.value("content-length").toLongLong(&ok);
    return ok ? length : -1;
}

qint64 headerContentRangeTotal(const QHash<QByteArray, QByteArray> &headers) {
    // 形如 "bytes 0-0/12345"：斜杠后即总长
    const QString range = QString::fromLatin1(headers.value("content-range"));
    const int slash = range.lastIndexOf(QLatin1Char('/'));
    if (slash < 0)
        return -1;
    bool ok = false;
    const qint64 total = range.mid(slash + 1).toLongLong(&ok);
    return ok ? total : -1;
}

QVector<ModelHost> modelHostChain(const Component &comp) {
    QVector<ModelHost> chain;
    for (const ModelHost host : comp.hosts) {
        if (host == ModelHost::None)
            break; // 链尾哨兵：短链没写满的格子留默认值
        chain.append(host);
    }
    return chain;
}

QString modelHostName(ModelHost host) {
    switch (host) {
    case ModelHost::None:
        break; // 链尾哨兵没有名字
    case ModelHost::HuggingFace:
        return QLatin1String(kHfHost);
    case ModelHost::ModelScope:
        return QLatin1String(kModelScopeHost);
    case ModelHost::HfMirror:
        return QLatin1String(kHfMirrorHost);
    }
    return {};
}

QUrl hfTreeApiUrl(const QString &host, const QString &repo) {
    return QUrl(QStringLiteral("https://%1/api/models/%2/tree/main?expand=true").arg(host, repo));
}

QUrl hfResolveUrl(const QString &host, const QString &repo, const QString &path) {
    return QUrl(QStringLiteral("https://%1/%2/resolve/main/%3").arg(host, repo, path));
}

QUrl modelscopeFilesApiUrl(const QString &repo) {
    return QUrl(QStringLiteral("https://%1/api/v1/models/%2/repo/files?Revision=master&Root=")
                    .arg(QLatin1String(kModelScopeHost), repo));
}

QUrl modelscopeResolveUrl(const QString &repo, const QString &path) {
    return QUrl(QStringLiteral("https://%1/models/%2/resolve/master/%3")
                    .arg(QLatin1String(kModelScopeHost), repo, path));
}

QUrl modelQueryUrl(ModelHost host, const Component &comp) {
    switch (host) {
    case ModelHost::None:
        break;
    case ModelHost::HuggingFace:
        return hfTreeApiUrl(QLatin1String(kHfHost), QLatin1String(comp.modelRepo));
    case ModelHost::HfMirror:
        return hfTreeApiUrl(QLatin1String(kHfMirrorHost), QLatin1String(comp.modelRepo));
    case ModelHost::ModelScope:
        return modelscopeFilesApiUrl(QLatin1String(comp.modelRepo));
    }
    return {};
}

QUrl modelDownloadUrl(ModelHost host, const Component &comp) {
    switch (host) {
    case ModelHost::None:
        break;
    case ModelHost::HuggingFace:
        return hfResolveUrl(QLatin1String(kHfHost), QLatin1String(comp.modelRepo),
                            QLatin1String(comp.modelPath));
    case ModelHost::HfMirror:
        return hfResolveUrl(QLatin1String(kHfMirrorHost), QLatin1String(comp.modelRepo),
                            QLatin1String(comp.modelPath));
    case ModelHost::ModelScope:
        return modelscopeResolveUrl(QLatin1String(comp.modelRepo), QLatin1String(comp.modelPath));
    }
    return {};
}

bool hfTreeLookup(const QJsonDocument &doc, const QString &path, qint64 *size, QByteArray *oid) {
    if (!doc.isArray())
        return false;
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject entry = value.toObject();
        if (entry[QLatin1String("path")].toString() != path)
            continue;
        if (size)
            *size = static_cast<qint64>(entry[QLatin1String("size")].toDouble(-1));
        if (oid) {
            const QJsonObject lfs = entry[QLatin1String("lfs")].toObject();
            *oid = lfs[QLatin1String("oid")].toString().toLatin1();
        }
        return true;
    }
    return false;
}

bool modelscopeFilesLookup(const QJsonDocument &doc, const QString &path, qint64 *size,
                           QByteArray *sha256) {
    if (!doc.isObject())
        return false;
    const QJsonArray files =
        doc.object()[QLatin1String("Data")].toObject()[QLatin1String("Files")].toArray();
    for (const QJsonValue &value : files) {
        const QJsonObject entry = value.toObject();
        if (entry[QLatin1String("Path")].toString() != path)
            continue;
        const qint64 entrySize = static_cast<qint64>(entry[QLatin1String("Size")].toDouble(-1));
        if (entrySize < 0)
            return false; // 缺 Size 即空结果（大小是可用性的唯一依据）
        if (size)
            *size = entrySize;
        if (sha256)
            *sha256 = entry[QLatin1String("Sha256")].toString().toLatin1();
        return true;
    }
    return false;
}

bool modelLookup(ModelHost host, const QJsonDocument &doc, const Component &comp, qint64 *size,
                 QByteArray *sha256) {
    const QString path = QLatin1String(comp.modelPath);
    return host == ModelHost::ModelScope ? modelscopeFilesLookup(doc, path, size, sha256)
                                         : hfTreeLookup(doc, path, size, sha256);
}

QString latestVersionText(const Component &comp, const RowFacts &row) {
    if (isModelComponent(comp))
        return emptyCellText();
    if (!row.querySettled)
        return pendingCellText();
    return row.query.version.isEmpty() ? wizText(kCellFailed) : row.query.version;
}

QString installedVersionText(const Component &comp, const RowFacts &row) {
    if (isModelComponent(comp))
        return row.installed ? wizText(kCellInstalled) : emptyCellText();
    return row.probeVersion.isEmpty() ? emptyCellText() : row.probeVersion;
}

QString downloadSizeText(const RowFacts &row) {
    if (!row.querySettled)
        return pendingCellText();
    return row.query.downloadBytes < 0 ? wizText(kCellFailed)
                                       : displayDownloadSize(row.query.downloadBytes);
}

QString versionFailureTip(const QString &reason) { return wizText(kTipVersionFailed, reason); }

QString sizeFailureTip(const QString &reason) { return wizText(kTipSizeFailed, reason); }

QString pendingCellText() { return pendingCell(); }

QString emptyCellText() { return emptyCell(); }

int compareDottedVersions(const QString &a, const QString &b) {
    const QStringList aParts = a.split(QLatin1Char('.'));
    const QStringList bParts = b.split(QLatin1Char('.'));
    const int count = qMax(aParts.size(), bParts.size());
    for (int i = 0; i < count; ++i) {
        const long long av = i < aParts.size() ? aParts.at(i).toLongLong() : 0LL;
        const long long bv = i < bParts.size() ? bParts.at(i).toLongLong() : 0LL;
        if (av != bv)
            return av < bv ? -1 : 1;
    }
    return 0;
}

QString parseProbeVersion(Id id, const QString &rawOutput) {
    const ProbeSpec &probe = component(id).probe;
    if (probe.kind != ProbeKind::Process || !probe.versionPattern)
        return {};
    // 每次现构造：正则在表里按组件各一份，static 会把首次调用的那条留给所有组件
    const QRegularExpression pattern(QString::fromLatin1(probe.versionPattern));
    const QString firstLine = rawOutput.section(QLatin1Char('\n'), 0, 0).trimmed();
    const auto match = pattern.match(firstLine);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString normalizeDllFileVersion(const QString &fourPartVersion) {
    const QStringList parts = fourPartVersion.split(QLatin1Char('.'));
    if (parts.size() < 2)
        return {};
    return parts.at(0) + QLatin1Char('.') + parts.at(1);
}

QString displayDownloadSize(qint64 bytes) {
    if (bytes < 0)
        return emptyCellText();
    return Util::formatFileSize(bytes);
}

const int *backoffSchedule() {
    static const int kBackoff[] = {2000, 4000, 8000};
    return kBackoff;
}

bool retryableDownloadFailure(DownloadFailure failure) {
    return failure == DownloadFailure::Network || failure == DownloadFailure::HttpServerError;
}

bool zeroByteSwitchHost(DownloadFailure failure, int httpStatus) {
    if (failure == DownloadFailure::Network)
        return true;
    return failure == DownloadFailure::HttpClientError && (httpStatus == 404 || httpStatus == 403);
}

RetryBudget::Verdict RetryBudget::decide(DownloadFailure failure, int httpStatus,
                                         qint64 receivedBytes, bool altHostAvailable) {
    if (failure == DownloadFailure::Canceled)
        return {Move::Fail, 0};
    if (receivedBytes == 0 && altHostAvailable && zeroByteSwitchHost(failure, httpStatus))
        return {Move::SwitchHost, 0};
    if (!retryableDownloadFailure(failure))
        return {Move::Fail, 0};
    if (m_attempts >= kDownloadRetryLimit)
        return {Move::Fail, 0};
    const int backoff = backoffSchedule()[m_attempts];
    ++m_attempts;
    return {Move::Retry, backoff};
}

int stepCount(const PlanEntry &entry) {
    switch (entry.action) {
    case Action::Install:
    case Action::Reinstall:
        return component(entry.id).extract.kind == ExtractKind::None ? 1 : 2;
    case Action::Remove:
        return 1;
    case Action::None:
    case Action::Skip:
    case Action::Keep:
        break;
    }
    return 0;
}

qint64 itemProgressWeight(const PlanEntry &entry, qint64 packageBytes) {
    const qint64 bytes = (entry.action == Action::Install || entry.action == Action::Reinstall)
                             ? qMax<qint64>(0, packageBytes)
                             : 0;
    return bytes + kProgressStepWeightBytes * stepCount(entry);
}

qint64 progressDenominator(const QVector<PlanEntry> &plan,
                           const std::function<qint64(Id)> &packageBytes) {
    qint64 total = 0;
    for (const PlanEntry &entry : plan)
        total += itemProgressWeight(entry, packageBytes(entry.id));
    return total;
}

qint64 planPeakDiskBytes(const QVector<PlanEntry> &plan,
                         const std::function<qint64(Id)> &packageBytes) {
    qint64 total = 0;
    for (const PlanEntry &entry : plan) {
        const bool model = isModelComponent(component(entry.id));
        const qint64 package = qMax<qint64>(0, packageBytes(entry.id));
        if (entry.action == Action::Install)
            total += model ? package : package * 3;
        else if (entry.action == Action::Reinstall)
            total += model ? package * 2 : package * 5;
    }
    return total;
}

QString confirmationText(const QVector<PlanEntry> &plan, const std::function<QString(Id)> &name,
                         const std::function<QString(Id)> &sizeText) {
    QVector<PlanEntry> removals, installs, skips;
    for (const PlanEntry &entry : plan) {
        switch (entry.action) {
        case Action::Remove:
            removals.append(entry);
            break;
        case Action::Install:
        case Action::Reinstall:
            installs.append(entry);
            break;
        case Action::Skip:
            skips.append(entry);
            break;
        case Action::Keep:
        case Action::None:
            break;
        }
    }

    QString text;
    const auto appendGroup = [&text](const QVector<PlanEntry> &entries, const char *title,
                                     const std::function<QString(Id)> &nameFn,
                                     const std::function<QString(Id)> *sizeFn) {
        if (entries.isEmpty())
            return;
        if (!text.isEmpty())
            text += QLatin1Char('\n');
        text += wizText(title);
        text += QLatin1Char('\n');
        int nameWidth = 0;
        int sizeWidth = 0;
        for (const PlanEntry &entry : entries) {
            nameWidth = qMax(nameWidth, nameFn(entry.id).size());
            if (sizeFn)
                sizeWidth = qMax(sizeWidth, (*sizeFn)(entry.id).size());
        }
        for (const PlanEntry &entry : entries) {
            QString line = nameFn(entry.id).leftJustified(nameWidth);
            if (sizeFn)
                line += QLatin1Char(' ') + (*sizeFn)(entry.id).rightJustified(sizeWidth);
            text += line + QLatin1Char('\n');
        }
    };

    appendGroup(removals, kGroupRemoved, name, nullptr);
    appendGroup(installs, kGroupInstalled, name, &sizeText);
    appendGroup(skips, kGroupSkipped, name, nullptr);
    if (!text.isEmpty())
        text += QLatin1Char('\n');
    text += wizText(kGroupContinue);
    return text;
}

} // namespace DependencyCatalog

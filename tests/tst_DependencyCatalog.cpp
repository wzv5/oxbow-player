#include "pch.h"

#include <QtTest/QtTest>

#include "installer/DependencyCatalog.h"

#include "Util.h"

// N150：安装器目录层契约测试——组件单表形态、四类来源解析（GitHub latest / nightly 两步
// 与 /releases 回退、mediaarea 页面与响应头、HF tree、ModelScope files）、地址装配、模型
// host 链、版本归一与比较、三列文案、状态行与新版本判定、计划生成、三态循环、计划矩阵、
// 可用性、重试裁定与额度、进度权重、磁盘峰值估算、确认文本。全部离线：来源解析吃
// tests/data/ 下的快照，其余用内联文本。不测网络与解压（归行为面）。

using namespace DependencyCatalog;

namespace {

const char *kFixtureDir = "data/";

QJsonDocument loadJson(const char *name) {
    QFile file(QFINDTESTDATA(QString(kFixtureDir) + name));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll());
}

QString loadText(const char *name) {
    QFile file(QFINDTESTDATA(QString(kFixtureDir) + name));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

SourceQuery makeQuery(bool ok, const QString &version, qint64 bytes) {
    SourceQuery query;
    query.ok = ok;
    query.version = version;
    query.downloadBytes = bytes;
    return query;
}

// 一行落定后的事实：查询结论 + 探测结论 + 在不在
RowFacts settledRow(const QString &queryVersion, qint64 bytes, const QString &probeVersion,
                    bool installed) {
    RowFacts row;
    row.querySettled = true;
    row.probeSettled = true;
    row.installed = installed;
    row.query = makeQuery(!queryVersion.isEmpty() || bytes >= 0, queryVersion, bytes);
    row.probeVersion = probeVersion;
    return row;
}

QVector<RowFacts> rowsOf(const RowFacts &row) {
    QVector<RowFacts> rows;
    for (int i = 0; i < kComponentCount; ++i)
        rows.append(row);
    return rows;
}

} // namespace

class TestDependencyCatalog : public QObject {
    Q_OBJECT

  private slots:
    void componentTableShape();
    void githubUrls();
    void githubParsing();
    void githubNightlyFallback();
    void mediaAreaParsing();
    void modelSourcesAndUrls();
    void hfTreeFixture();
    void modelscopeLookup();
    void versionCompare();
    void probeParsing();
    void columnText();
    void sizeColumnText();
    void newerVersionAndStatus();
    void planGeneration();
    void checkStateCycle();
    void planMatrix();
    void availability();
    void retryPolicy();
    void retryBudget();
    void progressWeights();
    void diskEstimate();
    void confirmationOnlyInstalls();
    void confirmationOnlyRemovals();
    void confirmationBothGroups();
    void confirmationOnlySkips();
    void confirmationSkipAndInstallMixed();
};

void TestDependencyCatalog::componentTableShape() {
    QCOMPARE(kComponentCount, 7);
    // 顺序即 UI 顺序：mediainfo, ffmpeg, whisper-cpp, turbo, vad, llama-cpp, hy-mt2
    QCOMPARE(component(idAt(0)).installId, "mediainfo");
    QCOMPARE(component(idAt(1)).installId, "ffmpeg");
    QCOMPARE(component(idAt(2)).installId, "whisper-cpp");
    QCOMPARE(component(idAt(3)).installId, "whisper-large-v3-turbo");
    QCOMPARE(component(idAt(4)).installId, "whisper-silero-vad");
    QCOMPARE(component(idAt(5)).installId, "llama-cpp");
    QCOMPARE(component(idAt(6)).installId, "hy-mt2-1.8b");
    QCOMPARE(QLatin1String(component(Id::WhisperCpp).installedMarker),
             "whisper.cpp/whisper-cli.exe");
    QCOMPARE(QLatin1String(component(Id::LlamaCpp).installedMarker), "llama.cpp/llama-server.exe");
    QCOMPARE(QLatin1String(component(Id::Ffmpeg).installTarget), "ffmpeg.exe");
    QCOMPARE(QLatin1String(component(Id::MediaInfo).installTarget), "MediaInfo.dll");
    QCOMPARE(QLatin1String(component(Id::HyMt2).installTarget), "model/Hy-MT2-1.8B-Q6_K.gguf");
    QVERIFY(component(Id::WhisperCpp).isDirectory);
    QVERIFY(component(Id::LlamaCpp).isDirectory);

    // 来源形态：模型一族只由 sourceKind 判定
    QCOMPARE(int(component(Id::MediaInfo).sourceKind), int(SourceKind::MediaAreaPage));
    QCOMPARE(int(component(Id::Ffmpeg).sourceKind), int(SourceKind::GitHubRelease));
    QCOMPARE(int(component(Id::WhisperCpp).sourceKind), int(SourceKind::GitHubNightlyRelease));
    QCOMPARE(int(component(Id::LlamaCpp).sourceKind), int(SourceKind::GitHubNightlyRelease));
    QVERIFY(isModelComponent(component(Id::WhisperTurbo)));
    QVERIFY(isModelComponent(component(Id::WhisperVad)));
    QVERIFY(isModelComponent(component(Id::HyMt2)));
    QVERIFY(!isModelComponent(component(Id::Ffmpeg)));
    QCOMPARE(int(component(Id::WhisperTurbo).extract.kind), int(ExtractKind::None));
    QCOMPARE(int(component(Id::MediaInfo).extract.kind), int(ExtractKind::SingleFile));
    QCOMPARE(int(component(Id::Ffmpeg).extract.kind), int(ExtractKind::SingleFile));
    QCOMPARE(int(component(Id::WhisperCpp).extract.kind), int(ExtractKind::DirStrip));
    QCOMPARE(int(component(Id::LlamaCpp).extract.kind), int(ExtractKind::DirRoot));
    QCOMPARE(QLatin1String(component(Id::WhisperCpp).extract.stripPrefix), "Release/");
    QCOMPARE(QLatin1String(component(Id::WhisperCpp).extract.requiredEntry), "whisper-cli.exe");
    QCOMPARE(QLatin1String(component(Id::Ffmpeg).extract.pattern), "*/bin/ffmpeg.exe");

    // 探测：参数与解析正则同在一张表；模型不探测，MediaInfo 读版本资源
    QCOMPARE(int(component(Id::Ffmpeg).probe.kind), int(ProbeKind::Process));
    QCOMPARE(QLatin1String(component(Id::Ffmpeg).probe.arg), "-version");
    QCOMPARE(int(component(Id::WhisperCpp).probe.kind), int(ProbeKind::Process));
    QCOMPARE(QLatin1String(component(Id::WhisperCpp).probe.arg), "--version");
    QCOMPARE(int(component(Id::MediaInfo).probe.kind), int(ProbeKind::DllVersion));
    QCOMPARE(int(component(Id::HyMt2).probe.kind), int(ProbeKind::None));

    // GitHub 仓库与资产名模式（非 GitHub 来源留默认值）
    QCOMPARE(QLatin1String(component(Id::Ffmpeg).githubRepo), "GyanD/codexffmpeg");
    QCOMPARE(QLatin1String(component(Id::WhisperCpp).githubRepo), "ggerganov/whisper.cpp");
    QCOMPARE(QLatin1String(component(Id::LlamaCpp).githubRepo), "ggml-org/llama.cpp");
    QVERIFY(component(Id::MediaInfo).githubRepo == nullptr);
    QVERIFY(component(Id::MediaInfo).assetPattern == nullptr);
    QVERIFY(githubAssetPattern(component(Id::MediaInfo)).pattern().isEmpty());
    QVERIFY(githubAssetPattern(component(Id::WhisperCpp)).isValid());

    // "已安装"判据：文件型与模型一律省写（由目标现算），只有目录型写可执行文件
    QVERIFY(component(Id::MediaInfo).installedMarker == nullptr);
    QVERIFY(component(Id::HyMt2).installedMarker == nullptr);
    QCOMPARE(QLatin1String(installedMarkerOf(component(Id::MediaInfo))), "MediaInfo.dll");
    QCOMPARE(QLatin1String(installedMarkerOf(component(Id::HyMt2))), "model/Hy-MT2-1.8B-Q6_K.gguf");
    QCOMPARE(QLatin1String(installedMarkerOf(component(Id::WhisperCpp))),
             "whisper.cpp/whisper-cli.exe");

    // 分类分组：三个根、标题一处落地、成员归属正确
    int count = 0;
    const CategoryGroup *groups = categoryGroups(count);
    QCOMPARE(count, 3);
    QCOMPARE(QLatin1String(groups[0].title), "Basic");
    QCOMPARE(QLatin1String(groups[1].title), "ASR");
    QCOMPARE(QLatin1String(groups[2].title), "Translation");
    QCOMPARE(int(groups[0].items.size()), 2);
    QCOMPARE(int(groups[1].items.size()), 3);
    QCOMPARE(int(groups[2].items.size()), 2);
    QVERIFY(std::find(groups[1].items.begin(), groups[1].items.end(), Id::WhisperTurbo) !=
            groups[1].items.end());
    QVERIFY(std::find(groups[2].items.begin(), groups[2].items.end(), Id::HyMt2) !=
            groups[2].items.end());
}

void TestDependencyCatalog::githubUrls() {
    const QString repo = QStringLiteral("ggerganov/whisper.cpp");
    QCOMPARE(githubLatestReleaseUrl(repo).toString(),
             QStringLiteral("https://api.github.com/repos/ggerganov/whisper.cpp/releases/latest"));
    QCOMPARE(
        githubReleaseByTagUrl(repo, QStringLiteral("b5454")).toString(),
        QStringLiteral("https://api.github.com/repos/ggerganov/whisper.cpp/releases/tags/b5454"));
    QCOMPARE(
        githubReleasesListUrl(repo, 30).toString(),
        QStringLiteral("https://api.github.com/repos/ggerganov/whisper.cpp/releases?per_page=30"));
    QVERIFY(githubUserAgent().startsWith("oxbow/"));
    QCOMPARE(githubNightlyTag(QStringLiteral("5454")), QStringLiteral("b5454"));
    QVERIFY(githubNightlyTag(QString()).isEmpty());
}

void TestDependencyCatalog::githubParsing() {
    // whisper.cpp：latest 拿发布版本与 nightly 行 → b#### 那条 release 的资产
    const QJsonDocument whisperLatest = loadJson("github_whisper_latest.json");
    const QString whisperVersion =
        githubReleaseVersion(whisperLatest.object()[QLatin1String("tag_name")].toString());
    QCOMPARE(whisperVersion, QStringLiteral("1.9.5"));
    const QString buildNumber =
        githubNightlyBuildNumber(whisperLatest.object()[QLatin1String("body")].toString());
    QVERIFY(!buildNumber.isEmpty());
    QVERIFY(buildNumber == QStringLiteral("5454"));
    const QJsonDocument whisperNightly = loadJson("github_whisper_nightly.json");
    const GitHubAsset whisperAsset =
        githubPickAsset(whisperNightly, githubAssetPattern(component(Id::WhisperCpp)));
    QVERIFY(whisperAsset.isValid());
    QCOMPARE(whisperAsset.name, QStringLiteral("whisper-bin-x64.zip"));
    QVERIFY(whisperAsset.size > 0);
    QVERIFY(!whisperAsset.sha256.isEmpty());
    QVERIFY(whisperAsset.browserUrl.endsWith(QLatin1String("whisper-bin-x64.zip")));

    // llama.cpp：资产名带构建号，按 ^llama-b\d+-bin-win-cpu-x64\.zip$ 挑
    const QJsonDocument llamaLatest = loadJson("github_llama_latest.json");
    QCOMPARE(githubReleaseVersion(llamaLatest.object()[QLatin1String("tag_name")].toString()),
             QStringLiteral("0.8.0"));
    const QJsonDocument llamaNightly = loadJson("github_llama_nightly.json");
    const GitHubAsset llamaAsset =
        githubPickAsset(llamaNightly, githubAssetPattern(component(Id::LlamaCpp)));
    QVERIFY(QRegularExpression(QStringLiteral("^llama-b\\d+-bin-win-cpu-x64\\.zip$"))
                .match(llamaAsset.name)
                .hasMatch());
    QVERIFY(llamaAsset.size > 0);
    QVERIFY(!llamaAsset.sha256.isEmpty());

    // ffmpeg：tag 即版本，essentials_build.7z 静态包
    const QJsonDocument ffmpegLatest = loadJson("github_ffmpeg_latest.json");
    QCOMPARE(githubReleaseVersion(ffmpegLatest.object()[QLatin1String("tag_name")].toString()),
             QStringLiteral("9.0.2"));
    const GitHubAsset ffmpegAsset =
        githubPickAsset(ffmpegLatest, githubAssetPattern(component(Id::Ffmpeg)));
    QCOMPARE(ffmpegAsset.name, QStringLiteral("ffmpeg-9.0.2-essentials_build.7z"));
    QVERIFY(ffmpegAsset.size > 0);
    QVERIFY(!ffmpegAsset.sha256.isEmpty());

    // 资产正则不命中：返回空结果而不是抛错；无效正则同样不抛
    const GitHubAsset miss =
        githubPickAsset(ffmpegLatest, QRegularExpression(QStringLiteral("^no-such-asset$")));
    QVERIFY(!miss.isValid());
    QVERIFY(miss.size < 0);
    QVERIFY(!githubPickAsset(ffmpegLatest, githubAssetPattern(component(Id::MediaInfo))).isValid());
    // 资产在但缺 size：isValid 为真、结论仍按不可用（fromAsset 在作业侧判）
    const QJsonDocument noSize = QJsonDocument(QJsonObject{
        {QLatin1String("assets"),
         QJsonArray{QJsonObject{{QLatin1String("name"), QLatin1String("whisper-bin-x64.zip")}}}}});
    QVERIFY(githubPickAsset(noSize, githubAssetPattern(component(Id::WhisperCpp))).isValid());
    QVERIFY(githubPickAsset(noSize, githubAssetPattern(component(Id::WhisperCpp))).size < 0);
}

void TestDependencyCatalog::githubNightlyFallback() {
    // /releases 回退：最近一条带目标资产且 tag 是 b\d+ 的 release
    const char *releasesJson = R"([
        {"tag_name": "b100", "assets": [{"name": "other.zip", "size": 1}]},
        {"tag_name": "v1.2.3", "assets": [{"name": "whisper-bin-x64.zip", "size": 5}]},
        {"tag_name": "b99", "assets": [{"name": "whisper-bin-x64.zip", "size": 9}]}
    ])";
    const QJsonArray releases = QJsonDocument::fromJson(QByteArray(releasesJson)).array();
    const QRegularExpression pattern = githubAssetPattern(component(Id::WhisperCpp));
    QCOMPARE(githubFallbackReleaseIndex(releases, pattern), 2);
    // 全都不带目标资产 → -1
    const char *emptyJson = R"([{"tag_name": "b100", "assets": []}])";
    QCOMPARE(
        githubFallbackReleaseIndex(QJsonDocument::fromJson(QByteArray(emptyJson)).array(), pattern),
        -1);
    // 说明里没有 nightly 行 → 空（触发回退）
    QCOMPARE(githubNightlyBuildNumber(QStringLiteral("no nightly line here")), QString());
    QCOMPARE(githubNightlyBuildNumber(QStringLiteral("foo **Nightly build:** [b42](x) bar")),
             QStringLiteral("42"));
    // nightly 行在但后面没有构建号 → 空
    QCOMPARE(githubNightlyBuildNumber(QStringLiteral("**Nightly build:**")), QString());
}

void TestDependencyCatalog::mediaAreaParsing() {
    const QString html = loadText("mediaarea_windows_download.html");
    QCOMPARE(mediaAreaVersion(html), QStringLiteral("26.10"));
    const QUrl url = mediaAreaDownloadUrl(QStringLiteral("26.10"));
    QVERIFY(url.toString().contains(
        QStringLiteral("download/binary/libmediainfo0/26.10/MediaInfo_DLL_26.10_Windows_x64_"
                       "WithoutInstaller.7z")));
    // 页面缺包链接 → 空
    QVERIFY(mediaAreaVersion(QStringLiteral("<html>nothing</html>")).isEmpty());

    // 体积两条内联响应：HEAD 的 Content-Length 与 Range 的 Content-Range
    QHash<QByteArray, QByteArray> headers;
    headers.insert("content-length", "12345");
    QCOMPARE(headerContentLength(headers), qint64(12345));
    headers.remove("content-length");
    QCOMPARE(headerContentLength(headers), qint64(-1));
    headers.insert("content-range", "bytes 0-0/1474785120");
    QCOMPARE(headerContentRangeTotal(headers), qint64(1474785120));
    headers.remove("content-range");
    QCOMPARE(headerContentRangeTotal(headers), qint64(-1));
    headers.insert("content-range", "garbage");
    QCOMPARE(headerContentRangeTotal(headers), qint64(-1));
    // 两条都缺 → 不可用（版本拿得到也不行）
    const SourceQuery noSize = makeQuery(true, QStringLiteral("26.10"), -1);
    QVERIFY(!isAvailable(noSize, component(Id::MediaInfo)));
}

void TestDependencyCatalog::modelSourcesAndUrls() {
    // repo 与仓库内路径钉死在表上
    QCOMPARE(QLatin1String(component(Id::WhisperTurbo).modelRepo), "ggerganov/whisper.cpp");
    QCOMPARE(QLatin1String(component(Id::WhisperTurbo).modelPath), "ggml-large-v3-turbo.bin");
    QCOMPARE(QLatin1String(component(Id::WhisperVad).modelRepo), "ggml-org/whisper-vad");
    QCOMPARE(QLatin1String(component(Id::WhisperVad).modelPath), "ggml-silero-v6.2.0.bin");
    QCOMPARE(QLatin1String(component(Id::HyMt2).modelRepo), "tencent/Hy-MT2-1.8B-GGUF");
    QCOMPARE(QLatin1String(component(Id::HyMt2).modelPath), "Hy-MT2-1.8B-Q6_K.gguf");

    // host 链：两个 Whisper = HF → hf-mirror；Hy-MT2 = HF → ModelScope → hf-mirror；
    // 非模型无链
    const QVector<ModelHost> whisperChain = modelHostChain(component(Id::WhisperTurbo));
    QCOMPARE(whisperChain.size(), 2);
    QCOMPARE(whisperChain.at(0), ModelHost::HuggingFace);
    QCOMPARE(whisperChain.at(1), ModelHost::HfMirror);
    const QVector<ModelHost> hymt2Chain = modelHostChain(component(Id::HyMt2));
    QCOMPARE(hymt2Chain.size(), 3);
    QCOMPARE(hymt2Chain.at(1), ModelHost::ModelScope);
    QVERIFY(modelHostChain(component(Id::Ffmpeg)).isEmpty());
    QCOMPARE(modelHostName(ModelHost::ModelScope), QStringLiteral("www.modelscope.cn"));
    QCOMPARE(modelHostName(ModelHost::HfMirror), QStringLiteral("hf-mirror.com"));

    // URL 形态逐字断言
    QCOMPARE(modelQueryUrl(ModelHost::HuggingFace, component(Id::WhisperTurbo)).toString(),
             QStringLiteral("https://huggingface.co/api/models/ggerganov/whisper.cpp/tree/main?"
                            "expand=true"));
    QCOMPARE(modelQueryUrl(ModelHost::HfMirror, component(Id::WhisperTurbo)).toString(),
             QStringLiteral("https://hf-mirror.com/api/models/ggerganov/whisper.cpp/tree/main?"
                            "expand=true"));
    QCOMPARE(modelQueryUrl(ModelHost::ModelScope, component(Id::HyMt2)).toString(),
             QStringLiteral("https://www.modelscope.cn/api/v1/models/Tencent-Hunyuan/"
                            "Hy-MT2-1.8B-GGUF/repo/files?Revision=master&Root="));
    QCOMPARE(modelDownloadUrl(ModelHost::HuggingFace, component(Id::WhisperTurbo)).toString(),
             QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
                            "ggml-large-v3-turbo.bin"));
    QCOMPARE(modelDownloadUrl(ModelHost::HfMirror, component(Id::HyMt2)).toString(),
             QStringLiteral("https://hf-mirror.com/tencent/Hy-MT2-1.8B-GGUF/resolve/main/"
                            "Hy-MT2-1.8B-Q6_K.gguf"));
    QCOMPARE(modelDownloadUrl(ModelHost::ModelScope, component(Id::HyMt2)).toString(),
             QStringLiteral("https://www.modelscope.cn/models/Tencent-Hunyuan/Hy-MT2-1.8B-GGUF/"
                            "resolve/master/Hy-MT2-1.8B-Q6_K.gguf"));
}

void TestDependencyCatalog::hfTreeFixture() {
    const QJsonDocument doc = loadJson("hf_whisper_cpp_tree.json");
    qint64 size = -1;
    QByteArray oid;
    QVERIFY(hfTreeLookup(doc, QStringLiteral("ggml-large-v3-turbo.bin"), &size, &oid));
    QCOMPARE(size, qint64(1624555275));
    QCOMPARE(QString::fromLatin1(oid),
             QStringLiteral("1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69"));
    // 缺该 path 的条目 → false
    QVERIFY(!hfTreeLookup(doc, QStringLiteral("absent.bin"), &size, &oid));
    // 缺 lfs 指针：size 可用、oid 置空（只记日志不比对，不判不可用）
    QByteArray missingOid = QByteArrayLiteral("keep");
    qint64 missingSize = -1;
    QVERIFY(hfTreeLookup(
        QJsonDocument(QJsonArray{QJsonObject{{QLatin1String("path"), QLatin1String("f.bin")},
                                             {QLatin1String("size"), 5}}}),
        QStringLiteral("f.bin"), &missingSize, &missingOid));
    QCOMPARE(missingSize, qint64(5));
    QVERIFY(missingOid.isEmpty());
    // 非数组响应 → false
    QVERIFY(!hfTreeLookup(QJsonDocument(QJsonObject{}), QStringLiteral("f.bin"), &size, &oid));
}

void TestDependencyCatalog::modelscopeLookup() {
    const QJsonDocument doc = loadJson("modelscope_hymt2_files.json");
    qint64 size = -1;
    QByteArray sha256;
    QVERIFY(modelscopeFilesLookup(doc, QStringLiteral("Hy-MT2-1.8B-Q6_K.gguf"), &size, &sha256));
    QCOMPARE(size, qint64(1474785120));
    QCOMPARE(QString::fromLatin1(sha256),
             QStringLiteral("d98fe604dec1f28f58f80d7d560f7177e584d3b8e5835862687660e5ff97cb40"));
    // 缺文件 / 缺 Size → 空结果
    QVERIFY(!modelscopeFilesLookup(doc, QStringLiteral("absent.gguf"), &size, &sha256));
    const QJsonDocument noSize = QJsonDocument(QJsonObject{
        {QLatin1String("Data"),
         QJsonObject{{QLatin1String("Files"),
                      QJsonArray{QJsonObject{{QLatin1String("Path"), QLatin1String("f.bin")}}}}}}});
    QVERIFY(!modelscopeFilesLookup(noSize, QStringLiteral("f.bin"), &size, &sha256));
    // 缺 Sha256 只置空，不判失败
    const QJsonDocument noHash = QJsonDocument(QJsonObject{
        {QLatin1String("Data"),
         QJsonObject{{QLatin1String("Files"),
                      QJsonArray{QJsonObject{{QLatin1String("Path"), QLatin1String("f.bin")},
                                             {QLatin1String("Size"), 7}}}}}}});
    QVERIFY(modelscopeFilesLookup(noHash, QStringLiteral("f.bin"), &size, &sha256));
    QCOMPARE(size, qint64(7));
    QVERIFY(sha256.isEmpty());
    // host 分派：ModelScope 走 Files[]，两个 HF 走 tree；同一文件两来源哈希一致
    const QJsonDocument mirrorDoc = loadJson("hf_hymt2_tree.json");
    qint64 mirrorSize = -1;
    QByteArray mirrorOid;
    QVERIFY(
        modelLookup(ModelHost::HfMirror, mirrorDoc, component(Id::HyMt2), &mirrorSize, &mirrorOid));
    QCOMPARE(mirrorSize, qint64(1474785120));
    QCOMPARE(QString::fromLatin1(mirrorOid),
             QStringLiteral("d98fe604dec1f28f58f80d7d560f7177e584d3b8e5835862687660e5ff97cb40"));
    QVERIFY(!modelLookup(ModelHost::ModelScope, mirrorDoc, component(Id::HyMt2), &mirrorSize,
                         &mirrorOid));
}

void TestDependencyCatalog::versionCompare() {
    QCOMPARE(compareDottedVersions(QStringLiteral("9.0.10"), QStringLiteral("9.0.2")), 1);
    QCOMPARE(compareDottedVersions(QStringLiteral("26.10"), QStringLiteral("26.10")), 0);
    QCOMPARE(compareDottedVersions(QStringLiteral("1.9.5"), QStringLiteral("1.9.4")), 1);
    QCOMPARE(compareDottedVersions(QStringLiteral("9.0"), QStringLiteral("9.0.1")), -1);
    QCOMPARE(compareDottedVersions(QStringLiteral("0.8.0"), QStringLiteral("0.8.0")), 0);
}

void TestDependencyCatalog::probeParsing() {
    // 解析正则来自组件表
    QCOMPARE(parseProbeVersion(Id::Ffmpeg,
                               QStringLiteral("ffmpeg version 9.0.2-essentials_build-www.gyan.dev "
                                              "Copyright\nlibavutil x")),
             QStringLiteral("9.0.2"));
    QCOMPARE(parseProbeVersion(Id::Ffmpeg, QStringLiteral("ffmpeg version 7.1 full")),
             QStringLiteral("7.1"));
    QCOMPARE(parseProbeVersion(Id::WhisperCpp, QStringLiteral("whisper.cpp version: 1.9.5\nx")),
             QStringLiteral("1.9.5"));
    QCOMPARE(
        parseProbeVersion(Id::LlamaCpp, QStringLiteral("version: 0.8.0-dev (build 12032)\nmore")),
        QStringLiteral("0.8.0"));
    // 空输入、乱输出、缺字段 → 空
    QVERIFY(parseProbeVersion(Id::Ffmpeg, QString()).isEmpty());
    QVERIFY(parseProbeVersion(Id::Ffmpeg, QStringLiteral("garbage")).isEmpty());
    QVERIFY(parseProbeVersion(Id::WhisperCpp, QStringLiteral("version: 1.2.3")).isEmpty());
    // 不探测的两族恒空
    QVERIFY(parseProbeVersion(Id::HyMt2, QStringLiteral("whatever")).isEmpty());
    QVERIFY(parseProbeVersion(Id::MediaInfo, QStringLiteral("26.10.0.0")).isEmpty());
    // DLL 四段版本取前两段
    QCOMPARE(normalizeDllFileVersion(QStringLiteral("26.10.0.0")), QStringLiteral("26.10"));
    QVERIFY(normalizeDllFileVersion(QStringLiteral("garbage")).isEmpty());
    QVERIFY(normalizeDllFileVersion(QStringLiteral("1")).isEmpty());
}

void TestDependencyCatalog::columnText() {
    // 未落定：非模型两列各显示 …，模型最新版本恒 —
    RowFacts pending;
    QCOMPARE(latestVersionText(component(Id::Ffmpeg), pending), pendingCellText());
    QCOMPARE(downloadSizeText(pending), pendingCellText());
    QCOMPARE(latestVersionText(component(Id::HyMt2), pending), emptyCellText());
    QCOMPARE(installedVersionText(component(Id::HyMt2), pending), emptyCellText());

    // 模型已安装列：目标在即 Installed、不在即 —（没有版本）
    RowFacts modelRow;
    modelRow.querySettled = true;
    modelRow.probeSettled = true;
    modelRow.installed = true;
    modelRow.query = makeQuery(true, QString(), 1474785120);
    QCOMPARE(latestVersionText(component(Id::HyMt2), modelRow), emptyCellText());
    QCOMPARE(installedVersionText(component(Id::HyMt2), modelRow), QStringLiteral("Installed"));
    QCOMPARE(downloadSizeText(modelRow), QStringLiteral("1.37 GB"));
    modelRow.installed = false;
    modelRow.query = makeQuery(false, QString(), -1);
    QCOMPARE(installedVersionText(component(Id::HyMt2), modelRow), emptyCellText());
    QCOMPARE(downloadSizeText(modelRow), QStringLiteral("Failed"));

    // 非模型：版本查询失败 → Failed（tooltip 由 UI 拼）；探测不出 → —
    RowFacts failedVersion = settledRow(QString(), 35430500, QStringLiteral("7.1"), true);
    QCOMPARE(latestVersionText(component(Id::Ffmpeg), failedVersion), QStringLiteral("Failed"));
    RowFacts noProbe = settledRow(QStringLiteral("9.0.2"), 35430500, QString(), false);
    QCOMPARE(installedVersionText(component(Id::Ffmpeg), noProbe), emptyCellText());
    QCOMPARE(
        installedVersionText(component(Id::Ffmpeg), settledRow(QStringLiteral("9.0.2"), 35430500,
                                                               QStringLiteral("7.1"), true)),
        QStringLiteral("7.1"));
    // 大小拿不到 → Failed（大小是硬门槛，不存在显示 — 后继续）
    QCOMPARE(downloadSizeText(settledRow(QStringLiteral("9.0.2"), -1, QStringLiteral("7.1"), true)),
             QStringLiteral("Failed"));
    QVERIFY(!versionFailureTip(QStringLiteral("boom")).isEmpty());
}

void TestDependencyCatalog::sizeColumnText() {
    QCOMPARE(displayDownloadSize(1474785120), Util::formatFileSize(1474785120));
    QCOMPARE(displayDownloadSize(1474785120), QStringLiteral("1.37 GB"));
    QCOMPARE(displayDownloadSize(-1), QStringLiteral("\u2014"));
    QCOMPARE(displayDownloadSize(0), QStringLiteral("0 B"));
}

void TestDependencyCatalog::newerVersionAndStatus() {
    const Component &ffmpeg = component(Id::Ffmpeg);
    // 已安装且可用、最新严格大于已装 → 提示
    QVERIFY(hasNewerVersion(
        ffmpeg, settledRow(QStringLiteral("9.0.2"), 1000, QStringLiteral("7.1"), true)));
    // 同版本、已装读不出、未安装、不可用（缺大小）都不提示
    QVERIFY(!hasNewerVersion(
        ffmpeg, settledRow(QStringLiteral("9.0.2"), 1000, QStringLiteral("9.0.2"), true)));
    QVERIFY(!hasNewerVersion(ffmpeg, settledRow(QStringLiteral("9.0.2"), 1000, QString(), true)));
    QVERIFY(!hasNewerVersion(
        ffmpeg, settledRow(QStringLiteral("9.0.2"), 1000, QStringLiteral("7.1"), false)));
    QVERIFY(!hasNewerVersion(ffmpeg,
                             settledRow(QStringLiteral("9.0.2"), -1, QStringLiteral("7.1"), true)));
    // 模型没有版本，永不参与
    QVERIFY(!hasNewerVersion(component(Id::HyMt2), settledRow(QString(), 1000, QString(), true)));

    // 状态行三态：任一未落定即 Checking…
    QVector<RowFacts> rows(kComponentCount);
    for (int i = 0; i < kComponentCount; ++i)
        rows[i] = settledRow(QStringLiteral("1.0"), 1000, QStringLiteral("1.0"), true);
    QCOMPARE(statusText(rows), QString());
    rows[static_cast<int>(Id::Ffmpeg)].probeVersion = QStringLiteral("0.9");
    QCOMPARE(statusText(rows), QStringLiteral("New versions available"));
    rows[static_cast<int>(Id::LlamaCpp)].querySettled = false;
    QCOMPARE(statusText(rows), QStringLiteral("Checking\u2026"));
    // 全部落定且模型只认 Installed 也算落定（探测侧出生即真）
    rows[static_cast<int>(Id::LlamaCpp)].querySettled = true;
    QVERIFY(allRowsSettled(rows));
}

void TestDependencyCatalog::planGeneration() {
    QVector<RowFacts> rows(kComponentCount);
    for (int i = 0; i < kComponentCount; ++i)
        rows[i] = settledRow(QStringLiteral("9.0.2"), 1000, QStringLiteral("7.1"), true);
    const auto stateOf = [](Id id) {
        return id == Id::Ffmpeg
                   ? Qt::PartiallyChecked // Keep：不进计划
                   : (id == Id::WhisperCpp
                          ? Qt::Checked
                          : (id == Id::MediaInfo ? Qt::Unchecked : Qt::PartiallyChecked));
    };
    const QVector<PlanEntry> plan = executablePlan(stateOf, rows);
    // 只有 ffmpeg(Keep) 与 mediainfo(Remove)、whisper-cpp(Reinstall) 落进计划
    QCOMPARE(plan.size(), 2);
    // 计划按 UI 序：mediainfo(0) 的删除排在 whisper-cpp(2) 的重装之前
    QCOMPARE(plan.at(0).id, Id::MediaInfo);
    QCOMPARE(plan.at(0).action, Action::Remove);
    QCOMPARE(plan.at(1).id, Id::WhisperCpp);
    QCOMPARE(plan.at(1).action, Action::Reinstall);
    QVERIFY(planChangesFiles(plan));

    // 全是 Keep / 全是未装未勾 → 计划空；只勾了不可用项 → 计划非空但不改文件
    QVector<RowFacts> untouched(kComponentCount);
    for (int i = 0; i < kComponentCount; ++i)
        untouched[i].installed = false;
    QVERIFY(executablePlan([](Id) { return Qt::Unchecked; }, untouched).isEmpty());
    QVERIFY(!planChangesFiles(QVector<PlanEntry>{}));
    QVERIFY(!planChangesFiles(QVector<PlanEntry>{{Id::Ffmpeg, Action::Keep}}));
    QVector<RowFacts> unavailable = rows;
    for (RowFacts &row : unavailable)
        row.query.downloadBytes = -1; // 拿不到大小 → 不可用
    const QVector<PlanEntry> skipOnly = executablePlan(
        [](Id id) { return id == Id::Ffmpeg ? Qt::Checked : Qt::PartiallyChecked; }, unavailable);
    QCOMPARE(skipOnly.size(), 1);
    QCOMPARE(skipOnly.at(0).action, Action::Skip);
    QVERIFY(!planChangesFiles(skipOnly));
}

void TestDependencyCatalog::checkStateCycle() {
    // 已装项：方块 → 对钩 → 空 → 方块
    QCOMPARE(nextCheckState(Qt::PartiallyChecked, true, true), Qt::Checked);
    QCOMPARE(nextCheckState(Qt::Checked, true, true), Qt::Unchecked);
    QCOMPARE(nextCheckState(Qt::Unchecked, true, true), Qt::PartiallyChecked);
    // 未装项跳过方块：空 → 对钩 → 空
    QCOMPARE(nextCheckState(Qt::Unchecked, false, true), Qt::Checked);
    QCOMPARE(nextCheckState(Qt::Checked, false, true), Qt::Unchecked);
    // 可用与否不改变循环（不可用项仍可打对钩，意图制）
    QCOMPARE(nextCheckState(Qt::PartiallyChecked, true, false), Qt::Checked);
    QCOMPARE(nextCheckState(Qt::Checked, false, false), Qt::Unchecked);
}

void TestDependencyCatalog::planMatrix() {
    const auto act = [](Qt::CheckState state, bool installed, bool available) {
        return planAction(Id::Ffmpeg, state, installed, available).action;
    };
    QCOMPARE(act(Qt::Checked, false, true), Action::Install);
    QCOMPARE(act(Qt::Checked, true, true), Action::Reinstall);
    QCOMPARE(act(Qt::Checked, true, false), Action::Skip);
    QCOMPARE(act(Qt::Checked, false, false), Action::Skip);
    QCOMPARE(act(Qt::PartiallyChecked, true, true), Action::Keep);
    QCOMPARE(act(Qt::PartiallyChecked, true, false), Action::Keep);
    QCOMPARE(act(Qt::PartiallyChecked, false, true), Action::None);
    QCOMPARE(act(Qt::Unchecked, true, true), Action::Remove);
    QCOMPARE(act(Qt::Unchecked, true, false), Action::Remove);
    QCOMPARE(act(Qt::Unchecked, false, true), Action::None);
}

void TestDependencyCatalog::availability() {
    const Component &ffmpeg = component(Id::Ffmpeg);
    QVERIFY(isAvailable(makeQuery(true, QStringLiteral("9.0.2"), 35430500), ffmpeg));
    QVERIFY(!isAvailable(makeQuery(true, QStringLiteral("9.0.2"), -1), ffmpeg));
    QVERIFY(!isAvailable(makeQuery(true, QString(), 35430500), ffmpeg));
    QVERIFY(!isAvailable(makeQuery(false, QStringLiteral("9.0.2"), 35430500), ffmpeg));
    // 模型只要求 size 成功；lfs.oid / Sha256 缺失（空哈希）不判不可用
    const Component &hy = component(Id::HyMt2);
    QVERIFY(isAvailable(makeQuery(true, QString(), 1474785120), hy));
    QVERIFY(!isAvailable(makeQuery(true, QString(), -1), hy));
    QVERIFY(!isAvailable(makeQuery(false, QString(), 1474785120), hy));
    // RowFacts 入口与 SourceQuery 入口同一判据
    RowFacts row = settledRow(QStringLiteral("9.0.2"), 35430500, QString(), false);
    QVERIFY(isAvailable(ffmpeg, row));
    row.query.downloadBytes = -1;
    QVERIFY(!isAvailable(ffmpeg, row));
}

void TestDependencyCatalog::retryPolicy() {
    // 可重试两类
    QVERIFY(retryableDownloadFailure(DownloadFailure::Network));
    QVERIFY(retryableDownloadFailure(DownloadFailure::HttpServerError));
    // 不重试五类
    QVERIFY(!retryableDownloadFailure(DownloadFailure::LocalWrite));
    QVERIFY(!retryableDownloadFailure(DownloadFailure::HttpClientError));
    QVERIFY(!retryableDownloadFailure(DownloadFailure::Canceled));
    QVERIFY(!retryableDownloadFailure(DownloadFailure::HashMismatch));
    QVERIFY(!retryableDownloadFailure(DownloadFailure::SizeMismatch));
    // 退避表与额度常量
    const int *backoff = backoffSchedule();
    QCOMPARE(backoff[0], 2000);
    QCOMPARE(backoff[1], 4000);
    QCOMPARE(backoff[2], 8000);
    QCOMPARE(kDownloadRetryLimit, 3);
    // 零字节换 host：连接失败与 404 / 403；其余不换
    QVERIFY(zeroByteSwitchHost(DownloadFailure::Network, 0));
    QVERIFY(zeroByteSwitchHost(DownloadFailure::HttpClientError, 404));
    QVERIFY(zeroByteSwitchHost(DownloadFailure::HttpClientError, 403));
    QVERIFY(!zeroByteSwitchHost(DownloadFailure::HttpClientError, 409));
    QVERIFY(!zeroByteSwitchHost(DownloadFailure::HttpServerError, 500));
    QVERIFY(!zeroByteSwitchHost(DownloadFailure::SizeMismatch, 200));
}

void TestDependencyCatalog::retryBudget() {
    // 退避按次推进，额度用尽即判失败
    RetryBudget budget;
    auto verdict = budget.decide(DownloadFailure::Network, 0, 0, false);
    QCOMPARE(int(verdict.move), int(RetryBudget::Move::Retry));
    QCOMPARE(verdict.backoffMs, 2000);
    QCOMPARE(budget.attemptsUsed(), 1);
    QCOMPARE(budget.decide(DownloadFailure::Network, 0, 0, false).backoffMs, 4000);
    QCOMPARE(budget.decide(DownloadFailure::HttpServerError, 503, 1024, false).backoffMs, 8000);
    verdict = budget.decide(DownloadFailure::Network, 0, 1024, false);
    QCOMPARE(int(verdict.move), int(RetryBudget::Move::Fail));

    // 额度按项独立：两项各自拿满 3 次
    RetryBudget first, second;
    for (int i = 0; i < kDownloadRetryLimit; ++i)
        QCOMPARE(int(first.decide(DownloadFailure::Network, 0, 0, false).move),
                 int(RetryBudget::Move::Retry));
    QCOMPARE(int(first.decide(DownloadFailure::Network, 0, 0, false).move),
             int(RetryBudget::Move::Fail));
    QCOMPARE(int(second.decide(DownloadFailure::Network, 0, 0, false).move),
             int(RetryBudget::Move::Retry));

    // 不重试的失败与撤销直接判失败
    QCOMPARE(int(RetryBudget().decide(DownloadFailure::HashMismatch, 200, 4096, false).move),
             int(RetryBudget::Move::Fail));
    QCOMPARE(int(RetryBudget().decide(DownloadFailure::LocalWrite, 0, 4096, false).move),
             int(RetryBudget::Move::Fail));
    QCOMPARE(int(RetryBudget().decide(DownloadFailure::Canceled, 0, 0, true).move),
             int(RetryBudget::Move::Fail));

    // 零字节 + 还有下一家 → 换 host，不吃额度也不退避；已收字节则不换
    RetryBudget switcher;
    const auto switched = switcher.decide(DownloadFailure::HttpClientError, 404, 0, true);
    QCOMPARE(int(switched.move), int(RetryBudget::Move::SwitchHost));
    QCOMPARE(switched.backoffMs, 0);
    QCOMPARE(switcher.attemptsUsed(), 0);
    QCOMPARE(int(switcher.decide(DownloadFailure::HttpClientError, 403, 1, true).move),
             int(RetryBudget::Move::Fail)); // 4xx 收到字节：既不换家也不重试
    // 末家（没有下一位）零字节失败 → 同 host 重试
    QCOMPARE(int(switcher.decide(DownloadFailure::Network, 0, 0, false).move),
             int(RetryBudget::Move::Retry));
}

void TestDependencyCatalog::progressWeights() {
    const qint64 mb = kProgressStepWeightBytes;
    QCOMPARE(kProgressStepWeightBytes, qint64(1024 * 1024));
    QCOMPARE(stepCount({Id::WhisperTurbo, Action::Install}), 1); // 模型：只落位一步
    QCOMPARE(stepCount({Id::WhisperCpp, Action::Reinstall}), 2); // 压缩包：解压 + 落位
    QCOMPARE(stepCount({Id::LlamaCpp, Action::Remove}), 1);
    QCOMPARE(stepCount({Id::Ffmpeg, Action::Keep}), 0);
    const QVector<PlanEntry> plan = {
        {Id::WhisperTurbo, Action::Install}, // P + 1 步
        {Id::WhisperCpp, Action::Reinstall}, // P + 2 步
        {Id::LlamaCpp, Action::Remove},      // 1 步
        {Id::Ffmpeg, Action::Keep},          {Id::HyMt2, Action::Skip},
    };
    const auto bytes = [](Id id) { return id == Id::WhisperTurbo ? qint64(1000) : qint64(2000); };
    QCOMPARE(itemProgressWeight(plan.at(0), 1000), 1000 + mb);
    QCOMPARE(progressDenominator(plan, bytes), 1000 + 2000 + mb * 4);
    // 纯删除计划只剩步骤权重；未知字节按 0 计，分母不会为 0 也无需特判
    const QVector<PlanEntry> removalOnly = {{Id::LlamaCpp, Action::Remove}};
    QCOMPARE(progressDenominator(removalOnly, [](Id) { return qint64(-1); }), mb);
    const QVector<PlanEntry> single = {{Id::Ffmpeg, Action::Install}};
    QCOMPARE(progressDenominator(single, [](Id) { return qint64(-1); }), mb * 2);
}

void TestDependencyCatalog::diskEstimate() {
    // 模型 Install = P、Reinstall = 2P；压缩包 Install = 3P、Reinstall = 5P
    const QVector<PlanEntry> plan = {
        {Id::HyMt2, Action::Install},   {Id::WhisperTurbo, Action::Reinstall},
        {Id::Ffmpeg, Action::Install},  {Id::WhisperCpp, Action::Reinstall},
        {Id::LlamaCpp, Action::Remove}, {Id::MediaInfo, Action::Skip},
        {Id::WhisperVad, Action::Keep},
    };
    const qint64 peak = planPeakDiskBytes(plan, [](Id id) {
        return id == Id::HyMt2 || id == Id::WhisperTurbo ? qint64(100) : qint64(10);
    });
    QCOMPARE(peak, qint64(100 + 200 + 30 + 50));
    // 未知大小（不可用项）不参与求和
    QCOMPARE(planPeakDiskBytes({{Id::Ffmpeg, Action::Install}}, [](Id) { return qint64(-1); }),
             qint64(0));
}

void TestDependencyCatalog::confirmationOnlyInstalls() {
    const QVector<PlanEntry> plan = {{Id::Ffmpeg, Action::Install}};
    const auto name = [](Id id) { return QString::fromLatin1(component(id).name); };
    const auto size = [](Id) { return QStringLiteral("33.8 MB"); };
    const QString text = confirmationText(plan, name, size);
    const QString expected = QStringLiteral("Will be installed:\n"
                                            "FFmpeg 33.8 MB\n"
                                            "\n"
                                            "Continue?");
    QCOMPARE(text, expected);
}

void TestDependencyCatalog::confirmationOnlyRemovals() {
    const QVector<PlanEntry> plan = {{Id::LlamaCpp, Action::Remove},
                                     {Id::WhisperCpp, Action::Remove}};
    const auto name = [](Id id) { return QString::fromLatin1(component(id).name); };
    const auto size = [](Id) { return QString(); };
    const QString text = confirmationText(plan, name, size);
    // 组内按最长名补空格（llama.cpp 补到 11 列）；按计划序逐字断言
    const QString expected = QStringLiteral("Will be removed:\n"
                                            "llama.cpp  \n"
                                            "whisper.cpp\n"
                                            "\n"
                                            "Continue?");
    QCOMPARE(text, expected);
}

void TestDependencyCatalog::confirmationBothGroups() {
    const QVector<PlanEntry> plan = {{Id::LlamaCpp, Action::Remove},
                                     {Id::Ffmpeg, Action::Install},
                                     {Id::WhisperCpp, Action::Reinstall}};
    const auto name = [](Id id) { return QString::fromLatin1(component(id).name); };
    const auto size = [](Id id) {
        return id == Id::Ffmpeg ? QStringLiteral("33.8 MB") : QStringLiteral("8.5 MB");
    };
    const QString text = confirmationText(plan, name, size);
    const QString expected = QStringLiteral("Will be removed:\n"
                                            "llama.cpp\n"
                                            "\n"
                                            "Will be installed:\n"
                                            "FFmpeg      33.8 MB\n"
                                            "whisper.cpp  8.5 MB\n"
                                            "\n"
                                            "Continue?");
    QCOMPARE(text, expected);
}

void TestDependencyCatalog::confirmationOnlySkips() {
    const QVector<PlanEntry> plan = {{Id::HyMt2, Action::Skip}};
    const auto name = [](Id id) { return QString::fromLatin1(component(id).name); };
    const auto size = [](Id) { return QString(); };
    const QString text = confirmationText(plan, name, size);
    const QString expected = QStringLiteral("Unavailable items that will be skipped:\n"
                                            "Hy-MT2 1.8B\n"
                                            "\n"
                                            "Continue?");
    QCOMPARE(text, expected);
}

void TestDependencyCatalog::confirmationSkipAndInstallMixed() {
    const QVector<PlanEntry> plan = {{Id::Ffmpeg, Action::Install}, {Id::HyMt2, Action::Skip}};
    const auto name = [](Id id) { return QString::fromLatin1(component(id).name); };
    const auto size = [](Id) { return QStringLiteral("1.37 GB"); };
    const QString text = confirmationText(plan, name, size);
    // 跳过组固定在安装组之后、问句之前；跳过项只列名字
    const QString expected = QStringLiteral("Will be installed:\n"
                                            "FFmpeg 1.37 GB\n"
                                            "\n"
                                            "Unavailable items that will be skipped:\n"
                                            "Hy-MT2 1.8B\n"
                                            "\n"
                                            "Continue?");
    QCOMPARE(text, expected);
}

QTEST_GUILESS_MAIN(TestDependencyCatalog)

#include "tst_DependencyCatalog.moc"

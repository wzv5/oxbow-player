#pragma once

#include "pch.h"

// N150：可选依赖安装器的目录层——组件定义是纯数据 + 纯函数。一个组件的全部知识面（来源
// 形态、仓库、资产名模式、探测参数与解析正则、解压形态、目标路径、模型 host 链、校验
// 方式）只在 kComponents 一处落地，取数一律读 component(id) 的字段。无状态、不碰文件系统
// 与网络，全部可离线单测（tst_DependencyCatalog）；碰文件的一律住 DependencySwap。
namespace DependencyCatalog {

// 组件 id；顺序即 UI 顺序（idAt 只对 [0, kComponentCount) 有意义，越界入参由调用方先挡）
enum class Id {
    MediaInfo,
    Ffmpeg,
    WhisperCpp,
    WhisperTurbo,
    WhisperVad,
    LlamaCpp,
    HyMt2,
};
inline constexpr int kComponentCount = 7;

Id idAt(int uiOrder);

// 来源形态：决定查询走哪条通路、有没有版本列、整包是否即目标文件
enum class SourceKind {
    GitHubRelease,        // latest release 的资产即安装包（ffmpeg）
    GitHubNightlyRelease, // latest 只给发布版本与 nightly 构建号，资产挂在 b#### 那条（whisper /
                          // llama）
    MediaAreaPage,        // 下载页抓版本，再按拼出的地址取包（MediaInfo）
    Model,                // 模型：没有版本，host 链逐个查，整包即目标文件
};

enum class Category {
    Basic,
    SpeechRecognition,
    Translation,
};

// —— 模型 host 链（查询与下载共用同一顺序）——
// None 是链尾哨兵：短链只写前几格，其余格子留默认值
enum class ModelHost { None, HuggingFace, ModelScope, HfMirror };

// 解压形态。None 即"整包就是目标文件"（模型族），落位侧只读这一根轴
enum class ExtractKind {
    None,       // 不解压：下载到的就是目标文件
    DirStrip,   // 包内子目录下全部、去一层前缀（whisper.cpp 的 Release/）
    DirRoot,    // 包内根下全部（llama.cpp）
    SingleFile, // 包内挑一个文件落到安装根（ffmpeg / MediaInfo）
};

struct ExtractSpec {
    ExtractKind kind = ExtractKind::None;
    const char *pattern = nullptr;       // SingleFile：包内条目通配（ffmpeg 是 */bin/ffmpeg.exe）
    const char *stripPrefix = nullptr;   // DirStrip：要去掉的目录前缀
    const char *requiredEntry = nullptr; // 落位前必须存在的条目（相对暂存目录）
};

// 现场探测形态
enum class ProbeKind {
    Process,    // 起进程，stdout 与 stderr 合流后按 versionPattern 取组 1
    DllVersion, // 不起进程：读文件版本资源，再取 major.minor
    None,       // 没有版本可探（模型族）
};

struct ProbeSpec {
    ProbeKind kind = ProbeKind::None;
    const char *arg = nullptr;            // Process：那一个探测参数（-version / --version）
    const char *versionPattern = nullptr; // Process：版本解析正则源串（捕获组 1）
};

// 组件表照 SettingsDialog 的 SettingItem 口径：每个组件都非写不可的七项不给默认值（漏写
// 即编译报错），其余按来源形态可省（指定初始化器按声明顺序写，省下的格子取默认值）
struct Component {
    Id id;
    const char *installId;                 // 备份名 / stage 与 extract 目录名（如 whisper-cpp）
    const char *name;                      // 产品名，不进翻译表
    Category category;                     // 所属分类（标题见 categoryGroups）
    const char *descriptionText;           // QT_TRANSLATE_NOOP 源串
    const char *installTarget;             // 相对插件目录；目录型即目录名
    SourceKind sourceKind;                 // 模型与否由此单点决定（见 isModelComponent）
    const char *installedMarker = nullptr; // "已安装"判据；空 = 同 installTarget（文件型）
    bool isDirectory = false;              // 目录型走整目录交换
    const char *githubRepo = nullptr;      // GitHub 两形态：owner/name
    const char *assetPattern = nullptr;    // GitHub：资产名正则源串
    ExtractSpec extract;
    ProbeSpec probe;
    const char *modelRepo = nullptr; // Model：仓库与仓库内文件路径
    const char *modelPath = nullptr;
    std::array<ModelHost, 3> hosts = {ModelHost::None, ModelHost::None, ModelHost::None};
};

const Component &component(Id id);

// "模型"这一族只有一个判据：来源形态
inline bool isModelComponent(const Component &comp) { return comp.sourceKind == SourceKind::Model; }

// 文件型的"已安装"判据就是目标本身
inline const char *installedMarkerOf(const Component &comp) {
    return comp.installedMarker ? comp.installedMarker : comp.installTarget;
}

// 类别根顺序即 UI 顺序；标题一处落地，成员按组件顺序排列
struct CategoryGroup {
    Category category;
    const char *title; // QT_TRANSLATE_NOOP 源串
    std::initializer_list<Id> items;
};
const CategoryGroup *categoryGroups(int &count);

// GitHub 资产挑选正则（表内空串返回无效正则，即永不命中）
QRegularExpression githubAssetPattern(const Component &comp);

// —— 三态勾选循环（可用与否不改变循环，进签名只为与计划矩阵同参）——
Qt::CheckState nextCheckState(Qt::CheckState current, bool installed, bool available);

// —— 计划矩阵（与执行共用）——
enum class Action { None, Install, Reinstall, Skip, Keep, Remove };

struct PlanEntry {
    Id id;
    Action action;
};

PlanEntry planAction(Id id, Qt::CheckState state, bool installed, bool available);

// —— 来源查询结果（作业层填、各纯函数消费）——
struct SourceQuery {
    bool ok = false;
    QString version; // 发布版本；模型恒空
    qint64 downloadBytes = -1;
    QByteArray sha256; // hex；空 = 来源无哈希（只记日志不比对）
    QUrl downloadUrl;
    QString error;      // 失败原因（英文源串），ok 时为空
    int hostIndex = -1; // 模型：命中的 host 在 host 链里的下标；非模型 -1
};

// 可用性：模型只要求 size；其余要求版本与 size 都成功
bool isAvailable(const SourceQuery &query, const Component &comp);

// —— 一行七项的事实（UI 侧唯一的行状态载体，三列文案与判据都从它算）——
struct RowFacts {
    bool querySettled = false; // 最新版本与下载大小两列各有结论
    bool probeSettled = false; // 已安装版本列有结论（模型由本地事实现算，出生即真）
    bool installed = false;    // 组件声明的目标存在
    QString probeVersion;      // 现场探测得到的版本；探测不出为空
    QString probeRaw;          // 探测的原始输出（探测不出时进 tooltip）
    SourceQuery query;
};

bool isAvailable(const Component &comp, const RowFacts &row);
bool rowSettled(const RowFacts &row);
// 全部七行三列各有结论（含本地探测）
bool allRowsSettled(const QVector<RowFacts> &rows);
// 已安装且可用、最新严格大于已装——只服务状态行与高亮，不驱动动作
bool hasNewerVersion(const Component &comp, const RowFacts &row);
// 状态行三态：未落定 Checking… / 有新版本 New versions available / 空
QString statusText(const QVector<RowFacts> &rows);

// 计划 = 逐行算动作后剔掉 None 与 Keep（Keep 不计入计划：什么都没改变即空计划）
QVector<PlanEntry> executablePlan(const std::function<Qt::CheckState(Id)> &checkState,
                                  const QVector<RowFacts> &rows);
// 计划里是否真有落位或删除动作（可写检查与磁盘预检的开关）
bool planChangesFiles(const QVector<PlanEntry> &plan);

// —— GitHub ——
QUrl githubLatestReleaseUrl(const QString &repo);
QUrl githubReleaseByTagUrl(const QString &repo, const QString &tag);
QUrl githubReleasesListUrl(const QString &repo, int perPage);
QByteArray githubUserAgent();
// 发布版本：tag_name 去引导 v（只在后接数字时）
QString githubReleaseVersion(const QString &tagName);
// 发布说明里 "Nightly build" 之后第一个构建号（纯数字）；没有该行返回空（走 /releases 回退）
QString githubNightlyBuildNumber(const QString &releaseBody);
// 构建号 → nightly release 的 tag（b####）
QString githubNightlyTag(const QString &buildNumber);

struct GitHubAsset {
    QString name;
    QString browserUrl; // browser_download_url
    qint64 size = -1;
    QByteArray sha256; // digest "sha256:HEX" 的 HEX 段

    bool isValid() const { return !name.isEmpty(); }
};

// 按 pattern 挑第一个命中的资产（size / digest 一并取出）；不命中返回空结果
GitHubAsset githubPickAsset(const QJsonDocument &release, const QRegularExpression &namePattern);

// /releases 回退：最近一条带目标资产且 tag 是 b\d+ 的 release 下标；没有返回 -1
int githubFallbackReleaseIndex(const QJsonArray &releases, const QRegularExpression &namePattern);

// —— mediaarea ——
QString mediaAreaVersion(const QString &downloadPageHtml);
QUrl mediaAreaDownloadUrl(const QString &version);
qint64 headerContentLength(const QHash<QByteArray, QByteArray> &headers);
qint64 headerContentRangeTotal(const QHash<QByteArray, QByteArray> &headers);

// —— 模型（HF tree / ModelScope files）——
QUrl hfTreeApiUrl(const QString &host, const QString &repo);
QUrl hfResolveUrl(const QString &host, const QString &repo, const QString &path);
QUrl modelscopeFilesApiUrl(const QString &repo);
QUrl modelscopeResolveUrl(const QString &repo, const QString &path);

// HF tree 响应（数组）里找 path：取 size 与 lfs.oid；lfs 缺失只 oid 置空不判失败
bool hfTreeLookup(const QJsonDocument &doc, const QString &path, qint64 *size, QByteArray *oid);
// ModelScope files 响应：Data.Files[] 找 Path；Sha256 缺失置空不判失败
bool modelscopeFilesLookup(const QJsonDocument &doc, const QString &path, qint64 *size,
                           QByteArray *sha256);

// 组件的 host 链（非模型空表）
QVector<ModelHost> modelHostChain(const Component &comp);
// host 名（本次向导会话的失败记录按它记）
QString modelHostName(ModelHost host);
// host 的查询 / 下载地址（ModelScope 走 files API 与 resolve/master）
QUrl modelQueryUrl(ModelHost host, const Component &comp);
QUrl modelDownloadUrl(ModelHost host, const Component &comp);
// 按 host 解析响应，取该文件的 size 与哈希（ModelScope 用 Sha256，其余用 lfs.oid）
bool modelLookup(ModelHost host, const QJsonDocument &doc, const Component &comp, qint64 *size,
                 QByteArray *sha256);

// —— 三列文案（状态列取值的唯一实现；模型没有版本，恒 —）——
QString pendingCellText(); // 查询进行中的单元格
QString emptyCellText();   // 空值占位
QString latestVersionText(const Component &comp, const RowFacts &row);
QString installedVersionText(const Component &comp, const RowFacts &row);
QString downloadSizeText(const RowFacts &row);
// 版本查询 / 大小查询失败的 tooltip（原因是作业给的英文串，UI 只包一层）
QString versionFailureTip(const QString &reason);
QString sizeFailureTip(const QString &reason);

// —— 版本比较与展示 ——
// 点分数字比较：段数不齐按 0 补齐（9.0 < 9.0.1）；负值 = a < b
int compareDottedVersions(const QString &a, const QString &b);
// 进程探测输出按表内正则取版本；解析不出返回空
QString parseProbeVersion(Id id, const QString &rawOutput);
// DLL 四段版本取前两段（26.10.0.0 → 26.10）
QString normalizeDllFileVersion(const QString &fourPartVersion);
// 下载大小：未知（负值）出 —，其余原样走 Util::formatFileSize
QString displayDownloadSize(qint64 bytes);

// —— 下载失败分类与重试策略 ——
enum class DownloadFailure {
    None,
    Network,         // 连接失败、超时、60 秒无字节
    HttpServerError, // 5xx 与 408 / 429
    LocalWrite,      // 磁盘满、无权限
    HttpClientError, // 其余 4xx（404 / 403 一类）
    Canceled,
    HashMismatch,
    SizeMismatch,
};

inline constexpr int kDownloadRetryLimit = 3; // 每项各自额度（合计 4 次尝试）
const int *backoffSchedule();                 // {2000, 4000, 8000} 毫秒

bool retryableDownloadFailure(DownloadFailure failure);
// 零字节换 host：连接失败 / 404 / 403 且未收到字节
bool zeroByteSwitchHost(DownloadFailure failure, int httpStatus);

// 单个下载项的重试裁定：额度按项独立，换 host 不吃额度也不退避
class RetryBudget {
  public:
    enum class Move { SwitchHost, Retry, Fail };
    struct Verdict {
        Move move;
        int backoffMs; // SwitchHost / Fail 为 0
    };
    // altHostAvailable = host 链上还有下一家（非模型恒 false）
    Verdict decide(DownloadFailure failure, int httpStatus, qint64 receivedBytes,
                   bool altHostAvailable);
    int attemptsUsed() const { return m_attempts; }
    int limit() const { return kDownloadRetryLimit; }

  private:
    int m_attempts = 0;
};

// —— 进度权重 ——
inline constexpr qint64 kProgressStepWeightBytes = 1024 * 1024;
// 压缩包两步（解压 + 落位）、模型与删除各一步
int stepCount(const PlanEntry &entry);
// 一项的全部权重（下载字节 + 步骤权重）；下载字节未知按 0 计
qint64 itemProgressWeight(const PlanEntry &entry, qint64 packageBytes);
// 分母 = 各项权重和
qint64 progressDenominator(const QVector<PlanEntry> &plan,
                           const std::function<qint64(Id)> &packageBytes);

// —— 磁盘峰值估算 ——
// Install：模型 = P、压缩包 = P + 2P（解压估算按 2P）；Reinstall：模型 = 2P、压缩包 = P + 4P；
// Keep / Skip / Remove / 不可用不计
qint64 planPeakDiskBytes(const QVector<PlanEntry> &plan,
                         const std::function<qint64(Id)> &packageBytes);

// —— 确认文本 ——
// 分组与对齐唯一实现：删除组 → 安装组（名 + 右对齐大小）→ 跳过组 → 问句；空组不出现。
// 入参计划已由 executablePlan 剔掉 Keep 与无动作
QString confirmationText(const QVector<PlanEntry> &plan, const std::function<QString(Id)> &name,
                         const std::function<QString(Id)> &sizeText);

} // namespace DependencyCatalog

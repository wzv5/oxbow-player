#pragma once

#include "pch.h"

// N146：AI 字幕的目录单点——ini 键名与缺省、模式取值、内置命令原文、语言清单与
// 长名→短名映射、默认提示词模板、节流／超时／退避常量、失败与复核原因的文案常量，
// 以及设置目录与流水线共用的纯函数。形状照 N99 的 ReporterSwitchCatalog：纯数据 +
// 纯函数，无状态、可单测；消费方只引本头，具体值都在 .cpp 单点，取数口 readSettings
// 也只认调用方递来的 QSettings。原因串不带句号（拼进提示模板用），独立提示句带句号。
namespace AiSubtitleCatalog {

// —— 模式取值（ini 数据串，不进翻译）——
inline constexpr const char *kModeBuiltin = "builtin";
inline constexpr const char *kModeCustom = "custom";

// —— ini 键名（设置目录声明与读写侧同引）——
inline constexpr const char *kAsrModeKey = "asrMode";
inline constexpr const char *kAsrCommandKey = "asrCommand";
inline constexpr const char *kTranslationModeKey = "translationMode";
inline constexpr const char *kTranslationApiEndpointKey = "translationApi";
inline constexpr const char *kTranslationApiKeyKey = "translationApiKey";
inline constexpr const char *kTranslationModelKey = "translationModel";
inline constexpr const char *kTranslationTargetLangKey = "translationTargetLang";
inline constexpr const char *kTranslationPromptKey = "translationPrompt";

// —— 常量 ——
inline constexpr int kMergeIntervalMs = 500;        // 临时 srt 落地 + reload 的合并节流窗口
inline constexpr int kRequestTimeoutMs = 60000;     // 每条翻译请求的总时限
inline constexpr int kServerReadyTimeoutMs = 90000; // 内置服务端认端口的上限
inline constexpr int kRateLimitMaxRetries = 5;      // 429 自动重试上限（每句）
inline constexpr int kRateLimitBackoffInitialMs = 2000;
inline constexpr int kRateLimitBackoffMaxMs = 30000;
inline constexpr int kRateLimitHeaderCapMs = 60000; // retry-after 整数秒的封顶

// —— 数据（内置命令原文与各项缺省）——
// 内置命令以"程序 + 实参"数组为权威形态（N149）：第 0 项是程序写法且带 `./`，与实参
// 里的 `./model/...` 同形（设置项给用户看的整条串照此显示），其余是实参。需要一整条
// 串的场合（设置项 fallback、ini 缺省值）用 builtinAsrCommandLine 拼接，不在消费点各
// 写一遍
// 目标语言在 .cpp 只有一张表（长名 + 短名）：清单顺序即 UI 顺序、首项亦即缺省，
// 短名只服务译文缓存文件名的语言后缀（2–3 字母主标签，可带 `-` 子标签）
const QStringList &builtinAsrCommand();
const QStringList &builtinServerCommand();
QString builtinAsrCommandLine(); // 内置 ASR 命令数组的单空格拼接
const QString &defaultTranslationPrompt();
const QString &defaultTargetLanguage(); // "Chinese"，清单首项亦即缺省
QStringList targetLanguages();          // 顺序即 UI 顺序
// 语言短名；映射缺失或该项短名留空都退长名原文（只是 mpv 认不出语言段、不自动拾取）。
// 提示词里的 {target_lang} 取长名，两者各司其职
QString shortLanguageName(const QString &longName);

// —— 命令行装配（拆与换两步；起进程归调用方）——
// Qt 的命令行拆分（QProcess::splitCommand），绝不经 shell
QStringList splitCommandLine(const QString &line);
// 实参级替换：逐实参把 token 子串换成 value（不许整串拼好后再替换）
QStringList expandArg(const QStringList &args, const QString &token, const QString &value);

// —— 存在性自检（N149：程序段统一走 Util::resolveExternalProgram，判据是
// "解析得到真实文件才可用"；模型串不补后缀也不进 PATH）——
// 内置命令的存在性自检取路径：第 0 项按三步解析，其余以 ./ 开头的实参按模型
// 相对路径按插件目录定形；程序解析不到即整表为空。返回绝对路径列表
QStringList selfCheckPaths(const QStringList &command, const QString &pluginDir,
                           const QString &iniDir);
bool allFilesExist(const QStringList &paths);
// 自定义档的自检前置：trim 后非空
bool isConfigured(const QString &value);

// —— 模式分派（ASR 命令行读值单点：内置取固定数组，自定义拆用户串）——
QStringList selectAsrCommand(const QString &mode, const QString &customCommand);

// —— 设置快照的读法单点（八项设置全写 oxbow.ini；"键 → 缺省"的配对只住这里）——
// 开档归调用方，本函数不碰路径；缺键落本模块缺省，已写值原样返回（不 trim、不校验
// 档位串，运行期判据归各消费方）
struct Settings {
    QString asrMode;
    QString asrCommand;
    QString translationMode;
    QString translationApi;
    QString translationApiKey;
    QString translationModel;
    QString translationTargetLang;
    QString translationPrompt;
};

Settings readSettings(QSettings &ini);

// —— 自检分派（判据分域各立：ASR 认解析得到的程序与插件目录下的模型，翻译认服务端
// 文件或远端端点，两域档位将来也不同步扩）——
// 内置档判内置命令解析出的程序与 `./` 实参都存在；ASR 自定义档判第 0 项解析得到
// 真实文件；翻译自定义档只判端点串非空（不探端口可用性）
bool asrAvailable(const QString &mode, const QString &customCommand, const QString &pluginDir,
                  const QString &iniDir);
bool translationAvailable(const QString &mode, const QString &customEndpoint,
                          const QString &pluginDir, const QString &iniDir);

// —— 请求侧归一 ——
// 端点归一：去空格 → 去尾部 / → 路径不以 /chat/completions 结尾则补上
QString normalizeEndpoint(const QString &raw);
// API Key 归一：trim；空 = 不带鉴权头（调用方判空）
QString normalizeApiKey(const QString &raw);
// 句子归一：trim；空串由调用方判空句（不发请求）
QString normalizedSentence(const QString &text);

// —— 翻译请求与回复 ——
// 提示词渲染：普通替换（禁 QString::arg），先 {target_lang} 后 {source_text}——
// 两趟替换下第二趟会扫到第一趟换入的内容，句文本最后换入才不被二次替换
QString renderPrompt(const QString &tmpl, const QString &targetLang, const QString &sourceText);
// 剥思考过程：成对 <think>…</think>（大小写不敏感、可跨行）整段删除；只剩开标签的
// 删到串尾；只剩闭标签的删到该闭标签为止
QString stripThinkingBlocks(QString text);
// 回复解析：choices[0].message.content 优先，缺失退 choices[0].text；两者皆无判
// 结构不符（false，out 不动）；取到的串剥思考过程再 trim
bool parseTranslationReply(const QByteArray &body, QString &translatedText);

// —— 限速退避（429 不算本句失败）——
// retry-after 头为整数秒：取该值封顶 kRateLimitHeaderCapMs；非整数或 HTTP-date
// 形态退回指数退避：kRateLimitBackoffInitialMs × 2^attemptIndex，封顶
// kRateLimitBackoffMaxMs。attemptIndex 从 0 计
int rateLimitWaitMs(const QString &retryAfterHeaderValue, int attemptIndex);

// —— 缓存基名（与 mpv sub-auto 的比对串同规）——
// 本地取 completeBaseName；oxbow:// 不 unwrap——mp_basename 对 URL 只按 / 切，
// 取最后一个 / 之后的编码段再按最后一个 . 剥扩展名（与 bstr_strip_ext 同规，
// %2F 不解码），与 mpv 的比对串逐字节一致
QString cacheBaseName(const QString &mediaPath);

// —— 失败与复核原因（QT_TRANSLATE_NOOP 常量 + translate 取值；不带句号）——
QString reasonAudioExtractionFailed(const QString &detail);
QString reasonFfmpegMissing();
QString reasonNoAudioTrack();
QString reasonAsrStartFailed(const QString &detail);
QString reasonAsrCommandInvalid(); // 命令行拆不出可执行程序（空串、坏档位取值一类）
QString reasonAsrFailed(const QString &detail);
QString reasonAsrNoOutput();
QString reasonServerStartFailed();
QString reasonServerExitedUnexpectedly();
QString reasonAuthenticationFailed();
QString reasonRateLimitRetriesExhausted();
QString reasonMalformedResponse();
QString reasonTranslationFailed(const QString &detail);
QString reasonNoPlayingItem();
QString reasonAudioExtractionNotSupported();

} // namespace AiSubtitleCatalog

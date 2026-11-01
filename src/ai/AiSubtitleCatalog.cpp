#include "pch.h"

#include "AiSubtitleCatalog.h"

#include "Util.h"

namespace AiSubtitleCatalog {

namespace {

// 常量式源文案（QT_TRANSLATE_NOOP 标注 + catalogText 取值，HttpClient 同款分工）：
// 用户可见的失败与复核原因在此单点收录；带 %1 的模板与独立串一律走常量，
// 取值函数见文件尾
constexpr const char *kAudioExtractionFailed =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "Audio extraction failed: %1");
constexpr const char *kFfmpegMissing =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "FFmpeg executable not found");
constexpr const char *kNoAudioTrack =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "No audio track available");
constexpr const char *kAsrStartFailed =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "Failed to start ASR: %1");
constexpr const char *kAsrCommandInvalid =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "ASR command line is invalid");
constexpr const char *kAsrFailed = QT_TRANSLATE_NOOP("AiSubtitleCatalog", "ASR failed: %1");
constexpr const char *kAsrNoOutput =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "ASR produced no subtitles");
constexpr const char *kServerStartFailed =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "Failed to start the local translation server");
constexpr const char *kServerExited =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "The local translation server exited unexpectedly");
constexpr const char *kAuthFailed =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "Translation authentication failed");
constexpr const char *kRateLimitExhausted =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "Translation rate limit retries exhausted");
constexpr const char *kMalformedResponse =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "Malformed translation response");
constexpr const char *kTranslationFailed =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "Translation failed: %1");
constexpr const char *kNoPlayingItem = QT_TRANSLATE_NOOP("AiSubtitleCatalog", "No playing item");
constexpr const char *kExtractionNotSupported =
    QT_TRANSLATE_NOOP("AiSubtitleCatalog", "This item does not support audio extraction");

QString catalogText(const char *source) {
    return QCoreApplication::translate("AiSubtitleCatalog", source);
}

// bstr_strip_ext 同规：按最后一个 '.' 截断（含首字符即 '.' 的退化形态，bstr 无豁免）
QString stripLastExtension(QString name) {
    const int dot = name.lastIndexOf(u'.');
    if (dot >= 0)
        name.truncate(dot);
    return name;
}

// 目标语言唯一的表本体：UI 清单与长名→短名映射同一条目一次写清。顺序即 UI 顺序、
// 首项亦即缺省；短名要落成 mp_guess_lang_from_filename 认得的形态（主标签 2–3 字母，
// `-` 后接子标签），留空即退长名
struct TargetLanguage {
    const char *longName;
    const char *shortName;
};

constexpr TargetLanguage kTargetLanguages[] = {
    {"Chinese", "zh"},
    {"English", "en"},
    {"French", "fr"},
    {"Portuguese", "pt"},
    {"Spanish", "es"},
    {"Japanese", "ja"},
    {"Turkish", "tr"},
    {"Russian", "ru"},
    {"Arabic", "ar"},
    {"Korean", "ko"},
    {"Thai", "th"},
    {"Italian", "it"},
    {"German", "de"},
    {"Vietnamese", "vi"},
    {"Malay", "ms"},
    {"Indonesian", "id"},
    {"Filipino", "fil"},
    {"Hindi", "hi"},
    {"TraditionalChinese", "zh-Hant"},
    {"Polish", "pl"},
    {"Czech", "cs"},
    {"Dutch", "nl"},
    {"Khmer", "km"},
    {"Burmese", "my"},
    {"Persian", "fa"},
    {"Gujarati", "gu"},
    {"Urdu", "ur"},
    {"Telugu", "te"},
    {"Marathi", "mr"},
    {"Hebrew", "he"},
    {"Bengali", "bn"},
    {"Tamil", "ta"},
    {"Ukrainian", "uk"},
    {"Tibetan", "bo"},
    {"Kazakh", "kk"},
    {"Mongolian", "mn"},
    {"Uyghur", "ug"},
    {"Cantonese", "yue"},
};

// 内置档的判据对象是内置命令解析定形后的那批文件（自定义串在这一档根本不参与运行）
bool builtinCommandFilesExist(const QStringList &command, const QString &pluginDir,
                              const QString &iniDir) {
    const QStringList paths = selfCheckPaths(command, pluginDir, iniDir);
    return !paths.isEmpty() && allFilesExist(paths);
}

} // namespace

// N149：数组即权威形态，程序是第 0 项；需要一整条串的场合由 builtinAsrCommandLine 拼接
const QStringList &builtinAsrCommand() {
    static const QStringList command = {
        QStringLiteral("./whisper/whisper-cli"),
        QStringLiteral("-m"),
        QStringLiteral("./model/ggml-large-v3-turbo.bin"),
        QStringLiteral("-l"),
        QStringLiteral("auto"),
        QStringLiteral("-sns"),
        QStringLiteral("--vad"),
        QStringLiteral("-vm"),
        QStringLiteral("./model/ggml-silero-v6.2.0.bin"),
        QStringLiteral("-f"),
        QStringLiteral("${wav_file}"),
    };
    return command;
}

const QStringList &builtinServerCommand() {
    static const QStringList command = {
        QStringLiteral("./llama.cpp/llama-server"),
        QStringLiteral("--model"),
        QStringLiteral("./model/Hy-MT2-1.8B-Q4_K_M.gguf"),
        QStringLiteral("--jinja"),
        QStringLiteral("-c"),
        QStringLiteral("8192"),
        QStringLiteral("-n"),
        QStringLiteral("1024"),
        QStringLiteral("--batch-size"),
        QStringLiteral("512"),
        QStringLiteral("--temperature"),
        QStringLiteral("0.7"),
        QStringLiteral("--top-p"),
        QStringLiteral("0.6"),
        QStringLiteral("--top-k"),
        QStringLiteral("20"),
        QStringLiteral("--repeat-penalty"),
        QStringLiteral("1.05"),
        QStringLiteral("--host"),
        QStringLiteral("127.0.0.1"),
        QStringLiteral("--port"),
        QStringLiteral("0"),
    };
    return command;
}

QString builtinAsrCommandLine() { return builtinAsrCommand().join(u' '); }

const QString &defaultTranslationPrompt() {
    static const QString prompt = QStringLiteral(
        "Translate the following text into {target_lang}. Note that you should"
        " **only output the translated result without any additional explanation**:\n\n"
        "{source_text}");
    return prompt;
}

const QString &defaultTargetLanguage() {
    static const QString language = QStringLiteral("Chinese");
    return language;
}

QStringList targetLanguages() {
    QStringList names;
    for (const TargetLanguage &language : kTargetLanguages)
        names << QString::fromLatin1(language.longName);
    return names;
}

QString shortLanguageName(const QString &longName) {
    // 只服务译文缓存文件名的语言后缀；提示词里的 {target_lang} 取长名（各司其职）。
    // 映射缺失或短名留空都退长名原文——该语言只是 mpv 认不出语言段、不自动拾取，
    // 不是错误
    for (const TargetLanguage &language : kTargetLanguages) {
        if (longName != QLatin1String(language.longName))
            continue;
        return *language.shortName ? QString::fromLatin1(language.shortName) : longName;
    }
    return longName;
}

QStringList splitCommandLine(const QString &line) { return QProcess::splitCommand(line); }

QStringList expandArg(const QStringList &args, const QString &token, const QString &value) {
    QStringList out;
    out.reserve(args.size());
    for (const QString &arg : args)
        out << QString(arg).replace(token, value);
    return out;
}

QStringList selfCheckPaths(const QStringList &command, const QString &pluginDir,
                           const QString &iniDir) {
    if (command.isEmpty())
        return {};
    // 程序段走通用解析（两目录 + PATH 三步，N149）；解析不到即整表为空、自检判不可用
    const QString program = Util::resolveExternalProgram(pluginDir, iniDir, command.first());
    if (program.isEmpty())
        return {};
    QStringList paths;
    paths << program;
    for (int i = 1; i < command.size(); ++i) {
        if (!command.at(i).startsWith(QStringLiteral("./")))
            continue;
        paths << QDir::cleanPath(QDir(pluginDir).filePath(command.at(i).mid(2)));
    }
    return paths;
}

bool allFilesExist(const QStringList &paths) {
    for (const QString &path : paths)
        if (!QFileInfo::exists(path))
            return false;
    return true;
}

bool isConfigured(const QString &value) { return !value.trimmed().isEmpty(); }

QStringList selectAsrCommand(const QString &mode, const QString &customCommand) {
    return mode == QLatin1String(kModeBuiltin) ? builtinAsrCommand()
                                               : splitCommandLine(customCommand);
}

Settings readSettings(QSettings &ini) {
    Settings settings;
    settings.asrMode =
        ini.value(QLatin1String(kAsrModeKey), QLatin1String(kModeBuiltin)).toString();
    settings.asrCommand =
        ini.value(QLatin1String(kAsrCommandKey), builtinAsrCommandLine()).toString();
    settings.translationMode =
        ini.value(QLatin1String(kTranslationModeKey), QLatin1String(kModeBuiltin)).toString();
    settings.translationApi = ini.value(QLatin1String(kTranslationApiEndpointKey)).toString();
    settings.translationApiKey = ini.value(QLatin1String(kTranslationApiKeyKey)).toString();
    settings.translationModel = ini.value(QLatin1String(kTranslationModelKey)).toString();
    settings.translationTargetLang =
        ini.value(QLatin1String(kTranslationTargetLangKey), defaultTargetLanguage()).toString();
    settings.translationPrompt =
        ini.value(QLatin1String(kTranslationPromptKey), defaultTranslationPrompt()).toString();
    return settings;
}

bool asrAvailable(const QString &mode, const QString &customCommand, const QString &pluginDir,
                  const QString &iniDir) {
    if (mode == QLatin1String(kModeBuiltin))
        return builtinCommandFilesExist(builtinAsrCommand(), pluginDir, iniDir);
    // 自定义档：非空前置之后判第 0 项解析得到真实文件（N149 从"非空即真"收紧）
    if (!isConfigured(customCommand))
        return false;
    const QStringList tokens = splitCommandLine(customCommand);
    if (tokens.isEmpty())
        return false;
    return !Util::resolveExternalProgram(pluginDir, iniDir, tokens.first()).isEmpty();
}

bool translationAvailable(const QString &mode, const QString &customEndpoint,
                          const QString &pluginDir, const QString &iniDir) {
    if (mode == QLatin1String(kModeBuiltin))
        return builtinCommandFilesExist(builtinServerCommand(), pluginDir, iniDir);
    return isConfigured(customEndpoint);
}

QString normalizeEndpoint(const QString &raw) {
    QString endpoint = raw.trimmed();
    while (endpoint.endsWith(u'/'))
        endpoint.chop(1);
    if (endpoint.isEmpty())
        return endpoint;
    if (endpoint.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive))
        return endpoint;
    return endpoint + QStringLiteral("/chat/completions");
}

QString normalizeApiKey(const QString &raw) { return raw.trimmed(); }

QString normalizedSentence(const QString &text) { return text.trimmed(); }

QString renderPrompt(const QString &tmpl, const QString &targetLang, const QString &sourceText) {
    // 两趟替换下第二趟会扫到第一趟换入的内容：语言先换、文本最后换入，句文本里
    // 若含 {target_lang} 字样才不会被二次替换
    QString out = tmpl;
    out.replace(QStringLiteral("{target_lang}"), targetLang);
    out.replace(QStringLiteral("{source_text}"), sourceText);
    return out;
}

QString stripThinkingBlocks(QString text) {
    static const QRegularExpression paired(QStringLiteral("<think>[\\s\\S]*?</think>"),
                                           QRegularExpression::CaseInsensitiveOption);
    text.remove(paired);
    // 只剩开标签（服务端被截断的常见形态）：从开标签起全删
    static const QRegularExpression openTag(QStringLiteral("<think>"),
                                            QRegularExpression::CaseInsensitiveOption);
    const int open = text.indexOf(openTag);
    if (open >= 0)
        text.truncate(open);
    // 只剩闭标签（服务端预填开标签的常见形态）：删到该闭标签为止
    static const QRegularExpression closeTag(QStringLiteral("</think>"),
                                             QRegularExpression::CaseInsensitiveOption);
    const int close = text.indexOf(closeTag);
    if (close >= 0)
        text.remove(0, close + closeTag.match(text).capturedLength());
    return text;
}

bool parseTranslationReply(const QByteArray &body, QString &translatedText) {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject())
        return false;
    const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty() || !choices.at(0).isObject())
        return false;
    const QJsonObject choice = choices.at(0).toObject();
    // content 优先；缺失（或非串）退 choices[0].text——少数非标准端点沿用旧版
    // completions 的回形
    QString text;
    const QJsonValue content =
        choice.value(QStringLiteral("message")).toObject().value(QStringLiteral("content"));
    if (content.isString()) {
        text = content.toString();
    } else {
        const QJsonValue legacy = choice.value(QStringLiteral("text"));
        if (!legacy.isString())
            return false;
        text = legacy.toString();
    }
    translatedText = stripThinkingBlocks(text).trimmed();
    return true;
}

int rateLimitWaitMs(const QString &retryAfterHeaderValue, int attemptIndex) {
    // 整数秒按该值封顶；非整数（含 HTTP-date）退回指数退避：初值起、翻倍、封顶
    bool ok = false;
    const qint64 seconds = retryAfterHeaderValue.trimmed().toLongLong(&ok);
    if (ok && seconds >= 0) {
        const qint64 ms = seconds * 1000;
        return static_cast<int>(qMin<qint64>(ms, kRateLimitHeaderCapMs));
    }
    qint64 ms = kRateLimitBackoffInitialMs;
    for (int i = 0; i < attemptIndex; ++i) {
        ms *= 2;
        if (ms >= kRateLimitBackoffMaxMs) {
            ms = kRateLimitBackoffMaxMs;
            break;
        }
    }
    return static_cast<int>(qMin<qint64>(ms, kRateLimitBackoffMaxMs));
}

QString cacheBaseName(const QString &mediaPath) {
    // 转接来源不能先 unwrap 再取基名：mp_basename 对 URL 只按 / 切（misc/path_utils.c），
    // 媒体侧拿到的是整段 percent-encode 的身份，须与 mpv 的比对串逐字节一致
    if (mediaPath.startsWith(QStringLiteral("oxbow://"))) {
        const int slash = mediaPath.lastIndexOf(u'/');
        QString segment = slash >= 0 ? mediaPath.mid(slash + 1) : mediaPath;
        return stripLastExtension(segment);
    }
    return QFileInfo(mediaPath).completeBaseName();
}

QString reasonAudioExtractionFailed(const QString &detail) {
    return catalogText(kAudioExtractionFailed).arg(detail);
}

QString reasonFfmpegMissing() { return catalogText(kFfmpegMissing); }

QString reasonNoAudioTrack() { return catalogText(kNoAudioTrack); }

QString reasonAsrStartFailed(const QString &detail) {
    return catalogText(kAsrStartFailed).arg(detail);
}

QString reasonAsrCommandInvalid() { return catalogText(kAsrCommandInvalid); }

QString reasonAsrFailed(const QString &detail) { return catalogText(kAsrFailed).arg(detail); }

QString reasonAsrNoOutput() { return catalogText(kAsrNoOutput); }

QString reasonServerStartFailed() { return catalogText(kServerStartFailed); }

QString reasonServerExitedUnexpectedly() { return catalogText(kServerExited); }

QString reasonAuthenticationFailed() { return catalogText(kAuthFailed); }

QString reasonRateLimitRetriesExhausted() { return catalogText(kRateLimitExhausted); }

QString reasonMalformedResponse() { return catalogText(kMalformedResponse); }

QString reasonTranslationFailed(const QString &detail) {
    return catalogText(kTranslationFailed).arg(detail);
}

QString reasonNoPlayingItem() { return catalogText(kNoPlayingItem); }

QString reasonAudioExtractionNotSupported() { return catalogText(kExtractionNotSupported); }

} // namespace AiSubtitleCatalog

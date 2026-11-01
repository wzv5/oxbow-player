#include "pch.h"

#include <QtTest/QtTest>

#include "ai/AiSubtitleCatalog.h"

#include <QProcess>
#include <QTemporaryDir>

#include "Util.h"

// N146：AI 字幕目录单点的契约测试——命令行拆分与占位符替换、模式分派、按域分派的
// 自检、八项设置的"键 → 缺省"读法、端点与 API Key 归一、限速退避、提示词渲染、
// 句子归一、回复解析（含剥思考过程）、缓存基名口径、语言清单与短名映射。
// 除 readSettings 走一份临时 ini 外全部是纯函数，不碰网络；N149 起唯一例外是
// customCommandBatchLaunch——它真实起飞一个临时 .bat 验证"bat 免 shell 直起"，
// 此外不碰进程。

using namespace AiSubtitleCatalog;

namespace {

// 造出给定清单上的文件（父目录一并建）：内置档自检判的就是这批路径。文件名带
// .exe 一形由 selfCheckPaths 自己给出，测试不另按平台拼
void createFiles(const QStringList &paths) {
    for (const QString &path : paths) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
}

} // namespace

class TestAiSubtitleCatalog : public QObject {
    Q_OBJECT

  private slots:
    void splitCommandLineAndExpandArgs();
    void builtinArraysShapeAndJoin();
    void selectAsrCommandByMode();
    void readSettingsPrefersWrittenValues();
    void selfCheckPathsResolveUnderPluginDir();
    void selfCheckRequiresConfiguredCustom();
    void selfCheckDispatchesByDomain();
    void customCommandBatchLaunch();
    void normalizeEndpointVariants();
    void normalizeApiKeyTrims();
    void rateLimitWaitMsPrefersHeader();
    void rateLimitWaitMsBackoffWithoutHeader();
    void renderPromptKeepsSourceLiteral();
    void normalizedSentenceTrims();
    void parseReplyPrefersContent();
    void parseReplyFallsBackToText();
    void parseReplyRejectsUnknownShape();
    void stripThinkingPaired();
    void stripThinkingOpenOnly();
    void stripThinkingCloseOnly();
    void cacheBaseNameLocal();
    void cacheBaseNameTransitKeepsTailVerbatim();
    void languageMappingCoversList();
};

void TestAiSubtitleCatalog::splitCommandLineAndExpandArgs() {
    // 引号包住带空格的路径：拆分按 Qt 规则，不经 shell
    const QStringList args =
        splitCommandLine(QStringLiteral("./funasr/llama-funasr-cli --enc ./model/a.gguf"
                                        " -a \"D:/my dir/clip.wav\" --srt"));
    QCOMPARE(args.size(), 6);
    QCOMPARE(args.at(0), QStringLiteral("./funasr/llama-funasr-cli"));
    QCOMPARE(args.at(3), QStringLiteral("-a"));
    QCOMPARE(args.at(4), QStringLiteral("D:/my dir/clip.wav"));

    // 占位符在实参级替换：整参与内嵌两态都换，其余实参不动
    const QStringList expanded =
        expandArg(args, QStringLiteral("${wav_file}"), QStringLiteral("D:/my dir/clip.wav"));
    QCOMPARE(expanded.at(4), QStringLiteral("D:/my dir/clip.wav"));
    QCOMPARE(expanded.at(1), QStringLiteral("--enc"));
    const QStringList embedded =
        expandArg(QStringList{QStringLiteral("--file=${wav_file}")}, QStringLiteral("${wav_file}"),
                  QStringLiteral("D:/a b.wav"));
    QCOMPARE(embedded.at(0), QStringLiteral("--file=D:/a b.wav"));
}

// N149：内置命令以数组为权威形态——第 0 项即程序写法且带 ./（与实参里的 ./model/...
// 同形，设置项给用户看的整条串照此），${wav_file} 独占一个实参位供实参级展开；
// 拼接出口与逐项 join 逐字节一致
void TestAiSubtitleCatalog::builtinArraysShapeAndJoin() {
    QCOMPARE(builtinAsrCommand().first(), QStringLiteral("./whisper/whisper-cli"));
    QCOMPARE(builtinServerCommand().first(), QStringLiteral("./llama.cpp/llama-server"));
    // 首段与模型实参同为 ./ 开头：UI 上"随包相对插件目录"一眼可辨
    QVERIFY(builtinAsrCommand().first().startsWith(QStringLiteral("./")));
    QVERIFY(builtinAsrCommand().contains(QStringLiteral("./model/ggml-large-v3-turbo.bin")));
    QVERIFY(builtinAsrCommand().last() == QStringLiteral("${wav_file}"));
    QCOMPARE(builtinAsrCommandLine(), builtinAsrCommand().join(u' '));
    // 拼接串回拆等于原数组（QProcess::splitCommand 对这批实参无歧义）
    QCOMPARE(splitCommandLine(builtinAsrCommandLine()), builtinAsrCommand());
}

void TestAiSubtitleCatalog::selectAsrCommandByMode() {
    // 内置忽略自定义串，取固定数组；自定义拆用户串（空串得空表，
    // "判不可用"由 isConfigured 与解析判据承担）
    QCOMPARE(selectAsrCommand(QStringLiteral("builtin"), QStringLiteral("whatever")),
             builtinAsrCommand());
    const QStringList customSplit{QStringLiteral("my"), QStringLiteral("tool"),
                                  QStringLiteral("--srt")};
    QCOMPARE(selectAsrCommand(QStringLiteral("custom"), QStringLiteral("my tool --srt")),
             customSplit);
    QVERIFY(selectAsrCommand(QStringLiteral("custom"), QString()).isEmpty());
}

void TestAiSubtitleCatalog::readSettingsPrefersWrittenValues() {
    // 八项的"键 → 缺省"读法单点：视图与流水线不再各写一遍 value(key, default)
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSettings ini(dir.path() + QStringLiteral("/oxbow.ini"), QSettings::IniFormat);
        ini.setValue(QLatin1String(kAsrModeKey), QStringLiteral("custom"));
        ini.setValue(QLatin1String(kTranslationApiEndpointKey), QStringLiteral("http://h:1/v1"));
        ini.sync();
    }
    QSettings ini(dir.path() + QStringLiteral("/oxbow.ini"), QSettings::IniFormat);
    const Settings values = readSettings(ini);
    // 已写值原样透出（不 trim、不校验档位串——运行期判据归各消费方）
    QCOMPARE(values.asrMode, QStringLiteral("custom"));
    QCOMPARE(values.translationApi, QStringLiteral("http://h:1/v1"));
    // 缺键落本模块缺省，与设置目录各项的 fallback 同一批值
    QCOMPARE(values.asrCommand, builtinAsrCommandLine());
    QCOMPARE(values.translationMode, QLatin1String(kModeBuiltin));
    QCOMPARE(values.translationTargetLang, defaultTargetLanguage());
    QCOMPARE(values.translationPrompt, defaultTranslationPrompt());
    QVERIFY(values.translationApiKey.isEmpty());
    QVERIFY(values.translationModel.isEmpty());
}

void TestAiSubtitleCatalog::selfCheckPathsResolveUnderPluginDir() {
#ifdef Q_OS_WIN
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pluginDir = dir.path();
    // 内置形：程序段走三步解析（文件须真实存在，先造程序再取表），./ 开头的模型实参
    // 按相对路径定形（不补后缀、不进 PATH），${wav_file} 与旗标不进清单
    const QStringList command =
        splitCommandLine(QStringLiteral("./funasr/llama-funasr-cli --enc ./model/enc.gguf"
                                        " -m ./model/llm.gguf --vad ./model/vad.gguf"
                                        " -a ${wav_file} --srt"));
    createFiles({pluginDir + QStringLiteral("/funasr/llama-funasr-cli.exe")});
    const QStringList paths = selfCheckPaths(command, pluginDir, QString());
    QCOMPARE(paths.size(), 4);
    QVERIFY(paths.at(0).endsWith(QStringLiteral("funasr/llama-funasr-cli.exe")));
    QVERIFY(paths.at(1).endsWith(QStringLiteral("model/enc.gguf")));
    QVERIFY(paths.at(2).endsWith(QStringLiteral("model/llm.gguf")));
    QVERIFY(paths.at(3).endsWith(QStringLiteral("model/vad.gguf")));

    // 存在性：模型补齐前判不可用，补齐后判可用
    QVERIFY(!allFilesExist(paths));
    createFiles(paths);
    QVERIFY(allFilesExist(paths));

    // 程序解析不到（文件撤掉）即整表为空、自检判不可用（N149）
    QVERIFY(QFile::remove(paths.at(0)));
    QVERIFY(selfCheckPaths(command, pluginDir, QString()).isEmpty());
#else
    QSKIP("程序解析依赖后缀枚举语义，只在 Windows 侧成立");
#endif
}

void TestAiSubtitleCatalog::selfCheckRequiresConfiguredCustom() {
    // 自定义档的前置：trim 后非空（其后 N149 起还要过解析判据，见 selfCheckDispatchesByDomain）
    QVERIFY(!isConfigured(QString()));
    QVERIFY(!isConfigured(QStringLiteral("   ")));
    QVERIFY(isConfigured(QStringLiteral("tool")));
    QVERIFY(isConfigured(QStringLiteral(" ./tool ")));
}

void TestAiSubtitleCatalog::selfCheckDispatchesByDomain() {
    // 判据按域各立一条出口（ASR 认解析得到的程序与插件目录下的文件，翻译认服务端
    // 文件或端点）：两档模式并非同一件事，将来各自的档位不同步扩也不互相牵扯
#ifdef Q_OS_WIN
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pluginDir = dir.path();
    const QString iniDir = QString(); // 本套自检不吃 ini 目录，传空串照走
    const QString builtin = QLatin1String(kModeBuiltin);
    const QString custom = QLatin1String(kModeCustom);

    // 空目录：两域的内置档都判不可用（程序解析不到即整表为空）
    QVERIFY(!asrAvailable(builtin, QStringLiteral("whatever"), pluginDir, iniDir));
    QVERIFY(!translationAvailable(builtin, QStringLiteral("http://h:1/v1"), pluginDir, iniDir));

    // ASR 自定义档从"非空即真"改为"解析不到即假"：首段指向不存在的文件判不可用
    QVERIFY(!asrAvailable(custom, QStringLiteral("mytool -f ${wav_file}"), pluginDir, iniDir));

    // 内置档不吃用户那一串：先造程序文件（解析前提），模型补齐前判不可用
    createFiles({pluginDir + QStringLiteral("/whisper/whisper-cli.exe")});
    QVERIFY(!asrAvailable(builtin, QString(), pluginDir, iniDir)); // 模型缺
    const QStringList asrPaths = selfCheckPaths(builtinAsrCommand(), pluginDir, iniDir);
    QCOMPARE(asrPaths.size(), 3); // 程序 + 两个模型（${wav_file} 不进表）
    createFiles(asrPaths);
    QVERIFY(asrAvailable(builtin, QStringLiteral("whatever"), pluginDir, iniDir));

    // 自定义档补上真实程序后转可用（只判程序，不查模型）
    createFiles({pluginDir + QStringLiteral("/mytool.exe")});
    QVERIFY(asrAvailable(custom, QStringLiteral("mytool -f ${wav_file}"), pluginDir, iniDir));
    // 带子目录写法与只摆目录根（第 ② 步）同样判可用
    createFiles({pluginDir + QStringLiteral("/tools/nested.exe")});
    QVERIFY(asrAvailable(custom, QStringLiteral("tools/nested"), pluginDir, iniDir));
    QVERIFY(asrAvailable(custom, QStringLiteral("./tools/nested"), pluginDir, iniDir));

    // 补齐服务端与模型：翻译内置档随之转可用
    createFiles({pluginDir + QStringLiteral("/llama.cpp/llama-server.exe")});
    QVERIFY(!translationAvailable(builtin, QStringLiteral("http://h:1/v1"), pluginDir, iniDir));
    const QStringList serverPaths = selfCheckPaths(builtinServerCommand(), pluginDir, iniDir);
    QCOMPARE(serverPaths.size(), 2); // 服务端 + 模型
    createFiles(serverPaths);
    QVERIFY(translationAvailable(builtin, QString(), pluginDir, iniDir));

    // 翻译自定义档只判端点串非空：不解析程序、不探端口
    QVERIFY(!translationAvailable(custom, QString(), pluginDir, iniDir));
    QVERIFY(translationAvailable(custom, QStringLiteral("http://h:1/v1"), pluginDir, iniDir));
#else
    QSKIP("程序解析依赖后缀枚举语义，只在 Windows 侧成立");
#endif
}

// N149：自定义命令指向临时 .bat——解析命中 .bat 后 QProcess 免 shell 直起、标准输出
// 可取回，并钉住"带空格路径的实参原样送达"（§2.5 唯一承诺的实参语义，其余特殊字符
// 由 cmd 二次展开、不承诺）。本套件唯一的真实起进程用例（文件头例外声明同源）
void TestAiSubtitleCatalog::customCommandBatchLaunch() {
#ifdef Q_OS_WIN
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pluginDir = dir.path();
    const QString bat = pluginDir + QStringLiteral("/marker.bat");
    {
        QFile file(bat);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        // %~ 形式剥掉 cmd 加的外层引号，回显的即是收到的实参原串
        QVERIFY(file.write("@echo off\r\necho [%~1][%~2]\r\n") > 0);
    }

    const QStringList command =
        selectAsrCommand(QStringLiteral("custom"), QStringLiteral("marker.bat -f ${wav_file}"));
    QCOMPARE(command.first(), QStringLiteral("marker.bat"));
    const QString program = Util::resolveExternalProgram(pluginDir, QString(), command.first());
    QVERIFY(QFileInfo(program).isFile());
    QCOMPARE(QDir::cleanPath(program), QDir::cleanPath(bat));

    // wav 落在带空格的子目录名里：实参级展开后整段仍是一个实参
    const QString wav = dir.filePath(QStringLiteral("my dir/clip.wav"));
    const QStringList args = expandArg(command.mid(1), QStringLiteral("${wav_file}"), wav);
    const QStringList wantArgs{QStringLiteral("-f"), wav};
    QCOMPARE(args, wantArgs);

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, args);
    QVERIFY(process.waitForStarted(3000));
    QVERIFY(process.waitForFinished(3000));
    QCOMPARE(process.exitCode(), 0);
    const QString echoed = QString::fromLocal8Bit(process.readAll()).trimmed();
    const QString wantEcho = QStringLiteral("[%1][%2]").arg(wantArgs.at(0), wantArgs.at(1));
    QVERIFY2(echoed == wantEcho, qPrintable(echoed));
#else
    QSKIP("bat 直起语义只在 Windows 侧成立");
#endif
}

void TestAiSubtitleCatalog::normalizeEndpointVariants() {
    QCOMPARE(normalizeEndpoint(QStringLiteral(" http://127.0.0.1:8080/v1/ ")),
             QStringLiteral("http://127.0.0.1:8080/v1/chat/completions"));
    QCOMPARE(normalizeEndpoint(QStringLiteral("http://h:1/v1/chat/completions")),
             QStringLiteral("http://h:1/v1/chat/completions"));
    QCOMPARE(normalizeEndpoint(QStringLiteral("http://h:1")),
             QStringLiteral("http://h:1/chat/completions"));
    QCOMPARE(normalizeEndpoint(QStringLiteral("http://h:1/")),
             QStringLiteral("http://h:1/chat/completions"));
    QCOMPARE(normalizeEndpoint(QString()), QString());
}

void TestAiSubtitleCatalog::normalizeApiKeyTrims() {
    QCOMPARE(normalizeApiKey(QStringLiteral("  sk-abc ")), QStringLiteral("sk-abc"));
    QCOMPARE(normalizeApiKey(QStringLiteral("   ")), QString());
    // 空 = 不带鉴权头（调用方判空）
}

void TestAiSubtitleCatalog::rateLimitWaitMsPrefersHeader() {
    // 整数秒按该值；封顶 60s；0 是合法值（立即重试）
    QCOMPARE(rateLimitWaitMs(QStringLiteral("5"), 0), 5000);
    QCOMPARE(rateLimitWaitMs(QStringLiteral(" 30 "), 2), 30000);
    QCOMPARE(rateLimitWaitMs(QStringLiteral("120"), 0), 60000);
    QCOMPARE(rateLimitWaitMs(QStringLiteral("0"), 3), 0);
}

void TestAiSubtitleCatalog::rateLimitWaitMsBackoffWithoutHeader() {
    // 非整数（含 HTTP-date 与负数）退回指数退避：2s 起、翻倍、单次封顶 30s
    QCOMPARE(rateLimitWaitMs(QString(), 0), 2000);
    QCOMPARE(rateLimitWaitMs(QString(), 1), 4000);
    QCOMPARE(rateLimitWaitMs(QString(), 2), 8000);
    QCOMPARE(rateLimitWaitMs(QString(), 3), 16000);
    QCOMPARE(rateLimitWaitMs(QString(), 4), 30000); // 32000 封顶
    QCOMPARE(rateLimitWaitMs(QString(), 10), 30000);
    QCOMPARE(rateLimitWaitMs(QStringLiteral("Wed, 21 Oct 2015 07:28:00 GMT"), 0), 2000);
    QCOMPARE(rateLimitWaitMs(QStringLiteral("-5"), 1), 4000);
}

void TestAiSubtitleCatalog::renderPromptKeepsSourceLiteral() {
    // %1 与花括号不是占位符，原样保留；句文本携带的 {target_lang} 不被二次替换——
    // 两趟替换下文本最后换入（冻结顺序句笔误的更正见规格变更记录）
    const QString tmpl = defaultTranslationPrompt();
    const QString rendered = renderPrompt(tmpl, QStringLiteral("Chinese"),
                                          QStringLiteral("say %1 and {target_lang} ok"));
    QVERIFY(rendered.contains(QStringLiteral("into Chinese.")));
    QVERIFY(rendered.contains(QStringLiteral("say %1 and {target_lang} ok")));
    QVERIFY(!rendered.contains(QStringLiteral("{source_text}")));
    QVERIFY(!rendered.contains(QStringLiteral("{target_lang}. Note")));
    // 常规形态：模板占位符都被换掉
    const QString plain = renderPrompt(QStringLiteral("T:{target_lang}|S:{source_text}"),
                                       QStringLiteral("French"), QStringLiteral("hi"));
    QCOMPARE(plain, QStringLiteral("T:French|S:hi"));
}

void TestAiSubtitleCatalog::normalizedSentenceTrims() {
    QCOMPARE(normalizedSentence(QStringLiteral("  hello world ")), QStringLiteral("hello world"));
    QVERIFY(normalizedSentence(QStringLiteral(" \t ")).isEmpty()); // 纯空白判空句
}

void TestAiSubtitleCatalog::parseReplyPrefersContent() {
    const QByteArray body =
        R"({"choices":[{"message":{"role":"assistant","content":"  <think>reason</think>  Bonjour "}}]})";
    QString out;
    QVERIFY(parseTranslationReply(body, out));
    QCOMPARE(out, QStringLiteral("Bonjour")); // 剥思考过程 + trim
}

void TestAiSubtitleCatalog::parseReplyFallsBackToText() {
    const QByteArray body = R"({"choices":[{"text":"<think>partial</think>Hola"}]})";
    QString out;
    QVERIFY(parseTranslationReply(body, out));
    QCOMPARE(out, QStringLiteral("Hola"));
}

void TestAiSubtitleCatalog::parseReplyRejectsUnknownShape() {
    QString out;
    QVERIFY(!parseTranslationReply(QByteArray("{}"), out));
    QVERIFY(!parseTranslationReply(QByteArray(R"({"choices":[]})"), out));
    QVERIFY(!parseTranslationReply(QByteArray(R"({"choices":[{"message":{}}]})"), out));
    QVERIFY(!parseTranslationReply(QByteArray("not json"), out));
    // 结构成立但内容为空：解析成功，"空串判本句失败"由调用方承担
    QVERIFY(parseTranslationReply(QByteArray(R"({"choices":[{"message":{"content":""}}]})"), out));
    QVERIFY(out.isEmpty());
}

void TestAiSubtitleCatalog::stripThinkingPaired() {
    // 成对（大小写不敏感、可跨行）整段删除
    QCOMPARE(stripThinkingBlocks(QStringLiteral("a<THINK>x\ny</think>b")), QStringLiteral("ab"));
    QCOMPARE(stripThinkingBlocks(QStringLiteral("<think>a</think><think>b</think>ok")),
             QStringLiteral("ok"));
}

void TestAiSubtitleCatalog::stripThinkingOpenOnly() {
    // 只剩开标签：删到串尾
    QCOMPARE(stripThinkingBlocks(QStringLiteral("keep<THINK>truncated")), QStringLiteral("keep"));
}

void TestAiSubtitleCatalog::stripThinkingCloseOnly() {
    // 只剩闭标签（服务端预填开标签的常见形态）：删到该闭标签为止
    QCOMPARE(stripThinkingBlocks(QStringLiteral("...reasoning</think>answer")),
             QStringLiteral("answer"));
}

void TestAiSubtitleCatalog::cacheBaseNameLocal() {
    QCOMPARE(cacheBaseName(QStringLiteral("D:/movies/Movie.2023.mkv")),
             QStringLiteral("Movie.2023")); // 只剥最后一个扩展名
    QCOMPARE(cacheBaseName(QStringLiteral("D:/movies/plain.mkv")), QStringLiteral("plain"));
    QCOMPARE(cacheBaseName(QStringLiteral("D:/movies/no.ext.at.all")), QStringLiteral("no.ext.at"));
}

void TestAiSubtitleCatalog::cacheBaseNameTransitKeepsTailVerbatim() {
    // oxbow:// 不 unwrap 也不解码：尾段逐字节当基名用，只按最后一个 . 剥扩展名——与
    // mpv 的比对串同源（显示解码走 Util::Url::fileName，两个口径各守各的）
    QCOMPARE(cacheBaseName(QStringLiteral("oxbow://0a1b-2c/x%2Fy/movie.mkv")),
             QStringLiteral("movie"));
    QCOMPARE(cacheBaseName(QStringLiteral("oxbow://0a1b-2c/x/a%3Fb.mkv")),
             QStringLiteral("a%3Fb")); // 转义形态保持
    QCOMPARE(cacheBaseName(QStringLiteral("oxbow://0a1b-2c/x/a%3Fb.zh.srt")),
             QStringLiteral("a%3Fb.zh"));
    QCOMPARE(cacheBaseName(QStringLiteral("oxbow://0a1b-2c/x/noext")), QStringLiteral("noext"));
}

void TestAiSubtitleCatalog::languageMappingCoversList() {
    const QStringList languages = targetLanguages();
    QCOMPARE(languages.size(), 38);
    QCOMPARE(languages.first(), defaultTargetLanguage()); // 首项亦即缺省
    QCOMPARE(languages.last(), QStringLiteral("Cantonese"));
    for (const QString &language : languages) {
        const QString shortName = shortLanguageName(language);
        QVERIFY(!shortName.isEmpty());
        QVERIFY(shortName != language); // 38 项全部有映射值（本轮无"显式留空"项）
    }
    // 三个非 639-1 写法按 mp_guess_lang_from_filename 的语法判据成立
    QCOMPARE(shortLanguageName(QStringLiteral("Filipino")), QStringLiteral("fil"));
    QCOMPARE(shortLanguageName(QStringLiteral("Cantonese")), QStringLiteral("yue"));
    QCOMPARE(shortLanguageName(QStringLiteral("TraditionalChinese")), QStringLiteral("zh-Hant"));
    // 清单外退长名原文
    QCOMPARE(shortLanguageName(QStringLiteral("NotInList")), QStringLiteral("NotInList"));
}

QTEST_GUILESS_MAIN(TestAiSubtitleCatalog)
#include "tst_AiSubtitleCatalog.moc"

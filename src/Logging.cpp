#include "pch.h"

#include "Logging.h"

#include "Constants.h"

namespace {

// 模式来源两处的名字：日志层的私有约定，只在本文件出现，不外露成全局常量
// （ini 顶层键与环境变量同形数字，各按 0/1/2 解析后取较大值；测试按这两个字面串覆盖）
constexpr char kLogModeIniKey[] = "logMode";
constexpr char kLogModeEnvVar[] = "OXBOW_LOG_MODE";

// 输出模式：初值 0（关）。install 写一次，此后各线程只读
std::atomic<int> g_mode{0};

// 全部去向共用一把锁：整行一次写完，多线程日志不出现撕裂行
QMutex g_sinkMutex;

// 文件 sink：进程寿命对象，有意不析构（规避静态析构顺序）
QFile *g_file = nullptr;

QString levelTag(Logging::Level level) {
    switch (level) {
    case Logging::Level::Debug:
        return QStringLiteral("DEBUG");
    case Logging::Level::Info:
        return QStringLiteral("INFO");
    case Logging::Level::Warning:
        return QStringLiteral("WARN");
    case Logging::Level::Error:
        return QStringLiteral("ERROR");
    }
    return QStringLiteral("INFO");
}

int normalizeMode(int value) { return value >= 0 && value <= 2 ? value : 0; }

int modeValue(const QString &raw) {
    bool ok = false;
    const int value = raw.trimmed().toInt(&ok);
    return ok ? normalizeMode(value) : 0;
}

int environmentMode() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(kLogModeEnvVar, &ok);
    return ok ? normalizeMode(value) : 0;
}

// 标准面两去向，同一串（调用方须已持 g_sinkMutex）。不判"有无控制台"，写失败无副作用
void writeStdSinks(const QString &line) {
    const QByteArray utf8 = line.toUtf8();
    fwrite(utf8.constData(), 1, size_t(utf8.size()), stderr);
    fflush(stderr);
#ifdef Q_OS_WIN
    OutputDebugStringW(reinterpret_cast<const wchar_t *>(line.utf16()));
#endif
}

void writeLine(Logging::Level level, const QDateTime &when, const QString &body) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode <= 0)
        return;

    const QString line = Logging::formatLine(level, when, body) + u'\n';

    QMutexLocker locker(&g_sinkMutex);
    if (mode >= 2 && g_file && g_file->isOpen()) {
        const QByteArray utf8 = line.toUtf8();
        if (g_file->write(utf8) != utf8.size() || !g_file->flush()) {
            // 写失败即关掉文件 sink，其余去向照常，避免每行重复失败。这条收口告知
            // 只能裸格式化后写标准面，不得走 Line：此刻已持 g_sinkMutex，而该锁不递归，
            // Line 析构重入 writeLine 即自锁死
            g_file->close();
            writeStdSinks(
                Logging::formatLine(Logging::Level::Warning, QDateTime::currentDateTime(),
                                    QStringLiteral("log file write failed, file sink closed")) +
                u'\n');
        }
    }
    writeStdSinks(line);
}

} // namespace

namespace Logging {

int resolveMode(const QString &configFile) {
    int iniMode = 0;
    {
        QSettings s(configFile, QSettings::IniFormat);
        iniMode = modeValue(s.value(QString::fromLatin1(kLogModeIniKey)).toString());
    }
    return std::max(iniMode, environmentMode());
}

QString filePathForPid(qint64 pid) {
    return QDir(QDir::tempPath()).filePath(QStringLiteral("oxbow.%1.log").arg(pid));
}

QString formatLine(Level level, const QDateTime &when, const QString &body) {
    // 扫码前缀由 App::nameAscii 现拼（改名只动 Constants.h），本层不另立字面串
    return QStringLiteral("[%1] [%2] [%3] %4")
        .arg(QString::fromLatin1(App::nameAscii),
             when.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")), levelTag(level), body);
}

void install(const QString &configFile) {
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    const int mode = resolveMode(configFile);
    g_mode.store(mode, std::memory_order_relaxed);
    if (mode < 2)
        return;

    const QString path = filePathForPid(QCoreApplication::applicationPid());
    auto *file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        // 打开失败不阻断启动：文件 sink 缺席，1 档去向照常；这条警告因此不会进文件
        Line(Level::Warning) << "log file open failed:" << path << file->errorString();
        delete file;
        return;
    }
    QMutexLocker locker(&g_sinkMutex);
    g_file = file;
}

Line::Line(Level level) : m_level(level) {
    if (g_mode.load(std::memory_order_relaxed) <= 0)
        return;
    m_when = QDateTime::currentDateTime();
    m_stream.emplace(&m_body);
}

Line::~Line() {
    if (!m_stream)
        return;
    m_stream.reset(); // 释放 QDebug：尾部内容落进 m_body
    writeLine(m_level, m_when, m_body);
}

Line &Line::noquote() {
    if (m_stream)
        m_stream->noquote();
    return *this;
}

} // namespace Logging

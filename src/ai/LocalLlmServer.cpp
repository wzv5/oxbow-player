#include "pch.h"

#include "LocalLlmServer.h"

#include "AiSubtitleCatalog.h"
#include "Logging.h"

namespace {

// llama-server 就绪行形态："... listening on http://127.0.0.1:<端口>"；命中端口为 0
// 按失败处理（不把 0 当有效监听端口）
const QRegularExpression
    kListenPattern(QStringLiteral("listening on http://127\\.0\\.0\\.1:(\\d+)"),
                   QRegularExpression::CaseInsensitiveOption);

constexpr int kTailBytes = 2048; // 失败时落日志的输出末段长度

} // namespace

LocalLlmServer::LocalLlmServer(QObject *parent) : QObject(parent) {
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_state == State::Starting)
            fail();
    });
}

LocalLlmServer::~LocalLlmServer() { finishProcess(); }

void LocalLlmServer::start(const QString &commandLine, const QString &pluginDir) {
    stop();
    m_line.clear();
    m_tail.clear();
    m_state = State::Starting;

    QStringList args = AiSubtitleCatalog::splitCommandLine(commandLine);
    if (args.isEmpty() || pluginDir.isEmpty()) {
        fail();
        return;
    }
    const QString program =
        AiSubtitleCatalog::resolvePluginProgramPath(pluginDir, args.takeFirst());

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setWorkingDirectory(pluginDir);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &LocalLlmServer::ingestOutput);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_state == State::Starting)
            fail();
    });
    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (m_state == State::Starting) {
                    fail(); // 起飞期自行退出
                    return;
                }
                if (m_state == State::Ready) {
                    m_state = State::Idle;
                    OXBOW_LOG_WARNING()
                        << "local translation server exited unexpectedly, code" << exitCode
                        << (exitStatus == QProcess::CrashExit ? QStringLiteral("(crashed)")
                                                              : QString());
                    emit exited();
                }
            });
    m_process->start(program, args);
    m_deadline.start(AiSubtitleCatalog::kServerReadyTimeoutMs);
}

void LocalLlmServer::stop() {
    m_deadline.stop();
    finishProcess();
    m_state = State::Idle;
}

void LocalLlmServer::ingestOutput() {
    // 排空恒先行：就绪之后不读，管道填满即把服务端卡在写日志上
    const QByteArray chunk = m_process->readAllStandardOutput();
    if (m_state != State::Starting || chunk.isEmpty())
        return;

    // 只拆已封口的行：端口数字被读块边界切开时（"…:8" / "08"），半行留着等下一拍
    // 凑齐再认——在累计全文上取首个匹配会认出一个错端口，之后每句都连不上
    m_line += chunk;
    for (qsizetype nl = m_line.indexOf('\n'); nl >= 0; nl = m_line.indexOf('\n')) {
        const QByteArray line = m_line.left(nl);
        m_line.remove(0, nl + 1);
        rememberTail(line + '\n');
        const QRegularExpressionMatch match = kListenPattern.match(QString::fromUtf8(line));
        if (!match.hasMatch())
            continue;
        bool ok = false;
        const quint16 port = match.captured(1).toUShort(&ok);
        if (!ok || port == 0) {
            fail();
            return;
        }
        m_state = State::Ready;
        m_deadline.stop();
        OXBOW_LOG_INFO() << "local translation server listening on port" << port;
        emit ready(port);
        return; // 就绪后不再记账（含余下的半行与本次未拆完的部分）
    }
}

void LocalLlmServer::rememberTail(const QByteArray &bytes) {
    m_tail += bytes;
    if (m_tail.size() > kTailBytes)
        m_tail.remove(0, m_tail.size() - kTailBytes);
}

void LocalLlmServer::fail() {
    m_deadline.stop();
    rememberTail(m_line); // 恰好死在半行上时，那半行是唯一的线索
    m_line.clear();
    OXBOW_LOG_WARNING() << "local translation server startup failed, output tail:"
                        << QString::fromUtf8(m_tail);
    finishProcess();
    m_state = State::Idle;
    emit failed();
}

void LocalLlmServer::finishProcess() {
    if (!m_process)
        return;
    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning)
        process->kill(); // kill 后异步收尸（N143 同款），不阻塞 GUI 线程等退出
    process->deleteLater();
}

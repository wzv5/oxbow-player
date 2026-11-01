#pragma once

#include "pch.h"

// N146：内置翻译服务端（llama-server）的子进程生命周期——拉起、认端口、收杀，
// 对应流水线的"初始化翻译"段。端口传 0 ⇒ 必须从合并输出认实际绑定端口：正则命中
// 形如 "listening on http://127.0.0.1:<端口>" 的行即就绪（--port 0 已实测打印实际
// 端口）；认端口上限内认不出、进程自行退出或命中的端口为 0 都判失败（0 不是有效
// 监听端口，不据此拼 base URL）。认端口只在**已封口的完整行**上进行——读块边界落在
// 端口数字中间时，半行留在缓冲等下一次凑齐，否则"…:8"会被当成端口 8 认下。
// 等待全程异步（QProcess 信号 + 单发定时器），不阻塞 GUI 线程；不做 localhost /
// IPv6 形态的兜底猜测。收杀照 N143 既有形态：kill 后 disconnect + deleteLater
// 异步收尸，不阻塞等退出（本进程不持任何待删的临时文件）。一次服务一个实例，
// 不复用（重复 start 先收杀上一实例）。
class LocalLlmServer : public QObject {
    Q_OBJECT
  public:
    explicit LocalLlmServer(QObject *parent = nullptr);
    ~LocalLlmServer() override;

    // 发起一次服务：command 为内置命令数组（第 0 项程序写法、其余实参）——程序段
    // 经 Util::resolveExternalProgram 三步解析并记起飞日志（N149），装配不再有拆串
    // 一步；以插件目录为工作目录，无占位符可换
    void start(const QStringList &command, const QString &pluginDir, const QString &iniDir);
    // 收杀（任务终态与取消同一条出口）；idle 时无操作
    void stop();

  signals:
    void ready(quint16 port); // 就绪：base URL = http://127.0.0.1:<port>/v1
    void failed();            // 起不来（启动失败 / 超时 / 进程自行退出 / 端口 0）
    void exited();            // 就绪之后的进程退出（翻译期非预期退出，调用方终止整条）

  private:
    enum class State { Idle, Starting, Ready };
    // 起飞期失败的唯一收口：停时限、收杀进程、回 idle（failed 只发一次）
    void fail();
    void finishProcess();
    // 合并输出的唯一入口：排空管道 + 在完整行里认端口（就绪后只排空，不再记账——
    // 服务端逐句都写日志，留着会把整段会话的输出全攥在内存里）
    void ingestOutput();
    // 输出末段按 kTailBytes 裁剪，只供失败时落日志
    void rememberTail(const QByteArray &bytes);

    QProcess *m_process = nullptr;
    QTimer m_deadline; // 认端口上限（单发）
    State m_state = State::Idle;
    QByteArray m_line; // 尚未封口的半行（只在 Starting 期积累）
    QByteArray m_tail; // 已封口行的末段，失败时落日志
};

#pragma once

#include "pch.h"

// 插件日志的唯一装配与出口点（N126）。
// 输出模式由 ini 顶层 logMode 与环境变量 OXBOW_LOG_MODE 合成（各按数字解析，取较大值）：
//   0 关（缺省）、1 调试输出（stderr，Windows 另写调试器）、2 文件（在 1 档之上加写文件）。
// 调用侧一律经 OXBOW_LOG_* 宏。本层不安装 Qt 消息处理器：Qt 自身日志照旧走 Qt 默认
// 处理器，不受模式影响、不带 [oxbow] 前缀；插件日志只有这里一条路径。
namespace Logging {

enum class Level { Debug, Info, Warning, Error };

// 合成输出模式：缺键 / 空 / 非数字 / 越界（<0 或 >2）按 0，两处取较大值
int resolveMode(const QString &configFile);

// 系统临时目录下的 oxbow.<pid>.log
QString filePathForPid(qint64 pid);

// 行格式唯一实现：`[oxbow] [yyyy-MM-dd HH:mm:ss.zzz] [WARN] 正文`
QString formatLine(Level level, const QDateTime &when, const QString &body);

// 入口调用一次：定模式、按需打开文件；重复调用直接返回。不收尾，文件随进程结束关闭
void install(const QString &configFile);

// 流式行对象：构造时按模式决定是否激活，关闭态零分配；析构时格式化并写出。
// 内部用 QDebug 只做流式格式化（写进 QString 缓冲），不触发 Qt 消息处理器
class Line {
  public:
    explicit Line(Level level);
    ~Line();

    Line(const Line &) = delete;
    Line &operator=(const Line &) = delete;

    Line &noquote();

    template <typename T> Line &operator<<(const T &value) {
        if (m_stream)
            *m_stream << value;
        return *this;
    }

  private:
    Level m_level;
    QDateTime m_when;
    QString m_body;
    std::optional<QDebug> m_stream;
};

} // namespace Logging

#define OXBOW_LOG_DEBUG() ::Logging::Line(::Logging::Level::Debug)
#define OXBOW_LOG_INFO() ::Logging::Line(::Logging::Level::Info)
#define OXBOW_LOG_WARNING() ::Logging::Line(::Logging::Level::Warning)
#define OXBOW_LOG_ERROR() ::Logging::Line(::Logging::Level::Error)

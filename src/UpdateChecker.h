#pragma once

#include "pch.h"

class ServiceRegistry;

// 检查更新后端（N151）。主窗 QObject 子对象，不入 ServiceRegistry；GUI 线程亲和，
// 公有接口只允许 GUI 线程调用。在途作业不捕获 this，退出不等待，随进程消失。
namespace UpdateCheck {

enum class Interval : int { Off = 0, Daily = 1, Weekly = 7 };

struct Release {
    QString version;       // 已去 tag 的 v 前缀
    QDateTime publishedAt; // UTC 指定
    QUrl pageUrl;          // release 页面（https）
};

// ini 读到的整数天 → 枚举；0/1/7 之外落 Daily
Interval intervalFromRaw(int days);

// 间隔为关恒 false；从未检查或时钟回拨（last > now）即到期，否则按流逝天数判
bool isDue(qint64 lastCheckSecs, Interval interval, qint64 nowSecs);

// 坏 JSON / 缺字段 / 形态不合（tag 无 v、去 v 后非点分数字、published_at 非法、
// html_url 非 https）返回 nullopt
std::optional<Release> parseLatestRelease(const QByteArray &json);

} // namespace UpdateCheck

class UpdateChecker : public QObject {
    Q_OBJECT

  public:
    enum class Status { Idle, Checking, UpToDate, UpdateAvailable, Failed };
    enum class Trigger { Manual, Automatic };

    // registry 非空；父对象由主窗给（this）
    explicit UpdateChecker(std::shared_ptr<ServiceRegistry> registry, QObject *parent = nullptr);

    UpdateChecker::Status status() const;        // 初值 Idle
    const UpdateCheck::Release &release() const; // 仅 UpdateAvailable / UpToDate 有效

    // 无条件发起：不看间隔；在途时复用当前作业
    void checkNow(UpdateChecker::Trigger trigger);

    // 自动入口：会话已查过（状态非 Idle）、ini 不可用、间隔为关或未到期一律跳过
    void maybeCheckAutomatically();

  signals:
    void stateChanged();
    void checkFinished(UpdateChecker::Trigger trigger, UpdateChecker::Status status);

  private:
    struct Outcome {
        bool ok = false;
        UpdateCheck::Release release;
        QString error; // 失败原因，只进日志
    };

    void onJobFinished();
    void writeLastCheck(qint64 secs) const;

    std::shared_ptr<ServiceRegistry> m_registry;
    QFutureWatcher<Outcome> *m_watcher = nullptr;
    UpdateChecker::Status m_status = UpdateChecker::Status::Idle;
    UpdateCheck::Release m_release;
    UpdateChecker::Trigger m_trigger = UpdateChecker::Trigger::Manual; // 在途作业来源
};

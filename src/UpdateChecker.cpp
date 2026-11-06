#include "pch.h"

#include "UpdateChecker.h"

#include "AppPaths.h"
#include "Constants.h"
#include "HttpClient.h"
#include "Logging.h"
#include "ServiceRegistry.h"
#include "Util.h"
#include "Version.h"

namespace UpdateCheck {

Interval intervalFromRaw(int days) {
    switch (days) {
    case 0:
        return Interval::Off;
    case 1:
        return Interval::Daily;
    case 7:
        return Interval::Weekly;
    default:
        return Interval::Daily;
    }
}

bool isDue(qint64 lastCheckSecs, Interval interval, qint64 nowSecs) {
    const int days = static_cast<int>(interval);
    if (days <= 0)
        return false;
    if (lastCheckSecs <= 0 || lastCheckSecs > nowSecs)
        return true;
    return nowSecs - lastCheckSecs >= static_cast<qint64>(days) * 86400;
}

namespace {
bool isDottedNumeric(const QString &s) {
    static const QRegularExpression re(QStringLiteral("^\\d+(?:\\.\\d+)+$"));
    return re.match(s).hasMatch();
}
} // namespace

std::optional<Release> parseLatestRelease(const QByteArray &json) {
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    const QJsonObject obj = doc.object();

    // /releases/latest 恒不返回 draft 与 prerelease，故不读这两个字段
    const QString tag = obj.value(QLatin1String("tag_name")).toString();
    if (!tag.startsWith(QLatin1Char('v')))
        return std::nullopt;
    const QString version = Util::GitHub::releaseVersion(tag);
    if (!isDottedNumeric(version))
        return std::nullopt;

    const QDateTime published =
        QDateTime::fromString(obj.value(QLatin1String("published_at")).toString(), Qt::ISODate);
    if (!published.isValid())
        return std::nullopt;

    const QUrl page(obj.value(QLatin1String("html_url")).toString());
    if (!page.isValid() || page.scheme() != QLatin1String("https"))
        return std::nullopt;

    return Release{version, published.toUTC(), page};
}

} // namespace UpdateCheck

UpdateChecker::UpdateChecker(std::shared_ptr<ServiceRegistry> registry, QObject *parent)
    : QObject(parent), m_registry(std::move(registry)),
      m_watcher(new QFutureWatcher<Outcome>(this)) {
    Q_ASSERT(m_registry);
    connect(m_watcher, &QFutureWatcherBase::finished, this, &UpdateChecker::onJobFinished);
}

UpdateChecker::Status UpdateChecker::status() const { return m_status; }

const UpdateCheck::Release &UpdateChecker::release() const { return m_release; }

void UpdateChecker::checkNow(UpdateChecker::Trigger trigger) {
    if (m_status == UpdateChecker::Status::Checking)
        return; // 在途：复用当前作业，不动 m_trigger

    const auto client = m_registry ? m_registry->resolve<HttpClient>() : nullptr;
    if (!client) {
        Q_ASSERT(false); // 装配错误：HttpClient 启动期必已 provide
        m_trigger = trigger;
        m_status = UpdateChecker::Status::Failed;
        m_release = {};
        OXBOW_LOG_WARNING() << "update check: http client unavailable";
        emit stateChanged();
        emit checkFinished(trigger, m_status);
        return;
    }

    m_trigger = trigger;
    m_status = UpdateChecker::Status::Checking;
    m_release = {};
    emit stateChanged();

    HttpClient::Request request;
    request.url = Util::GitHub::latestReleaseApiUrl(App::projectRepo());
    request.method = "GET";
    request.headers.append({QByteArrayLiteral("User-Agent"), Util::GitHub::userAgent()});
    request.headers.append(
        {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/vnd.github+json")});

    m_watcher->setFuture(QtConcurrent::run([client = *client, request]() -> Outcome {
        Outcome outcome;
        const HttpClient::Response response = client.exec(request);
        if (response.canceled) {
            outcome.error = QStringLiteral("canceled");
        } else if (response.status != 200) {
            outcome.error =
                response.error.isEmpty()
                    ? QStringLiteral("HTTP %1").arg(response.status)
                    : QStringLiteral("HTTP %1: %2").arg(response.status).arg(response.error);
        } else if (auto release = UpdateCheck::parseLatestRelease(response.body)) {
            outcome.ok = true;
            outcome.release = *release;
        } else {
            outcome.error = QStringLiteral("invalid latest release payload");
        }
        return outcome;
    }));
}

void UpdateChecker::onJobFinished() {
    const Outcome outcome = m_watcher->result();
    if (outcome.ok) {
        // 当前版本来自构建期 PROJECT_VERSION，恒为三段点分数字，与 release 侧同形
        m_release = outcome.release;
        m_status = Util::Version::compare(outcome.release.version, App::version()) > 0
                       ? UpdateChecker::Status::UpdateAvailable
                       : UpdateChecker::Status::UpToDate;
        writeLastCheck(QDateTime::currentSecsSinceEpoch());
    } else {
        m_status = UpdateChecker::Status::Failed;
        m_release = {};
        OXBOW_LOG_DEBUG() << "update check failed:" << outcome.error;
    }
    emit stateChanged();
    emit checkFinished(m_trigger, m_status);
}

void UpdateChecker::maybeCheckAutomatically() {
    if (m_status != UpdateChecker::Status::Idle) {
        OXBOW_LOG_DEBUG() << "update check skipped: already checked this session";
        return;
    }

    const QString ini = App::paths().iniFile();
    if (ini.isEmpty()) {
        OXBOW_LOG_DEBUG() << "update check skipped: ini unavailable";
        return;
    }

    QSettings settings(ini, QSettings::IniFormat);
    bool ok = false;
    const int raw =
        settings.value(QStringLiteral("updateCheckInterval"), App::updateCheckIntervalDefault())
            .toInt(&ok);
    const UpdateCheck::Interval interval =
        UpdateCheck::intervalFromRaw(ok ? raw : App::updateCheckIntervalDefault());
    const qint64 last = settings.value(QStringLiteral("updateLastCheck"), 0).toLongLong();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const bool due = UpdateCheck::isDue(last, interval, now);
    OXBOW_LOG_DEBUG() << "update check"
                      << "interval=" << static_cast<int>(interval) << "last=" << last
                      << "due=" << due;
    if (interval == UpdateCheck::Interval::Off || !due)
        return;
    checkNow(UpdateChecker::Trigger::Automatic);
}

void UpdateChecker::writeLastCheck(qint64 secs) const {
    const QString ini = App::paths().iniFile();
    if (ini.isEmpty())
        return; // 无落点即不写；自动入口启动时已跳过
    QSettings settings(ini, QSettings::IniFormat);
    settings.setValue(QStringLiteral("updateLastCheck"), secs);
}

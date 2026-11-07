#include "pch.h"

#include "ui/CacheStatusOsd.h"

#include "Util.h"
#include "mpvbridge/MpvController.h"

namespace {

// N153：与 OsdController 的 1001 分立——每个 libmpv 客户端独立 id 命名空间，同 id
// 重发即替换，format=none 同 id 即清除；两条通道互不覆盖、可同时存在
constexpr int kOverlayId = 1002;

// 与 OsdController 同款转义（N113 定的口径）：花括号 → \{ \}（避免被当 override 块），
// 真实换行 → \N；`\` 与 $ 原样（ASS 只解释 \N / \n / \h 三个行内序列，数组命令不做
// 属性展开）。本案文案是本类常量串，转义只为与既有通道口径一致
QString escapeOverlayText(const QString &text) {
    QString escaped = text;
    escaped.replace(QLatin1Char('{'), QLatin1String("\\{"));
    escaped.replace(QLatin1Char('}'), QLatin1String("\\}"));
    escaped.replace(QLatin1Char('\n'), QLatin1String("\\N"));
    return escaped;
}

} // namespace

CacheStatusOsd::CacheStatusOsd(QWidget *hostWindow, QWidget *videoWidget, MpvController *controller)
    : QObject(hostWindow), m_host(hostWindow), m_videoWidget(videoWidget),
      m_controller(controller) {
    m_delayTimer = new QTimer(this);
    m_delayTimer->setSingleShot(true);
    connect(m_delayTimer, &QTimer::timeout, this, &CacheStatusOsd::onDelayTimeout);
    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &CacheStatusOsd::onRefreshTick);
}

void CacheStatusOsd::setEnabled(bool enabled) {
    m_enabled = enabled;
    updateState();
}

void CacheStatusOsd::setSuppressed(bool suppressed) {
    m_suppressed = suppressed;
    updateState();
}

bool CacheStatusOsd::displayCondition() const {
    return (m_seeking || m_underrun || m_paused) && m_enabled && !m_suppressed;
}

void CacheStatusOsd::onSeekingChanged(bool seeking) {
    m_seeking = seeking;
    if (seeking)
        m_percent = 0; // seek 重置 demuxer 缓存，百分比从零重计
    updateState();
}

void CacheStatusOsd::onCacheUnderrunChanged(bool underrun) {
    m_underrun = underrun;
    updateState();
}

void CacheStatusOsd::onPausedForCacheChanged(bool paused) {
    m_paused = paused;
    updateState();
}

// 三者任一转假：停两张表，正显示的立即清除（无消失延迟、无淡出）；三者全真且未显示
// 且延迟表未活跃：起一次 1s 延迟——重复收到 paused == true 不重启计时
void CacheStatusOsd::updateState() {
    if (displayCondition()) {
        if (!m_active && !m_delayTimer->isActive())
            m_delayTimer->start(1000);
        return;
    }
    m_delayTimer->stop();
    m_refreshTimer->stop();
    m_percent = 0;
    if (m_active) {
        m_active = false;
        m_controller->commandAsync({QStringLiteral("osd-overlay"), QString::number(kOverlayId),
                                    QStringLiteral("none"), QString()});
    }
}

// 延迟到点：再确认三者仍全真（延迟期内任一转假时表已被停掉，此为兜底），当场下发
// 首帧并进入周期刷新；尺寸尚无落点（≤0）时首帧静默，活跃态照立，由刷新节拍补发
void CacheStatusOsd::onDelayTimeout() {
    if (!displayCondition() || m_active)
        return;
    m_active = true;
    m_refreshTimer->start(1000);
    refreshNow();
}

void CacheStatusOsd::onRefreshTick() {
    // 显示态内每 1s 无条件刷新：不做与上次内容的相等比对，数值未变也照发。
    // mpv 侧 cache-speed 是 1s 窗口统计、buffering 百分比同为秒级更新，更密的
    // 节拍只是空转重发同值
    if (!m_active) {
        m_refreshTimer->stop();
        return;
    }
    refreshNow();
}

// res 变了即按新 res 重发（\pos 的 y 依赖 res）；不重启延迟、不改活跃态，
// 与 N113「尺寸变化不给提示续命」同款
void CacheStatusOsd::reposition() {
    if (!m_active)
        return;
    const int w = m_videoWidget->width();
    const int h = m_videoWidget->height();
    if (w <= 0 || h <= 0 || (w == m_resW && h == m_resH))
        return;
    refreshNow();
}

void CacheStatusOsd::refreshNow() {
    const int w = m_videoWidget->width();
    const int h = m_videoWidget->height();
    if (w <= 0 || h <= 0)
        return;
    m_resW = w;
    m_resH = h;
    // 速度不观察，按节拍同步现读；读取失败、不可用与真实 0 一律按 0 显示（0 B/s），
    // 不区分、不读第二次
    const qint64 speed = m_controller->propertyInt64(QStringLiteral("cache-speed"), 0);
    // 百分比弃用 cache-buffering-state——它只在 paused_for_cache 分支更新，其余时刻
    // 是 seek 重启时被 mpv 重置的 100；改现算 demuxer-cache-duration ÷ cache-pause-wait
    // （复刻 mpv 的原式，clamp 0–99），两种状态都正确。读取失败按 0%
    const double duration =
        m_controller->propertyDouble(QStringLiteral("demuxer-cache-duration"), 0.0);
    // 回落 0：非正数走下面的守卫落 0%——回落成某个正数会把读失败折成假高百分比
    const double pauseWait = m_controller->propertyDouble(QStringLiteral("cache-pause-wait"), 0.0);
    // 钳在双精度一侧再转整数：比值超出 int 值域时 static_cast<int> 是未定义行为，
    // cache-pause-wait 可为极小正数
    const int raw =
        pauseWait > 0 ? static_cast<int>(qBound(0.0, 100.0 * duration / pauseWait, 99.0)) : 0;
    m_percent = qMax(m_percent, raw);
    // 两行拼一条 Dialogue 用 \N 行内换行——data 按换行拆条，分开写会各自按 \pos
    // 定位而叠在一起；\rDefault 钉在 mpv 对 osd-overlay 轨道的编译默认样式（用户
    // --osd-* 不入内，N113 实测口径），\fn 钉住宿主字体族
    const QString data =
        QStringLiteral(
            "{\\rDefault\\an1\\pos(12,%1)\\fn%2\\fs18\\b0\\i0\\u0\\s0\\fsp0\\fscx100\\fscy100"
            "\\bord1\\shad0\\blur0.4\\1a&H40&\\3a&H00&\\c&HFFFFFF&\\3c&H000000&\\q2}%3\\N%4")
            .arg(QString::number(h - 8), m_host->font().family(),
                 escapeOverlayText(tr("Buffering… %1%").arg(m_percent)),
                 escapeOverlayText(
                     tr("%1/s").arg(Util::formatFileSize(qMax<qint64>(qint64(0), speed)))));
    // 异步下发（N87）：缓冲期 core 可能正忙在 demuxer / 网络等待上，同步命令会占住
    // 调用线程；osd-overlay 纯渲染副作用、无返回值消费。z=-1 让位 console 等其它 custom OSD
    m_controller->commandAsync({QStringLiteral("osd-overlay"), QString::number(kOverlayId),
                                QStringLiteral("ass-events"), data, QString::number(w),
                                QString::number(h), QStringLiteral("-1")});
}

#include "pch.h"

#include "ui/OsdController.h"

#include "Constants.h"
#include "mpvbridge/MpvController.h"

// ---- N113：OSD 三形态控制器（通道分派 / 合并队列 / 2s 时限）----

namespace {

// N113 §3.3 文字转义：花括号 → \{ \}（避免被当 override 块），真实换行 → \N（\q2 下
// 仍生效，且避免 data 按行分裂成多条 Dialogue）；`\` 原样（ASS 只解释 \N / \n / \h
// 三个行内序列，Windows 文件名不含 \，标题含字面 \N 属极端情况）；$ 原样（数组命令
// 不做属性展开）
QString escapeOverlayText(const QString &text) {
    QString escaped = text;
    escaped.replace(QLatin1Char('{'), QLatin1String("\\{"));
    escaped.replace(QLatin1Char('}'), QLatin1String("\\}"));
    escaped.replace(QLatin1Char('\n'), QLatin1String("\\N"));
    return escaped;
}

// 独立形态 20pt 的逻辑像素 em：pt → 逻辑像素按 Windows 的 96 dpi 约定折算，与显示器
// DPI 与缩放无关。overlay 的 res 取视频区逻辑尺寸 ⇒ 1 ASS 单位 = 1 逻辑像素，两侧同尺，
// 故"与 20pt 等大"的目标 em 就是这个常量
constexpr double kStandaloneEmPx = 20.0 / 72.0 * 96.0;

// N113 §3.2：ASS 的 `\fs` 在 libass 里锚的是「上伸 + 下伸」（libass 以
// FT_SIZE_REQUEST_TYPE_REAL_DIM 申请字号），独立形态的 20pt 锚的是 em——两者差一个
// 随字体变的系数 (ascent + descent) / units_per_EM，故按主窗字体现算而非写死：
// `\fs` = 目标 em 逻辑像素 × 系数。
// 探测口径：把 QRawFont 的 pixelSize 设成 units_per_EM，此时 1 设计单位 = 1 像素，
// ascent() / descent() 直接就是设计度量——比值与 Qt 返回逻辑像素还是设备像素无关
double overlayFontSize(const QFont &font) {
    QRawFont raw = QRawFont::fromFont(font);
    const double upem = raw.isValid() ? raw.unitsPerEm() : 0.0;
    if (upem <= 0)
        return kStandaloneEmPx; // 度量取不到：按系数 1 下发（em 口径），不做折算
    raw.setPixelSize(upem);
    const double scaled = kStandaloneEmPx * (raw.ascent() + raw.descent()) / upem;
    // 度量异常（系数非正）同退 em 口径——宁可字号差一档，不可下发一条画不出来的 `\fs`
    return scaled > 0 ? scaled : kStandaloneEmPx;
}

// N113 §3.2 ASS 模板：\rDefault 把基底钉在「全默认」那一套（与用户 --osd-* 无关），
// 其余可视属性逐项显式覆盖；res 取视频区逻辑尺寸 ⇒ 1 ASS 单位 = 1 逻辑像素，\fs 即
// 固定逻辑字号。\pos(22,18) = 独立形态的容器边距 12 + 标签内绘偏移 (10, 6)；\bord 取
// 2.5 对齐 QPen 总宽 5 的居中外延（libass 的 \bord 直接是外延宽度）
QString assembleOverlayAss(const QString &family, double fontSize, const QString &text) {
    return QStringLiteral("{\\rDefault\\an7\\pos(22,18)\\fn%1\\fs%2\\b1\\i0\\u0\\s0\\fsp0\\fscx100"
                          "\\fscy100\\bord2.5\\shad0\\alpha&H00&\\c&HFFFFFF&\\3c&H000000&\\q2}%3")
        .arg(family, QString::number(fontSize, 'f', 2), escapeOverlayText(text));
}

// 无背景，仅描边白字（20pt），不使用 style sheet
class OsdLabel : public QLabel {
  public:
    using QLabel::QLabel;

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        const QString text = this->text();
        if (text.isEmpty())
            return;
        QFont f = font();
        f.setPointSize(20);
        f.setBold(true);
        p.setFont(f);
        QFontMetrics fm(f);
        const int padX = 10;
        const int padY = 6;
        QPainterPath path;
        path.addText(padX, padY + fm.ascent(), f, text);
        QPen pen(Qt::black, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(path, pen);
        p.fillPath(path, QBrush(Qt::white));
    }
    QSize sizeHint() const override {
        QFont f = font();
        f.setPointSize(20);
        f.setBold(true);
        QFontMetrics fm(f);
        const QString t = text();
        const int w = fm.horizontalAdvance(t) + 20;
        const int h = fm.height() + 12;
        return QSize(w, h);
    }
};

} // namespace

OsdController::OsdController(QWidget *hostWindow, QWidget *videoWidget, MpvController *controller)
    : QObject(hostWindow), m_host(hostWindow), m_videoWidget(videoWidget),
      m_controller(controller) {
    // 字号折算只依赖字体度量（常量），构造期算一次缓存——显示期内每条消息复用，
    // 不为一条 2s 的临时提示反复查字体库（QRawFont::fromFont 官方即标注开销偏大）。
    // 探测用粗体字面：ASS 下发 \b1，libass 实际选的就是它
    QFont probeFont = m_host->font();
    probeFont.setBold(true);
    m_overlayFontSize = overlayFontSize(probeFont);
    // 2s 定时器各形态分派共用（独立形态隐藏叠加窗；overlay 形态发 format=none），
    // 构造期创建——overlay 通道不经过 ensureOverlay
    m_osdTimer = new QTimer(this);
    m_osdTimer->setSingleShot(true);
    connect(m_osdTimer, &QTimer::timeout, this, [this] {
        switch (m_mode) {
        case Mode::Standalone:
            if (m_osdLabel)
                m_osdLabel->hide();
            if (m_videoOverlay && !m_osdLabel->isVisible())
                m_videoOverlay->hide();
            break;
        case Mode::Overlay:
            clearOverlay();
            break;
        case Mode::ShowText:
        case Mode::Off:
            break; // show-text 不起本定时器；关态无消息
        }
    });
}

OsdController::Mode OsdController::modeFromIniValue(const QString &raw) {
    const QString v = raw.trimmed();
    if (v == QLatin1String("off"))
        return Mode::Off;
    if (v == QLatin1String("show-text"))
        return Mode::ShowText;
    if (v == QLatin1String("standalone"))
        return Mode::Standalone;
    return Mode::Overlay; // "overlay" 与其余一切值（缺键时调用方取 "overlay" 传入）
}

void OsdController::setMode(Mode mode) {
    if (mode == m_mode)
        return;
    // 离开当前形态：先做该形态的清除，再落新值
    switch (m_mode) {
    case Mode::Overlay:
        m_osdTimer->stop();
        clearOverlay(); // format=none：正显示的立即消失
        break;
    case Mode::Standalone:
        m_osdTimer->stop();
        if (m_videoOverlay)
            m_videoOverlay->hide(); // 立即收起，不留 2s 残影
        break;
    case Mode::ShowText:
        m_osdTimer->stop(); // 本形态不在定时器上，停一下统一离态口径（无害）
        break;
    case Mode::Off:
        break;
    }
    m_mode = mode;
}

void OsdController::setSuppressed(bool suppressed) { m_suppressed = suppressed; }

void OsdController::show(const QString &text) {
    // 小窗与关：消息照收照丢，不排队、不补显——暂停提示 / 书签确认等均有时效，
    // 退出小窗后补显已失意义，故退出路径不涉及 OSD 恢复（N68 / 四态语义）
    if (m_suppressed || m_mode == Mode::Off)
        return;
    switch (m_mode) {
    case Mode::Overlay:
        showOverlay(text);
        break;
    case Mode::Standalone:
        ensureOverlay();
        m_osdLabel->setText(text);
        m_osdLabel->adjustSize();
        reposition();
        m_videoOverlay->show();
        m_osdLabel->show();
        m_videoOverlay->raise();
        m_osdTimer->start(2000);
        break;
    case Mode::ShowText:
        // 完全跟随用户 --osd-*（位置 / 字号 / 颜色随配置，时长随 --osd-duration）；
        // 与 mpv 自身提示共享文本 OSD 槽，后到者覆盖先到者。不带 duration / level
        // ——时长交 --osd-duration；level 缺省值手册未载，实测 --osd-level=0 下照显
        m_controller->command({QStringLiteral("show-text"), text});
        break;
    case Mode::Off:
        break;
    }
}

void OsdController::queue(Priority priority, const QString &text) {
    switch (priority) {
    case Priority::PlayPause:
        m_pendingPlayPause = text;
        break;
    case Priority::Stop:
        m_pendingStop = text;
        break;
    case Priority::NowPlaying:
        m_pendingNowPlaying = text;
        break;
    }
    if (!m_coalescePending) {
        m_coalescePending = true;
        QTimer::singleShot(0, this, &OsdController::flush);
    }
}

void OsdController::reposition() {
    if (!m_videoWidget)
        return;
    // overlay 形态：res 在下发那刻定死，mpv 侧 overlay 的 ASS frame 恒等于 vo 输出
    // 尺寸、PlayRes 恒等于命令 res——尺寸一变，二者之比就把整条等比缩放（冷起播时
    // 小窗发出、紧随其后的分辨率自适应把窗口撑大，字即被放大数倍），故按新 res 重发
    if (m_mode == Mode::Overlay) {
        refreshOverlay();
        return;
    }
    if (!m_videoOverlay)
        return;
    const QPoint globalPos = m_videoWidget->mapToGlobal(QPoint(0, 0));
    m_videoOverlay->setGeometry(QRect(globalPos, m_videoWidget->size()));
}

void OsdController::showOverlay(const QString &text) {
    // §3.3：空串按清除处理，不发空事件
    if (text.isEmpty()) {
        m_osdTimer->stop();
        if (m_overlayActive)
            clearOverlay();
        return;
    }
    // 视频区逻辑尺寸任一 ≤ 0 即无落点，静默丢弃（§3.1；极早期无落点属已知限制）
    const int w = m_videoWidget->width();
    const int h = m_videoWidget->height();
    if (w <= 0 || h <= 0)
        return;
    m_overlayText = text;
    m_overlayActive = true;
    sendOverlay(text, w, h);
    m_osdTimer->start(2000); // 同 id 重发即替换，重启 2s 定时器
}

// 显示期内视频区逻辑尺寸变了：按新 res 重发同一条（窗口 resize / 起播自适应 /
// 播放列表显隐 / 全屏与小窗进出都经本路）。寿命不变——不重启 2s 定时器，一条消息
// 自始至终活 2s，尺寸变化不给它续命（连续拖窗也不会把提示一直留着）
void OsdController::refreshOverlay() {
    if (!m_overlayActive)
        return;
    const int w = m_videoWidget->width();
    const int h = m_videoWidget->height();
    if (w <= 0 || h <= 0 || (w == m_overlayResW && h == m_overlayResH))
        return;
    sendOverlay(m_overlayText, w, h);
}

void OsdController::sendOverlay(const QString &text, int w, int h) {
    m_overlayResW = w;
    m_overlayResH = h;
    // 数组命令（mpv_command）默认不做属性展开，文案不怕 $ / ${...}；z=-1 让位给
    // console 等其它 custom OSD（实测负值可用，console 打开时覆盖本条）
    m_controller->command({QStringLiteral("osd-overlay"),
                           QString::number(static_cast<int>(App::OsdOverlayId::Message)),
                           QStringLiteral("ass-events"),
                           assembleOverlayAss(m_host->font().family(), m_overlayFontSize, text),
                           QString::number(w), QString::number(h), QStringLiteral("-1")});
}

void OsdController::clearOverlay() {
    m_overlayActive = false;
    m_overlayText.clear();
    m_controller->command({QStringLiteral("osd-overlay"),
                           QString::number(static_cast<int>(App::OsdOverlayId::Message)),
                           QStringLiteral("none"), QString()});
}

void OsdController::ensureOverlay() {
    if (m_videoOverlay)
        return;
    Qt::WindowFlags overlayFlags = Qt::Tool | Qt::FramelessWindowHint |
                                   Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput;
#ifndef Q_OS_WIN
    // Linux/X11：合成器对受管 Tool 窗施加"保持在屏内"的保屏钳制（四向、逐轴），
    // 主窗拖出工作区后 OSD 冻在屏缘不再跟随；绕过窗口管理器即脱离该路径，恢复
    // 自由定位。Windows 的 SetWindowPos 无此钳制，不引入该 hint（语义也与 X11 不同）
    overlayFlags |= Qt::X11BypassWindowManagerHint;
#endif
    m_videoOverlay = new QWidget(m_host, overlayFlags);
    m_videoOverlay->setAttribute(Qt::WA_TranslucentBackground);
    m_videoOverlay->setAttribute(Qt::WA_ShowWithoutActivating);
    m_videoOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // 容器透明，内部控件自绘
    auto *layout = new QVBoxLayout(m_videoOverlay);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(0);
    // B12：SetNoConstraint——浮窗几何恒由 setGeometry(视频区矩形) 决定，
    // 不被内容 sizeHint 撑大；超宽文本（长文件名）由父窗口边界自然硬切，
    // 杜绝默认约束模式把内容最小尺寸施加为顶级窗口尺寸而压过播放列表
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    m_osdLabel = new OsdLabel(m_videoOverlay);
    m_osdLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_osdLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_osdLabel->hide();
    layout->addWidget(m_osdLabel, 0, Qt::AlignTop | Qt::AlignLeft);
    layout->addStretch(1);
}

void OsdController::flush() {
    m_coalescePending = false;
    QString text;
    if (!m_pendingNowPlaying.isEmpty())
        text = m_pendingNowPlaying;
    else if (!m_pendingStop.isEmpty())
        text = m_pendingStop;
    else if (!m_pendingPlayPause.isEmpty())
        text = m_pendingPlayPause;
    m_pendingNowPlaying.clear();
    m_pendingStop.clear();
    m_pendingPlayPause.clear();
    if (!text.isEmpty())
        show(text);
}

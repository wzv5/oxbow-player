#include "pch.h"

#include "NavigatorPanel.h"

#include "MpvController.h"

namespace {
// 配色：画布底用窗口默认背景（跟随主题）；画面框内部固定深色模拟视频黑幕。
// 视口矩形纯色填充、红色边框写死（N1 冻结口径）
const QColor kScreenBg(0x10, 0x10, 0x10);   // 画面框内部（视频黑幕）
const QColor kViewFill(0x50, 0x50, 0x50);   // 视口矩形填充
const QColor kViewBorder(0xFF, 0x30, 0x30); // 视口矩形红色边框（PS 风格）

const double kZoomMin = -2.0; // 25%
const double kZoomMax = 3.0;  // 800%
const qreal kMargin = 8.0;    // 画布内边距

// contain-fit：窗口内按比例内接的视频尺寸（zoom=0 时 mpv 的显示大小）
QSizeF fitSize(const QSize &window, double aspect) {
    const double winAspect = window.width() / double(window.height());
    if (aspect >= winAspect)
        return QSizeF(window.width(), window.width() / aspect);
    return QSizeF(window.height() * aspect, window.height());
}
} // namespace

NavigatorCanvas::NavigatorCanvas(MpvController *controller, QWidget *renderArea, QWidget *parent)
    : QWidget(parent), m_controller(controller), m_render(renderArea) {
    setMinimumSize(180, 130);

    connect(controller, &MpvController::videoZoomChanged, this, [this](double z) {
        m_zoom = qBound(kZoomMin, z, kZoomMax);
        emit zoomVisualChanged(displayPercent());
        update();
    });
    connect(controller, &MpvController::videoPanXChanged, this, [this](double v) {
        m_panX = v;
        update();
    });
    connect(controller, &MpvController::videoPanYChanged, this, [this](double v) {
        m_panY = v;
        update();
    });
    connect(controller, &MpvController::videoSizeChanged, this, [this](int w, int h) {
        const bool wasActive = isActive();
        m_videoW = w;
        m_videoH = h;
        if (isActive() != wasActive)
            emit activeChanged(isActive());
        emit zoomVisualChanged(displayPercent()); // 几何/源分辨率变化→百分比重算
        update();
    });
    // 按需创建（N3）：width/height 只在变化时推送，本实例错过了播放开始时的
    // 首推，从控制器缓存播种初始尺寸，否则播放中打开恒显示"无视频"
    const QSize seed = controller->videoSize();
    m_videoW = seed.width();
    m_videoH = seed.height();
}

QSizeF NavigatorCanvas::currentFit() const {
    if (!m_render || m_videoH <= 0)
        return QSizeF();
    return fitSize(m_render->size(), double(m_videoW) / m_videoH);
}

int NavigatorCanvas::displayPercent() const {
    const QSizeF fit = currentFit();
    if (fit.width() <= 0 || m_videoW <= 0)
        return 0;
    // 1:1 相对百分比：显示宽 / 源宽（contain 等比 + 缩放等比，任轴等价）
    const double scale = fit.width() * std::pow(2.0, m_zoom) / m_videoW;
    return int(scale * 100.0 + 0.5);
}

int NavigatorCanvas::zoomToPercent(double zoom) const {
    const QSizeF fit = currentFit();
    if (fit.width() <= 0 || m_videoW <= 0)
        return 0;
    const double scale = fit.width() * std::pow(2.0, zoom) / m_videoW;
    return int(scale * 100.0 + 0.5);
}

double NavigatorCanvas::percentToZoom(int percent) const {
    const QSizeF fit = currentFit();
    if (fit.width() <= 0 || m_videoW <= 0)
        return 0.0;
    return std::log2(percent / 100.0 * m_videoW / fit.width());
}

QRectF NavigatorCanvas::frameRect() const {
    if (!isActive())
        return QRectF();
    const QRectF avail = rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);
    const qreal videoAspect = qreal(m_videoW) / m_videoH;
    const qreal availAspect = avail.width() / avail.height();
    qreal fw, fh;
    if (videoAspect >= availAspect) {
        fw = avail.width();
        fh = fw / videoAspect;
    } else {
        fh = avail.height();
        fw = fh * videoAspect;
    }
    return QRectF(avail.left() + (avail.width() - fw) / 2, avail.top() + (avail.height() - fh) / 2,
                  fw, fh);
}

double NavigatorCanvas::clampPan(double pan, int windowPx, int videoPx) const {
    const double w = windowPx, v = videoPx;
    if (w <= 0 || v <= 0)
        return 0.0;
    // 仅超窗轴可拖：铺满窗口不产生新黑边；小轴锁死居中，
    // 比例不匹配的自然黑边保留、不可经平移改变（维护者定稿口径）
    if (v >= w)
        return qBound((w - v) / (2.0 * v), pan, (v - w) / (2.0 * v));
    return 0.0;
}

QRectF NavigatorCanvas::viewRect() const {
    const QRectF f = frameRect();
    if (!isActive() || f.isEmpty() || !m_render)
        return QRectF();
    // mpv 渲染模型：视频按 contain-fit 居中，zoom 以 2 为底缩放，
    // pan 按显示尺寸的分数平移。窗口左右/上下边缘换算成视频归一化坐标
    // 后与 [0,1] 取交——有黑边时矩形自然贴满整边，与实际所见一致。
    const QSize ws = m_render->size();
    const double aspect = double(m_videoW) / m_videoH;
    const QSizeF fit = fitSize(ws, aspect);
    const double s = std::pow(2.0, m_zoom);
    const double vw = fit.width() * s, vh = fit.height() * s;
    const double x0 = (ws.width() - vw) / 2 + m_panX * vw;
    const double y0 = (ws.height() - vh) / 2 + m_panY * vh;
    const double uLo = qBound(0.0, -x0 / vw, 1.0);
    const double uHi = qBound(0.0, (ws.width() - x0) / vw, 1.0);
    const double vLo = qBound(0.0, -y0 / vh, 1.0);
    const double vHi = qBound(0.0, (ws.height() - y0) / vh, 1.0);
    if (uHi <= uLo || vHi <= vLo)
        return QRectF(); // 画面完全离开窗口（钳制下不应出现，防御）
    return QRectF(f.left() + f.width() * uLo, f.top() + f.height() * vLo, f.width() * (uHi - uLo),
                  f.height() * (vHi - vLo));
}

void NavigatorCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), palette().window()); // 画布底=窗口默认背景（维护者指定）

    if (!isActive()) {
        p.setPen(QPen(palette().color(QPalette::PlaceholderText)));
        p.drawText(rect(), Qt::AlignCenter, tr("No video"));
        return;
    }

    const QRectF f = frameRect();
    p.fillRect(f, kScreenBg);                          // 画面区深色，与画布底形成对比
    p.setPen(QPen(palette().color(QPalette::Mid), 1)); // 主题中间调边框，浅/深色主题均可见
    p.drawRect(f);

    const QRectF v = viewRect();
    p.fillRect(v, kViewFill);
    p.setPen(QPen(kViewBorder, 2));
    p.drawRect(v);
}

void NavigatorCanvas::resizeEvent(QResizeEvent *) {
    update(); // 矩形几何依赖画布尺寸
}

void NavigatorCanvas::sendZoom(double zoom) {
    m_zoom = qBound(kZoomMin, zoom, kZoomMax);
    m_controller->setProperty(QStringLiteral("video-zoom"), m_zoom);
    emit zoomVisualChanged(displayPercent());
    update();
}

void NavigatorCanvas::requestZoom(double zoom) {
    if (isActive())
        sendZoom(zoom);
}

void NavigatorCanvas::sendPan(double panX, double panY) {
    m_panX = panX;
    m_panY = panY;
    m_controller->setProperty(QStringLiteral("video-pan-x"), panX);
    m_controller->setProperty(QStringLiteral("video-pan-y"), panY);
    update();
}

void NavigatorCanvas::requestReset() {
    if (!isActive())
        return;
    sendZoom(0.0);
    sendPan(0.0, 0.0);
}

void NavigatorCanvas::requestNativeSize() {
    if (!isActive() || !m_render)
        return;
    // 目标：显示尺寸 = 源分辨率。fit 保形，两轴所需 zoom 相同，取宽轴求解
    const QSize ws = m_render->size();
    const QSizeF fit = fitSize(ws, double(m_videoW) / m_videoH);
    if (fit.width() <= 0)
        return;
    // 源远大于窗时所需负 zoom 可能越 25% 下限，取下限（尽量接近）
    sendZoom(qBound(kZoomMin, std::log2(m_videoW / fit.width()), kZoomMax));
    sendPan(0.0, 0.0); // 居中；视口矩形经属性推送自动联动
}

void NavigatorCanvas::panToVideoPoint(double u, double v) {
    if (!isActive() || !m_render)
        return;
    // 反解 mpv 渲染模型：窗口中心对准归一化点 ⇔ pan = 0.5 − 目标点
    // （推导与窗口/视频比例无关），再按"画面不得完全离窗"钳制
    const QSize ws = m_render->size();
    const double aspect = double(m_videoW) / m_videoH;
    const QSizeF disp = fitSize(ws, aspect) * std::pow(2.0, m_zoom);
    const double panX = clampPan(0.5 - u, ws.width(), int(disp.width()));
    const double panY = clampPan(0.5 - v, ws.height(), int(disp.height()));
    sendPan(panX, panY);
}

void NavigatorCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !isActive()) {
        event->ignore();
        return;
    }
    const QPointF pos = event->position();
    const QRectF r = viewRect();
    if (!r.isEmpty() && r.contains(pos)) {
        m_dragging = true;
        m_dragGrabOffset = pos - r.topLeft();
    } else {
        const QRectF f = frameRect();
        const double u = qBound(0.0, (pos.x() - f.left()) / f.width(), 1.0);
        const double v = qBound(0.0, (pos.y() - f.top()) / f.height(), 1.0);
        panToVideoPoint(u, v);
    }
}

void NavigatorCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (!m_dragging)
        return;
    const QRectF f = frameRect();
    const QRectF r = viewRect();
    if (f.isEmpty() || r.isEmpty())
        return;
    // 以"期望的矩形左上角 + 矩形尺寸"反推视口中心对应的视频归一化坐标
    const QPointF desiredTopLeft = event->position() - m_dragGrabOffset;
    const double u = (desiredTopLeft.x() + r.width() / 2 - f.left()) / f.width();
    const double v = (desiredTopLeft.y() + r.height() / 2 - f.top()) / f.height();
    panToVideoPoint(u, v);
}

void NavigatorCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton)
        m_dragging = false;
}

void NavigatorCanvas::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && viewRect().contains(event->position()))
        requestReset();
    else
        event->ignore();
}

void NavigatorCanvas::wheelEvent(QWheelEvent *event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0 || !isActive()) {
        event->ignore();
        return;
    }
    // 每档约 ±10%：以 1:1 相对百分比为基准乘除，钳到 zoom 上下界对应百分比
    double pct = displayPercent();
    pct *= std::pow(1.1, steps);
    const int lo = zoomToPercent(kZoomMin), hi = zoomToPercent(kZoomMax);
    pct = qBound(double(lo), pct, double(hi));
    sendZoom(percentToZoom(int(pct + 0.5)));
}

// ---------------------------------------------------------------------------

NavigatorPanel::NavigatorPanel(MpvController *controller, QWidget *renderArea, QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint) {
    setWindowTitle(tr("Navigator"));
    setMinimumSize(200, 200);
    resize(220, 260);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    m_canvas = new NavigatorCanvas(controller, renderArea, this);
    layout->addWidget(m_canvas, 1);

    // 底部控制行：滑块（zoom 指数线性刻度）| 百分比框 | 复位
    auto *row = new QWidget;
    row->setFixedHeight(28);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);
    m_zoomSlider = new QSlider(Qt::Horizontal, row);
    m_zoomSlider->setRange(0, 5000); // (zoom+2)*1000，[-2,3] 线性映射
    m_zoomSpin = new QSpinBox(row);
    m_zoomSpin->setRange(25, 800);
    m_zoomSpin->setSuffix(QStringLiteral("%"));
    auto *nativeBtn = new QPushButton(QStringLiteral("1:1"), row);
    nativeBtn->setToolTip(
        tr("Native size: 1:1 at source resolution, centered; parts outside the window are "
           "clipped."));
    nativeBtn->setFixedWidth(36);
    // ↺（U+21BA）：复位到填充窗口，与 1:1 原始尺寸语义区分
    auto *resetBtn = new QPushButton(QStringLiteral("\u21BA"), row);
    resetBtn->setToolTip(tr("Reset view: restore window-filling zoom (mpv default)."));
    resetBtn->setFixedWidth(28);
    for (auto *w : std::initializer_list<QWidget *>{m_zoomSlider, m_zoomSpin, nativeBtn, resetBtn})
        w->setFocusPolicy(Qt::NoFocus); // C2 同款约束：Space 归全局快捷键
    rowLayout->addWidget(m_zoomSlider, 1);
    rowLayout->addWidget(m_zoomSpin);
    rowLayout->addWidget(nativeBtn);
    rowLayout->addWidget(resetBtn);
    layout->addWidget(row);

    // mpv 为唯一事实源：任何来源的缩放变化都经推送统一回刷两个控件。
    // 滑块按 zoom 线性刻度（currentZoom）；百分比框按 1:1 相对百分比，量程随
    // 当前窗口几何动态取 zoom 上下界对应百分比（避免 100% 越界不可输入）
    connect(m_canvas, &NavigatorCanvas::zoomVisualChanged, this, [this](int percent) {
        {
            QSignalBlocker b(m_zoomSlider);
            m_zoomSlider->setValue(int((m_canvas->currentZoom() + 2.0) * 1000 + 0.5));
        }
        {
            QSignalBlocker b(m_zoomSpin);
            m_zoomSpin->setRange(m_canvas->zoomToPercent(kZoomMin),
                                 m_canvas->zoomToPercent(kZoomMax));
            m_zoomSpin->setValue(percent);
        }
    });

    connect(m_canvas, &NavigatorCanvas::activeChanged, this, [this](bool active) {
        m_zoomSlider->setEnabled(active);
        m_zoomSpin->setEnabled(active);
    });
    // 按需创建（N3）：构造时即可能已有视频（从控制器缓存播种），
    // 初始可用性与缩放显示按实际状态设定，不再假设"先灰置等推送"
    const bool active = m_canvas->hasVideo();
    m_zoomSlider->setEnabled(active);
    m_zoomSpin->setEnabled(active);
    if (active) {
        const int percent = m_canvas->displayPercent();
        {
            QSignalBlocker b(m_zoomSlider); // 防回环触发 requestZoom
            m_zoomSlider->setValue(int((m_canvas->currentZoom() + 2.0) * 1000 + 0.5));
        }
        {
            QSignalBlocker b(m_zoomSpin);
            m_zoomSpin->setRange(m_canvas->zoomToPercent(kZoomMin),
                                 m_canvas->zoomToPercent(kZoomMax));
            m_zoomSpin->setValue(percent);
        }
    }

    connect(m_zoomSlider, &QSlider::valueChanged, this,
            [this](int value) { m_canvas->requestZoom(value / 1000.0 - 2.0); });
    connect(m_zoomSpin, &QSpinBox::valueChanged, this,
            [this](int percent) { m_canvas->requestZoom(m_canvas->percentToZoom(percent)); });
    connect(resetBtn, &QPushButton::clicked, m_canvas, &NavigatorCanvas::requestReset);
    connect(nativeBtn, &QPushButton::clicked, m_canvas, &NavigatorCanvas::requestNativeSize);
}

void NavigatorPanel::resetAndHide() {
    m_canvas->requestReset();
    hide();
}

void NavigatorPanel::closeEvent(QCloseEvent *event) {
    // 关闭面板 = 复位画面 + 隐藏（N1 冻结口径）；关闭后通知主窗口取消菜单勾选
    m_canvas->requestReset();
    emit panelClosed();
    event->accept();
}

#include "ChapterSlider.h"
#include "pch.h"

ChapterSlider::ChapterSlider(QWidget *parent) : QSlider(parent) {}

void ChapterSlider::setMarkTimes(const QList<double> &times) {
    if (m_markTimes == times)
        return;
    m_markTimes = times;
    m_marksLayer = QPixmap(); // 失效，下次绘制重建
    update();
}

void ChapterSlider::setDuration(double duration) {
    if (m_duration == duration)
        return;
    m_duration = duration;
    m_marksLayer = QPixmap(); // 比例变化需重建
    update();
}

double ChapterSlider::timeAtGlobalX(int globalX) const {
    if (m_duration <= 0.0)
        return -1.0;
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect groove =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
    const int span = groove.width() - handle.width();
    if (span <= 0)
        return -1.0;
    const int dx = mapFromGlobal(QPoint(globalX, 0)).x() - handle.width() / 2 - groove.left();
    const int value =
        QStyle::sliderValueFromPosition(opt.minimum, opt.maximum, qBound(0, dx, span), span);
    return m_duration * value / double(opt.maximum - opt.minimum);
}

void ChapterSlider::changeEvent(QEvent *event) {
    QSlider::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        m_marksLayer = QPixmap(); // 标记颜色取自调色板，需重建
        update();
    }
}

// 标记层预渲染：仅在标记/时长/尺寸/调色板变化时执行一次，此后每次重绘仅 blit。
// N15 实测后保留：二分定位证实标记绘制本身无辜（真凶是自绘路径中单独的手柄
// 绘制调用，见 paintEvent 注释）；预渲染作为与标记数解耦的冗余保险，使热路径
// 标记成本恒为零，即使将来标记数量激增也不受影响
void ChapterSlider::rebuildMarksLayer() {
    m_marksLayer = QPixmap(size());
    if (m_marksLayer.isNull())
        return;
    m_marksLayer.fill(Qt::transparent);
    QPainter p(&m_marksLayer);
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
    if (groove.isEmpty())
        groove = rect().adjusted(8, 0, -8, -6); // 兜底，避免样式返回空
    if (groove.isEmpty() || m_duration <= 0.0)
        return;
    QColor col = palette().color(QPalette::Highlight);
    if (!col.isValid() || col.alpha() == 0)
        col = QColor(0, 120, 215);
    p.setPen(QPen(col.darker(130), 1));
    p.setBrush(col);
    p.setRenderHint(QPainter::Antialiasing, true); // 预渲染一次，AA 成本可接受
    for (double t : m_markTimes) {
        if (t < 0.0 || t > m_duration)
            continue;
        const int x = groove.x() + int(t / m_duration * groove.width());
        // 尖端取 groove.bottom()+1：标记先画、轨道后画（单次全量绘制），
        // 避免轨道底边像素覆盖尖端（“紧贴”退为 1px 间距，视觉无感）
        int y0 = groove.bottom() + 1;
        if (y0 + 7 > height())
            y0 = height() - 8;
        if (y0 < groove.top())
            y0 = groove.bottom() + 1;
        const int cx = qBound(groove.left() + 5, x, groove.right() - 5);
        // 单三角 10×7
        QPolygon poly;
        poly << QPoint(cx, y0) << QPoint(cx - 5, y0 + 7) << QPoint(cx + 5, y0 + 7);
        p.drawPolygon(poly);
    }
}

void ChapterSlider::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    if (m_markTimes.isEmpty() || m_duration <= 0.0) {
        QSlider::paintEvent(event);
        return;
    }
    if (m_marksLayer.isNull() || m_marksLayer.size() != size())
        rebuildMarksLayer();
    QPainter p(this);
    // 标记层先画（轨道与手柄之下），随后**一次**全量绘制轨道+手柄。
    // N15 实测定论（维护者逐项注释定位）：卡顿根因是“单独调用手柄绘制”——
    // 分两次 drawComplexControl 时，仅 SC_SliderHandle 的单独调用触发
    // Vista 样式慢路径，在鼠标驱动的高频重绘下吃满单核；QSlider 原生
    // 单次全量调用无恙。标记绘制本身无辜（预渲染 blit 仅为冗余保险）
    if (!m_marksLayer.isNull())
        p.drawPixmap(0, 0, m_marksLayer);
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    opt.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
    style()->drawComplexControl(QStyle::CC_Slider, &opt, &p, this);
}

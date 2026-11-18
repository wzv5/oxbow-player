#include "pch.h"

#include "ThemedIconEngine.h"

#include "MaterialIconCatalog.h"

namespace {

// 底图缓存：键为连字名 + 设备像素尺寸。SourceIn 把底图颜色整体弃用，缓存的形状与
// alpha 因此与 tint、与主题全都正交——换配色后新造的引擎照样命中。连字名与尺寸有限、
// 内容在进程期内不变，故不设淘汰；图标栅格化只在 GUI 线程发生，不加锁。
// （typedef 是给 Q_GLOBAL_STATIC 让路：宏参里的模板逗号不被尖括号保护）
using BaseCache = QHash<QPair<QString, QSize>, QPixmap>;
Q_GLOBAL_STATIC(BaseCache, gBaseCache)

// 失败面统一画方框叉错误图标（family 空或名字排不成单字形）；空名是唯一的空图形态。
// 字形判定自足、不查清单——清单惰性到首次开对话框才解析，控制栏按钮在主窗构造期就要
// 出图。有效判据：非空、长度 ≥ 2、恰一 run 一字形（GSUB 连字恒由 ≥2 字形合并而来）
QPixmap basePixmap(const QString &name, const QSize &dev) {
    const auto key = qMakePair(name, dev);
    if (auto it = gBaseCache()->find(key); it != gBaseCache()->end())
        return it.value();

    QPixmap pm(dev);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    bool valid = false;
    if (!name.isEmpty()) {
        const QString family = MaterialIconCatalog::family();
        if (!family.isEmpty()) {
            // 大小只由 em 方格决定（该字体每字形 advance 恒 1em）；不按字形紧包围盒缩放，
            // 否则窄图标被放大、满格图标不变，整批相对视觉尺寸失稳
            QFont font(family);
            font.setPixelSize(qMin(dev.width(), dev.height()));
            // 禁字体合并：缺字时宁可判无效，不落到系统字体把连字名画成一串字母
            font.setStyleStrategy(
                QFont::StyleStrategy(font.styleStrategy() | QFont::NoFontMerging));

            // 连字替换必须走复杂排版：horizontalAdvance 不走 shaping，会把 volume_up
            // 量成 9em——行宽只从排好版的 QTextLine 取
            QTextOption option;
            option.setUseDesignMetrics(true);
            QTextLayout layout(name, font);
            layout.setTextOption(option);
            layout.beginLayout();
            QTextLine line = layout.createLine();
            line.setLineWidth(std::numeric_limits<qreal>::max() / 1024);
            layout.endLayout();

            const QList<QGlyphRun> runs = layout.glyphRuns();
            valid = name.size() >= 2 && runs.size() == 1 && runs.first().glyphIndexes().size() == 1;
            if (valid) {
                p.setRenderHint(QPainter::TextAntialiasing);
                p.setPen(Qt::black); // 底图恒不透明黑，颜色由随后的 SourceIn 整体替换
                // 横向按行宽、纵向按 ascent/descent 行盒居中
                const QPointF origin((dev.width() - line.naturalTextWidth()) / 2.0,
                                     (dev.height() - (line.ascent() + line.descent())) / 2.0);
                line.draw(&p, origin);
            }
        }
    }

    if (!name.isEmpty() && !valid) {
        // 方框叉纯 QPainter 绘制、不依赖字体，字体通路失效也画得出
        const qreal side = qMin(dev.width(), dev.height());
        const qreal margin = qMax(2.0, side * 0.12);
        const QRectF box(margin, margin, dev.width() - 2 * margin, dev.height() - 2 * margin);
        QPen pen(Qt::black, qMax(1.0, side * 0.075)); // 底图恒不透明黑，SourceIn 整体替换
        pen.setCapStyle(Qt::FlatCap);
        pen.setJoinStyle(Qt::MiterJoin);
        p.setPen(pen);
        p.drawRect(box);
        const qreal inset = box.width() * 0.28;
        p.drawLine(QPointF(box.left() + inset, box.top() + inset),
                   QPointF(box.right() - inset, box.bottom() - inset));
        p.drawLine(QPointF(box.right() - inset, box.top() + inset),
                   QPointF(box.left() + inset, box.bottom() - inset));
    }
    p.end();

    gBaseCache()->insert(key, pm);
    return pm;
}

// 单色字体图标的运行期染色引擎：取底图后在透明离屏 pixmap 上做 SourceIn，tint 的
// A 通道（再乘 Disabled 固定系数）随之乘进图标，边缘抗锯齿 alpha 原样保留；不可把
// 染色直接叠在按钮已绘背景上（SourceIn 会把背景一并乘进来）。Off / On 两态由 paint
// 按 QIcon::State 选色——QIconEngine::addPixmap 对非 QPixmapIconEngine 是空操作，
// 用它注册 On 态会静默保持 Off 色
class ThemedIconEngine : public QIconEngine {
  public:
    ThemedIconEngine(const QString &name, const QColor &offTint, const QColor &onTint)
        : m_name(name), m_offTint(offTint), m_onTint(onTint) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override {
        if (!painter->isActive() || rect.isEmpty())
            return;
        const qreal scale = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
        const QPixmap pm = rendered(rect.size(), scale, mode, state);
        painter->drawPixmap(rect, pm); // pm 为设备像素面，painter 的 DPR 变换正好 1:1
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        QPixmap pm = rendered(size, scale, mode, state);
        pm.setDevicePixelRatio(scale);
        return pm;
    }

    QIconEngine *clone() const override {
        return new ThemedIconEngine(m_name, m_offTint, m_onTint);
    }

  private:
    // 该状态 / 模式下的 tint：On 态取 onTint（未给则回落 off），Disabled 恒乘固定系数
    QColor tintFor(QIcon::Mode mode, QIcon::State state) const {
        QColor tint = state == QIcon::On && m_onTint.isValid() ? m_onTint : m_offTint;
        if (mode == QIcon::Disabled)
            tint.setAlpha(qRound(tint.alpha() * 0.4));
        return tint;
    }

    // 染色渲染：产出恰为请求逻辑尺寸 × scale 的设备像素面（DPR 由调用方记账）
    QPixmap rendered(const QSize &size, qreal scale, QIcon::Mode mode, QIcon::State state) const {
        const QSize dev(qRound(size.width() * scale), qRound(size.height() * scale));
        if (dev.isEmpty())
            return QPixmap();
        const QPixmap src = basePixmap(m_name, dev);
        QPixmap out(dev);
        out.fill(Qt::transparent);
        QPainter p(&out);
        p.drawPixmap(0, 0, src);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(out.rect(), tintFor(mode, state));
        p.end();
        return out;
    }

    QString m_name;
    QColor m_offTint;
    QColor m_onTint;
};

} // namespace

QIcon Ui::themedIcon(const QString &name, const QColor &tint) {
    return themedIcon(name, tint, QColor());
}

QIcon Ui::themedIcon(const QString &name, const QColor &offTint, const QColor &onTint) {
    return QIcon(new ThemedIconEngine(name, offTint, onTint));
}

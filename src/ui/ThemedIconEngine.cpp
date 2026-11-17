#include "pch.h"

#include "ThemedIconEngine.h"

#include "Logging.h"

namespace {

// 底图缓存：键为连字名 + 设备像素尺寸。SourceIn 把底图颜色整体弃用，缓存的形状与
// alpha 因此与 tint、与主题全都正交——换配色后新造的引擎照样命中。连字名与尺寸有限、
// 内容在进程期内不变，故不设淘汰；图标栅格化只在 GUI 线程发生，不加锁。
// （typedef 是给 Q_GLOBAL_STATIC 让路：宏参里的模板逗号不被尖括号保护）
using BaseCache = QHash<QPair<QString, QSize>, QPixmap>;
Q_GLOBAL_STATIC(BaseCache, gBaseCache)

// 字体加载单点，进程内只加一次；family 从 QFontDatabase 取实际值，不写死。失败面：
// 资源缺失记一次警告并返回空串，图标渲染为空——字体是唯一通路，没有兜底
QString materialIconFamily() {
    static const QString family = [] {
        const int id = QFontDatabase::addApplicationFont(
            QStringLiteral(":/fonts/MaterialSymbolsRounded_Filled-Regular.ttf"));
        const QStringList families =
            id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
        if (families.isEmpty()) {
            OXBOW_LOG_WARNING() << "icon font resource missing or unusable, icons render empty";
            return QString();
        }
        return families.first();
    }();
    return family;
}

// 该连字名在该尺寸下的底图（DPR 恒 1、恰为 dev 大小）；取不到不缓存，留待下次重试
QPixmap basePixmap(const QString &name, const QSize &dev) {
    const auto key = qMakePair(name, dev);
    if (auto it = gBaseCache()->find(key); it != gBaseCache()->end())
        return it.value();

    const QString family = materialIconFamily();
    if (family.isEmpty())
        return QPixmap();

    // 大小只由 em 方格决定（该字体每字形 advance 恒 1em）；不按字形紧包围盒缩放，
    // 否则窄图标被放大、满格图标不变，整批相对视觉尺寸失稳
    QFont font(family);
    font.setPixelSize(qMin(dev.width(), dev.height()));
    // 禁字体合并：缺字时宁可空，不落到系统字体把连字名画成一串字母
    font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoFontMerging));

    // 连字替换必须走复杂排版：horizontalAdvance 不走 shaping，会把 volume_up 量成
    // 9em——行宽只从排好版的 QTextLine 取
    QTextOption option;
    option.setUseDesignMetrics(true);
    QTextLayout layout(name, font);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setLineWidth(std::numeric_limits<qreal>::max() / 1024);
    layout.endLayout();

    QPixmap pm(dev);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setPen(Qt::black); // 底图恒不透明黑，颜色由随后的 SourceIn 整体替换
    // 横向按行宽、纵向按 ascent/descent 行盒居中
    const QPointF origin((dev.width() - line.naturalTextWidth()) / 2.0,
                         (dev.height() - (line.ascent() + line.descent())) / 2.0);
    line.draw(&p, origin);
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
        if (pm.isNull()) // 字体缺失或名字空：不出图，也不刷 QPainter 的空位图警告
            return;
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
        if (src.isNull())
            return QPixmap();
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

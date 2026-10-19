#include "pch.h"

#include "ThemedIconEngine.h"

namespace {

// 底图缓存：键为素材路径 + 设备像素尺寸。SourceIn 把底图颜色整体弃用，缓存的形状与
// alpha 因此与 tint、与主题全都正交——换配色后新造的引擎照样命中。素材与尺寸有限、
// 内容在进程期内不变，故不设淘汰；图标栅格化只在 GUI 线程发生，不加锁。
// （typedef 是给 Q_GLOBAL_STATIC 让路：宏参里的模板逗号不被尖括号保护）
using BaseCache = QHash<QPair<QString, QSize>, QPixmap>;
Q_GLOBAL_STATIC(BaseCache, gBaseCache)

// 该路径在该尺寸下的底图（DPR 恒 1、恰为 dev 大小）；取不到不缓存，留待下次重试
QPixmap basePixmap(const QString &svgPath, const QSize &dev) {
    const auto key = qMakePair(svgPath, dev);
    if (auto it = gBaseCache()->find(key); it != gBaseCache()->end())
        return it.value();

    QPixmap pm = QIcon(svgPath).pixmap(dev); // svg 由部署的 iconengines 插件按尺寸栅格化
    if (pm.isNull())
        return pm;
    pm.setDevicePixelRatio(1.0);
    if (pm.size() != dev)
        pm = pm.scaled(dev, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    gBaseCache()->insert(key, pm);
    return pm;
}

// 单色 SVG 素材的运行期染色引擎：取底图后在透明离屏 pixmap 上做 SourceIn，tint 的
// A 通道（再乘 Disabled 固定系数）随之乘进图标，边缘抗锯齿 alpha 原样保留；不可把
// 染色直接叠在按钮已绘背景上（SourceIn 会把背景一并乘进来）。Off / On 两态由 paint
// 按 QIcon::State 选色——QIconEngine::addPixmap 对非 QPixmapIconEngine 是空操作，
// 用它注册 On 态会静默保持 Off 色
class ThemedIconEngine : public QIconEngine {
  public:
    ThemedIconEngine(const QString &svgPath, const QColor &offTint, const QColor &onTint)
        : m_svgPath(svgPath), m_offTint(offTint), m_onTint(onTint) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override {
        if (!painter->isActive() || rect.isEmpty())
            return;
        const qreal scale = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
        const QPixmap pm = rendered(rect.size(), scale, mode, state);
        if (pm.isNull()) // 插件缺失或素材读不到：不出图，也不刷 QPainter 的空位图警告
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
        return new ThemedIconEngine(m_svgPath, m_offTint, m_onTint);
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
        const QPixmap src = basePixmap(m_svgPath, dev);
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

    QString m_svgPath;
    QColor m_offTint;
    QColor m_onTint;
};

} // namespace

QIcon Ui::themedIcon(const QString &svgPath, const QColor &tint) {
    return themedIcon(svgPath, tint, QColor());
}

QIcon Ui::themedIcon(const QString &svgPath, const QColor &offTint, const QColor &onTint) {
    return QIcon(new ThemedIconEngine(svgPath, offTint, onTint));
}

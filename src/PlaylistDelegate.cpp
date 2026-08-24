#include "pch.h"

#include "PlaylistDelegate.h"

#include "PlaylistItem.h"

namespace {
constexpr int kIconSize = 16;
constexpr int kMarginH = 6;
constexpr int kMarginV = 4;
constexpr int kItemPadding = 4;

QString formatDuration(double seconds)
{
    if (seconds < 0 || !qIsFinite(seconds))
        return QString();
    const qint64 total = qint64(seconds + 0.5);
    const qint64 h = total / 3600;
    const qint64 m = (total % 3600) / 60;
    const qint64 s = total % 60;
    // 小时恒两位起步，超 99 小时自然扩展为三位及以上
    return QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(int(m), 2, 10, QLatin1Char('0'))
        .arg(int(s), 2, 10, QLatin1Char('0'));
}
} // namespace

PlaylistDelegate::PlaylistDelegate(const QHash<QString, PlaylistItem> *meta, QObject *parent)
    : QStyledItemDelegate(parent), m_meta(meta)
{
}

void PlaylistDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                             const QModelIndex &index) const
{
    const QString path = index.data(Qt::UserRole).toString();
    const bool playing = index.data(Qt::UserRole + 1).toBool();

    painter->save();

    // 选中背景：浅灰平铺（维护者裁决，替代过重的系统高亮色）
    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, QColor(232, 232, 232));

    int x = option.rect.left() + kMarginH;
    const int cy = option.rect.center().y();

    // 图标：16px 垂直居中
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    if (!icon.isNull())
        icon.paint(painter, QRect(x, cy - kIconSize / 2, kIconSize, kIconSize));
    x += kIconSize + kItemPadding;

    QFontMetrics fm(option.font);
    // 三色规则：正在播放降饱和蓝 / 选中黑 / 其余深灰；一律不用粗体
    const QColor textColor = playing ? QColor(0x19, 0x76, 0xD2)
                                     : (option.state & QStyle::State_Selected)
                                           ? QColor(Qt::black)
                                           : QColor(0x60, 0x60, 0x60);
    painter->setFont(option.font);
    painter->setPen(textColor);

    // 序号：前导零位数随列表总数自适应，右对齐到固定宽列
    const int totalRows = qMax(index.model()->rowCount(), 1);
    int digits = 1;
    for (int n = totalRows; n >= 10; n /= 10)
        ++digits;
    const QString prefix =
        QString::number(index.row() + 1).rightJustified(digits, QLatin1Char('0')) + QLatin1Char('.');
    const int prefixW = fm.horizontalAdvance(QString(digits, QLatin1Char('0')) + QLatin1Char('.'));
    painter->drawText(QRect(x, option.rect.top(), prefixW, option.rect.height()),
                      Qt::AlignVCenter | Qt::AlignRight, prefix);
    x += prefixW + kItemPadding;

    // 时长区：已求值时占据右侧固定宽，未求值时不存在（文件名占满整行）
    const int rightEdge = option.rect.right() - kMarginH;
    int durationW = 0;
    if (m_meta) {
        const auto it = m_meta->constFind(path);
        if (it != m_meta->constEnd() && it->duration.has_value()) {
            const QString dur = formatDuration(it->duration.value());
            // 矩形右对齐绘制：值列与右缘保留一个键距的呼吸空隙
            const int adv = fm.horizontalAdvance(dur);
            durationW = adv + kItemPadding;
            painter->drawText(QRect(rightEdge - durationW, option.rect.top(),
                                    adv, option.rect.height()),
                              Qt::AlignVCenter | Qt::AlignRight, dur);
        }
    }

    // 文件名：占满剩余宽度，超长省略
    const QRect nameRect(x, option.rect.top(),
                         qMax(rightEdge - durationW - x, 0), option.rect.height());
    painter->drawText(nameRect, Qt::AlignVCenter,
                      fm.elidedText(index.data(Qt::DisplayRole).toString(),
                                    Qt::ElideRight, nameRect.width()));

    painter->restore();
}

QSize PlaylistDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);
    const int textH = QFontMetrics(option.font).height();
    return QSize(kMarginH * 2 + kIconSize + textH + kItemPadding * 2,
                 kMarginV * 2 + qMax(kIconSize, textH));
}

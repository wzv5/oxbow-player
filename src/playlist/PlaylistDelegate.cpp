#include "pch.h"

#include "PlaylistDelegate.h"

#include "PlaylistTheme.h"
#include "Util.h"
#include "entities/PlaylistEntity.h"

namespace {
constexpr int kIconSize = 16;
constexpr int kMarginH = 6;
constexpr int kMarginV = 4;
constexpr int kItemPadding = 4;

QString formatDuration(double seconds) {
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

PlaylistDelegate::PlaylistDelegate(
    std::shared_ptr<const QHash<QString, std::optional<double>>> duration, QObject *parent)
    : QStyledItemDelegate(parent), m_duration(std::move(duration)) {}

void PlaylistDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                             const QModelIndex &index) const {
    // 占位行无实体（EntityRole 为空）；实体指针经 quintptr 往返
    const auto *entity =
        reinterpret_cast<const PlaylistEntity *>(index.data(Qt::UserRole).value<quintptr>());
    const bool playing = index.data(Qt::UserRole + 1).toBool();
    const bool failed = index.data(Qt::UserRole + 5).toBool(); // N104：ErrorRole，裸数字同款口径

    painter->save();

    // 选中背景：派生浅灰平铺（维护者裁决，替代过重的系统高亮色；随主题派生）
    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, PlaylistTheme::selectionBackground(option.palette));

    int x = option.rect.left() + kMarginH;
    const int cy = option.rect.center().y();

    // 图标：16px 垂直居中
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    if (!icon.isNull())
        icon.paint(painter, QRect(x, cy - kIconSize / 2, kIconSize, kIconSize));
    x += kIconSize + kItemPadding;

    QFontMetrics fm(option.font);
    // 四色规则：播放失败红 / 正在播放蓝 / 选中 / 其余灰，随主题派生；一律不用粗体
    // （N104：错误优先于正播——两真同帧是"这条刚报 error、PlayingRole 还没等回显
    // 回落"，此刻 mpv 已跳走，红比蓝贴近事实）
    const QColor textColor = PlaylistTheme::rowForeground(option.palette, failed, playing,
                                                          option.state & QStyle::State_Selected);
    painter->setFont(option.font);
    painter->setPen(textColor);

    // 序号：前导零位数随列表总数自适应，右对齐到固定宽列
    const int totalRows = qMax(index.model()->rowCount(), 1);
    int digits = 1;
    for (int n = totalRows; n >= 10; n /= 10)
        ++digits;
    const QString prefix =
        QString::number(index.row() + 1).rightJustified(digits, QLatin1Char('0')) +
        QLatin1Char('.');
    const int prefixW = fm.horizontalAdvance(QString(digits, QLatin1Char('0')) + QLatin1Char('.'));
    painter->drawText(QRect(x, option.rect.top(), prefixW, option.rect.height()),
                      Qt::AlignVCenter | Qt::AlignRight, prefix);
    x += prefixW + kItemPadding;

    // 时长区：已求值时占据右侧固定宽，未求值时不存在（文件名占满整行）。
    // 两级取值：①mpv 推送时长仓（按实体 key 查）；②实体已缓存的媒体元数据
    // （兜底，绝不触发求值——绘制路径禁止同步解析）
    const int rightEdge = option.rect.right() - kMarginH;
    int durationW = 0;
    if (entity) {
        std::optional<double> seconds;
        if (m_duration) {
            const auto it = m_duration->constFind(entity->key());
            if (it != m_duration->constEnd())
                seconds = it.value();
        }
        if (!seconds.has_value())
            if (const auto *mi = entity->cachedMediaMeta())
                seconds = mi->duration;
        if (seconds.has_value()) {
            const QString dur = formatDuration(*seconds);
            if (!dur.isEmpty()) {
                // 矩形右对齐绘制：值列与右缘保留一个键距的呼吸空隙
                const int adv = fm.horizontalAdvance(dur);
                durationW = adv + kItemPadding;
                painter->drawText(
                    QRect(rightEdge - durationW, option.rect.top(), adv, option.rect.height()),
                    Qt::AlignVCenter | Qt::AlignRight, dur);
            }
        }
    }

    // 文件名：占满剩余宽度，超长省略
    const QRect nameRect(x, option.rect.top(), qMax(rightEdge - durationW - x, 0),
                         option.rect.height());
    painter->drawText(
        nameRect, Qt::AlignVCenter,
        fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, nameRect.width()));

    painter->restore();
}

QSize PlaylistDelegate::sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const {
    Q_UNUSED(index);
    const int textH = QFontMetrics(option.font).height();
    return QSize(kMarginH * 2 + kIconSize + textH + kItemPadding * 2,
                 kMarginV * 2 + qMax(kIconSize, textH));
}

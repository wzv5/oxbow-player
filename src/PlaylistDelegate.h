#pragma once

#include <QStyledItemDelegate>

#include <QHash>

struct PlaylistItem;

// 播放列表条目绘制（N4）：[16px 系统图标] NNN. 文件名 …… hh:mm:ss。
// 条目数据：Qt::UserRole = 文件路径（时长经此查全局元数据仓），
// Qt::UserRole+1 = 正在播放标记（由 setPlaylist 按 mpv current 写入）。
class PlaylistDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    // meta 为 PlaylistWidget 全局元数据仓的只读视图，生命周期由其保证
    explicit PlaylistDelegate(const QHash<QString, PlaylistItem> *meta, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

private:
    const QHash<QString, PlaylistItem> *m_meta; // 非拥有
};

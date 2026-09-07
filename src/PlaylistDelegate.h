#pragma once

#include "pch.h"

class PlaylistEntity;

// 播放列表条目绘制（N4/N26）：[16px 系统图标] NNN. 文件名 …… hh:mm:ss。
// 条目数据：Qt::UserRole = 实体指针（quintptr，占位行为空；时长经实体 key
// 查全局时长仓，兜底实体已缓存的媒体元数据——绝不触发求值），
// Qt::UserRole+1 = 正在播放标记（由 setPlaylist 按 mpv current 写入）。
class PlaylistDelegate : public QStyledItemDelegate {
    Q_OBJECT
  public:
    // duration 为 PlaylistWidget 全局时长仓（mpv 推送，键为规范化路径）的只读视图，
    // 生命周期由其保证
    explicit PlaylistDelegate(const QHash<QString, std::optional<double>> *duration,
                              QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

  private:
    const QHash<QString, std::optional<double>> *m_duration; // 非拥有
};

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
    // duration 为全局时长仓（mpv 推送，键为规范化路径）的只读共享视图：
    // **共享而非裸指向**——页面是面板的子对象，要晚于面板的成员析构才被销毁，
    // 指向面板成员的裸指针在这段窗口里就是悬空读；共享计数令仓活到最后一份引用
    explicit PlaylistDelegate(std::shared_ptr<const QHash<QString, std::optional<double>>> duration,
                              QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

  private:
    std::shared_ptr<const QHash<QString, std::optional<double>>> m_duration; // 共享只读
};

#pragma once

#include "pch.h"

// 加载/预读遮罩（N19）：PlaylistWidget 的子控件，覆盖整个播放列表区域（含 tab 栏）。
// 两种模式：Loading（「正在加载」，无进度行、无取消按钮）/ MetaScan（三行：
// 标题「正在读取元数据」/ 进度 `done / total` / 取消按钮）。
// 不设 WA_TransparentForMouseEvents（吞掉点击，与列表禁用双保险），
// 不设 WA_NativeWindow（AGENTS alien 陷阱，保持自绘）。
class LoadingOverlay : public QWidget {
    Q_OBJECT
  public:
    enum class Mode { Loading, MetaScan };

    explicit LoadingOverlay(QWidget *parent = nullptr);

    void begin(Mode mode, int total = 0);
    void setProgress(int done, int total);
    void finish(); // 隐藏

  signals:
    void cancelRequested();

  protected:
    void paintEvent(QPaintEvent *event) override;
    // 吞掉鼠标事件，防穿透到下层列表
    void mousePressEvent(QMouseEvent *event) override { event->accept(); }
    void mouseDoubleClickEvent(QMouseEvent *event) override { event->accept(); }
    void wheelEvent(QWheelEvent *event) override { event->accept(); }

  private:
    QLabel *m_title = nullptr;
    QLabel *m_progress = nullptr;
    QPushButton *m_cancel = nullptr;
};

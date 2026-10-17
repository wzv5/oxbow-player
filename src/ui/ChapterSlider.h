#pragma once
#include "pch.h"

class ChapterSlider : public QSlider {
    Q_OBJECT
  public:
    explicit ChapterSlider(QWidget *parent = nullptr);
    void setMarkTimes(const QList<double> &times);
    void setDuration(double duration);
    // N107：全局 X → 悬停时刻（秒）。纯几何换算，无状态、不发信号；算法与
    // QSlider 自身点击定位同口径（样式反查 groove/handle，扣滑块半宽后线性
    // 映射）。全仓悬停时刻换算只此一份：进度条 tooltip 那一路同样调它。
    // 时长未定返回 -1。
    double timeAtGlobalX(int globalX) const;

  protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    // 标记层预渲染为透明位图，热路径仅 blit——重建只在标记/时长/尺寸/
    // 调色板变化时发生（N15 实测后保留的冗余保险，见 cpp 注释）
    void rebuildMarksLayer();
    QList<double> m_markTimes;
    double m_duration = 0.0;
    QPixmap m_marksLayer; // 预渲染标记层；null 即待重建
};

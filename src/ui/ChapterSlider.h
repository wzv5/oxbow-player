#pragma once
#include "pch.h"

class ChapterSlider : public QSlider {
    Q_OBJECT
  public:
    explicit ChapterSlider(QWidget *parent = nullptr);
    void setMarkTimes(const QList<double> &times);
    void setDuration(double duration);

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

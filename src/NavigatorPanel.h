#pragma once

#include <QWidget>

class QCloseEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
class QSlider;
class QSpinBox;
class MpvController;

// 画布：绘制全画面边界与视口矩形，处理点击跳转/拖拽平移/滚轮缩放。
// 状态以 mpv 推送的属性值为准；拖拽/滚轮路径会乐观地同步本地副本，
// 避免等待属性回推造成的迟滞（回推值一致，不会跳变）。
class NavigatorCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit NavigatorCanvas(MpvController *controller, QWidget *renderArea,
                             QWidget *parent = nullptr);

    void refresh() { update(); } // 渲染区域尺寸变化时由 MainWindow 驱动重绘

    // 按需创建播种后的初始状态查询（N3）：面板据此初始化控件可用性与显示值
    bool hasVideo() const { return isActive(); }
    int zoomPercent() const;

    // 以下入口供面板控件调用；空闲态（无视频）下为空操作
    void requestZoom(double zoom);
    void requestReset();
    void requestNativeSize(); // 按源分辨率 1:1 显示并居中

signals:
    // 缩放变化（含外部/属性推送来源），percent 为 25~800 的整数百分比
    void zoomVisualChanged(int percent);
    // 有视频 ↔ 无视频，面板据此启用/灰置控件
    void activeChanged(bool active);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    bool isActive() const { return m_videoW > 0 && m_videoH > 0; }
    QRectF frameRect() const; // 全画面在画布中的内接矩形（按视频宽高比）
    QRectF viewRect() const;  // 视口矩形（画布坐标）；无视频时为空
    // 平移钳制（按轴）：超窗轴可拖且铺满不产生新黑边；小轴锁死居中
    double clampPan(double pan, int windowPx, int videoPx) const;
    void sendZoom(double zoom);      // 本地立即生效并发往 mpv
    void sendPan(double panX, double panY);
    void panToVideoPoint(double u, double v); // 把窗口视野中心对准视频归一化坐标 (u,v)

    MpvController *m_controller;
    QWidget *m_render = nullptr;    // 视频渲染区域，映射计算需要其真实尺寸
    double m_zoom = 0.0;            // video-zoom：0=填充窗口，1=2倍（2 为底指数）
    double m_panX = 0.0, m_panY = 0.0; // video-pan-x/y：画面位移占视频宽高比
    int m_videoW = 0, m_videoH = 0;
    bool m_dragging = false;
    QPointF m_dragGrabOffset;       // 按下点到视口矩形左上角的偏移
};

// 导航器面板（N1）：PS 导航器面板的无缩略图变体。
// 视口矩形直接映射 mpv 的 video-zoom / video-pan-x/y。
// 独立 Qt::Tool 浮窗、零持久化：不记忆几何与显隐，
// 每次显示由 MainWindow 定位到其右下角。
class NavigatorPanel : public QWidget
{
    Q_OBJECT
public:
    explicit NavigatorPanel(MpvController *controller, QWidget *renderArea,
                            QWidget *parent = nullptr);

    // 复位画面（恢复填充窗口）并隐藏；菜单取消勾选与系统关闭钮共用该语义
    void resetAndHide();

    // 主窗口尺寸/位置变化时驱动：重绘视口映射
    void refresh() { m_canvas->refresh(); }

signals:
    // 用户点了系统关闭钮：MainWindow 据此取消视图菜单勾选
    void panelClosed();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    NavigatorCanvas *m_canvas = nullptr;
    QSlider *m_zoomSlider = nullptr;
    QSpinBox *m_zoomSpin = nullptr;
};

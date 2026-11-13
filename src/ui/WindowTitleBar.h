#pragma once

#include "pch.h"

// N156：主窗自绘标题栏（三态组件）。宿主窗指针显式传入（QWidget *，不取
// MainWindow *）：标题绘制时现读该指针的 windowTitle()、不缓存；
// WindowTitleChange / WindowStateChange 过滤器也装在该宿主窗上。全屏浮窗重挂后
// window() 会指向浮窗，显式指针把"宿主窗"与 window() 解耦。
// 组件与全部按钮 NoFocus（N117），不入 Tab 链；颜色绘制时现取
// （AppearanceManager），无缓存。标题与 glyph 按宿主激活态分档，激活事件经
// 宿主指针上的过滤器驱动重画。三态：Normal（图标 + 省略标题 + 三控制按钮 +
// 菜单按钮）/ Mini（省略标题 + 仅关闭按钮）/ Floating（标题 + 退出全屏 + 关闭）。
class WindowTitleBar : public QWidget {
    Q_OBJECT
  public:
    enum class Mode { Normal, Mini, Floating };

    explicit WindowTitleBar(QWidget *hostWindow, QWidget *parent = nullptr);
    ~WindowTitleBar() override;

    // 三态收口：高度与按钮配置（Normal 32 / Mini miniTitleBarHeight / Floating 32）
    void setMode(Mode mode);
    // 菜单按钮显隐下发（仅 Normal 态生效；Mini / Floating 恒关）——主窗
    // applyFrame 在 Normal 态按 m_menuBarVisibleTruth 调用，组件不读主窗
    // 私有成员
    void setMenuButtonVisible(bool visible);
    // 最大化 glyph 当前形态的共用判据（绘制分支与测试断言同源）：宿主最大化即真
    bool maximizeGlyphRestore() const;
    // N156：Windows 自绘最大化钮的 NC 命中区。仅 Normal 态且按钮可见时非空，
    // 坐标为标题栏本地；主窗据此在 WM_NCHITTEST 答 HTMAXBUTTON。该区域答成
    // NC 后 Qt 收不到这片的鼠标事件，悬停 / 按下态由主窗从 NC 消息回灌
    QRect maximizeButtonRect() const;
    // N156：系统标题栏信息（WM_GETTITLEBARINFOEX）用的最小化 / 关闭钮矩形，
    // 同为本组件坐标；按钮不可见时为空
    QRect minimizeButtonRect() const;
    QRect closeButtonRect() const;
    void setMaximizeButtonHovered(bool hovered);
    void setMaximizeButtonDown(bool down);
    bool maximizeButtonDown() const;
    // 显式宿主窗指针（诊断与测试断言用：重挂进浮窗后 window() 会指向浮窗，宿主
    // 指针不变——这正是显式指针存在的意义）
    QWidget *hostWindow() const { return m_hostWindow; }

  signals:
    void minimizeRequested();
    // Normal 态的双击 / 按钮与 Floating 态的退出全屏按钮同发——语义分流
    // （最大化·还原 / toggleFullscreen）由 MainWindow 按当前态决定
    void maximizeRestoreRequested();
    void closeRequested();
    void menuRequested(const QPoint &globalPos);
    // 标题区拖动三拍：移动超 startDragDistance 首发按下点，随后逐条上报移动，
    // 释放收口。组件不自判原生 / 自驱动，统一交主窗 dispatchGesture（2.7）；
    // 原生路径下循环接管指针后组件不再有事件，多余信号由主窗按环路活性吸收
    void dragMoveRequested(const QPoint &globalPos);
    void dragMoveUpdated(const QPoint &globalPos);
    void dragMoveFinished();
    void systemMenuRequested(const QPoint &globalPos);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    class TitleBarButton;
    void applyModeLayout();
    // 标题绘制矩形（避开右侧按钮区）
    QRect titleRect() const;
    // 宿主图标矩形（仅 Normal 态绘制；单击弹系统菜单、双击关闭）
    QRect iconRect() const;
    QColor titleForeground(const QPalette &p) const;
    // 最大化钮 tooltip 按宿主窗口态刷新（tooltip 是缓存文案，无法绘制时现取）；
    // 仅 Normal 态生效，形态收口与 WindowStateChange 两处共用
    void updateMaximizeButtonTooltip();

    QWidget *m_hostWindow = nullptr;
    Mode m_mode = Mode::Normal;
    bool m_menuButtonVisible = false; // Normal 态下菜单按钮的显示意愿（下发值）
    QIcon m_icon;                     // 宿主 exe 图标（构造期取一次，与 AboutDialog 同源）
    TitleBarButton *m_minButton = nullptr;
    TitleBarButton *m_maxButton = nullptr;
    TitleBarButton *m_closeButton = nullptr;
    TitleBarButton *m_menuButton = nullptr;
    TitleBarButton *m_exitFsButton = nullptr;
    bool m_dragArmed = false; // 本次按住是否由标题栏自身接收（子按钮让出 move 时不拖窗）
    bool m_dragSent = false;  // 本次按住是否已跨过拖动阈值（跨过前 move 不上报）
    QPoint m_pressGlobalPos;  // 按下点全局坐标（dragMoveRequested 的载荷）
};

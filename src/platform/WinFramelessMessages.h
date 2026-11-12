#pragma once

#include "pch.h"

class WindowTitleBar;

// N156：主窗无框形态的 Windows 原生消息层（WinFrameless 的消息侧——后者管 style /
// placement / 系统菜单的下发，本类管 WM_* 的回答）。与 X11Util / WinFrameless 同纪律：
// 源内无平台守卫，本单元是否参与构建只由 CMakeLists 的 WIN32 段决定；调用方
// （MainWindow）的调用点自持 #ifdef Q_OS_WIN。
//
// 两个入口对应 Qt 的两趟原生消息通道：
//   nativeEventFilter —— 应用级过滤器（自装自拆，TaskbarIntegration 同款），收子 HWND
//     的 WM_NCHITTEST / WM_SETCURSOR 与最大化钮的 NC 鼠标 / leave；
//   handleMessage —— 主窗窗过程那趟（MainWindow::nativeEvent 转发），收 WM_NCCALCSIZE /
//     WM_GETTITLEBARINFOEX / WM_NCHITTEST / WM_GETMINMAXINFO 与 mini 原生循环的
//     WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE / WM_SIZING / WM_MOVING。
// 窗口几何 / DPR / 标题栏矩形等经具体指针（QWidget / WindowTitleBar）直读；只有主窗
// 私有的窗口态真相与动作经 Host 接口取（fullscreen / mini / videoRatio /
// toggleMaximizeRestore）。mini 原生拖动的未吸附锚点是本层私有状态，随层持有。
class WinFramelessMessages : public QObject, public QAbstractNativeEventFilter {
  public:
    // 宿主真相与动作面（MainWindow 实现，全部 GUI 线程）。窗口态真相只在主窗
    // （m_fullscreen / m_mini），本层不另持一份
    class Host {
      public:
        virtual ~Host() = default;
        virtual bool fullscreen() const = 0;
        virtual bool mini() const = 0;
        // 视频宽高比（无流由主窗给 16:9，与进入路径同源）
        virtual double videoRatio() const = 0;
        virtual void toggleMaximizeRestore() = 0;
    };

    WinFramelessMessages(QWidget *mainWindow, WindowTitleBar *titleBar, Host *host,
                         QObject *parent = nullptr);
    ~WinFramelessMessages() override;

    // 提前拆应用级过滤器（幂等）。必须在主窗析构开头调用：本层是主窗子对象，析构
    // 落在 ~QObject 阶段（晚于 ~MainWindow 体），而 Host 基类 vtable 在 ~MainWindow
    // 体结束后即切片——过滤器若留到那时才拆，析构期收到的原生消息经 m_host 虚调用即崩
    void detach();

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
    // 窗过程那趟的消息回答；true = 已处理（吞掉），false = 交回基线。message 是 MSG*
    // （头文件不引 Win32 类型，与 WinFrameless 的 unsigned long long 入参同纪律）
    bool handleMessage(void *message, qintptr *result);

    // 跨形态残留清理：mini 原生拖动的未吸附锚点作废（toggleMini 进出两侧调用，
    // 杜绝下一轮拖动按旧基准判距）
    void clearDragAnchors();

  private:
    // 自绘最大化钮的 NC 命中区（主窗坐标点是否落在按钮上；仅 Normal 态、未全屏 /
    // 迷你、标题栏在主窗内时成立）
    bool maximizeButtonHit(const QPoint &windowPos) const;

    QWidget *m_main;
    WindowTitleBar *m_titleBar;
    Host *m_host;
    // N68：mini 拖动期的未吸附基准（物理像素，仅 GUI 线程在拖动模态循环内读写）。
    // 吸附结果不得作为下一条 WM_MOVING 的判定基准——系统给的下条矩形建立在上一次
    // 已吸附的位置上，据此判距会把每轮位移又拉回贴齐位，慢速拖动永远脱不开（实测）。
    // 故在 WM_ENTERSIZEMOVE 记下起点矩形与光标，整轮拖动只用"起点 + 鼠标总位移"作原始位
    QRect m_dragOriginRect;
    QPoint m_dragCursorOrigin;
};
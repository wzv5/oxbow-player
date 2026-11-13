#pragma once

#include "pch.h"

class WindowTitleBar;
class WindowStateHost;

// Windows 原生消息层：nativeEventFilter 收子 HWND 那趟，handleMessage 收主窗窗过程那趟。
// 窗口态真相与动作经 WindowStateHost 接口取；未吸附锚点是本层私有状态
class WinFramelessMessages : public QObject, public QAbstractNativeEventFilter {
  public:
    WinFramelessMessages(QWidget *mainWindow, WindowTitleBar *titleBar, WindowStateHost *host,
                         QObject *parent = nullptr);
    ~WinFramelessMessages() override;

    // 幂等。析构晚于主窗函数体，届时 Host vtable 已切片；须提前拆过滤器，否则析构期
    // 原生消息经虚调用崩溃
    void detach();

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
    // true = 已吞
    bool handleMessage(void *message, qintptr *result);

    // 作废未吸附锚点，避免下一轮拖动按旧基准判距
    void clearDragAnchors();

  private:
    bool maximizeButtonHit(const QPoint &windowPos) const;

    QWidget *m_main;
    WindowTitleBar *m_titleBar;
    WindowStateHost *m_host;
    // 吸附结果不得作下一条 WM_MOVING 的判距基准，否则慢速拖动永远脱不开；
    // 整轮拖动只用"起点 + 鼠标总位移"
    QRect m_dragOriginRect;
    QPoint m_dragCursorOrigin;
};
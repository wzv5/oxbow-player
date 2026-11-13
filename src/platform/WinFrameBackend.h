#pragma once

#include "pch.h"

#include "FrameBackend.h"

class WindowStateHost;
class WindowTitleBar;
class WinFramelessMessages;

class WinFrameBackend final : public FrameBackend {
  public:
    WinFrameBackend(QWidget *window, WindowTitleBar *titleBar, WindowStateHost *host);

    void applyCreationFixups() override;
    void setRounded(bool rounded) override;
    bool roundedCorners() const override;
    void showSystemMenu(const QPoint &globalPos) override;
    QRect normalPlacementRect() const override;
    bool restoreGeometryDeferred() const override;
    void setPlacementAtomic(const QRect &normalRect, bool maximized) override;
    void enterFullscreenNative() override;
    void exitFullscreenNative() override;
    bool handleMessage(void *message, qintptr *result) override;
    void clearDragAnchors() override;
    void detach() override;
    bool nativeGestures() const override;
    void beginSystemMove(const QPoint &globalPos) override;
    void beginSystemResize(Qt::Edges edges, const QPoint &globalPos) override;
    void enterMiniShell(bool fromMaximized) override;
    void exitMiniShell() override;

  private:
    void applyNativeRect(const QRect &rect);

    QWidget *m_window;
    WindowStateHost *m_host;
    // DWM 圆角是否生效
    bool m_rounded = false;
    // 全屏期是否临时去掉窗口框
    bool m_fullscreenFrameless = false;
    std::unique_ptr<WinFramelessMessages> m_messages;
};

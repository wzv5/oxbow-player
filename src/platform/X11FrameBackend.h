#pragma once

#include "pch.h"

#include "FrameBackend.h"

class WindowStateHost;

class X11FrameBackend final : public FrameBackend {
  public:
    X11FrameBackend(QWidget *window, WindowStateHost *host);

    void enterFullscreenNative() override;
    void exitFullscreenNative() override;
    void applyTopmost(bool topmost) override;
    bool restoreGeometryDeferred() const override;
    bool nativeGestures() const override;
    void beginSystemMove(const QPoint &globalPos) override;
    void beginSystemResize(Qt::Edges edges, const QPoint &globalPos) override;
    void enterMiniShell(bool fromMaximized) override;
    void exitMiniShell() override;

  private:
    QWidget *m_window;
    WindowStateHost *m_host;
};

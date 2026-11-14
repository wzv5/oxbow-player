#pragma once

#include "pch.h"

class WindowStateHost;
class WindowTitleBar;

// 窗框平台后端基类；默认实现即「本平台没有额外动作」
class FrameBackend {
  public:
    virtual ~FrameBackend() = default;

    // 创建期补丁：style 族与常态圆角
    virtual void applyCreationFixups() {}
    virtual void setRounded(bool /*rounded*/) {}
    virtual bool roundedCorners() const { return false; }

    virtual void showSystemMenu(const QPoint & /*globalPos*/) {}

    // workspace 逻辑坐标；失败返回空矩形
    virtual QRect normalPlacementRect() const { return {}; }
    // true = 还原矩形分拍抵达，须等落定
    virtual bool restoreGeometryDeferred() const { return false; }
    virtual void setPlacementAtomic(const QRect & /*normalRect*/, bool /*maximized*/) {}
    // 最大化来源专用；目标矩形由后端自取所在屏
    virtual void enterFullscreenNative() {}
    virtual void exitFullscreenNative() {}

    virtual void applyTopmost(bool /*topmost*/) {}

    // true = 已吞
    virtual bool handleMessage(void * /*message*/, qintptr * /*result*/) { return false; }
    virtual void clearDragAnchors() {}
    virtual void detach() {}

    virtual bool nativeGestures() const { return false; }
    virtual void beginSystemMove(const QPoint & /*globalPos*/) {}
    virtual void beginSystemResize(Qt::Edges /*edges*/, const QPoint & /*globalPos*/) {}

    virtual void enterMiniShell(bool /*fromMaximized*/) {}
    virtual void exitMiniShell() {}
};

std::unique_ptr<FrameBackend> createFrameBackend(QWidget *window, WindowTitleBar *titleBar,
                                                 WindowStateHost *host);

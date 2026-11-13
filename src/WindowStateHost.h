#pragma once

// 主窗窗口态真相与动作面
class WindowStateHost {
  public:
    virtual ~WindowStateHost() = default;
    virtual bool fullscreen() const = 0;
    virtual bool mini() const = 0;
    virtual double videoRatio() const = 0;
    virtual void toggleMaximizeRestore() = 0;
};

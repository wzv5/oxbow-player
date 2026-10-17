#pragma once

#include "pch.h"

// N107：缩略图宿主窗——第二个 mpv core 的 VO 直接渲染进它的原生气窗（wid）。
// 只负责窗口形态与背景，不做任何绘制逻辑、不接任何事件：
// - 顶层工具窗：不进任务栏与 Alt-Tab，无框；
// - 不取焦点（WS_EX_NOACTIVATE 口径，点击也不激活）；
// - 背景置黑：出画前（show 先于 loadfile 是 N103 硬约束的必然结果）以黑底盖过
//   空窗，不引入整窗 alpha；
// - WA_NativeWindow 预先强制原生化：mpv 需要 HWND，且必须在 loadfile 之前存在。
// mpv 会在其上自建子 HWND 承载画面（与主窗视频区同款）；Qt 画的黑底只在
// VO 子窗出现前可见。
class ThumbWindow : public QWidget {
  public:
    explicit ThumbWindow(QWidget *parent = nullptr);

    // wid 用原生句柄；调用即保证原生窗口已创建（幂等）
    WId hostWid();
};

#pragma once

#include "pch.h"

// N127：任务栏表现面的纯推导——不碰 Qt 对象、不碰 Windows 类型，便于单测。
// - 空闲优先：无文件即无进度、无徽标、三键置灰，暂停位不参与；
// - 非空闲时徽标恒挂，语义由暂停位单选（暂停 / 播放）；
// - 时长未知（<=0 或非有限值）不显示进度，其余表现照常；
// - 比例按 time-pos / duration 钳制到 [0,1]，负值与越界一律夹回；
// - time-pos 非有限值按 0 处理。
enum class TaskbarProgress { None, Normal, Paused };

struct TaskbarState {
    TaskbarProgress progress = TaskbarProgress::None;
    double fraction = 0.0;       // 0..1；progress == None 时无意义
    bool showBadge = false;      // 覆盖图标是否挂
    bool badgePaused = false;    // 徽标语义：true = 暂停、false = 播放
    bool buttonsEnabled = false; // 预览工具条三键是否可用
};

inline TaskbarState deriveTaskbarState(bool idle, bool paused, double timePos, double duration) {
    TaskbarState state;
    if (idle)
        return state;

    state.showBadge = true;
    state.badgePaused = paused;
    state.buttonsEnabled = true;

    if (!std::isfinite(duration) || duration <= 0.0)
        return state;

    const double pos = std::isfinite(timePos) ? std::clamp(timePos, 0.0, duration) : 0.0;
    state.progress = paused ? TaskbarProgress::Paused : TaskbarProgress::Normal;
    state.fraction = pos / duration;
    return state;
}

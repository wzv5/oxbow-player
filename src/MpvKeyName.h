#pragma once

#include "pch.h"

// N87：Qt 按键事件 → mpv 键名（input.conf 命名法）的翻译单点。插件未认领的按键
// 据此回落 mpv，故"取不到名字"必须是显式的空串——调用方按空串跳过转发。
namespace MpvKey {

// 纯修饰键按下、IME 组合期、以及特殊键表与码点判据都覆盖不到的键值返回空串。
// 宁可不转发，不可发错键名：错的键名会在 mpv 侧触发用户自己的绑定。
QString nameFromEvent(const QKeyEvent *event);

} // namespace MpvKey

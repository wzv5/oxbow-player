#pragma once

#include "pch.h"

// N87：Qt 按键事件 → mpv 键名（input.conf 命名法）的翻译单点。插件未认领的按键
// 据此回落 mpv，故"取不到名字"必须是显式的空串——调用方按空串跳过转发。
namespace MpvKey {

// 纯修饰键按下、IME 组合期、以及特殊键表与码点判据都覆盖不到的键值返回空串。
// 宁可不转发，不可发错键名：错的键名会在 mpv 侧触发用户自己的绑定。
QString nameFromEvent(const QKeyEvent *event);

// N89：mpv 键名 ↔ QKeySequence 与归一化——键位编辑器的键列录制（写盘）、载入
// 反解（解不出即显示留空）与同键分组 / 跨源比对共用。
// "解不出"一律交可判别的空值：序列交空 QKeySequence（数据层保留原文）、归一化
// 名交原串（解不出的串自成一组，永不与内置集匹配）。

// QKeySequence → mpv 键名：逐段 count() / operator[] 取值后以 '-' 连接。mpv 的
// 多键序列分隔是 '-'，与 Qt 显示串的 ", " 不同形，不得解析显示串。
// 不做归一化（写盘形态交 mpv 启动时自行归一；比对侧一律过 normalizedKeyName）。
QString nameFromSequence(const QKeySequence &sequence);

// mpv 键名 → QKeySequence：多段反解。Qt 的 QKeySequence 至多 4 段（mpv 上限 16），
// 超出的必然落"解不出"。任一段解不出 → 空 QKeySequence（MBTN_* / WHEEL_* /
// JOY_*、表外符号键、非法键名行、0x<hex> 与非 ASCII 裸字符都在这一支）。
QKeySequence sequenceFromName(const QString &name);

// mpv 键名 → 归一化名：'-' 切分（"-" 键自身写作 "--"，切分成对识别）、修饰名与
// 主键名大小写不敏感、修饰按 mpv 回吐的固定顺序（Shift、Ctrl、Alt、Meta）重排、
// ASCII 区间 Shift 折进（字母进大小写：Shift+v ≡ V；其余丢弃：Shift+2 ≡ 2、
// Shift+SPACE ≡ SPACE）。任一段解不出 → 原串原样返回。
QString normalizedKeyName(const QString &name);

} // namespace MpvKey

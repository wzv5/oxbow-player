#pragma once

#include "pch.h"

// N87：Qt 按键事件 → mpv 键名（input.conf 命名法）的翻译单点。插件未认领的按键
// 据此回落 mpv，故"取不到名字"必须是显式的空串——调用方按空串跳过转发。
namespace MpvKey {

// 纯修饰键按下、IME 组合期、以及特殊键表与码点判据都覆盖不到的键值返回空串。
// 宁可不转发，不可发错键名：错的键名会在 mpv 侧触发用户自己的绑定。
QString nameFromEvent(const QKeyEvent *event);

// N157：鼠标按键事件 → mpv 键名（带修饰前缀）。ExtraButton4–14 映 MBTN9–19，
// 其余表外按钮交空串，调用方整事件跳过转发。
QString nameFromMouseEvent(const QMouseEvent *event);

// N157：滚轮键名（带修饰前缀）。horizontal 选左右族，forward 选正向。
QString wheelName(bool horizontal, bool forward, Qt::KeyboardModifiers mods);

// N89：mpv 键名 ↔ QKeySequence 与归一化——键列录制与同键分组 / 跨源比对共用；
// 载入判合法性与键列标红走 isValidKeyName（N157）。
// "解不出"一律交可判别的空值：序列交空 QKeySequence、归一化名交原串（自成一组）。

// QKeySequence → mpv 键名：逐段 count() / operator[] 取值后以 '-' 连接。mpv 的
// 多键序列分隔是 '-'，与 Qt 显示串的 ", " 不同形，不得解析显示串。
// 不做归一化（写盘形态交 mpv 启动时自行归一；比对侧一律过 normalizedKeyName）。
QString nameFromSequence(const QKeySequence &sequence);

// mpv 键名 → QKeySequence：多段反解。Qt 的 QKeySequence 至多 4 段（mpv 上限 16），
// 超出的必然落"解不出"。任一段解不出 → 空 QKeySequence（MBTN_* / WHEEL_* /
// JOY_*、表外符号键、非法键名行、0x<hex> 与非 ASCII 裸字符都在这一支）。
QKeySequence sequenceFromName(const QString &name);

// mpv 键名 → 归一化名：'-' 切分（"-" 键自身写作 "--"）、修饰名与主键名大小写不
// 敏感、修饰按 mpv 顺序重排、ASCII 区间 Shift 折进（Shift+v ≡ V、Shift+2 ≡ 2）。
// 段级两路取名：码点路径解不出的段查名字层镜像（鼠标键 / 别名 / 媒体键名），仍
// 解不出的段交原文（自成一组，永不与内置集匹配）。
QString normalizedKeyName(const QString &name);

// N157：键名合法性——段数 ≤ 16，逐段剥修饰后基名须为镜像表名、单个码点或 0x 形态，
// 口径同 mpv `mp_input_get_key_from_name`。空串恒 false。
bool isValidKeyName(const QString &name);

// N157：补全候选——镜像表 canonical 名的去重保序清单，构造一次缓存。
QStringList keyNameCandidates();

} // namespace MpvKey

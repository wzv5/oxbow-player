#include "pch.h"

#include "MpvKeyName.h"

namespace {

// 特殊键表：只收非文本键——字母数字符号一律另有取名路径，不进本表（否则物理键位会截走
// 布局翻译的结果，德语 QWERTZ 的 z 就成了 y）。拼写即 mpv key_names[] 的第二列；注意
// Qt 的 Key_Enter 是小键盘回车（主回车为 Key_Return），故映 KP_ENTER 而非 ENTER。
// N89 起数据化并服务正反两个方向：正向（键值 → 名）服务取名，反向服务反解与归一化
struct NamedKey {
    int key;
    const char *name;
};

constexpr NamedKey kSpecialKeys[] = {
    {Qt::Key_Escape, "ESC"},
    {Qt::Key_Return, "ENTER"},
    {Qt::Key_Enter, "KP_ENTER"},
    {Qt::Key_Space, "SPACE"},
    {Qt::Key_Tab, "TAB"},
    {Qt::Key_Backtab, "TAB"}, // mpv 无 BACKTAB，反向靠 Shift 位（调用方补）
    {Qt::Key_Backspace, "BS"},
    {Qt::Key_Delete, "DEL"},
    {Qt::Key_Insert, "INS"},
    {Qt::Key_PageUp, "PGUP"},
    {Qt::Key_PageDown, "PGDWN"},
    {Qt::Key_Home, "HOME"},
    {Qt::Key_End, "END"},
    {Qt::Key_Left, "LEFT"},
    {Qt::Key_Right, "RIGHT"},
    {Qt::Key_Up, "UP"},
    {Qt::Key_Down, "DOWN"},
    {Qt::Key_Print, "PRINT"},
    {Qt::Key_Pause, "PAUSE"},
    {Qt::Key_Menu, "MENU"},
    // 媒体键。名字按 mpv 的媒体段（MM_BASE 偏移表）逐字对应；`PLAY` 与 `PLAYPAUSE`
    // 是两个键，Qt 的 MediaPause 单列，故 MediaTogglePlayPause 才映 PLAYPAUSE
    {Qt::Key_MediaPlay, "PLAY"},
    {Qt::Key_MediaPause, "PAUSE"},
    {Qt::Key_MediaTogglePlayPause, "PLAYPAUSE"},
    {Qt::Key_MediaStop, "STOP"},
    {Qt::Key_MediaNext, "NEXT"},
    {Qt::Key_MediaPrevious, "PREV"},
    {Qt::Key_MediaRecord, "RECORD"},
    {Qt::Key_AudioRewind, "REWIND"},
    {Qt::Key_AudioForward, "FORWARD"},
    {Qt::Key_VolumeUp, "VOLUME_UP"},
    {Qt::Key_VolumeDown, "VOLUME_DOWN"},
    {Qt::Key_VolumeMute, "MUTE"},
    {Qt::Key_LaunchMail, "MAIL"},
    // `#` 无需在此列：mpv 的 SHARP 就是 { '#', "SHARP" } 那个码点项的别名，而 Qt 的
    // '#' 键值是 Key_NumberSign = 0x23，落在码点取名路径上直接交出 '#'（解析同值）
};

// 小键盘段：Qt 的小键盘 key() 与主键盘同值（数字就是 Key_0–Key_9、句点是 Key_Period），
// 唯一区别是 KeypadModifier——故本道判定必须排在所有取名路径之前，晚一步 KP5 就被压成
// 字符 '5'。KP_DEC（小数点）与 KP_DEL（102 键盘的逗号）在 mpv 侧是两个键，不可互换
constexpr NamedKey kKeypadKeys[] = {
    {Qt::Key_Period, "KP_DEC"},     {Qt::Key_Comma, "KP_DEL"},
    {Qt::Key_Plus, "KP_ADD"},       {Qt::Key_Minus, "KP_SUBTRACT"},
    {Qt::Key_Slash, "KP_DIVIDE"},   {Qt::Key_Asterisk, "KP_MULTIPLY"},
    {Qt::Key_Enter, "KP_ENTER"},    {Qt::Key_Home, "KP_HOME"},
    {Qt::Key_End, "KP_END"},        {Qt::Key_PageUp, "KP_PGUP"},
    {Qt::Key_PageDown, "KP_PGDWN"}, {Qt::Key_Insert, "KP_INS"},
    {Qt::Key_Delete, "KP_DEL"},     {Qt::Key_Left, "KP_LEFT"},
    {Qt::Key_Right, "KP_RIGHT"},    {Qt::Key_Up, "KP_UP"},
    {Qt::Key_Down, "KP_DOWN"},
    // numlock 关时的 KP5（mpv 侧名 KP_BEGIN）不映：Qt 无 Key_Begin 枚举，实际报
    // Key_Clear / Key_Select 一族且两平台不一——发错键名比不发更糟，故不接
};

QString specialName(int key) {
    for (const NamedKey &e : kSpecialKeys)
        if (e.key == key)
            return QString::fromLatin1(e.name);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    return QString();
}

QString keypadName(int key) {
    for (const NamedKey &e : kKeypadKeys)
        if (e.key == key)
            return QString::fromLatin1(e.name);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return QStringLiteral("KP%1").arg(key - Qt::Key_0);
    return QString();
}

// 按码点取名：Qt 枚举里可打印 ASCII 的取值就是该字符本身，特殊键全在 0x01000000 段、不会
// 被本判据误吞。字母枚举值恒为大写，而 mpv 的归一化只升不降（Shift+a 提成 A，却不把 A
// 降回 a），故无 Shift 时必须转小写，否则用户绑的小写字母与 ctrl+a 一类组合键集体落空
QString charName(int key, bool shifted) {
    const int masked = key & ~int(Qt::KeyboardModifierMask);
    if (masked < 0x20 || masked > 0x7E)
        return QString();
    const int c = (!shifted && masked >= 0x41 && masked <= 0x5A) ? masked + 0x20 : masked;
    return QString(QChar(char16_t(c)));
}

// 主键名（无修饰前缀）→ 键码，可携带 KeypadModifier / ShiftModifier 位：
// 小写字母 → 字母键无 Shift（mpv 侧小写字符），大写字母 → 字母键带 Shift
// （mpv 侧大写字符，见 mpv mp_normalize_keycode 的 Shift 折进）。
// 符号键名大小写不敏感（手册：Symbolic key names are case-insensitive）。
// 豁免面（N89 §3.2）：0x<hex> 写法与非 ASCII 裸字符不作反解。解不出交 nullopt。
std::optional<int> keyFromBaseName(const QString &name) {
    if (name.isEmpty())
        return std::nullopt;
    if ((name.front() == QLatin1Char('F') || name.front() == QLatin1Char('f')) &&
        name.size() >= 2) {
        bool ok = false;
        const int n = name.mid(1).toInt(&ok);
        if (ok && n >= 1 && n <= 24)
            return int(Qt::Key_F1) + n - 1;
    }
    for (const NamedKey &e : kSpecialKeys)
        if (name.compare(QLatin1String(e.name), Qt::CaseInsensitive) == 0)
            return e.key;
    for (const NamedKey &e : kKeypadKeys)
        if (name.compare(QLatin1String(e.name), Qt::CaseInsensitive) == 0)
            return e.key | int(Qt::KeypadModifier);
    // 小键盘数字与 F 族都是生成式名（keypadName 交 KP%1），未逐项登记在表里
    if (name.size() == 3 &&
        (name.front() == QLatin1Char('K') || name.front() == QLatin1Char('k')) &&
        (name.at(1) == QLatin1Char('P') || name.at(1) == QLatin1Char('p'))) {
        const int d = int(name.at(2).unicode()) - int(u'0');
        if (d >= 0 && d <= 9)
            return (int(Qt::Key_0) + d) | int(Qt::KeypadModifier);
    }
    // 字符别名里只有 SHARP 不走码点反向（"#" 的表名；SPACE 等其余别名已在 kSpecialKeys）
    if (name.compare(QLatin1String("SHARP"), Qt::CaseInsensitive) == 0)
        return int(Qt::Key_NumberSign);
    if (name.size() == 1) {
        const char16_t c = name.front().unicode();
        if (c >= 0x20 && c <= 0x7E) {
            if (c >= 'a' && c <= 'z')
                return int(Qt::Key_A) + (c - 'a');
            if (c >= 'A' && c <= 'Z')
                return (int(Qt::Key_A) + (c - 'A')) | int(Qt::ShiftModifier);
            return int(c);
        }
    }
    return std::nullopt;
}

// 单段（一个组合键内的一个按键名）→ Qt 键组合（键值 | 修饰位）。修饰名与主键名
// 均大小写不敏感，修饰先按 '+' 逐个剥离（与 mpv mp_input_get_key_from_name 同构）；
// "+" 字面量特例（"Ctrl++" → Ctrl + '+' 键）。任一步解不出交 nullopt。
std::optional<int> codeFromSegment(const QString &seg) {
    int mods = 0;
    QString s = seg;
    while (true) {
        const qsizetype p = s.indexOf(QLatin1Char('+'));
        if (p < 0)
            break;
        const QString prefix = s.left(p);
        if (prefix.compare(QLatin1String("Shift"), Qt::CaseInsensitive) == 0) {
            mods |= int(Qt::ShiftModifier);
        } else if (prefix.compare(QLatin1String("Ctrl"), Qt::CaseInsensitive) == 0) {
            mods |= int(Qt::ControlModifier);
        } else if (prefix.compare(QLatin1String("Alt"), Qt::CaseInsensitive) == 0) {
            mods |= int(Qt::AltModifier);
        } else if (prefix.compare(QLatin1String("Meta"), Qt::CaseInsensitive) == 0) {
            mods |= int(Qt::MetaModifier);
        } else if (s == QLatin1String("+")) {
            return int(Qt::Key_Plus) | mods; // 字面 '+' 的特例
        } else {
            return std::nullopt; // 非修饰名的 '+' → 非法
        }
        s = s.mid(p + 1);
    }
    const std::optional<int> base = keyFromBaseName(s);
    if (!base)
        return std::nullopt;
    return *base | mods;
}

// 组合键切分：mpv 以 '-' 分隔多键序列，'-' 键自身写作 "--"（切分成对识别）。
// 孤立的段首 '-'（畸形）并入下段、交由段解析判失败，不产空段。
QStringList splitCombo(const QString &name) {
    QStringList segs;
    qsizetype start = 0;
    qsizetype i = 0;
    while (i < name.size()) {
        if (name.at(i) != QLatin1Char('-')) {
            ++i;
            continue;
        }
        if (i == start) {
            if (i + 1 < name.size() && name.at(i + 1) == QLatin1Char('-')) {
                segs.append(QStringLiteral("-")); // "--" → 一个 '-' 键
                i += 2;
                if (i < name.size() && name.at(i) == QLatin1Char('-'))
                    ++i; // 后随的分隔符
                start = i;
            } else {
                ++i;
            }
            continue;
        }
        segs.append(name.mid(start, i - start));
        ++i;
        start = i;
    }
    if (start < name.size())
        segs.append(name.mid(start));
    return segs;
}

// 修饰前缀组装：按 mpv 回吐的固定顺序（modifier_names：Shift、Ctrl、Alt、Meta）。
// Shift 只在表键在场时拼——文本键的 Shift 位会被 mpv 归一化丢弃（折进大小写），
// 拼上只是混淆语义。
QString assembleName(const QString &base, bool fromTable, Qt::KeyboardModifiers mods) {
    QString name;
    if (fromTable && mods.testFlag(Qt::ShiftModifier))
        name += QStringLiteral("Shift+");
    if (mods.testFlag(Qt::ControlModifier))
        name += QStringLiteral("Ctrl+");
    if (mods.testFlag(Qt::AltModifier))
        name += QStringLiteral("Alt+");
    if (mods.testFlag(Qt::MetaModifier))
        name += QStringLiteral("Meta+");
    name += base;
    return name;
}

// 键值 + 修饰位 → 主键名（无修饰前缀）。KeypadModifier 优先判表（晚一步 KP5 就被
// 压成字符 '5'）；表键交 fromTable=true（决定 Shift 是否拼前缀）。取不到交空串。
QString baseNameFromCode(int key, Qt::KeyboardModifiers mods, bool *fromTable) {
    if (mods.testFlag(Qt::KeypadModifier)) {
        const QString n = keypadName(key);
        if (n.isEmpty())
            return QString(); // 表外的小键盘键值（NumLock / ScrollLock 等）在 mpv 侧没有名字
        if (fromTable)
            *fromTable = true;
        return n;
    }
    const QString n = specialName(key);
    if (!n.isEmpty()) {
        if (fromTable)
            *fromTable = true;
        return n;
    }
    if (fromTable)
        *fromTable = false;
    return charName(key, mods.testFlag(Qt::ShiftModifier));
}

// N89 序列化 / 归一化共用的主键名出口：'#' 键按 mpv key_names 的表名交 SHARP——
// 键位写 "#" 会被 input.conf 整行当注释（§3.2），而 mpv 回吐 0x23 时同样命中表名
// SHARP，归一与跨源比对必须同形。事件转发路径（nameFromEvent）保持码点 '#'：
// keypress 注入走命令参数不落行首，解析同值，两处口径不同是有意的
QString canonicalBaseName(int key, Qt::KeyboardModifiers mods, bool *fromTable) {
    QString base = baseNameFromCode(key, mods, fromTable);
    if (base == QLatin1String("#"))
        return QStringLiteral("SHARP");
    return base;
}

} // namespace

QString MpvKey::nameFromEvent(const QKeyEvent *event) {
    if (!event)
        return QString();

    const Qt::KeyboardModifiers mods = event->modifiers();
    const int key = int(event->key()) & ~int(Qt::KeyboardModifierMask);
    const bool shifted = mods.testFlag(Qt::ShiftModifier);

    bool fromTable = false;
    QString base;
    // 事件路径刻意不复用 baseNameFromCode：布局翻译后的字符（含 Shift / Caps Lock 结果）
    // 必须先看，物理位取名只作兜底。次序颠倒会把 Caps Lock 的大写字母交成小写、把
    // Shift+2 交成 "2"，而 mpv 认的是按键实际产出的字符（手册：Shift+2 应写作 @）。
    if (mods.testFlag(Qt::KeypadModifier)) {
        base = keypadName(key); // 表外的小键盘键值（NumLock / ScrollLock 等）不回落
        fromTable = !base.isEmpty();
    } else if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        // Ctrl / Alt / Meta 在场时 text() 不可依赖（Ctrl+字母给的是控制字符，
        // Ctrl+符号两平台形态不一），故一律由键值取名
        base = specialName(key);
        fromTable = !base.isEmpty();
        if (base.isEmpty())
            base = charName(key, shifted);
    } else {
        base = specialName(key);
        fromTable = !base.isEmpty();
        if (base.isEmpty()) {
            const QString typed = event->text();
            if (typed.size() == 1) {
                const char16_t c = typed.at(0).unicode();
                if (c >= 0x20 && c != 0x7F)
                    base = typed;
            }
        }
        if (base.isEmpty())
            base = charName(key, shifted); // 兜底：text() 不出值时按物理位交名，不任其蒸发
    }

    if (base.isEmpty())
        return QString();
    return assembleName(base, fromTable, mods);
}

QString MpvKey::nameFromSequence(const QKeySequence &sequence) {
    if (sequence.isEmpty())
        return QString();
    QStringList segs;
    for (int i = 0; i < sequence.count(); ++i) {
        // 逐段 count() / operator[] 取值（QKeyCombination 接口取键值与修饰位）
        const QKeyCombination combo = sequence[i];
        const int key = int(combo.key());
        const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
        bool fromTable = false;
        const QString base = canonicalBaseName(key, mods, &fromTable);
        if (base.isEmpty())
            return QString(); // 任一段取不到名 → 整串作废
        segs.append(assembleName(base, fromTable, mods));
    }
    return segs.join(QLatin1Char('-'));
}

QKeySequence MpvKey::sequenceFromName(const QString &name) {
    const QStringList segs = splitCombo(name);
    if (segs.isEmpty() || segs.size() > 4)
        return QKeySequence(); // Qt 侧至多 4 段（mpv 上限 MP_MAX_KEY_DOWN=16）
    int codes[4] = {0, 0, 0, 0};
    for (int i = 0; i < segs.size(); ++i) {
        const std::optional<int> code = codeFromSegment(segs.at(i));
        if (!code)
            return QKeySequence(); // 任一段无效 → 整串作废（mpv 同语义）
        codes[i] = *code;
    }
    switch (segs.size()) {
    case 1:
        return QKeySequence(codes[0]);
    case 2:
        return QKeySequence(codes[0], codes[1]);
    case 3:
        return QKeySequence(codes[0], codes[1], codes[2]);
    default:
        return QKeySequence(codes[0], codes[1], codes[2], codes[3]);
    }
}

QString MpvKey::normalizedKeyName(const QString &name) {
    if (name.isEmpty())
        return name;
    const QStringList segs = splitCombo(name);
    QStringList canon;
    for (const QString &seg : segs) {
        const std::optional<int> code = codeFromSegment(seg);
        if (!code)
            return name; // 任一段解不出 → 原串（自成一组，永不与内置集匹配）
        const auto mods = Qt::KeyboardModifiers(*code & int(Qt::KeyboardModifierMask));
        const int key = *code & ~int(Qt::KeyboardModifierMask);
        // mpv mp_normalize_keycode：ASCII 区间（< MP_KEY_BASE = 0x1000000）Shift 一律
        // 丢弃、字母折进大小写。字母的大小写映射已由码点路径天然完成（小写 → 无 Shift
        // 位、大写 → Shift 位，见 keyFromBaseName），此处只需把非字母文本键的 Shift
        // 丢掉（Shift+2 ≡ 2、Shift+SPACE ≡ SPACE）；特殊键（方向键等）Shift 保留
        Qt::KeyboardModifiers emitMods = mods;
        if (key < 0x1000000 && !(key >= Qt::Key_A && key <= Qt::Key_Z))
            emitMods &= ~Qt::ShiftModifier;
        bool fromTable = false;
        const QString base = canonicalBaseName(key, emitMods, &fromTable);
        if (base.isEmpty())
            return name;
        canon.append(assembleName(base, fromTable, emitMods));
    }
    return canon.join(QLatin1Char('-'));
}

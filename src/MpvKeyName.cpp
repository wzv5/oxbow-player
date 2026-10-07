#include "pch.h"

#include "MpvKeyName.h"

namespace {

// 特殊键表：只收非文本键——字母数字符号一律另有取名路径，不进本表（否则物理键位会截走
// 布局翻译的结果，德语 QWERTZ 的 z 就成了 y）。拼写即 mpv key_names[] 的第二列；注意
// Qt 的 Key_Enter 是小键盘回车（主回车为 Key_Return），故映 KP_ENTER 而非 ENTER
QString specialName(int key) {
    switch (key) {
    case Qt::Key_Escape:
        return QStringLiteral("ESC");
    case Qt::Key_Return:
        return QStringLiteral("ENTER");
    case Qt::Key_Enter:
        return QStringLiteral("KP_ENTER");
    case Qt::Key_Space:
        return QStringLiteral("SPACE");
    case Qt::Key_Tab:
        return QStringLiteral("TAB");
    case Qt::Key_Backtab:
        return QStringLiteral("TAB"); // mpv 无 BACKTAB，反向靠 Shift 位（调用方补）
    case Qt::Key_Backspace:
        return QStringLiteral("BS");
    case Qt::Key_Delete:
        return QStringLiteral("DEL");
    case Qt::Key_Insert:
        return QStringLiteral("INS");
    case Qt::Key_PageUp:
        return QStringLiteral("PGUP");
    case Qt::Key_PageDown:
        return QStringLiteral("PGDWN");
    case Qt::Key_Home:
        return QStringLiteral("HOME");
    case Qt::Key_End:
        return QStringLiteral("END");
    case Qt::Key_Left:
        return QStringLiteral("LEFT");
    case Qt::Key_Right:
        return QStringLiteral("RIGHT");
    case Qt::Key_Up:
        return QStringLiteral("UP");
    case Qt::Key_Down:
        return QStringLiteral("DOWN");
    case Qt::Key_Print:
        return QStringLiteral("PRINT");
    case Qt::Key_Pause:
        return QStringLiteral("PAUSE");
    case Qt::Key_Menu:
        return QStringLiteral("MENU");
    // 媒体键。名字按 mpv 的媒体段（MM_BASE 偏移表）逐字对应；`PLAY` 与 `PLAYPAUSE`
    // 是两个键，Qt 的 MediaPause 单列，故 MediaTogglePlayPause 才映 PLAYPAUSE
    case Qt::Key_MediaPlay:
        return QStringLiteral("PLAY");
    case Qt::Key_MediaPause:
        return QStringLiteral("PAUSE");
    case Qt::Key_MediaTogglePlayPause:
        return QStringLiteral("PLAYPAUSE");
    case Qt::Key_MediaStop:
        return QStringLiteral("STOP");
    case Qt::Key_MediaNext:
        return QStringLiteral("NEXT");
    case Qt::Key_MediaPrevious:
        return QStringLiteral("PREV");
    case Qt::Key_MediaRecord:
        return QStringLiteral("RECORD");
    case Qt::Key_AudioRewind:
        return QStringLiteral("REWIND");
    case Qt::Key_AudioForward:
        return QStringLiteral("FORWARD");
    case Qt::Key_VolumeUp:
        return QStringLiteral("VOLUME_UP");
    case Qt::Key_VolumeDown:
        return QStringLiteral("VOLUME_DOWN");
    case Qt::Key_VolumeMute:
        return QStringLiteral("MUTE");
    case Qt::Key_LaunchMail:
        return QStringLiteral("MAIL");
    // `#` 无需在此列：mpv 的 SHARP 就是 { '#', "SHARP" } 那个码点项的别名，而 Qt 的
    // '#' 键值是 Key_NumberSign = 0x23，落在码点取名路径上直接交出 '#'（解析同值）
    default:
        break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    return QString();
}

// 小键盘段：Qt 的小键盘 key() 与主键盘同值（数字就是 Key_0–Key_9、句点是 Key_Period），
// 唯一区别是 KeypadModifier——故本道判定必须排在所有取名路径之前，晚一步 KP5 就被压成
// 字符 '5'。KP_DEC（小数点）与 KP_DEL（102 键盘的逗号）在 mpv 侧是两个键，不可互换
QString keypadName(int key) {
    switch (key) {
    case Qt::Key_Period:
        return QStringLiteral("KP_DEC");
    case Qt::Key_Comma:
        return QStringLiteral("KP_DEL");
    case Qt::Key_Plus:
        return QStringLiteral("KP_ADD");
    case Qt::Key_Minus:
        return QStringLiteral("KP_SUBTRACT");
    case Qt::Key_Slash:
        return QStringLiteral("KP_DIVIDE");
    case Qt::Key_Asterisk:
        return QStringLiteral("KP_MULTIPLY");
    case Qt::Key_Enter:
        return QStringLiteral("KP_ENTER");
    case Qt::Key_Home:
        return QStringLiteral("KP_HOME");
    case Qt::Key_End:
        return QStringLiteral("KP_END");
    case Qt::Key_PageUp:
        return QStringLiteral("KP_PGUP");
    case Qt::Key_PageDown:
        return QStringLiteral("KP_PGDWN");
    case Qt::Key_Insert:
        return QStringLiteral("KP_INS");
    case Qt::Key_Delete:
        return QStringLiteral("KP_DEL");
    // numlock 关时的 KP5（mpv 侧名 KP_BEGIN）不映：Qt 无 Key_Begin 枚举，实际报
    // Key_Clear / Key_Select 一族且两平台不一——发错键名比不发更糟，故不接
    case Qt::Key_Left:
        return QStringLiteral("KP_LEFT");
    case Qt::Key_Right:
        return QStringLiteral("KP_RIGHT");
    case Qt::Key_Up:
        return QStringLiteral("KP_UP");
    case Qt::Key_Down:
        return QStringLiteral("KP_DOWN");
    default:
        break;
    }
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

} // namespace

QString MpvKey::nameFromEvent(const QKeyEvent *event) {
    if (!event)
        return QString();

    const Qt::KeyboardModifiers mods = event->modifiers();
    const int key = int(event->key()) & ~int(Qt::KeyboardModifierMask);
    const bool shifted = mods.testFlag(Qt::ShiftModifier);

    QString base;
    // 名字来自特殊键表才拼 Shift：文本键的 Shift 位会被 mpv 归一化丢弃，拼上只是混淆语义
    bool fromTable = false;

    if (mods.testFlag(Qt::KeypadModifier)) {
        base = keypadName(key);
        fromTable = !base.isEmpty();
        if (base.isEmpty())
            return QString(); // 表外的小键盘键值（NumLock / ScrollLock 等）在 mpv 侧没有名字
    } else if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        // Ctrl / Alt / Meta 在场时 text() 不可依赖（Ctrl+字母给的是控制字符，
        // Ctrl+符号两平台形态不一），故一律由键值取名。
        base = specialName(key);
        fromTable = !base.isEmpty();
        if (base.isEmpty())
            base = charName(key, shifted);
    } else {
        base = specialName(key);
        fromTable = !base.isEmpty();
        if (base.isEmpty()) {
            const QString typed = event->text(); // 布局翻译后的字符，含 Shift / Caps Lock 结果
            if (typed.size() == 1) {
                const char16_t c = typed.at(0).unicode();
                if (c >= 0x20 && c != 0x7F)
                    base = typed;
            }
        }
        if (base.isEmpty())
            base = charName(key, shifted); // 兜底：text() 万一不出值，按物理位交名而不任其蒸发
    }

    if (base.isEmpty())
        return QString();

    QString name;
    if (mods & Qt::ControlModifier)
        name += QStringLiteral("Ctrl+");
    if (mods & Qt::AltModifier)
        name += QStringLiteral("Alt+");
    if (mods & Qt::MetaModifier)
        name += QStringLiteral("Meta+");
    if (fromTable && shifted)
        name += QStringLiteral("Shift+");
    name += base;
    return name;
}

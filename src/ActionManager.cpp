#include "pch.h"

#include "ActionManager.h"

ActionManager::ActionManager(QWidget *host) : m_host(host) {
    // 注册表 = 全部 UI 入口清单（A15）：稳定英文名 / 显示名 / 键位在此同点声明，
    // 顺序即头文件枚举声明顺序，新增动作须与枚举插入位置对齐
    declareAction(Id::Open, QStringLiteral("Open"), tr("&Open File…"), QKeySequence::Open);
    declareAction(Id::Settings, QStringLiteral("Settings"), tr("&Settings"));
    declareAction(Id::Shortcuts, QStringLiteral("Shortcuts"), tr("&Keyboard Shortcuts…"));
    // N78：视频截图两动作——取图经 MpvController::screenshotRaw，逻辑归主窗口。
    // Ctrl+C 全仓无占用：焦点在文本控件时控件以 ShortcutOverride 认领（仍是
    // 文本复制），主窗体焦点下触发截图
    declareAction(Id::ScreenshotCopy, QStringLiteral("ScreenshotCopy"),
                  tr("&Copy Screenshot to Clipboard"), QStringLiteral("Ctrl+C"));
    declareAction(Id::ScreenshotSave, QStringLiteral("ScreenshotSave"), tr("Save Screenshot &As…"),
                  QStringLiteral("Ctrl+E"));
    declareAction(Id::Quit, QStringLiteral("Quit"), tr("&Quit"), QKeySequence::Quit);
    declareAction(Id::PlayPause, QStringLiteral("PlayPause"), tr("&Play/Pause"), Qt::Key_Space);
    declareAction(Id::Stop, QStringLiteral("Stop"), tr("&Stop"));
    declareAction(Id::PrevFile, QStringLiteral("PrevFile"), tr("P&revious File"), Qt::Key_PageUp);
    declareAction(Id::NextFile, QStringLiteral("NextFile"), tr("&Next File"), Qt::Key_PageDown);
    // N5：全局音量步进（播放列表经 ShortcutOverride 认领导航键豁免）
    declareAction(Id::VolumeUp, QStringLiteral("VolumeUp"), tr("Volume &Up"), Qt::Key_Up);
    declareAction(Id::VolumeDown, QStringLiteral("VolumeDown"), tr("Volume &Down"), Qt::Key_Down);
    // N38：静音开关；勾选态由 mpv 的 mute 属性推送驱动，命令不读勾选态
    declareAction(Id::Mute, QStringLiteral("Mute"), tr("&Mute"), Qt::Key_M, true);
    // N6：速度控制三动作，助记符与快捷键同字母
    declareAction(Id::SpeedUp, QStringLiteral("SpeedUp"), tr("&Speed Up"), Qt::Key_C);
    declareAction(Id::SpeedDown, QStringLiteral("SpeedDown"), tr("Speed Down"), Qt::Key_X);
    declareAction(Id::SpeedReset, QStringLiteral("SpeedReset"), tr("Reset Speed"), Qt::Key_Z);
    declareAction(Id::Playlist, QStringLiteral("Playlist"), tr("P&laylist"),
                  QStringLiteral("Ctrl+L"), true);
    declareAction(Id::Navigator, QStringLiteral("Navigator"), tr("&Navigator"),
                  QStringLiteral("Ctrl+Shift+N"), true);
    declareAction(Id::Fullscreen, QStringLiteral("Fullscreen"), tr("&Fullscreen"),
                  QStringLiteral("F11"), true);
    // N68：小窗模式（PiP）；勾选态由 toggleMini 收口回写
    declareAction(Id::MiniWindow, QStringLiteral("MiniWindow"), tr("Mini &Window"),
                  QStringLiteral("I"), true);
    // 无状态开关：显隐真相由 mpv 的 stats 脚本持有，插件侧不跟踪勾选态
    declareAction(Id::ToggleStats, QStringLiteral("ToggleStats"), tr("Toggle &Status Info"),
                  Qt::Key_T);
    declareAction(Id::About, QStringLiteral("About"), tr("&About"));
    // N6：Esc 闭环与 ±5 秒相对 seek，隐绑定无菜单实体（显示名可翻译，对话框用）。
    // Escape 不可改绑：改绑可能封死全屏 / 小窗 / 视频区的退出路径
    declareShortcut(Id::Escape, QStringLiteral("Escape"), tr("Escape"), Qt::Key_Escape, false);
    declareShortcut(Id::SeekForward, QStringLiteral("SeekForward"), tr("Seek Forward"),
                    Qt::Key_Right);
    declareShortcut(Id::SeekBackward, QStringLiteral("SeekBackward"), tr("Seek Backward"),
                    Qt::Key_Left);
    // N15：添加书签
    declareAction(Id::AddBookmark, QStringLiteral("AddBookmark"), tr("Add &Bookmark"), Qt::Key_P);
    declareAction(Id::BookmarkEditor, QStringLiteral("BookmarkEditor"), tr("Bookmark &Editor…"));
}

bool ActionManager::nameTaken(const QString &idName) const {
    return std::any_of(m_entries.cbegin(), m_entries.cend(),
                       [&](const Entry &e) { return e.name == idName; });
}

void ActionManager::declareAction(Id id, const QString &idName, const QString &text,
                                  const QKeySequence &key, bool checkable, bool modifiable) {
    Entry &e = m_entries[id];
    Q_ASSERT(!e.action && !e.shortcut); // 重复声明显影
    Q_ASSERT(!nameTaken(idName));
    e.defaultKey = key;
    e.name = idName;
    e.displayText = text;
    e.modifiable = modifiable;
    auto *a = new QAction(text, m_host);
    a->setCheckable(checkable);
    if (!key.isEmpty()) {
        a->setShortcut(key);
        m_host->addAction(a); // 双挂载窗口侧焊死于此：带键创建即挂载
    }
    e.action = a;
}

void ActionManager::declareShortcut(Id id, const QString &idName, const QString &displayText,
                                    const QKeySequence &key, bool modifiable) {
    Entry &e = m_entries[id];
    Q_ASSERT(!e.action && !e.shortcut);
    Q_ASSERT(!nameTaken(idName));
    e.defaultKey = key;
    e.name = idName;
    e.displayText = displayText;
    e.modifiable = modifiable;
    e.shortcut = new QShortcut(key, m_host);
}

QAction *ActionManager::action(Id id) const { return m_entries.value(id).action; }

QShortcut *ActionManager::shortcut(Id id) const { return m_entries.value(id).shortcut; }

QKeySequence ActionManager::defaultKey(Id id) const {
    const auto it = m_entries.constFind(id);
    return it == m_entries.cend() ? QKeySequence() : it->defaultKey;
}

QKeySequence ActionManager::key(Id id) const {
    const auto it = m_entries.constFind(id);
    if (it == m_entries.cend())
        return QKeySequence();
    if (it->action)
        return it->action->shortcut();
    if (it->shortcut)
        return it->shortcut->key();
    return QKeySequence();
}

void ActionManager::applyKey(Id id, const QKeySequence &seq) {
    const auto it = m_entries.constFind(id);
    if (it == m_entries.cend())
        return; // 调用方一律来自 orderedIds
    if (it->action) {
        it->action->setShortcut(seq);
        // 空键出生的动作要设键才触发。补挂载判据是窗口动作表已含它，而非按键空否：
        // 清空不卸载，按键判断会在「清空 → 再设键」路径上重复 addAction
        if (!seq.isEmpty() && !m_host->actions().contains(it->action))
            m_host->addAction(it->action);
    } else if (it->shortcut) {
        it->shortcut->setKey(seq);
    }
}

QString ActionManager::name(Id id) const {
    const auto it = m_entries.constFind(id);
    return it == m_entries.cend() ? QString() : it->name;
}

QString ActionManager::displayText(Id id) const {
    const auto it = m_entries.constFind(id);
    return it == m_entries.cend() ? QString() : it->displayText;
}

bool ActionManager::modifiable(Id id) const {
    const auto it = m_entries.constFind(id);
    return it == m_entries.cend() ? false : it->modifiable;
}

QHash<QString, ActionManager::Id> ActionManager::nameId() const {
    // 反向表由登记表推导，两表永不失配
    QHash<QString, Id> reverse;
    reverse.reserve(m_entries.size());
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        reverse.insert(it->name, it.key());
    return reverse;
}

QList<ActionManager::Id> ActionManager::orderedIds() const {
    // 声明顺序即顺序：按 [0, Count) 推导
    QList<Id> ids;
    ids.reserve(int(Id::Count));
    for (int v = 0; v < int(Id::Count); ++v)
        ids << Id(v);
    return ids;
}

QKeySequence ActionManager::storedToKey(const QString &stored) {
    // 显式空哨兵在解析前短路，不依赖 fromString 对 "--" 的行为
    if (stored == QLatin1String("--"))
        return QKeySequence();
    const QKeySequence seq = QKeySequence::fromString(stored, QKeySequence::PortableText);
    // 非 "--"、非空却解析为空 = ini 手写非法值：按空处理，但必须留痕
    if (seq.isEmpty())
        qWarning() << "Unparsable shortcut value in oxbow.ini, treated as empty:" << stored;
    return seq;
}

QHash<ActionManager::Id, QKeySequence> ActionManager::effectiveKeys(const QString &iniPath) const {
    // 归口单点：主窗收口与对话框快照共用，此处之外不得再有第二份归一遍历
    QHash<Id, QKeySequence> keys;
    QSettings ini(iniPath, QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("shortcuts"));
    for (Id id : orderedIds()) {
        const QString stored = ini.value(name(id)).toString();
        if (!modifiable(id)) {
            // 不可改绑条目不进结果表：ini 覆盖防卫跳过并留痕
            if (!stored.isEmpty())
                qWarning() << "Shortcut" << name(id) << "is not modifiable; ini override ignored";
            continue;
        }
        keys.insert(id, stored.isEmpty() ? defaultKey(id) : storedToKey(stored));
    }
    // 未知 Id 键（用户手写 / 版本改名）天然不参与归一，此处只留痕
    const auto reverse = nameId();
    for (const QString &key : ini.childKeys()) {
        if (!reverse.contains(key))
            qWarning() << "Unknown shortcut id in oxbow.ini [shortcuts], skipped:" << key;
    }
    ini.endGroup();
    return keys;
}

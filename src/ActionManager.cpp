#include "pch.h"

#include "ActionManager.h"

ActionManager::ActionManager(QWidget *host) : m_host(host) {
    // 注册表 = 全部 UI 入口清单（A15）
    declareAction(Id::Open, tr("&Open File…"), QKeySequence::Open);
    declareAction(Id::Settings, tr("&Settings"));
    declareAction(Id::Quit, tr("&Quit"), QKeySequence::Quit);
    declareAction(Id::PlayPause, tr("&Play/Pause"), Qt::Key_Space);
    declareAction(Id::Stop, tr("&Stop"));
    declareAction(Id::PrevFile, tr("P&revious File"), Qt::Key_PageUp);
    declareAction(Id::NextFile, tr("&Next File"), Qt::Key_PageDown);
    // N5：全局音量步进（播放列表经 ShortcutOverride 认领导航键豁免）
    declareAction(Id::VolumeUp, tr("Volume &Up"), Qt::Key_Up);
    declareAction(Id::VolumeDown, tr("Volume &Down"), Qt::Key_Down);
    // N6：速度控制三动作，助记符与快捷键同字母
    declareAction(Id::SpeedUp, tr("&Speed Up"), Qt::Key_C);
    declareAction(Id::SpeedDown, tr("Speed Down"), Qt::Key_X);
    declareAction(Id::SpeedReset, tr("Reset Speed"), Qt::Key_Z);
    declareAction(Id::Playlist, tr("P&laylist"), QStringLiteral("Ctrl+L"), true);
    declareAction(Id::Navigator, tr("&Navigator"), QStringLiteral("Ctrl+Shift+N"), true);
    declareAction(Id::Fullscreen, tr("&Fullscreen"), QStringLiteral("F11"), true);
    // 无状态开关：显隐真相由 mpv 的 stats 脚本持有，插件侧不跟踪勾选态
    declareAction(Id::ToggleStats, tr("Toggle &Status Info"), Qt::Key_T);
    declareAction(Id::About, tr("&About"));
    declareShortcut(Id::Escape, Qt::Key_Escape);
    // N6：±5 秒相对 seek，隐绑定无菜单实体
    declareShortcut(Id::SeekForward, Qt::Key_Right);
    declareShortcut(Id::SeekBackward, Qt::Key_Left);
    // N15：添加书签
    declareAction(Id::AddBookmark, tr("Add &Bookmark"), Qt::Key_P);
    declareAction(Id::BookmarkEditor, tr("Bookmark &Editor…"));
}

void ActionManager::declareAction(Id id, const QString &text, const QKeySequence &key,
                                  bool checkable) {
    auto *a = new QAction(text, m_host);
    a->setCheckable(checkable);
    if (!key.isEmpty()) {
        a->setShortcut(key);
        m_host->addAction(a); // 双挂载窗口侧焊死于此：带键创建即挂载
    }
    m_entries[id].action = a;
}

void ActionManager::declareShortcut(Id id, const QKeySequence &key) {
    m_entries[id].shortcut = new QShortcut(key, m_host);
}

QAction *ActionManager::action(Id id) const { return m_entries.value(id).action; }

QShortcut *ActionManager::shortcut(Id id) const { return m_entries.value(id).shortcut; }

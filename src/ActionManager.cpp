#include "pch.h"

#include "ActionManager.h"

ActionManager::ActionManager(QWidget *host) : m_host(host)
{
    // 注册表 = 全部 UI 入口清单（A15）
    declareAction(Id::Open,      tr("打开文件(&O)…"), QKeySequence::Open);
    declareAction(Id::Settings,  tr("设置(&S)"));
    declareAction(Id::Quit,      tr("退出(&Q)"),      QKeySequence::Quit);
    declareAction(Id::PlayPause, tr("播放/暂停(&P)"),  Qt::Key_Space);
    declareAction(Id::Stop,      tr("停止(&S)"));
    declareAction(Id::PrevFile,  tr("上一个文件(&R)"));
    declareAction(Id::NextFile,  tr("下一个文件(&N)"));
    // N5：全局音量步进（播放列表经 ShortcutOverride 认领导航键豁免）
    declareAction(Id::VolumeUp,   tr("音量增大(&U)"), Qt::Key_Up);
    declareAction(Id::VolumeDown, tr("音量减小(&D)"), Qt::Key_Down);
    declareAction(Id::Playlist,  tr("播放列表(&L)"),  QStringLiteral("Ctrl+L"), true);
    declareAction(Id::Navigator, tr("导航器(&N)"),    QStringLiteral("Ctrl+Shift+N"), true);
    declareAction(Id::Fullscreen,tr("全屏(&F)"),      QStringLiteral("F11"), true);
    declareAction(Id::About,     tr("关于(&A)"));
    declareShortcut(Id::Escape,  Qt::Key_Escape);
}

void ActionManager::declareAction(Id id, const QString &text,
                                  const QKeySequence &key, bool checkable)
{
    auto *a = new QAction(text, m_host);
    a->setCheckable(checkable);
    if (!key.isEmpty()) {
        a->setShortcut(key);
        m_host->addAction(a); // 双挂载窗口侧焊死于此：带键创建即挂载
    }
    m_entries[id].action = a;
}

void ActionManager::declareShortcut(Id id, const QKeySequence &key)
{
    m_entries[id].shortcut = new QShortcut(key, m_host);
}

QAction *ActionManager::action(Id id) const
{
    return m_entries.value(id).action;
}

QShortcut *ActionManager::shortcut(Id id) const
{
    return m_entries.value(id).shortcut;
}

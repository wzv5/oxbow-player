#pragma once

#include "pch.h"

// A15：全部 UI 动作的出生与登记处——集中声明使键位清单一眼可见；
// 带键动作创建即双挂载主窗口（快捷键不随菜单栏隐藏失活）；零信号连接，
// 业务逻辑归主窗口。容器侧禁止自行创建 QAction，一律引用此处的主实例。
class ActionManager : public QObject
{
    Q_OBJECT
public:
    enum class Id {
        Open, Settings, Quit,
        PlayPause, Stop, PrevFile, NextFile,
        VolumeUp, VolumeDown,
        SeekForward, SeekBackward, // 隐绑定：无菜单实体，仅键盘触发（N6）
        SpeedUp, SpeedDown, SpeedReset,
        Playlist, Navigator, Fullscreen,
        About,
        Escape, // 隐绑定：无菜单实体，仅键盘触发
        AddBookmark, BookmarkEditor
    };

    explicit ActionManager(QWidget *host);

    // 容器挂载 / 状态控制取用（禁用一次生效处处）
    QAction *action(Id id) const;
    // 隐绑定取用（Escape）；连接与守卫逻辑由主窗口完成
    QShortcut *shortcut(Id id) const;

private:
    void declareAction(Id id, const QString &text,
                       const QKeySequence &key = QKeySequence(),
                       bool checkable = false);
    void declareShortcut(Id id, const QKeySequence &key);

    QWidget *m_host;
    struct Entry {
        QAction *action = nullptr;
        QShortcut *shortcut = nullptr;
    };
    QHash<Id, Entry> m_entries;
};

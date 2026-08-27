#pragma once
#include "pch.h"

class BookmarkManager;

class BookmarkEditorDialog : public QDialog
{
    Q_OBJECT
public:
    using JumpCallback = std::function<void(const QString &, double)>;
    explicit BookmarkEditorDialog(BookmarkManager *mgr, JumpCallback jumpCb, QWidget *parent = nullptr);

    void refresh();

private slots:
    void onSelectionChanged();
    void onJump();
    void onRemoveSelected();
    void onRemoveInvalid();
    void onClearAll();
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);

private:
    BookmarkManager *m_mgr;
    JumpCallback m_jumpCb;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_jumpBtn = nullptr;
    QPushButton *m_removeSelBtn = nullptr;
    QPushButton *m_removeInvalidBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
};

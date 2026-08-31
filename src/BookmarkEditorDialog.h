#pragma once
#include "pch.h"

class BookmarkManager;

class BookmarkEditorDialog : public QDialog {
    Q_OBJECT
  public:
    using JumpCallback = std::function<void(const QString &, double)>;
    // currentPath：当前播放文件归一化键（N16：打开即聚焦该文件）；空表示不聚焦
    explicit BookmarkEditorDialog(BookmarkManager *mgr, JumpCallback jumpCb,
                                  const QString &currentPath = QString(),
                                  QWidget *parent = nullptr);

    void refresh();

  private slots:
    void onSelectionChanged();
    void onJump();
    void onRemoveSelected();
    void onRemoveInvalid();
    void onClearAll();
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onApply();

  private:
    BookmarkManager *m_mgr;
    JumpCallback m_jumpCb;
    QString
        m_focusFile; // N16：refresh
                     // 重建后应选中并展开的文件父节点（归一化键）；非空时仅展开该文件、其余收起，空时全部展开
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_jumpBtn = nullptr;
    QPushButton *m_removeSelBtn = nullptr;
    QPushButton *m_removeInvalidBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLineEdit *m_timeEdit = nullptr;
    QLineEdit *m_titleEdit = nullptr;
    QPushButton *m_applyBtn = nullptr;
    QString m_selPath;   // 当前选中叶的书签键
    int m_selIndex = -1; // 当前选中叶在文件内的索引
};

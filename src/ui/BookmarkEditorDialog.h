#pragma once
#include "pch.h"

#include "BookmarkManager.h" // N54：回调形参用到嵌套类型 Origin，需完整类型

class BookmarkEditorDialog : public QDialog {
    Q_OBJECT
  public:
    // N54：跳转回调追加书签来源（列表身份 + 所在层 id 序列）。由本对话框按选中叶的
    // 组向 manager 现取，主窗口据此分流跳转
    using JumpCallback =
        std::function<void(const QString &, double, const BookmarkManager::Origin &)>;
    // N54：列表名现查回调（列表身份 → 显示名，查不到返回空串）。名字**不落盘**：
    // 运行期查即改名即刻生效，也免存一份必然漂移的副本。
    // N57 §2.6：原 `ListNameLookup` + `ListAliveLookup` 塌成这一个出口——"列表还在不在"
    // 与"列表叫什么"本就是同一次按身份直查的两面（空返回即无此列表），存活谓词在
    // onRemoveInvalid 里由它就地派生，不再让调用方传两份同源判据。
    // 落点在注册表（`PlaylistRegistry::nameOf`），不经视图反查 tab
    using ListLookup = std::function<QString(const QUuid &listUuid)>;
    // N77 §2.3：currentGroup——当前播放文件所在的组（正播身份 + key）；打开即聚焦
    // 该组（若存在书签）。空组（nil 身份 + 空 key）表示不聚焦
    explicit BookmarkEditorDialog(BookmarkManager *mgr, JumpCallback jumpCb, ListLookup listLookup,
                                  const BookmarkManager::Group &currentGroup = {},
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
    // N77 §2.3：从条目数据还原组标识（UserRole 存 key，UserRole+1 存 uuid 字面串）
    BookmarkManager::Group groupOfItem(QTreeWidgetItem *item) const;
    // N54：根节点标题 = "[列表名] 显示路径"。列表名按组身份现查，查不到（无身份
    // 书签、或所属列表已删）前缀显示 Unknown——只影响呈现，跳转仍用组与来源
    QString fileItemText(const BookmarkManager::Group &group) const;

    BookmarkManager *m_mgr;
    JumpCallback m_jumpCb;
    ListLookup m_listLookup; // N57 §2.6：名字与"还在不在"共用一次按身份直查
    // N77 §2.3：焦点 / 选区改组形态（uuid + key），同文件多组各占一行、互不混淆
    BookmarkManager::Group
        m_focusGroup; // N16：refresh 重建后应选中并展开的文件父节点（组）；空组时全部展开
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
    BookmarkManager::Group m_selGroup; // 当前选中叶的书签组
    int m_selIndex = -1;               // 当前选中叶在文件内的索引
};

#pragma once
#include "pch.h"

#include "BookmarkManager.h" // N54：回调形参用到嵌套类型 Origin，需完整类型

class BookmarkEditorDialog : public QDialog {
    Q_OBJECT
  public:
    // N54：跳转回调追加书签来源（列表身份 + 所在层 id 序列）。由本对话框按选中叶的
    // 归一化键向 manager 现取，主窗口据此分"回绑定列表 / 回默认 tab"两条路
    using JumpCallback =
        std::function<void(const QString &, double, const BookmarkManager::Origin &)>;
    // N54：列表名现查回调（列表身份 uuid → tab 标题，查不到返回空串）。名字**不落盘**：
    // 运行期查即改名即刻生效，也免存一份必然漂移的副本
    using ListNameLookup = std::function<QString(const QString &listUuid)>;
    // N54：列表是否仍存在的判据（"移除失效项"据此清掉所属列表已删的远端书签）。未接
    // 时该半条规则不生效——无从判定就不删，另半条（远端且无来源 uuid = 数据异常）不
    // 依赖它，照删
    using ListAliveLookup = std::function<bool(const QString &listUuid)>;
    // currentPath：当前播放文件归一化键（N16：打开即聚焦该文件）；空表示不聚焦
    explicit BookmarkEditorDialog(BookmarkManager *mgr, JumpCallback jumpCb,
                                  ListNameLookup nameLookup, ListAliveLookup listAlive,
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
    // N54：根节点标题 = "[列表名] 显示路径"。列表名按来源 uuid 现查，查不到（无身份
    // 书签、或所属列表已删）前缀显示 Unknown——只影响呈现，跳转仍用键与来源
    QString fileItemText(const QString &key) const;

    BookmarkManager *m_mgr;
    JumpCallback m_jumpCb;
    ListNameLookup m_nameLookup; // N54：根节点标题里的列表名
    ListAliveLookup m_listAlive; // N54：移除失效项用的"列表是否还在"判据
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

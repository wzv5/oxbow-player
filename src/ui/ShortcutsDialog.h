#pragma once

#include "pch.h"

#include "ActionManager.h"

// N86：快捷键编辑对话框——两列表格（动作 / 按键），行内 QKeySequenceEdit 捕获。
// 状态走行级视觉（加粗 = 与声明默认不同；高亮 = 冲突），操作走右键菜单（清空 /
// 恢复默认，对选中集批量生效），不设状态列与操作列。
//
// 非模态、按需创建、复用不重建（生命周期归 MainWindow）。QDialog 是独立顶级窗口，
// 焦点在本窗时主窗的 WindowShortcut 本就不活跃，无需绑定或事件层防护。
// 落盘成功即发 applied() 由主窗从 ini 重载；本类不认识 console、不持有让位状态。
class ShortcutsDialog : public QDialog {
    Q_OBJECT
  public:
    explicit ShortcutsDialog(ActionManager *actions, const QString &iniPath,
                             QWidget *parent = nullptr);

  signals:
    // 落盘成功后发出（失败不发），主窗口接统一生效入口
    void applied();

  protected:
    // reject() 只 hide、不发 Close 事件，WA_DeleteOnClose 不生效——先回滚再隐藏，
    // 复用重开才不会看到被丢弃的改动
    void reject() override;
    // 每次打开重读 ini 快照，dirty 比对才不失真
    void showEvent(QShowEvent *event) override;
    // Del / 退格 / Esc 清空该行而非记作快捷键
    bool eventFilter(QObject *watched, QEvent *event) override;
    // 调色板 / 样式变更重跑行视觉（重算冲突 tint 与编辑框底色）
    void changeEvent(QEvent *event) override;

  private slots:
    // 冲突校验 + 四情形落盘；返回 false 时 OK 不得关闭对话框
    bool apply();
    // 控件值变化 → 更新 pending；批量设值期间不逐行刷视觉
    void onRowChanged(ActionManager::Id id, const QKeySequence &seq);
    // 右键菜单：每次新建即用即毁，无选中行时两项皆灰
    void showContextMenu(const QPoint &viewportPos);

  private:
    int rowOf(ActionManager::Id id) const;
    // 把编辑器与 pending 拉回当前快照（回滚与重载共用同一段实现）
    void resetFromSnapshot();
    // 行级视觉一次性重算：加粗 + 冲突高亮
    void refreshRowVisuals();
    void updateApplyButton();
    // 冲突行收集：非空同键归组；不可改绑条目的声明默认也计入分组（不在表里但占键）
    QList<int> conflictRows() const;
    QColor conflictTint() const;

    ActionManager *m_actions = nullptr;
    QString m_iniPath; // oxbow.ini 路径（[shortcuts] 节读写）

    QTableWidget *m_table = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
    QPushButton *m_applyButton = nullptr;

    QList<ActionManager::Id> m_rowOrder;   // 行号 → Id
    QHash<ActionManager::Id, int> m_rowOf; // Id → 行号

    QHash<ActionManager::Id, QKeySequence> m_pending;  // 控件当前值
    QHash<ActionManager::Id, QKeySequence> m_snapshot; // 上次落盘的值
    bool m_batchUpdating = false;                      // 批量设值期间守卫，视觉重算收口为一次
};

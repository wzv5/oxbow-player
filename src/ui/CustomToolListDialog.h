#pragma once

#include "pch.h"

#include "ActionManager.h"
#include "CustomToolManager.h"

// N134：自定义工具编辑对话框（模态）。工作副本永不为空：表为空即补一条占位空
// 条目等待输入。确定语义收口在 CustomToolManager::sanitize（规范化 + 校验 + 丢
// 占位条目），本类只做列表 / 表单的呈现与违规行的定位；N138 增 Key 行
class CustomToolListDialog : public QDialog {
    Q_OBJECT
  public:
    explicit CustomToolListDialog(const QList<CustomTool> &tools, const ActionManager *actions,
                                  QWidget *parent = nullptr);

    QList<CustomTool> tools() const; // accept 通过后的保留行（名称已重组、命令已 trim）

  protected:
    void accept() override;
    // 主题切换时重染按钮图标（PaletteChange / StyleChange）
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void addTool();
    void deleteTool();
    void moveTool(int delta);
    void loadRow(int row);
    void updateButtons();
    void retintIcons();
    static QString listText(const QString &name);

    QListWidget *m_list = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_commandEdit = nullptr;
    QKeySequenceEdit *m_keyEdit = nullptr;
    QLineEdit *m_noteEdit = nullptr;
    QToolButton *m_addButton = nullptr;
    QToolButton *m_deleteButton = nullptr;
    QToolButton *m_upButton = nullptr;
    QToolButton *m_downButton = nullptr;
    QList<CustomTool> m_tools;
    const ActionManager *m_actions; // accept 现取键表：模态期间键位不会变
    bool m_updating = false;        // 装载期间挡住表单回写
};

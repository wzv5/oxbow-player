#pragma once

#include "pch.h"

#include "CustomToolManager.h"

// N134：自定义工具编辑对话框（模态）。工作副本永不为空：表为空即补一条占位空
// 条目等待输入——空条目（名称与命令均空）确定时静默忽略，恰一者为空即拦截补全。
// 名称编辑按原始串，确定时规范化保存（切段 trim、丢空段、> 前后统一单空格）
class CustomToolListDialog : public QDialog {
    Q_OBJECT
  public:
    explicit CustomToolListDialog(const QList<CustomTool> &tools, QWidget *parent = nullptr);

    QList<CustomTool> tools() const; // accept 通过后的保留行（逐条 trim）

  protected:
    void accept() override;
    // 主题切换时重染按钮图标（PaletteChange / StyleChange）
    void changeEvent(QEvent *event) override;

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
    QLineEdit *m_noteEdit = nullptr;
    QToolButton *m_addButton = nullptr;
    QToolButton *m_deleteButton = nullptr;
    QToolButton *m_upButton = nullptr;
    QToolButton *m_downButton = nullptr;
    QList<CustomTool> m_tools;
    bool m_updating = false; // 装载期间挡住表单回写
};

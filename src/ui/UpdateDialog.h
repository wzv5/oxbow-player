#pragma once

#include "pch.h"

class UpdateChecker;

// 检查更新对话框（N151）：三行信息 + 底部主按钮，自动与手动入口共用。只读
// UpdateChecker 状态并显示，自身不发起检查。非模态、WA_DeleteOnClose，单实例
// 由主窗 QPointer 兜底；对 UpdateChecker 只持非拥有裸指针（主窗持有）。
class UpdateDialog : public QDialog {
    Q_OBJECT

  public:
    UpdateDialog(UpdateChecker *checker, QWidget *parent);

  private:
    void refresh(); // 连 UpdateChecker::stateChanged
    void onPrimaryClicked();

    UpdateChecker *m_checker; // 非拥有；主窗持有
    QLabel *m_currentValue = nullptr;
    QLabel *m_latestValue = nullptr;
    QLabel *m_publishedValue = nullptr;
    QPushButton *m_primary = nullptr;
};

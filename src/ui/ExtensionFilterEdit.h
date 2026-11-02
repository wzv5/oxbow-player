#pragma once

#include "pch.h"

// 后缀名过滤器行（N130）：列表设置各提供者页共用的"编辑框 + 重置按钮"。
// 复位行为内聚为回填全局默认媒体扩展名清单；只承载取值与回填，
// 不转发编辑框信号、不做校验
class ExtensionFilterEdit : public QWidget {
    Q_OBJECT
  public:
    explicit ExtensionFilterEdit(QWidget *parent = nullptr);

    QString text() const;
    void setText(const QString &text);

  protected:
    void changeEvent(QEvent *event) override;

  private:
    void retintResetButton();

    QLineEdit *m_edit = nullptr;
    QPushButton *m_resetBtn = nullptr;
};

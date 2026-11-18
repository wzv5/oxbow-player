#pragma once

#include "pch.h"

// N169：Material Symbols 连字名浏览器（模态）。过滤框 + 图标网格（QListWidget
// IconMode），委托现画连字图标与名字，不进全局底图缓存。进入时按传入的当前名预选
// （清单内才选），无选中项时确定禁用，确认即取选中项名字
class IconBrowserDialog : public QDialog {
    Q_OBJECT
  public:
    explicit IconBrowserDialog(const QString &current, QWidget *parent = nullptr);

    QString selectedName() const; // accept 后的选中项名字

  private:
    void rebuild(const QString &filter);
    void updateOkState();

    QListWidget *m_grid = nullptr;
    QLineEdit *m_filter = nullptr;
    QPushButton *m_okButton = nullptr;
};

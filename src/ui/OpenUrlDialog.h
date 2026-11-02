#pragma once

#include "pch.h"

// N131：手输"打开链接 / 添加链接"对话框，文件菜单（整表替换并起播）与播放列表右键
// （追加进当前 tab）共用同一份——差别只有标题，故不设"用途"枚举。
// 只回答"输了哪些条目"：切分走 Util::Url::splitEntryText，定形在这里补形态；
// 造实体、播放与持久化一概不进本类。
class OpenUrlDialog : public QDialog {
    Q_OBJECT
  public:
    explicit OpenUrlDialog(const QString &title, QWidget *parent = nullptr);

    // 当前输入的已定形身份串表（按输入序；空输入得空表）
    QStringList entries() const;

  private:
    void browse();
    void refreshOkState();

    QPlainTextEdit *m_edit = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

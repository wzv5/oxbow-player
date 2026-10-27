#pragma once

#include "pch.h"

// N102：关于对话框——独立 QDialog，上部宿主 exe 图标 + 软件标题与版权行，
// 下部只读 QTextEdit 承载构建信息（Version / Build time / mpv version /
// Qt version，名称与值 \t 分隔）。mpv 版本不取自 MpvController：由它构造期填入
// App::mpvVersion()，本类只读取值。模态弹出由调用方
// exec() 兑现；本类不设窗口图标，标题栏沿用 Win32 默认的宿主 exe 图标。
class AboutDialog : public QDialog {
    Q_OBJECT
  public:
    explicit AboutDialog(QWidget *parent = nullptr);
};

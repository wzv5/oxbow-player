#include "pch.h"

#include "ui/ThumbWindow.h"

ThumbWindow::ThumbWindow(QWidget *parent) : QWidget(parent) {
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_NativeWindow);
    setAutoFillBackground(true);
    QPalette p = palette();
    p.setColor(QPalette::Window, Qt::black);
    setPalette(p);
    hostWid(); // 构造期即原生化：loadfile 前句柄必须已存在
}

WId ThumbWindow::hostWid() { return winId(); }

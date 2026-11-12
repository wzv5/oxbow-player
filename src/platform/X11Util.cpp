#include "pch.h"

#include "X11Util.h"

#include <X11/Xlib.h>

// N83：实现见头注释。自建 Display 连接（与 Qt 的 xcb 连接互不共享，无线程 / 锁
// 交互），懒开一次常驻；X 环境不可用（XOpenDisplay 失败）时静默跳过，置顶即不
// 生效。X11 头的宏污染（None / False 等）隔离在本单文件之内（头文件不引 Xlib 头），
// 仅此处的 Xlib 调用使用它们
void setWindowAboveEwmh(unsigned long long win, bool on) {
    static Display *dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return;
    XEvent xev{};
    xev.xclient.type = ClientMessage;
    xev.xclient.window = static_cast<Window>(win);
    xev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = on ? 1 : 0; // _NET_WM_STATE_ADD / _NET_WM_STATE_REMOVE
    xev.xclient.data.l[1] = static_cast<long>(XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False));
    xev.xclient.data.l[2] = 0;
    xev.xclient.data.l[3] = 1; // source indication: 1 = application
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
    XFlush(dpy);
}

// N141：与 setWindowAboveEwmh 同一条通道，只换 atom（用途与代价见头文件）
void setWindowFullscreenEwmh(unsigned long long win, bool on) {
    static Display *dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return;
    XEvent xev{};
    xev.xclient.type = ClientMessage;
    xev.xclient.window = static_cast<Window>(win);
    xev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = on ? 1 : 0; // _NET_WM_STATE_ADD / _NET_WM_STATE_REMOVE
    xev.xclient.data.l[1] = static_cast<long>(XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False));
    xev.xclient.data.l[2] = 0;
    xev.xclient.data.l[3] = 1; // source indication: 1 = application
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
    XFlush(dpy);
}

// N156：见头注释。不 ungrab（保留按键隐式 grab）；坐标/方向由调用方给定
void sendMoveResizeEwmh(unsigned long long win, int rootX, int rootY, int direction) {
    static Display *dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return;
    XEvent xev{};
    xev.xclient.type = ClientMessage;
    xev.xclient.window = static_cast<Window>(win);
    xev.xclient.message_type = XInternAtom(dpy, "_NET_WM_MOVERESIZE", False);
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = rootX;
    xev.xclient.data.l[1] = rootY;
    xev.xclient.data.l[2] = direction;
    xev.xclient.data.l[3] = Button1;
    xev.xclient.data.l[4] = 1; // source indication: 1 = application（本消息无 timestamp 字段）
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
    XFlush(dpy);
}

// N156：与 setWindowAboveEwmh 同一条通道，一条消息携带 SKIP_TASKBAR 与
// SKIP_PAGER 两个 atom
void setWindowSkipTaskbarEwmh(unsigned long long win, bool on) {
    static Display *dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return;
    XEvent xev{};
    xev.xclient.type = ClientMessage;
    xev.xclient.window = static_cast<Window>(win);
    xev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = on ? 1 : 0; // _NET_WM_STATE_ADD / _NET_WM_STATE_REMOVE
    xev.xclient.data.l[1] =
        static_cast<long>(XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False));
    xev.xclient.data.l[2] = static_cast<long>(XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False));
    xev.xclient.data.l[3] = 1; // source indication: 1 = application
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
    XFlush(dpy);
}

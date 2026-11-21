#pragma once

#include "pch.h"

#include "ScriptMenuSpec.h"

class MainWindow;
class MpvController;
class QEventLoop;
class QMenu;
class ScriptMessageBus;
class QWidget;

// N172：context-menu 协议宿主——忙判据、菜单渲染弹出、命令 / 条目回调派发，以及恒发的
// 关闭事件（条目回调必先于本单关闭事件到达，脚本在其中清理安全）；传输与分派全归
// N174 总线，本宿主不自连客户端消息
class ScriptMenuHost : public QObject {
    Q_OBJECT
  public:
    // controller 供条目预绑命令与自连 mpvShutdown 置退出标志；window 是弹层父指针基线
    explicit ScriptMenuHost(ScriptMessageBus *bus, MpvController *controller, MainWindow *window,
                            QWidget *video);

  private slots:
    void shutdown();

  private:
    void handleContextMenu(const QStringList &args);
    void handleShow(const QStringList &args);
    void reject(const QString &owner, const QString &menuId, QLatin1String reason);
    bool busy() const;
    void buildMenu(const ScriptMenuSpec::Menu &menu, const QString &owner, const QString &menuId);
    void buildNodes(QMenu *parent, const QList<ScriptMenuSpec::Node> &nodes);
    void onRootAboutToHide();
    void sendClosed();
    void awaitClose();

    QPointer<ScriptMessageBus> m_bus; // 总线先于宿主析构，回投前判空
    QPointer<MpvController> m_controller;
    MainWindow *m_window = nullptr;
    QWidget *m_video = nullptr;
    QPointer<QMenu> m_menu; // 当前活动脚本菜单根；空即无活动菜单
    QString m_owner;        // 当前菜单周期；关闭事件发出后才随守候循环清
    QString m_menuId;
    bool m_selected = false;      // 本周期有条目被点；关闭事件的 dismissed 载荷由此取值
    bool m_closedSent = false;    // 本周期关闭事件已发（守卫，防顶替链重复发送）
    bool m_replacing = false;     // show 顶替路径的 close 标志：aboutToHide 内同步发送
    bool m_exiting = false;       // mpvShutdown 已置位：拒绝 show、关闭事件不发送
    QEventLoop *m_loop = nullptr; // 当前守候循环；空即未守候
};

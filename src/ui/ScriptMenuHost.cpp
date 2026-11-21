#include "pch.h"

#include "ScriptMenuHost.h"

#include "Logging.h"
#include "MainWindow.h"
#include "Util.h"
#include "mpvbridge/MpvController.h"
#include "mpvbridge/ScriptMessageBus.h"

namespace {

// N172 协议 token 与写死的出站名，只在本模块消费
constexpr char kInbound[] = "context-menu";
constexpr char kVerbShow[] = "show";
constexpr char kReasonBadArgs[] = "bad-args";
constexpr char kReasonBadId[] = "bad-id";
constexpr char kReasonBadJson[] = "bad-json";
constexpr char kReasonEmpty[] = "empty";
constexpr char kReasonBusy[] = "busy";

// 关闭事件的 dismissed 载荷：script-message 参数恒为字符串
constexpr char kTrue[] = "true";
constexpr char kFalse[] = "false";

const QString kRejectedMessage =
    Util::scriptEventName(QStringLiteral("context-menu"), QStringLiteral("rejected"));
const QString kClosedMessage =
    Util::scriptEventName(QStringLiteral("context-menu"), QStringLiteral("closed"));

} // namespace

ScriptMenuHost::ScriptMenuHost(ScriptMessageBus *bus, MpvController *controller, MainWindow *window,
                               QWidget *video)
    : QObject(window), m_bus(bus), m_controller(controller), m_window(window), m_video(video) {
    // context 挂本宿主，析构即自动断开
    m_bus->registerHandler(QLatin1String(kInbound), this,
                           [this](const QStringList &args) { handleContextMenu(args); });
    if (controller)
        connect(controller, &MpvController::mpvShutdown, this, &ScriptMenuHost::shutdown);
}

void ScriptMenuHost::shutdown() {
    // 退出期不再发任何消息（总线先于本宿主停发，补发必然失败）。守候循环必须当场收掉而
    // 不是只置标志：应用收口后 QEventLoop::exec 立即返回，循环条件会在不收口的情况下空转
    m_exiting = true;
    if (m_loop)
        m_loop->quit();
}

// args 布局：[context-menu, show, owner, menu-id, json]
void ScriptMenuHost::handleContextMenu(const QStringList &args) {
    const QString verb = args.value(1);
    if (verb == QLatin1String(kVerbShow))
        handleShow(args);
    else
        OXBOW_LOG_DEBUG() << "context-menu: unknown verb ignored:" << verb;
}

void ScriptMenuHost::handleShow(const QStringList &args) {
    const QString owner = args.value(2);
    const QString menuId = args.value(3);
    if (args.size() < 5) {
        if (args.size() > 2) // owner 段存在才回投
            reject(owner, menuId, QLatin1String(kReasonBadArgs));
        return; // json 之后的多余参数忽略
    }
    if (!ScriptMenuSpec::isValidMenuId(menuId)) {
        reject(owner, menuId, QLatin1String(kReasonBadId));
        return;
    }
    const ScriptMenuSpec::ParseResult parsed = ScriptMenuSpec::parse(args.value(4).toUtf8());
    if (parsed.error == ScriptMenuSpec::ParseError::BadJson) {
        // 解析成因是脚本作者的第一个线索，reject 那行只有 token
        OXBOW_LOG_DEBUG() << "context-menu: bad json id =" << menuId << "—" << parsed.detail;
        reject(owner, menuId, QLatin1String(kReasonBadJson));
        return;
    }
    if (parsed.error == ScriptMenuSpec::ParseError::Empty) {
        OXBOW_LOG_DEBUG() << "context-menu: empty menu id =" << menuId << "—" << parsed.detail;
        reject(owner, menuId, QLatin1String(kReasonEmpty));
        return;
    }
    if (busy()) {
        reject(owner, menuId, QLatin1String(kReasonBusy));
        return;
    }
    // 校验全部通过才动当前菜单：顶替路径的 close 无点击在途（点击即关，开着就说明
    // 没点），aboutToHide 内同步发旧单关闭事件，"先发旧关闭、再弹新"由此保住
    if (m_menu) {
        m_replacing = true;
        m_menu->close();
        m_replacing = false;
    }
    buildMenu(parsed.menu, owner, menuId);
    OXBOW_LOG_DEBUG() << "context-menu: shown" << menuId << "owner =" << owner;
    if (!m_loop) // 守候循环未在跑（首次 show）；重入的 show 由既有循环守候
        awaitClose();
}

void ScriptMenuHost::reject(const QString &owner, const QString &menuId, QLatin1String reason) {
    OXBOW_LOG_DEBUG() << "context-menu: rejected" << menuId << "reason =" << reason
                      << "owner =" << owner;
    if (!m_bus)
        return;
    m_bus->reply(owner, kRejectedMessage, {menuId, reason});
}

// 任一命中即拒绝，不排队
bool ScriptMenuHost::busy() const {
    if (m_exiting)
        return true;
    // 主窗不在激活态：activeWindow 为空，或沿父链走不到主窗（主窗自身、全屏浮层、
    // 当前脚本菜单都满足；最小化 / 隐藏 / 其他应用在前台不满足）
    QWidget *active = QApplication::activeWindow();
    bool activeReachable = false;
    for (QWidget *w = active; w; w = w->parentWidget()) {
        if (w == m_window) {
            activeReachable = true;
            break;
        }
    }
    if (!activeReachable)
        return true;
    if (QApplication::activeModalWidget())
        return true;
    if (QWidget *popup = QApplication::activePopupWidget()) {
        // 判"属于"沿父链走：子菜单展开时返回的是子菜单，子菜单是根菜单的子对象
        for (QWidget *w = popup; w; w = w->parentWidget()) {
            if (w == m_menu)
                return false;
        }
        return true; // 外来弹层（含视频区右键菜单、组合框下拉等）
    }
    return false;
}

void ScriptMenuHost::buildMenu(const ScriptMenuSpec::Menu &menu, const QString &owner,
                               const QString &menuId) {
    m_owner = owner;
    m_menuId = menuId;
    m_selected = false;
    m_closedSent = false;
    QMenu *root = new QMenu(m_window); // 父指针显式挂主窗，不从子件的 window() 现取
    buildNodes(root, menu.items);
    // 弹出前按 N119 既有口径清焦；点视频区关菜单的那一下不重放成暂停
    if (QWidget *fw = QApplication::focusWidget())
        fw->clearFocus();
    m_menu = root;
    connect(root, &QMenu::aboutToHide, this, &ScriptMenuHost::onRootAboutToHide);
    root->setNoReplayFor(m_video);
    root->popup(QCursor::pos()); // 位置恒为当前光标
}

void ScriptMenuHost::buildNodes(QMenu *parent, const QList<ScriptMenuSpec::Node> &nodes) {
    for (const ScriptMenuSpec::Node &node : nodes) {
        switch (node.kind) {
        case ScriptMenuSpec::NodeKind::Separator:
            parent->addSeparator();
            break;
        case ScriptMenuSpec::NodeKind::Submenu:
            buildNodes(parent->addMenu(node.title), node.children);
            break;
        case ScriptMenuSpec::NodeKind::Item: {
            QAction *action = parent->addAction(node.title);
            action->setEnabled(!node.disabled);
            if (node.checkable) {
                action->setCheckable(true);
                action->setChecked(node.checked);
            }
            // 捕获值不捕获菜单对象；命令与回调并存时命令在前
            const QString command = node.command;
            const QString callback = node.callback;
            const QString itemId = node.id;
            connect(action, &QAction::triggered, this,
                    [this, owner = m_owner, menuId = m_menuId, itemId, command, callback] {
                        m_selected = true;
                        if (!command.isEmpty()) {
                            if (m_controller) {
                                const int rc = m_controller->command(command);
                                if (rc != 0)
                                    OXBOW_LOG_WARNING()
                                        << "context-menu: command failed menu =" << menuId
                                        << "item =" << itemId << "rc =" << rc;
                            } else {
                                OXBOW_LOG_WARNING()
                                    << "context-menu: command dropped menu =" << menuId
                                    << "item =" << itemId;
                            }
                        }
                        if (!callback.isEmpty() && m_bus) {
                            const int rc = m_bus->reply(owner, callback, {menuId, itemId});
                            if (rc != 0)
                                OXBOW_LOG_WARNING()
                                    << "context-menu: callback failed menu =" << menuId
                                    << "item =" << itemId << "rc =" << rc;
                        }
                    });
            break;
        }
        }
    }
}

// 只连根菜单：子菜单收起不代表菜单结束
void ScriptMenuHost::onRootAboutToHide() {
    QMenu *menu = m_menu.data();
    if (!menu)
        return; // 已收口
    OXBOW_LOG_DEBUG() << "context-menu: closing" << m_menuId;
    m_menu.clear();
    menu->deleteLater(); // 队列删除：替换与退出路径都不在本栈内碰对象
    // 顶替的 close 无点击在途（点击即关，开着就说明没点），同步发旧单关闭事件：
    // 清理可达性不拖入新菜单生命周期，"先发旧关闭、再弹新"也由此保住
    if (m_replacing)
        sendClosed();
    if (m_loop)
        m_loop->quit();
}

// 守候循环：popup 的交互在受控事件循环内进行；aboutToHide 只收口并 quit，Qt 菜单
// "先收菜单、后发动作"（activateAction 的 Trigger 分支先 hideUpToMenuBar 再触发条目）
// 使触发链在 loop 返回前完成——返回点 hide 与点击均已处理，此时发关闭事件，条目回调
// 必先于本单 closed 到达，脚本在此清理安全。顶替链在同一循环里依次守候，不递归增长
void ScriptMenuHost::awaitClose() {
    while (m_menu && !m_exiting) {
        QEventLoop loop;
        m_loop = &loop;
        loop.exec();
        m_loop = nullptr;
    }
    sendClosed();
    m_owner.clear();
    m_menuId.clear();
}

void ScriptMenuHost::sendClosed() {
    if (m_closedSent)
        return;
    m_closedSent = true;
    const bool selected = m_selected;
    m_selected = false;
    if (m_exiting || !m_bus)
        return;
    // dismissed 载荷为字符串 "true"/"false"：true 即关闭时无条目被点
    const QString dismissed = QLatin1String(selected ? kFalse : kTrue);
    const int rc = m_bus->reply(m_owner, kClosedMessage, {m_menuId, dismissed});
    OXBOW_LOG_DEBUG() << "context-menu: closed sent" << m_menuId << "dismissed =" << dismissed
                      << "rc =" << rc;
}

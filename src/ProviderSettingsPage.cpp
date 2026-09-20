#include "pch.h"

#include "ProviderSettingsPage.h"

// 基类只承载接口：构造转发父窗口，三个纯虚由各提供者的页面实现
ProviderSettingsPage::ProviderSettingsPage(QWidget *parent) : QWidget(parent) {}

// N52 §2.8：转接行一次实现（基类唯一一处），按能力渲染不给非法组合留入口。
// 页面布局约定为纵向（QVBoxLayout），转接行落调用点当前位置（子类安排在
// stretch 之前即视觉底行）
void ProviderSettingsPage::enableTransitOption(std::optional<bool> initial) {
    auto *box = qobject_cast<QVBoxLayout *>(layout());
    Q_ASSERT(box);
    if (initial.has_value()) {
        m_transitCheck = new QCheckBox(tr("Relay through plugin (custom protocol)"));
        m_transitCheck->setChecked(*initial);
        box->addWidget(m_transitCheck);
    } else {
        // Required：形态恒转接，勾选框不存在（改为说明文字）
        auto *note = new QLabel(tr("This list always plays through the plugin relay"));
        note->setEnabled(false); // 禁用态取调色板的灰，跟随主题
        box->addWidget(note);
    }
}

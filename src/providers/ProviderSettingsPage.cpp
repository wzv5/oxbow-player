#include "pch.h"

#include "ProviderSettingsPage.h"

ProviderSettingsPage::ProviderSettingsPage(QWidget *parent) : QWidget(parent) {}

void ProviderSettingsPage::enableTransitOption(std::optional<bool> initial) {
    auto *box = qobject_cast<QVBoxLayout *>(layout());
    Q_ASSERT(box);
    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    box->addWidget(separator);
    if (initial.has_value()) {
        m_transitCheck = new QCheckBox(tr("Relay through plugin (custom protocol)"));
        m_transitCheck->setChecked(*initial);
        box->addWidget(m_transitCheck);
    } else {
        // Required：形态恒转接，故不出勾选框
        auto *note = new QLabel(tr("This playlist always plays through the plugin relay"));
        note->setEnabled(false); // 禁用态取调色板灰，跟随主题
        box->addWidget(note);
    }
}

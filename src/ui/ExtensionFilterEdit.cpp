#include "pch.h"

#include "ExtensionFilterEdit.h"

#include "Constants.h"
#include "ui/ThemedIconEngine.h"

ExtensionFilterEdit::ExtensionFilterEdit(QWidget *parent) : QWidget(parent) {
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    m_edit = new QLineEdit;
    row->addWidget(m_edit, 1);
    m_resetBtn = new QPushButton;
    m_resetBtn->setToolTip(tr("Reset to default extension list"));
    m_resetBtn->setAccessibleName(tr("Reset to default extension list"));
    m_resetBtn->setFixedSize(28, 28);
    const int iconSide = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    m_resetBtn->setIconSize(QSize(iconSide, iconSide));
    retintResetButton();
    row->addWidget(m_resetBtn);
    connect(m_resetBtn, &QPushButton::clicked, this,
            [this] { setText(App::defaultMediaExtensions().join(QLatin1Char(' '))); });
}

QString ExtensionFilterEdit::text() const { return m_edit->text(); }

void ExtensionFilterEdit::setText(const QString &text) { m_edit->setText(text); }

void ExtensionFilterEdit::retintResetButton() {
    m_resetBtn->setIcon(
        Ui::themedIcon(App::iconPath(App::iconReset()), palette().color(QPalette::WindowText)));
}

void ExtensionFilterEdit::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() != QEvent::PaletteChange && event->type() != QEvent::StyleChange)
        return;
    if (m_resetBtn)
        retintResetButton();
}

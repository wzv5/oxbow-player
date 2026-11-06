#include "ui/UpdateDialog.h"

#include "Logging.h"
#include "UpdateChecker.h"
#include "Version.h"

UpdateDialog::UpdateDialog(UpdateChecker *checker, QWidget *parent)
    : QDialog(parent), m_checker(checker) {
    Q_ASSERT(m_checker); // 检查器由主窗持有并注入，非空是契约（N151）
    setWindowTitle(tr("Check for Updates"));

    auto *form = new QFormLayout;
    // 键值间隙取四空格宽，与关于对话框的制表位间隙同源（N102）
    const QFontMetrics metrics = fontMetrics();
    form->setHorizontalSpacing(metrics.horizontalAdvance(QStringLiteral("    ")));
    const int valueMinWidth = metrics.horizontalAdvance(QStringLiteral("000.000.000"));

    auto makeValueLabel = [this, valueMinWidth] {
        auto *label = new QLabel(this);
        label->setMinimumWidth(valueMinWidth);
        return label;
    };
    m_currentValue = makeValueLabel();
    m_latestValue = makeValueLabel();
    m_publishedValue = makeValueLabel();
    form->addRow(tr("Current version"), m_currentValue);
    form->addRow(tr("Latest version"), m_latestValue);
    form->addRow(tr("Release Date"), m_publishedValue);

    auto *buttonBox = new QDialogButtonBox(this);
    m_primary = buttonBox->addButton(tr("Download"), QDialogButtonBox::AcceptRole);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &UpdateDialog::onPrimaryClicked);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(form);
    mainLayout->addWidget(buttonBox);

    // 锁尺寸按最宽状态测：发布时间行先落一条与结果同宽的 19 字夹具（成功回填的日期串就
    // 是这个宽度），按钮文案取两态里较宽的 Download；锁完由 refresh() 落实际文案
    m_publishedValue->setText(QStringLiteral("0000-00-00 00:00:00"));
    setFixedSize(sizeHint());

    connect(m_checker, &UpdateChecker::stateChanged, this, &UpdateDialog::refresh);
    refresh(); // 手动入口打开时显示占位串，自动入口打开时直接显示已就绪的结果
}

void UpdateDialog::refresh() {
    const UpdateChecker::Status status = m_checker->status();
    m_currentValue->setText(App::version());
    if (status == UpdateChecker::Status::UpdateAvailable ||
        status == UpdateChecker::Status::UpToDate) {
        const UpdateCheck::Release &release = m_checker->release();
        m_latestValue->setText(release.version);
        m_publishedValue->setText(
            release.publishedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    } else {
        const QString placeholder =
            status == UpdateChecker::Status::Failed ? tr("Failed") : tr("Checking…");
        m_latestValue->setText(placeholder);
        m_publishedValue->setText(placeholder);
    }
    m_primary->setText(status == UpdateChecker::Status::UpdateAvailable ? tr("Download")
                                                                        : tr("OK"));
}

void UpdateDialog::onPrimaryClicked() {
    if (m_checker->status() == UpdateChecker::Status::UpdateAvailable) {
        if (!QDesktopServices::openUrl(m_checker->release().pageUrl)) {
            OXBOW_LOG_WARNING() << "failed to open release page";
            return; // 保留对话框，用户可再点
        }
    }
    accept();
}

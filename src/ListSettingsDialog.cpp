#include "ListSettingsDialog.h"
#include "pch.h"

#include "ProviderCatalog.h"
#include "ProviderSettingsPage.h"

ListSettingsDialog::ListSettingsDialog(QWidget *parent, const QString &currentName,
                                       const PlaylistProvider *current,
                                       const QStringList &occupiedNames,
                                       const std::shared_ptr<ServiceRegistry> &registry)
    : QDialog(parent), m_occupiedNames(occupiedNames) {
    setWindowTitle(tr("List Settings"));
    // 最小宽度写死 450：窗口宽度本由 sizeHint 链算出（行 2 单选项数、行 3 当前页表单、
    // 按钮行三者取最大），内容窄的页（如静态页只有一句提示）会把窗口压得很窄。
    // 钉一个下限同时兼顾将来登记表加提供者后单选行变宽的情形
    setMinimumWidth(450);
    auto *layout = new QVBoxLayout(this);

    // ——行 1 + 行 2：名称与列表类型同属一个 QFormLayout，标签列自动对齐——
    auto *formLayout = new QFormLayout;
    m_nameEdit = new QLineEdit(currentName);
    formLayout->addRow(tr("List Name"), m_nameEdit);

    auto *typeRow = new QHBoxLayout;
    auto *group = new QButtonGroup(this);
    // ——行 3：表单区，页序与登记表同序——
    m_pageStack = new QStackedWidget;

    const QList<ProviderEntry> &catalog = providerCatalog();
    int defaultIndex = 0; // 新建（current 为空）时选中第 0 项，即默认提供者
    bool matchedType = false;
    for (int i = 0; i < catalog.size(); ++i) {
        const ProviderEntry &entry = catalog.at(i);
        auto *radio = new QRadioButton(entry.displayName());
        m_radios << radio;
        typeRow->addWidget(radio);
        group->addButton(radio);
        // 存量提供者是否本条目：按类型键比对（键与条目同源，故必然命中）
        const bool isCurrentType = current && current->typeKey() == QLatin1String(entry.typeKey);
        if (isCurrentType) {
            defaultIndex = i;
            matchedType = true;
        }
        // 页面构造期即完成回填：命中才把存量提供者作回显来源，否则传 nullptr
        // （新建 / 换类型），页面据此预填默认扩展名清单
        auto *page = entry.createPage(isCurrentType ? current : nullptr, registry, m_pageStack);
        m_pages << page;
        m_pageStack->addWidget(page);
    }
    // 未命中属登记表漏登或键写错（编程错误）；Release 下回落第 0 项
    Q_ASSERT(!current || matchedType);
    typeRow->addStretch(1);
    // 尾部固定留 12px：伸缩项在空间不足时会收缩到 0，届时最后那个单选项会贴住窗口
    // 右边——登记表加提供者、单选项变多后就会发生，故在伸缩项之后再钉一段固定间距
    typeRow->addSpacing(12);
    formLayout->addRow(tr("List Type"), typeRow);

    layout->addLayout(formLayout);
    // 表单区承担纵向伸缩，按钮行因此固定在底部
    layout->addWidget(m_pageStack, 1);

    // ——行 4：按钮行——
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    // 顶部留白：表单区高度恰为当前页内容高，末行（备注）与按钮行只隔默认行距，
    // 显得太挤；这段间距挂给按钮行顶部，不挂给页内末行
    buttons->setContentsMargins(0, 12, 0, 0);
    connect(buttons, &QDialogButtonBox::accepted, this, &ListSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // 切换单选即换表单：toggled 在选中与取消选中各发一次，必须判 checked 才切，
    // 否则会拿"被取消的那条"的下标去切页
    for (int i = 0; i < m_radios.size(); ++i) {
        connect(m_radios.at(i), &QRadioButton::toggled, this, [this, i](bool checked) {
            if (checked)
                m_pageStack->setCurrentIndex(i);
        });
    }
    // 接线完成后再选中，避免"接线前 setChecked 丢失联动"
    Q_ASSERT(defaultIndex >= 0 && defaultIndex < m_radios.size());
    m_radios.at(defaultIndex)->setChecked(true);
}

QString ListSettingsDialog::listName() const { return m_nameEdit->text().trimmed(); }

std::optional<std::shared_ptr<PlaylistProvider>> ListSettingsDialog::takeProvider() {
    return std::move(m_result);
}

void ListSettingsDialog::accept() {
    // 名称三条（列表级，与提供者无关，留在对话框）
    const QString name = listName();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("List Settings"), tr("List name cannot be empty."));
        return;
    }
    if (name == tr("Default")) {
        QMessageBox::warning(this, tr("List Settings"),
                             tr("Cannot use the name of the default list."));
        return;
    }
    if (m_occupiedNames.contains(name)) {
        QMessageBox::warning(this, tr("List Settings"),
                             tr("This name is already used by another list."));
        return;
    }
    // 提供者三条：校验与错误文案都由选中页给出，非选中页既不显示也不参与
    const int index = m_pageStack->currentIndex();
    Q_ASSERT(index >= 0 && index < m_pages.size());
    ProviderSettingsPage *page = m_pages.at(index);
    if (const std::optional<QString> error = page->validate()) {
        QMessageBox::warning(this, tr("List Settings"), *error);
        return; // 留在对话框
    }
    // 未变更（存量同类型且参数未动）→ 不产出新提供者，调用方仅应用改名
    if (page->hasChanges())
        m_result = page->build();
    QDialog::accept();
}

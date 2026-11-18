#include "pch.h"

#include "Constants.h"
#include "ThemedIconEngine.h"
#include "ui/CustomToolListDialog.h"
#include "ui/IconBrowserDialog.h"
#include "ui/MaterialIconCatalog.h"

CustomToolListDialog::CustomToolListDialog(const QList<CustomTool> &tools,
                                           const ActionManager *actions, QWidget *parent)
    : QDialog(parent), m_tools(tools), m_actions(actions) {
    setWindowTitle(tr("Customize Tool List"));

    m_list = new QListWidget(this);
    m_list->setMinimumHeight(m_list->fontMetrics().height() * 6 + 2 * m_list->frameWidth());

    m_addButton = new QToolButton(this);
    m_addButton->setToolTip(tr("Add"));
    m_deleteButton = new QToolButton(this);
    m_deleteButton->setToolTip(tr("Delete"));
    m_upButton = new QToolButton(this);
    m_upButton->setToolTip(tr("Move Up"));
    m_downButton = new QToolButton(this);
    m_downButton->setToolTip(tr("Move Down"));
    // 照主窗控制行按钮先例放大到 24px
    for (QToolButton *tb : {m_addButton, m_deleteButton, m_upButton, m_downButton})
        tb->setIconSize(QSize(24, 24));

    // 弹性空白吃掉中段高度，列表底与 Move Down 底对齐
    auto *buttonColumn = new QVBoxLayout;
    buttonColumn->setSpacing(4);
    buttonColumn->addWidget(m_addButton);
    buttonColumn->addWidget(m_deleteButton);
    buttonColumn->addStretch();
    buttonColumn->addWidget(m_upButton);
    buttonColumn->addWidget(m_downButton);
    retintIcons();

    auto *listArea = new QHBoxLayout;
    listArea->addWidget(m_list, 1);
    listArea->addLayout(buttonColumn);

    m_nameEdit = new QLineEdit(this);
    auto *nameHint = new QLabel(tr("Use > to create submenus, e.g. Playback > Speed"), this);
    nameHint->setEnabled(false);
    nameHint->setFocusPolicy(Qt::NoFocus);
    m_commandEdit = new QLineEdit(this);
    auto *commandHint = new QLabel(tr("Same as in input.conf"), this);
    commandHint->setEnabled(false);
    commandHint->setFocusPolicy(Qt::NoFocus);
    m_keyEdit = new QKeySequenceEdit(this);
    // 只认单组合键：多段串与 AM 的单段键在 planKeys 里永不相等，会把歧义交回 Qt
    // 裁决。录入器限长 1 只是录入体验，不是硬限制（手改 INI 收多段）
    m_keyEdit->setMaximumSequenceLength(1);
    if (auto *le = m_keyEdit->findChild<QLineEdit *>())
        le->setContextMenuPolicy(Qt::NoContextMenu); // 内建右键菜单会遮住记录器自身
    m_keyEdit->installEventFilter(this);
    // N168：下拉两项顺序对齐 BarSide 的 0 / 1；图标提示恒禁用，不随勾选翻转
    m_barShowCheck = new QCheckBox(tr("Show as control bar button"), this);
    m_barSideCombo = new QComboBox(this);
    m_barSideCombo->addItem(tr("Left"));
    m_barSideCombo->addItem(tr("Right"));
    m_barIconEdit = new QLineEdit(this);
    // 连字名无 '-' 分段，用裸 QCompleter、不覆写 splitPath；候选与校验同源
    auto *iconCompleter = new QCompleter(MaterialIconCatalog::names(), m_barIconEdit);
    iconCompleter->setCompletionMode(QCompleter::PopupCompletion);
    iconCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    iconCompleter->setFilterMode(Qt::MatchStartsWith);
    m_barIconEdit->setCompleter(iconCompleter);
    m_barIconButton = new QToolButton(this);
    m_barIconButton->setText(QStringLiteral("…"));
    m_barIconButton->setToolTip(tr("Browse Icons"));
    m_barIconButton->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::MinimumExpanding);
    auto *iconHint = new QLabel(tr("Material Symbols ligature name, e.g. play_arrow"), this);
    iconHint->setEnabled(false);
    iconHint->setFocusPolicy(Qt::NoFocus);

    auto *iconRow = new QHBoxLayout;
    iconRow->setContentsMargins(0, 0, 0, 0);
    iconRow->addWidget(m_barIconEdit, 1);
    iconRow->addWidget(m_barIconButton);

    // 勾选框与提示走单件行跨两列，与列表区左对齐；字段行保持两列形态
    auto *form = new QFormLayout;
    form->addRow(tr("Name"), m_nameEdit);
    form->addRow(nameHint);
    form->addRow(tr("Command"), m_commandEdit);
    form->addRow(commandHint);
    form->addRow(tr("Key"), m_keyEdit);
    form->addRow(m_barShowCheck);
    form->addRow(tr("Button position"), m_barSideCombo);
    form->addRow(tr("Button icon"), iconRow);
    form->addRow(iconHint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *main = new QVBoxLayout(this);
    main->addLayout(listArea);
    main->addLayout(form);
    main->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_updating)
            loadRow(row);
    });
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        m_tools[m_list->currentRow()].name = text;
        m_list->currentItem()->setText(listText(text));
    });
    connect(m_commandEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        m_tools[m_list->currentRow()].command = text;
    });
    connect(m_barShowCheck, &QCheckBox::toggled, this, [this](bool on) {
        updateBarControlsEnabled();
        if (m_updating)
            return;
        m_tools[m_list->currentRow()].barShow = on;
    });
    connect(m_barSideCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating)
            return;
        m_tools[m_list->currentRow()].barSide = index == 1 ? BarSide::Right : BarSide::Left;
    });
    connect(m_barIconEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        m_tools[m_list->currentRow()].barIcon = text;
    });
    connect(m_keyEdit, &QKeySequenceEdit::keySequenceChanged, this,
            [this](const QKeySequence &seq) {
                if (m_updating)
                    return;
                m_tools[m_list->currentRow()].key = seq;
            });
    connect(m_addButton, &QAbstractButton::clicked, this, [this] { addTool(); });
    connect(m_deleteButton, &QAbstractButton::clicked, this, [this] { deleteTool(); });
    connect(m_upButton, &QAbstractButton::clicked, this, [this] { moveTool(-1); });
    connect(m_downButton, &QAbstractButton::clicked, this, [this] { moveTool(+1); });
    connect(m_barIconButton, &QToolButton::clicked, this, [this] {
        IconBrowserDialog dialog(m_barIconEdit->text(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        m_barIconEdit->setText(dialog.selectedName());
    });

    for (const CustomTool &tool : m_tools)
        m_list->addItem(listText(tool.name));
    if (m_tools.isEmpty()) {
        m_tools.append(CustomTool{});
        m_list->addItem(listText(QString()));
    }
    loadRow(0);
    m_list->setCurrentRow(0);
    m_nameEdit->setFocus();
}

QList<CustomTool> CustomToolListDialog::tools() const { return m_tools; }

QString CustomToolListDialog::listText(const QString &name) {
    return name.isEmpty() ? tr("(unnamed)") : name;
}

void CustomToolListDialog::retintIcons() {
    const QColor tint = palette().color(QPalette::WindowText);
    m_addButton->setIcon(Ui::themedIcon(App::iconAdd(), tint));
    m_deleteButton->setIcon(Ui::themedIcon(App::iconDeleteForever(), tint));
    m_upButton->setIcon(Ui::themedIcon(App::iconKeyboardArrowUp(), tint));
    m_downButton->setIcon(Ui::themedIcon(App::iconKeyboardArrowDown(), tint));
}

// m_addButton 为空即控件树尚未建成，无事可做
void CustomToolListDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() != QEvent::PaletteChange && event->type() != QEvent::StyleChange)
        return;
    if (!m_addButton)
        return;
    retintIcons();
}

// N138：裸键 Del / 退格 / Esc 清空该键而非记作快捷键（KeypadModifier 不算修饰）
bool CustomToolListDialog::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if ((ke->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier &&
            (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace ||
             ke->key() == Qt::Key_Escape)) {
            if (qobject_cast<QKeySequenceEdit *>(watched)) {
                m_keyEdit->setKeySequence(QKeySequence());
                return true;
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CustomToolListDialog::loadRow(int row) {
    if (row < 0 || row >= m_tools.size())
        return;
    m_updating = true;
    m_nameEdit->setText(m_tools[row].name);
    m_commandEdit->setText(m_tools[row].command);
    m_keyEdit->setKeySequence(m_tools[row].key);
    m_barShowCheck->setChecked(m_tools[row].barShow);
    m_barSideCombo->setCurrentIndex(m_tools[row].barSide == BarSide::Right ? 1 : 0);
    m_barIconEdit->setText(m_tools[row].barIcon);
    updateBarControlsEnabled();
    updateButtons();
    m_updating = false;
}

void CustomToolListDialog::updateButtons() {
    const int row = m_list->currentRow();
    m_upButton->setEnabled(row > 0);
    m_downButton->setEnabled(row >= 0 && row < m_tools.size() - 1);
}

void CustomToolListDialog::updateBarControlsEnabled() {
    const bool on = m_barShowCheck->isChecked();
    m_barSideCombo->setEnabled(on);
    m_barIconEdit->setEnabled(on);
    m_barIconButton->setEnabled(on);
}

void CustomToolListDialog::addTool() {
    m_tools.append(CustomTool{});
    m_list->addItem(listText(QString()));
    m_list->setCurrentRow(m_list->count() - 1);
    updateButtons();
    m_nameEdit->setFocus();
}

void CustomToolListDialog::deleteTool() {
    const int row = m_list->currentRow();
    if (row < 0)
        return;
    m_updating = true;
    delete m_list->takeItem(row);
    m_tools.removeAt(row);
    m_updating = false;
    bool seeded = false;
    if (m_tools.isEmpty()) {
        m_tools.append(CustomTool{});
        m_list->addItem(listText(QString()));
        seeded = true;
    }
    m_list->setCurrentRow(qMin(row, m_list->count() - 1));
    loadRow(m_list->currentRow()); // setCurrentRow 落在同行时不再发信号
    if (seeded)
        m_nameEdit->setFocus();
}

void CustomToolListDialog::moveTool(int delta) {
    const int row = m_list->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_tools.size())
        return;
    m_tools[row].name = m_nameEdit->text(); // 未提交的编辑随行带走
    m_tools[row].command = m_commandEdit->text();
    m_tools[row].key = m_keyEdit->keySequence();
    m_tools[row].barShow = m_barShowCheck->isChecked();
    m_tools[row].barSide = m_barSideCombo->currentIndex() == 1 ? BarSide::Right : BarSide::Left;
    m_tools[row].barIcon = m_barIconEdit->text();
    m_tools.swapItemsAt(row, target);
    m_updating = true;
    QListWidgetItem *item = m_list->takeItem(row);
    m_list->insertItem(target, item);
    m_updating = false;
    m_list->setCurrentRow(target);
    loadRow(target);
}

// 校验与规范化全数收口在 CustomToolManager::sanitize；对话框只管把违规摊到行上。
// N138：工具间重键拦下，AM 占用只提示、照常保存
void CustomToolListDialog::accept() {
    using Problem = CustomToolManager::Problem;
    CustomToolManager::Sanitized sanitized = CustomToolManager::sanitize(m_tools);
    if (sanitized.problem != Problem::None) {
        m_list->setCurrentRow(sanitized.row);
        if (sanitized.problem == Problem::DuplicateKey) {
            m_keyEdit->setFocus();
            QMessageBox::warning(
                this, windowTitle(),
                tr("The key \"%1\" is already used by another tool.").arg(sanitized.key));
            return;
        }
        // 缺哪个字段聚焦哪个；无效路径与命名冲突都归名称一侧
        (sanitized.problem == Problem::MissingCommand ? m_commandEdit : m_nameEdit)->setFocus();
        QMessageBox::warning(
            this, windowTitle(),
            sanitized.problem == Problem::PrefixConflict
                ? tr("\"%1\" cannot be both a tool and a submenu.").arg(sanitized.path)
                : tr("Each tool needs both a name and a command."));
        return;
    }
    // 名称或命令有空的行只会被 sanitize 静默丢弃，坏图标随占位行丢弃；跳过校验。
    // 其余行 barShow 且图标非空时名字必须在清单内；m_tools 下标即对话框行号
    for (int i = 0; i < m_tools.size(); ++i) {
        const QString icon = m_tools[i].barIcon.trimmed();
        if (!m_tools[i].barShow || icon.isEmpty())
            continue;
        if (CustomToolManager::normalizedPath(m_tools[i].name).isEmpty() ||
            m_tools[i].command.trimmed().isEmpty())
            continue;
        if (!MaterialIconCatalog::contains(icon)) {
            m_list->setCurrentRow(i);
            m_barIconEdit->setFocus();
            QMessageBox::warning(
                this, windowTitle(),
                tr("\"%1\" is not a valid Material Symbols ligature name.").arg(icon));
            return;
        }
    }
    // 重键已被 sanitize 拦下，此处 conflict 只会是 AM 占用；提示不拦截
    const CustomToolManager::KeyPlan plan =
        CustomToolManager::planKeys(sanitized.tools, m_actions->claimedKeys());
    if (!plan.conflicts.isEmpty()) {
        QStringList lines;
        for (const CustomToolManager::KeyConflict &c : plan.conflicts) {
            lines << QStringLiteral("%1 (%2)").arg(c.key.toString(QKeySequence::PortableText),
                                                   c.owner);
        }
        QMessageBox::information(
            this, windowTitle(),
            tr("These keys are already used by the application, so these shortcuts will "
               "not trigger. The menu entries still work:\n%1")
                .arg(lines.join(QLatin1Char('\n'))));
    }
    m_tools = std::move(sanitized.tools); // 全部为空条目时即空表，保存空列表
    QDialog::accept();
}

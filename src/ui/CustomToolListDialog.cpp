#include "pch.h"

#include "Constants.h"
#include "ThemedIconEngine.h"
#include "ui/CustomToolListDialog.h"

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
    // 裁决。Qt < 6.5 无此属性即降级——这只是录入体验，不是硬限制（手改 INI 收多段）
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    m_keyEdit->setMaximumSequenceLength(1);
#endif
    if (auto *le = m_keyEdit->findChild<QLineEdit *>())
        le->setContextMenuPolicy(Qt::NoContextMenu); // 内建右键菜单会遮住记录器自身
    m_keyEdit->installEventFilter(this);
    m_noteEdit = new QLineEdit(this);

    auto *form = new QFormLayout;
    form->addRow(tr("Name"), m_nameEdit);
    form->addRow(QString(), nameHint);
    form->addRow(tr("Command"), m_commandEdit);
    form->addRow(QString(), commandHint);
    form->addRow(tr("Key"), m_keyEdit);
    form->addRow(tr("Note"), m_noteEdit);

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
    connect(m_noteEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_updating)
            return;
        m_tools[m_list->currentRow()].note = text;
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
    m_addButton->setIcon(Ui::themedIcon(App::iconPath(App::iconAdd()), tint));
    m_deleteButton->setIcon(Ui::themedIcon(App::iconPath(App::iconRemove()), tint));
    m_upButton->setIcon(Ui::themedIcon(App::iconPath(App::iconMoveUp()), tint));
    m_downButton->setIcon(Ui::themedIcon(App::iconPath(App::iconMoveDown()), tint));
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
    m_noteEdit->setText(m_tools[row].note);
    updateButtons();
    m_updating = false;
}

void CustomToolListDialog::updateButtons() {
    const int row = m_list->currentRow();
    m_upButton->setEnabled(row > 0);
    m_downButton->setEnabled(row >= 0 && row < m_tools.size() - 1);
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
    m_tools[row].note = m_noteEdit->text();
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

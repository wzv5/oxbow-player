#include "pch.h"

#include "ShortcutsDialog.h"

namespace {

// 键序列比较一律经 toString() 归一：等价序列在 operator== 下可能不等价
bool keysEqual(const QKeySequence &a, const QKeySequence &b) {
    return a.toString(QKeySequence::PortableText) == b.toString(QKeySequence::PortableText);
}

// 内部行编辑自带 Select All / Copy 菜单，会遮住行级右键菜单——关掉使事件穿透到
// 编辑框自己的 CustomContextMenu。
// 占位文本不触碰：Qt 按自己的时机把它写回（清空后按 Del 可见其复现），稳不住，
// 空序列行就按原生形态显示占位文案
void disableInternalLineEditMenu(QKeySequenceEdit *edit) {
    if (auto *le = edit->findChild<QLineEdit *>())
        le->setContextMenuPolicy(Qt::NoContextMenu);
}

} // namespace

ShortcutsDialog::ShortcutsDialog(ActionManager *actions, const QString &iniPath, QWidget *parent)
    : QDialog(parent), m_actions(actions), m_iniPath(iniPath) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    // 不设模态：行内捕获按键要窗口能收到事件
    resize(480, 570);
    // 构造期只建骨架，键位取值单点在 showEvent——否则首开读两遍 ini、警告各刷两遍

    auto *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(2); // 只有两列：状态走行级视觉，操作走右键菜单
    m_table->setHorizontalHeaderLabels({tr("Action"), tr("Key")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setIconSize(QSize(0, 0));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection); // 多选：右键批量
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->setColumnWidth(1,
                            fontMetrics().horizontalAdvance(QStringLiteral("Ctrl+Shift+X")) + 48);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QWidget::customContextMenuRequested, this, &ShortcutsDialog::showContextMenu);

    for (ActionManager::Id id : m_actions->orderedIds()) {
        // 不可改绑的条目不进编辑器
        if (!m_actions->modifiable(id))
            continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_rowOrder.append(id);
        m_rowOf.insert(id, row);

        // 动作列原样取文案：助记符在表格里只作字面字符渲染，不加工
        auto *nameItem = new QTableWidgetItem(m_actions->displayText(id));
        nameItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        m_table->setItem(row, 0, nameItem);
        auto *keyItem = new QTableWidgetItem;
        keyItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        m_table->setItem(row, 1, keyItem);

        auto *edit = new QKeySequenceEdit(m_table);
        // 只认单组合键：原生可录 4 键序列（如 "S, S, S, S"），限长 1 后按下第一个
        // 组合键即结束捕获。Qt < 6.5 无此属性，降级为原生行为
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        edit->setMaximumSequenceLength(1);
#endif
        disableInternalLineEditMenu(edit);
        // Del / 退格 / Esc 清空，见 eventFilter
        edit->installEventFilter(this);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [this, id](const QKeySequence &seq) { onRowChanged(id, seq); });
        // 编辑器上的右键也转给行级菜单
        edit->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(edit, &QWidget::customContextMenuRequested, this, [this, edit](const QPoint &pos) {
            showContextMenu(edit->mapTo(m_table->viewport(), pos));
        });
        m_table->setCellWidget(row, 1, edit);
    }

    // setCellWidget 不撑高行：取编辑器自然高度，否则裁掉边框
    int rowHeight = fontMetrics().height() + 8;
    if (auto *first = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(0, 1)))
        rowHeight = qMax(rowHeight, first->sizeHint().height());
    m_table->verticalHeader()->setDefaultSectionSize(rowHeight);

    layout->addWidget(m_table, 1);

    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_applyButton = m_buttonBox->button(QDialogButtonBox::Apply);
    m_applyButton->setEnabled(false); // 初值禁用，由 dirty 驱动
    connect(m_applyButton, &QPushButton::clicked, this, &ShortcutsDialog::apply);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, [this] {
        if (apply())
            accept();
    });
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttonBox);
}

int ShortcutsDialog::rowOf(ActionManager::Id id) const { return m_rowOf.value(id, -1); }

// reject() 只 hide、不发 Close 事件，WA_DeleteOnClose 不生效：复用会命中同一对象，
// 故先回滚再隐藏，被 Cancel 丢弃的改动不会在重开时复活
void ShortcutsDialog::reject() {
    resetFromSnapshot();
    QDialog::reject();
}

void ShortcutsDialog::showEvent(QShowEvent *event) {
    // 每次打开都重读 ini：手改 ini 或主窗重载之后快照得跟上现状，dirty 才不失真
    QDialog::showEvent(event);
    m_snapshot = m_actions->effectiveKeys(m_iniPath);
    resetFromSnapshot();
}

// 把编辑器与 pending 拉回当前快照：回滚（reject / 落盘失败）与重载（showEvent）
// 只是调用理由不同，共用同一段代码
void ShortcutsDialog::resetFromSnapshot() {
    m_batchUpdating = true;
    for (auto it = m_snapshot.cbegin(); it != m_snapshot.cend(); ++it) {
        if (auto *edit = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(rowOf(it.key()), 1)))
            edit->setKeySequence(it.value());
    }
    m_pending = m_snapshot;
    m_batchUpdating = false;
    refreshRowVisuals();
    updateApplyButton();
}

void ShortcutsDialog::onRowChanged(ActionManager::Id id, const QKeySequence &seq) {
    m_pending.insert(id, seq);
    if (!m_batchUpdating) {
        // 单行改动即时重算：修正冲突后高亮立刻清除，不等 Apply
        refreshRowVisuals();
        updateApplyButton();
    }
}

bool ShortcutsDialog::eventFilter(QObject *watched, QEvent *event) {
    // Del / 退格 / Esc 清空该行而非记作快捷键（Esc 是退出闭环专用键，不参与设置）。
    // 裸键才接管：带修饰键的组合是合法录制目标，小键盘的 KeypadModifier 不算修饰。
    // 清空经既有 keySequenceChanged 路径更新 pending 与视觉
    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if ((ke->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier &&
            (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace ||
             ke->key() == Qt::Key_Escape)) {
            if (auto *edit = qobject_cast<QKeySequenceEdit *>(watched)) {
                edit->setKeySequence(QKeySequence());
                return true;
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

QColor ShortcutsDialog::conflictTint() const {
    // 高亮色 25% 混入底色：全取调色板，随主题变化
    const QPalette pal = palette();
    const QColor base = pal.color(QPalette::Active, QPalette::Base);
    const QColor hl = pal.color(QPalette::Active, QPalette::Highlight);
    return QColor(base.red() + (hl.red() - base.red()) / 4,
                  base.green() + (hl.green() - base.green()) / 4,
                  base.blue() + (hl.blue() - base.blue()) / 4);
}

QList<int> ShortcutsDialog::conflictRows() const {
    // 按 toString() 归一串分组，非空且相同者即冲突；空序列不参与。不可改绑条目
    // （如 Escape）不在表里但占着键位，其声明默认以 -1 哨兵并入分组、不进结果
    QHash<QString, QList<int>> groups;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QKeySequence seq = m_pending.value(m_rowOrder.at(row));
        if (!seq.isEmpty())
            groups[seq.toString(QKeySequence::PortableText)].append(row);
    }
    for (ActionManager::Id id : m_actions->orderedIds()) {
        if (m_actions->modifiable(id))
            continue;
        const QKeySequence seq = m_actions->defaultKey(id);
        if (!seq.isEmpty())
            groups[seq.toString(QKeySequence::PortableText)].append(-1);
    }
    QList<int> rows;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        if (it.value().size() > 1) {
            for (int row : it.value()) {
                if (row >= 0)
                    rows.append(row);
            }
        }
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

// 冲突底色由 Base / Highlight 现算后写入条目并 pin 进编辑框，调色板变更不会自跟随；
// 本对话框非模态、复用不重建，故外观事件到达时当场重算（m_table 守卫兜住建成前的事件）
void ShortcutsDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) && m_table)
        refreshRowVisuals();
}

void ShortcutsDialog::refreshRowVisuals() {
    // 两种信号独立叠加：加粗 = 与声明默认不同；高亮 = 冲突
    const QList<int> conflicts = conflictRows();
    const QSet<int> conflictSet(conflicts.cbegin(), conflicts.cend());
    const QColor tint = conflictTint();
    const QFont plain = m_table->font();
    QFont bold = plain;
    bold.setBold(true);
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const ActionManager::Id id = m_rowOrder.at(row);
        const bool dirty = !keysEqual(m_pending.value(id), m_actions->defaultKey(id));
        const bool conflicted = conflictSet.contains(row);
        m_table->item(row, 0)->setFont(dirty ? bold : plain);
        // cell widget 会盖住 item 背景，故第二列的底色要两处同步设
        m_table->item(row, 0)->setData(Qt::BackgroundRole,
                                       conflicted ? QVariant(QBrush(tint)) : QVariant());
        m_table->item(row, 1)->setData(Qt::BackgroundRole,
                                       conflicted ? QVariant(QBrush(tint)) : QVariant());
        if (auto *edit = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(row, 1))) {
            QPalette pal = edit->palette();
            pal.setColor(QPalette::Base, conflicted ? tint : palette().color(QPalette::Base));
            edit->setPalette(pal);
        }
    }
}

void ShortcutsDialog::updateApplyButton() {
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (!keysEqual(it.value(), m_snapshot.value(it.key()))) {
            m_applyButton->setEnabled(true);
            return;
        }
    }
    m_applyButton->setEnabled(false);
}

void ShortcutsDialog::showContextMenu(const QPoint &viewportPos) {
    const QList<QModelIndex> selection = m_table->selectionModel()->selectedRows();
    QMenu menu(this);
    QAction *clearAct = menu.addAction(tr("Clear"));
    QAction *resetAct = menu.addAction(tr("Restore Defaults"));
    // 无选中行时两项皆灰
    clearAct->setEnabled(!selection.isEmpty());
    resetAct->setEnabled(!selection.isEmpty());
    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(viewportPos));
    if (!chosen)
        return;
    // 只改控件不动 ini：清空与恢复默认是两个不同动作，落盘收在 Apply
    m_batchUpdating = true;
    if (chosen == clearAct) {
        for (const QModelIndex &idx : selection) {
            if (auto *edit = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(idx.row(), 1)))
                edit->setKeySequence(QKeySequence());
        }
    } else if (chosen == resetAct) {
        // 无声明默认的动作，恢复默认即清空
        for (const QModelIndex &idx : selection) {
            if (auto *edit = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(idx.row(), 1)))
                edit->setKeySequence(m_actions->defaultKey(m_rowOrder.at(idx.row())));
        }
    }
    m_batchUpdating = false;
    refreshRowVisuals();
    updateApplyButton();
}

bool ShortcutsDialog::apply() {
    // 冲突不通过：不落盘、不关窗
    const QList<int> conflicts = conflictRows();
    if (!conflicts.isEmpty()) {
        refreshRowVisuals();
        m_table->scrollToItem(m_table->item(conflicts.first(), 0),
                              QAbstractItemView::EnsureVisible);
        QStringList lines;
        QSet<QString> fixed; // 与不可改绑条目同键时，固定方也要点名
        for (int row : conflicts) {
            lines << m_table->item(row, 0)->text();
            const QKeySequence &seq = m_pending.value(m_rowOrder.at(row));
            for (ActionManager::Id id : m_actions->orderedIds()) {
                if (!m_actions->modifiable(id) && keysEqual(m_actions->defaultKey(id), seq))
                    fixed.insert(m_actions->displayText(id));
            }
        }
        QStringList fixedList(fixed.cbegin(), fixed.cend());
        std::sort(fixedList.begin(), fixedList.end());
        lines += fixedList;
        QMessageBox::warning(this, tr("Conflicting Shortcuts"),
                             tr("These actions share the same key sequence:\n%1")
                                 .arg(lines.join(QLatin1Char('\n'))));
        return false;
    }

    // 只写与 snapshot 有差异者；不可改绑条目显式跳过，防写出 Escape=-- 废键
    QSettings ini(m_iniPath, QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("shortcuts"));
    for (ActionManager::Id id : m_actions->orderedIds()) {
        if (!m_actions->modifiable(id))
            continue;
        const QKeySequence pending = m_pending.value(id);
        if (keysEqual(pending, m_snapshot.value(id)))
            continue;
        const QString name = m_actions->name(id);
        if (pending.isEmpty()) {
            // 有默认存 "--"（删键会弹回默认），无默认删键（与未设键等效）
            if (!m_actions->defaultKey(id).isEmpty())
                ini.setValue(name, QStringLiteral("--"));
            else
                ini.remove(name);
        } else {
            const QString text = pending.toString(QKeySequence::PortableText);
            // "--" 不可能由合法序列产出；真出现即断言显影，不静默错判
            Q_ASSERT(text != QLatin1String("--"));
            // 等于声明默认则删键，不留冗余覆盖
            if (keysEqual(pending, m_actions->defaultKey(id)))
                ini.remove(name);
            else
                ini.setValue(name, text);
        }
    }
    ini.sync();
    if (ini.status() != QSettings::NoError) {
        // 落盘失败：回滚控件并报错，不留"界面新、ini 旧"
        resetFromSnapshot();
        QMessageBox::warning(this, tr("Shortcuts Not Saved"),
                             tr("Writing the configuration file failed; all changes "
                                "were reverted."));
        return false;
    }

    // 落盘成功即生效：主窗口经 applied() 从 ini 重载键位
    m_snapshot = m_pending;
    refreshRowVisuals();
    updateApplyButton();
    emit applied();
    return true;
}

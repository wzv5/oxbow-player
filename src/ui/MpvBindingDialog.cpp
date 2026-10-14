#include "pch.h"

#include "MpvBindingDialog.h"

#include "mpvbridge/MpvKeyName.h" // N89：键名归一 / 反解 / 序列化（同键分组与键列共用）

namespace {

// 是否过滤鼠标键位（MBTN* / MOUSE* / WHEEL* / AXIS*）：插件当前不向 mpv 转发鼠标
// 消息，mpv 侧的鼠标绑定永不触发，列出来只添噪。未来接入鼠标转发后改回 false
constexpr bool kFilterMouseBindings = true;

// 鼠标键位判定：组合键落在鼠标键族即算。大小写不敏感——自定义档的键名是文件
// 原文，未必是 mpv 归一形
bool isMouseKey(const QString &key) {
    const QString lower = key.toLower();
    return lower.contains(QLatin1String("mbtn")) || lower.contains(QLatin1String("mouse")) ||
           lower.contains(QLatin1String("wheel")) || lower.contains(QLatin1String("axis"));
}

// 灰（禁用态）标记：条目携带，委托按选中态取 QPalette::Disabled 的 Text /
// HighlightedText（§3.1——灰只改前景色，绝不动 ItemIsEnabled）
constexpr int kGrayRole = Qt::UserRole + 1;

// 内部行编辑自带 Select All / Copy 菜单，会遮住行级右键菜单——关掉使事件穿透到
// 编辑器自己的 CustomContextMenu（N86 同款）
void disableInternalLineEditMenu(QKeySequenceEdit *edit) {
    if (auto *le = edit->findChild<QLineEdit *>())
        le->setContextMenuPolicy(Qt::NoContextMenu);
}

// 灰行绘制：未选中行取 QPalette::Disabled, Text，选中态改取 Disabled, HighlightedText，
// 选择反馈仍保留但字色可辨（条目级 ForegroundRole 分不出选中态，须在委托里改）
class GrayDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        if (!index.data(kGrayRole).toBool()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        const QPalette &pal = opt.palette;
        // 选中与未选中两态同取 QPalette::Disabled, Text：Disabled / HighlightedText 在
        // Windows 浅色调色板里近黑，选中即黑字，字色反而不如底色能交代选择；选择反馈
        // 交给底色，字色两态保持同一灰
        const QColor gray = pal.color(QPalette::Disabled, QPalette::Text);
        opt.palette.setColor(QPalette::All, QPalette::Text, gray);
        opt.palette.setColor(QPalette::All, QPalette::HighlightedText, gray);
        QStyledItemDelegate::paint(painter, opt, index);
    }
};

} // namespace

MpvBindingDialog::MpvBindingDialog(MpvController *mpv, ActionManager *actions, QWidget *parent)
    : QDialog(parent), m_mpv(mpv), m_actions(actions) {
    setWindowTitle(tr("mpv Key Bindings"));
    // 不设模态：行内捕获按键要窗口能收到事件（N86 同款）
    resize(640, 570);

    auto *layout = new QVBoxLayout(this);

    // 三行布局（§3.6）：过滤框 / 列表 / 按钮行
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter"));
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(150);
    // 节流：textChanged 只重启一个单次 QTimer，到点才重扫全表
    connect(m_filter, &QLineEdit::textChanged, m_filterTimer, qOverload<>(&QTimer::start));
    connect(m_filterTimer, &QTimer::timeout, this, [this] {
        // 过滤条件一变即清空选中集（在定时器那一轮里做）
        m_table->clearSelection();
        refreshRowVisuals();
    });
    layout->addWidget(m_filter);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4); // 来源 / 命令 / 键 / 注释：状态走行级视觉，操作走右键菜单
    m_table->setHorizontalHeaderLabels({tr("Source"), tr("Command"), tr("Key"), tr("Comment")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setIconSize(QSize(0, 0));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection); // 多选：右键批量
    // 命令 / 注释列就地可编辑（自定义行与空白行）；键列走记录器控件，不经条目编辑
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setColumnWidth(0, fontMetrics().horizontalAdvance(tr("Custom")) + 40);
    m_table->setColumnWidth(2,
                            fontMetrics().horizontalAdvance(QStringLiteral("Ctrl+Shift+X")) + 48);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QWidget::customContextMenuRequested, this,
            &MpvBindingDialog::showContextMenu);
    connect(m_table, &QTableWidget::itemChanged, this, &MpvBindingDialog::onItemChanged);
    m_table->setItemDelegate(new GrayDelegate(m_table));

    // setCellWidget 不撑高行：取记录器自然高度，否则裁掉边框
    layout->addWidget(m_table, 1);
    const int rowHeight = fontMetrics().height() + 8;
    m_table->verticalHeader()->setDefaultSectionSize(rowHeight);

    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_applyButton = m_buttonBox->button(QDialogButtonBox::Apply);
    m_okButton = m_buttonBox->button(QDialogButtonBox::Ok);
    m_applyButton->setEnabled(false); // 初值禁用，由脏态驱动
    connect(m_applyButton, &QPushButton::clicked, this, &MpvBindingDialog::apply);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, [this] {
        if (apply())
            accept();
    });
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttonBox);
}

void MpvBindingDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    rebuild(); // 每次打开重读：input.conf 与 input-bindings 各一次，快照随之重建
}

bool MpvBindingDialog::eventFilter(QObject *watched, QEvent *event) {
    // Del / 退格 / Esc 清空该行而非记作键（裸键才接管：带修饰键的组合是合法录制
    // 目标，小键盘的 KeypadModifier 不算修饰）。清空经既有 keySequenceChanged 路径
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

void MpvBindingDialog::rebuild() {
    m_batchUpdating = true;
    m_failedLock = false;
    m_deletedLines = 0;

    // 配置路径（§6）：打开时调一次。拿不到路径（--no-config）与文件存在却读不到走
    // 同一口径——报错、锁死编辑与 Apply，没有任何补偿路径（重启口径下写不了文件
    // 就等于改动无处可生效）
    m_confPath = m_mpv->inputConfPath();
    m_noPath = m_confPath.isEmpty();
    m_conf.reset();
    QString loadError;
    if (m_noPath) {
        loadError = tr("No writable configuration directory; key bindings cannot be edited.");
    } else {
        // 键名合法性判定器（变更记录 2）：经 MpvKeyName 反解认合法
        m_conf = InputConf::open(m_confPath, [](const QString &name) {
            return !MpvKey::sequenceFromName(name).isEmpty();
        });
        if (!m_conf)
            loadError = tr("Could not read the configuration file; key bindings cannot be edited.");
    }

    // 两档只读数据（打开时读一次，手册明写该属性不支持变更通知）
    const auto tiers = m_mpv->inputBindings();
    m_builtinKeys.clear();
    for (const auto &b : tiers.builtins)
        m_builtinKeys.insert(MpvKey::normalizedKeyName(b.key));

    m_rows.clear();
    int parseFailures = 0;
    for (const auto &b : tiers.builtins) {
        Row r;
        r.tier = Row::Tier::Builtin;
        r.key = b.key;
        r.command = b.cmd;
        r.comment = b.comment;
        m_rows.append(r);
    }
    for (const auto &b : tiers.scripts) {
        Row r;
        r.tier = Row::Tier::Script;
        r.key = b.key;
        r.command = b.cmd;
        r.comment = b.comment;
        m_rows.append(r);
    }
    if (m_conf) {
        const QList<InputConfLine> &lines = m_conf->lines();
        for (int i = 0; i < lines.size(); ++i) {
            if (lines.at(i).kind != InputConfLine::Kind::KeyValue)
                continue; // Comment / Directive / Empty 不进列表（Directive 不显示）
            Row r;
            r.tier = Row::Tier::Custom;
            r.line = i;
            r.origKey = lines.at(i).key;
            r.origCommand = lines.at(i).command;
            r.origComment = lines.at(i).comment;
            r.origDisabled = lines.at(i).disabled;
            // 载入逐行反解：解不出 → 键列留空（数据层保留原文）并计数。
            // 鼠标键位行不在计数之列：它们被过滤隐藏，提示只服务可见的留空行
            if (!isMouseKey(r.origKey) && MpvKey::sequenceFromName(r.origKey).isEmpty())
                ++parseFailures;
            m_rows.append(r);
        }
    }
    // 内置行打开时的生效态快照（脏比对基准）
    const QHash<QString, int> eff = effectiveCustomRows();
    for (Row &r : m_rows)
        if (r.tier == Row::Tier::Builtin)
            r.origState = builtinState(r, eff);

    m_blankCommand.clear();
    m_blankComment.clear();
    rebuildTable();
    m_batchUpdating = false;

    if (parseFailures > 0)
        QTimer::singleShot(0, this, [this] {
            // 每趟打开最多弹一次，不逐行弹、不列明细（§3.2）
            QMessageBox::information(this, tr("mpv Key Bindings"),
                                     tr("Some key sequences could not be parsed."));
        });
    if (!loadError.isEmpty()) {
        QTimer::singleShot(0, this, [this, loadError] {
            QMessageBox::warning(this, tr("mpv Key Bindings"), loadError);
        });
    }
}

void MpvBindingDialog::rebuildTable() {
    // 整表重排期间守卫 itemChanged（setRowCount / setItem 会触发）
    const bool wasBatch = m_batchUpdating;
    m_batchUpdating = true;
    m_table->setRowCount(0);
    for (int row = 0; row < m_rows.size(); ++row) {
        m_table->insertRow(row);
        populateRow(row);
    }
    populateBlankRow(); // 空白录入行恒建：不可编辑时只是摘掉可编辑位。
                        // rowCount == m_rows.size() + 1 是不变量，末行随处可取
    Q_ASSERT(m_table->rowCount() == m_rows.size() + 1);
    m_batchUpdating = wasBatch;
    refreshRowVisuals();
    updateApplyButton();
}

void MpvBindingDialog::populateRow(int row) {
    const Row &r = m_rows.at(row);
    const bool custom = r.tier == Row::Tier::Custom;
    const QString source = r.tier == Row::Tier::Builtin  ? tr("Builtin")
                           : r.tier == Row::Tier::Script ? tr("Script")
                                                         : tr("Custom");
    const QString command = rowCommand(row);
    const QString comment = rowComment(row);

    auto *srcItem = new QTableWidgetItem(source);
    srcItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled); // 只读、非分组控件
    m_table->setItem(row, 0, srcItem);

    auto *cmdItem = new QTableWidgetItem(command);
    cmdItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled |
                      (custom ? Qt::ItemIsEditable : Qt::ItemFlags()));
    m_table->setItem(row, 1, cmdItem);

    // 键列：自定义行被记录器控件盖住（条目仅承载选中）；内置 / 脚本行只读文本
    auto *keyItem = new QTableWidgetItem(custom ? QString() : r.key);
    keyItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    m_table->setItem(row, 2, keyItem);
    if (custom)
        makeRecorder(row, MpvKey::sequenceFromName(m_conf->lines().at(r.line).key));

    auto *cmtItem = new QTableWidgetItem(comment);
    cmtItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled |
                      (custom ? Qt::ItemIsEditable : Qt::ItemFlags()));
    m_table->setItem(row, 3, cmtItem);
}

QKeySequenceEdit *MpvBindingDialog::makeRecorder(int row, const QKeySequence &seq) {
    auto *edit = new QKeySequenceEdit(m_table);
    // 不调用 setMaximumSequenceLength：默认即容 4 段（QKeySequence 本身上限；mpv 上限
    // 16，超出的走"解不出"留空支）——低版本 Qt 无此方法，调用即崩（§3.2 / 验收 13 明禁）
    disableInternalLineEditMenu(edit);
    edit->installEventFilter(this); // Del / 退格 / Esc 清空，见 eventFilter
    edit->setKeySequence(seq);
    // 行号不捕获、信号时刻现查：转正插行 / 删除重建都会移动行，捕获行号必然过期
    // （实测：空白行录制器捕获的行号在转正后指向刚转正那行，后续按键改错行）
    connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this, edit](const QKeySequence &s) {
        const int r = rowOfEditor(edit);
        if (r >= 0)
            onRecorderChanged(r, s);
    });
    // 编辑器上的右键也转给行级菜单（N86 同款）
    edit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(edit, &QWidget::customContextMenuRequested, this, [this, edit](const QPoint &pos) {
        showContextMenu(edit->mapTo(m_table->viewport(), pos));
    });
    m_table->setCellWidget(row, 2, edit);
    return edit;
}

int MpvBindingDialog::rowOfEditor(const QKeySequenceEdit *edit) const {
    for (int row = 0; row <= m_rows.size(); ++row) {
        if (qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(row, 2)) == edit)
            return row; // 末行（== m_rows.size()）即空白录入行
    }
    return -1;
}

void MpvBindingDialog::populateBlankRow() {
    const int row = blankRow();
    // 不可编辑（无路径 / 文件读不到）时末行只作占位：命令 / 注释摘掉可编辑位，
    // 录制器禁可用，避免用户往一个永远写不出去的表里填东西
    const Qt::ItemFlags fillable =
        Qt::ItemIsEnabled | (editable() ? Qt::ItemIsEditable : Qt::ItemFlags());
    m_table->insertRow(row);
    // 来源列空且不可编辑（§3.4）：来源列编辑无落点，还会在转正插行时把打开中的
    // 单元格编辑器锚错行，看起来整表错位
    auto *srcItem = new QTableWidgetItem;
    srcItem->setFlags(Qt::ItemIsEnabled);
    m_table->setItem(row, 0, srcItem);
    auto *cmdItem = new QTableWidgetItem(m_blankCommand);
    cmdItem->setFlags(fillable);
    m_table->setItem(row, 1, cmdItem);
    auto *keyItem = new QTableWidgetItem;
    keyItem->setFlags(Qt::ItemIsEnabled); // 键列由记录器承载，条目仅占位
    m_table->setItem(row, 2, keyItem);
    auto *cmtItem = new QTableWidgetItem(m_blankComment);
    cmtItem->setFlags(fillable);
    m_table->setItem(row, 3, cmtItem);
    m_blankEditor = makeRecorder(row, QKeySequence());
    m_blankEditor->setEnabled(editable());
}

void MpvBindingDialog::onRecorderChanged(int row, const QKeySequence &seq) {
    if (m_batchUpdating || !editable())
        return;
    if (row >= m_rows.size()) {
        checkBlankPromotion(); // 空白录入行：键已进编辑器，转正判据实时查
        return;
    }
    const Row &r = m_rows.at(row);
    if (r.tier != Row::Tier::Custom)
        return;
    // 录到的序列化成 mpv 键名写回模型；清空 → 空（Apply 校验拦住，见 apply()）。
    // 未被用户改过的行不经过这里，数据层保留原文（含反解不出的原始串）
    const InputConfLine &l = m_conf->lines().at(r.line);
    m_conf->setLineFields(r.line, MpvKey::nameFromSequence(seq), l.command, l.comment);
    afterEdit();
}

void MpvBindingDialog::onItemChanged(QTableWidgetItem *item) {
    if (m_batchUpdating || !item || !editable())
        return;
    const int row = item->row();
    if (row >= m_rows.size()) {
        // 空白录入行的命令 / 注释（成员是跨重排唯一的留存处，条目每次重建都读它）
        if (item->column() == 1)
            m_blankCommand = item->text();
        else if (item->column() == 3)
            m_blankComment = item->text();
        checkBlankPromotion(); // 转正与否都由它收口一次视觉
        return;
    }
    const Row &r = m_rows.at(row);
    if (r.tier != Row::Tier::Custom)
        return; // 内置 / 脚本行 flags 挡住编辑态，兜底忽略
    const InputConfLine &l = m_conf->lines().at(r.line);
    if (item->column() == 1)
        m_conf->setLineFields(r.line, l.key, item->text(), l.comment);
    else if (item->column() == 3)
        m_conf->setLineFields(r.line, l.key, l.command, item->text());
    else
        return;
    afterEdit();
}

void MpvBindingDialog::checkBlankPromotion() {
    if (!m_blankEditor)
        return;
    // 转正规则（§3.4）：键与命令都非空 → 转正为一条 pending 自定义行，落在自定义档
    // 末尾；空白行自身重置后继续充当末尾录入行
    const QString key = MpvKey::nameFromSequence(m_blankEditor->keySequence());
    if (!key.isEmpty() && !m_blankCommand.isEmpty()) {
        appendCustomRow(key, m_blankCommand, m_blankComment);
        m_blankCommand.clear();
        m_blankComment.clear();
        m_batchUpdating = true; // 清空回写不得再触发一次转正
        m_blankEditor->setKeySequence(QKeySequence());
        m_table->item(blankRow(), 1)->setText(QString());
        m_table->item(blankRow(), 3)->setText(QString());
        m_batchUpdating = false;
    }
    afterEdit(); // 未转正也要刷：空白行的 tooltip 随输入变化
}

int MpvBindingDialog::appendCustomRow(const QString &key, const QString &command,
                                      const QString &comment) {
    Q_ASSERT(editable()); // 无路径 / 无模型时不该有新增行（菜单与转正都已按此收口）
    const int lineIdx = m_conf->appendLine(key, command, comment);
    Row r;
    r.tier = Row::Tier::Custom;
    r.line = lineIdx;
    r.origKey = key;
    r.origCommand = command;
    r.origComment = comment;
    r.appended = true; // 本轮新增行，恒脏
    m_rows.append(r);
    const int row = m_rows.size() - 1;
    m_batchUpdating = true;  // insertRow / setItem 会触发 itemChanged，守卫之
    m_table->insertRow(row); // 落在空白录入行之前（空白行恒为末行）
    populateRow(row);
    m_batchUpdating = false;
    return row;
}

void MpvBindingDialog::removeCustomRow(int row) {
    // 删除不进逐行快照比对（行与快照同归于尽），以计数入脏；本轮新增行不计——
    // 不在打开时的快照里，删掉即无净变化（禁用内置 → 启用的往返因此归零）
    if (!m_rows.at(row).appended)
        ++m_deletedLines;
    const int lineIdx = m_rows.at(row).line;
    m_conf->removeLine(lineIdx);
    m_rows.removeAt(row);
    for (Row &r : m_rows)
        if (r.tier == Row::Tier::Custom && r.line > lineIdx)
            --r.line;
    rebuildTable(); // 行删除使后续下标漂移：整表重排（记录器行号随之重建）
}

QString MpvBindingDialog::rowKeyText(int row) const {
    const Row &r = m_rows.at(row);
    return r.tier == Row::Tier::Custom ? m_conf->lines().at(r.line).key : r.key;
}

QString MpvBindingDialog::rowCommand(int row) const {
    const Row &r = m_rows.at(row);
    return r.tier == Row::Tier::Custom ? m_conf->lines().at(r.line).command : r.command;
}

QString MpvBindingDialog::rowComment(int row) const {
    const Row &r = m_rows.at(row);
    return r.tier == Row::Tier::Custom ? m_conf->lines().at(r.line).comment : r.comment;
}

// 逐行脏判据（§3.1 加粗）：与打开时的快照比内容与禁用态；本轮新增行恒脏
bool MpvBindingDialog::customRowDirty(const Row &row, const InputConfLine &l) const {
    return row.appended || l.disabled != row.origDisabled || l.key != row.origKey ||
           l.command != row.origCommand || l.comment != row.origComment;
}

// 丢弃该行本轮的全部改动（含禁用态），还原到打开时的快照
void MpvBindingDialog::restoreCustomRow(int row) {
    const Row &r = m_rows.at(row);
    Q_ASSERT(r.tier == Row::Tier::Custom);
    m_batchUpdating = true; // 还原触发的条目 / 记录器变更不得再走回编辑路径
    m_conf->setLineFields(r.line, r.origKey, r.origCommand, r.origComment);
    const bool nowDisabled = m_conf->lines().at(r.line).disabled;
    if (nowDisabled && !r.origDisabled)
        m_conf->enableLine(r.line);
    else if (!nowDisabled && r.origDisabled)
        m_conf->disableLine(r.line);
    if (auto *edit = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(row, 2)))
        edit->setKeySequence(MpvKey::sequenceFromName(r.origKey));
    m_table->item(row, 1)->setText(r.origCommand);
    m_table->item(row, 3)->setText(r.origComment);
    m_batchUpdating = false;
}

// 一次编辑落地后的统一收口：判据全本地现算，故刷视觉与 Apply 态是同一件事的两半
void MpvBindingDialog::afterEdit() {
    refreshRowVisuals();
    updateApplyButton();
}

QHash<QString, int> MpvBindingDialog::effectiveCustomRows() const {
    QHash<QString, int> eff;
    if (!m_conf)
        return eff;
    const QList<InputConfLine> &lines = m_conf->lines();
    for (int row = 0; row < m_rows.size(); ++row) {
        const Row &r = m_rows.at(row);
        if (r.tier != Row::Tier::Custom)
            continue;
        const InputConfLine &l = lines.at(r.line);
        if (l.disabled || l.key.isEmpty())
            continue; // 被注释的行不参与选取（§2）
        // 同键取文件序最后一条为生效行（mpv 槽位覆写语义，附录 C 第 3 条）
        eff.insert(MpvKey::normalizedKeyName(l.key), row);
    }
    return eff;
}

int MpvBindingDialog::builtinState(const Row &row, const QHash<QString, int> &eff) const {
    const int er = eff.value(MpvKey::normalizedKeyName(row.key), -1);
    if (er < 0)
        return StateNormal;
    const InputConfLine &l = m_conf->lines().at(m_rows.at(er).line);
    return l.command.compare(QLatin1String("ignore"), Qt::CaseInsensitive) == 0 ? StateDisabled
                                                                                : StateCovered;
}

void MpvBindingDialog::refreshRowVisuals() {
    const bool wasBatch = m_batchUpdating;
    m_batchUpdating = true;
    Q_ASSERT(m_table->rowCount() == m_rows.size() + 1); // 末行恒为空白录入行
    const QHash<QString, int> eff = effectiveCustomRows();
    const QString needle = m_filter->text().toLower();
    const QFont plain = m_table->font();

    for (int row = 0; row < m_rows.size(); ++row) {
        const Row &r = m_rows.at(row);
        const QString key = rowKeyText(row);
        const QString command = rowCommand(row);
        const QString comment = rowComment(row);
        bool gray = false;
        bool strike = false;
        bool bold = false;
        bool merged = false;
        QString stateTip; // 行级状态话术，挂在来源列（整行唯一的非内容列，不会被裁）
        if (r.tier == Row::Tier::Builtin) {
            const int st = builtinState(r, eff);
            gray = st == StateDisabled;  // 该键的生效自定义行恰为 ignore
            strike = st == StateCovered; // 该键被自定义行顶掉
            bold = st != r.origState;
            if (gray)
                stateTip = tr("Disabled by your own ignore entry; this command "
                              "will not run after restart.");
            else if (strike)
                stateTip = tr("Your own binding for this key overrides the builtin one; "
                              "the builtin command will not run after restart.");
        } else if (r.tier == Row::Tier::Script) {
            // 脚本条目不参与任何判据（§3.1）
        } else {
            const InputConfLine &l = m_conf->lines().at(r.line);
            gray = l.disabled; // 被行首 "# " 注释掉：灰，不叠加删除线
            const QString norm = MpvKey::normalizedKeyName(l.key);
            const bool hasBuiltin = m_builtinKeys.contains(norm);
            if (!l.disabled) {
                // 无键行（Duplicate 出的待录行等）不参与"被顶掉"判定
                if (!l.key.isEmpty())
                    strike = eff.value(norm, -1) != row; // 同键有更靠后的未注释行
                // 归并判据四条（§3.5）：未注释、命令恰为 ignore（大小写不敏感）、
                // 同键在内置集存在、该行是该键的生效行——全本地算出
                merged = l.command.compare(QLatin1String("ignore"), Qt::CaseInsensitive) == 0 &&
                         hasBuiltin && eff.value(norm, -1) == row;
            }
            if (gray)
                stateTip = hasBuiltin
                               ? tr("Commented out; after restart this key falls back to its "
                                    "builtin binding.")
                               : tr("Commented out; after restart this key has no action.");
            else if (strike)
                stateTip = tr("A later line binds the same key; that later line is the one "
                              "that applies.");
            bold = customRowDirty(r, l);
        }
        // 归并只在视图层（§3.5）：ignore 生效行并进内置行作禁用标志，不单独成行；
        // commit 按原文件行序回写，被归并的行不丢失不移位。鼠标键位过滤同为视图层：
        // 对所有来源生效（含用户自定义绑定），行仍在模型里、写入按原样保留。
        // 过滤框只做行隐藏，键 / 命令 / 注释三列子串、大小写不敏感，来源列不参与
        // 匹配（§3.6）
        const bool mouseRow = kFilterMouseBindings && isMouseKey(key);
        bool hidden = merged || mouseRow;
        if (!hidden && !needle.isEmpty())
            hidden = !(key + QLatin1Char(' ') + command + QLatin1Char(' ') + comment)
                          .toLower()
                          .contains(needle);
        m_table->setRowHidden(row, hidden);
        if (hidden)
            continue;

        // 三行判据的视觉一次性组装：删除线与加粗同在一个 QFont 上
        QFont f = plain;
        f.setStrikeOut(strike);
        f.setBold(bold);
        for (int col = 0; col < 4; ++col) {
            if (auto *it = m_table->item(row, col)) {
                it->setFont(f);
                it->setData(kGrayRole, gray);
            }
        }
        if (auto *edit = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(row, 2))) {
            edit->setFont(f);
            QPalette pal = edit->palette();
            const QColor text = gray ? palette().color(QPalette::Disabled, QPalette::Text)
                                     : palette().color(QPalette::Text);
            pal.setColor(QPalette::Text, text);
            edit->setPalette(pal);
        }
        // 状态话术给来源列，条目 tooltip 给完整内容：列宽裁不下的长命令由此可读，
        // 自定义行的键列给 mpv 键名原文（反解不出而显示留空的行也由此可读）
        m_table->item(row, 0)->setToolTip(stateTip);
        m_table->item(row, 1)->setToolTip(command);
        m_table->item(row, 3)->setToolTip(comment);
        if (auto *edit = qobject_cast<QKeySequenceEdit *>(m_table->cellWidget(row, 2)))
            edit->setToolTip(key);
        else
            m_table->item(row, 2)->setToolTip(key);
    }
    // 末尾空白录入行恒不隐藏（过滤与归并都不隐藏它）
    m_table->setRowHidden(blankRow(), false);
    m_table->item(blankRow(), 1)->setToolTip(m_blankCommand);
    m_table->item(blankRow(), 3)->setToolTip(m_blankComment);
    m_batchUpdating = wasBatch;
}

bool MpvBindingDialog::isDirty() const {
    if (!editable())
        return false;
    if (m_deletedLines > 0)
        return true; // 有删除：逐行快照看不见，以计数为准
    const QHash<QString, int> eff = effectiveCustomRows();
    for (const Row &r : m_rows) {
        if (r.tier == Row::Tier::Builtin) {
            if (builtinState(r, eff) != r.origState)
                return true;
        } else if (r.tier == Row::Tier::Custom) {
            if (customRowDirty(r, m_conf->lines().at(r.line)))
                return true;
        }
    }
    return false; // 空白录入行不参与脏态（§3.4）
}

void MpvBindingDialog::updateApplyButton() {
    // Apply 的可用判据 = 可编辑 且 有脏态 且 未失败锁定（§4.1）；锁定位不随脏态解除
    const bool canWrite = editable() && !m_failedLock;
    m_applyButton->setEnabled(canWrite && isDirty());
    m_okButton->setEnabled(canWrite);
}

bool MpvBindingDialog::supports(int row, MenuOp op, const QHash<QString, int> &eff) const {
    const Row &r = m_rows.at(row);
    if (op == OpDuplicate)
        return true; // 对所有行类型可用（仅单行选中时启用，另行判）
    if (r.tier == Row::Tier::Script)
        return false; // 脚本行只读，其余三项皆无
    if (r.tier == Row::Tier::Builtin) {
        const int st = builtinState(r, eff);
        switch (op) {
        case OpDisable:
            return st == StateNormal; // 已被顶掉的内置条目，禁用没有意义
        case OpEnable:
            return st == StateDisabled;
        default:
            return false; // 内置删不掉
        }
    }
    const InputConfLine &l = m_conf->lines().at(r.line);
    switch (op) {
    case OpDisable:
        // 未录键的行（Duplicate 出的待录行）不给禁用：那只会写出 "#  <命令>" 这样的无键
        // 废行，§3.4 授权当场禁用的是"刚转正的行"，键已齐
        return !l.disabled && !l.key.isEmpty();
    case OpEnable:
        return l.disabled;
    default:
        return true; // 自定义行（生效 / 被顶掉 / 被注释）皆可删
    }
}

void MpvBindingDialog::showContextMenu(const QPoint &viewportPos) {
    if (!editable())
        return;
    const QModelIndex idx = m_table->indexAt(viewportPos);
    if (!idx.isValid() || idx.row() >= m_rows.size())
        return; // 空白行不进选中集，无菜单
    // 右键落在未选中行上：就地单选，批量语义按当前选中集走
    if (!m_table->selectionModel()->isRowSelected(idx.row()))
        m_table->selectRow(idx.row());
    QList<int> sel;
    for (const QModelIndex &i : m_table->selectionModel()->selectedRows())
        if (i.row() < m_rows.size())
            sel.append(i.row());
    // 生效行映射算一次就够：内置行的禁用 / 启用判据要查它，逐行再算就是 O(n²)
    const QHash<QString, int> eff = effectiveCustomRows();

    QMenu menu(this);
    menu.setToolTipsVisible(true);
    QAction *dupAct = menu.addAction(tr("Duplicate"));
    QAction *disableAct = menu.addAction(tr("Disable"));
    QAction *enableAct = menu.addAction(tr("Enable"));
    QAction *deleteAct = menu.addAction(tr("Delete"));
    // 菜单项当且仅当选中集里每一行都支持该操作时才启用；Duplicate 仅单行选中时启用
    const auto allSupported = [this, &sel, &eff](MenuOp op) {
        return !sel.isEmpty() && std::all_of(sel.cbegin(), sel.cend(), [this, op, &eff](int row) {
            return supports(row, op, eff);
        });
    };
    dupAct->setEnabled(sel.size() == 1);
    disableAct->setEnabled(allSupported(OpDisable));
    enableAct->setEnabled(allSupported(OpEnable));
    deleteAct->setEnabled(allSupported(OpDelete));

    // 禁用自定义条目的 tooltip 按"回落到内置 / 彻底无动作"分别给话术（§3.3）
    if (sel.size() == 1 && supports(sel.first(), OpDisable, eff) &&
        m_rows.at(sel.first()).tier == Row::Tier::Custom) {
        const InputConfLine &l = m_conf->lines().at(m_rows.at(sel.first()).line);
        disableAct->setToolTip(
            m_builtinKeys.contains(MpvKey::normalizedKeyName(l.key))
                ? tr("Comment out this line; the key falls back to the builtin binding "
                     "after restart.")
                : tr("Comment out this line; the key has no action after restart."));
    }

    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(viewportPos));
    if (!chosen)
        return;

    if (chosen == dupAct) {
        // 新行落在自定义档末尾、空白行之前；命令与注释照搬，键恒留空（不替用户猜）。
        // 键留空的新行未填键，Apply 时按"该行没有键"报错（与空白行同一条校验）
        const int newRow =
            appendCustomRow(QString(), rowCommand(sel.first()), rowComment(sel.first()));
        afterEdit();
        // 建完立即滚动到该行并选中，用户只须录一个键
        m_table->scrollToItem(m_table->item(newRow, 0), QAbstractItemView::EnsureVisible);
        m_table->selectRow(newRow);
        return;
    }

    m_batchUpdating = true;
    if (chosen == disableAct) {
        for (int row : sel) {
            const Row &r = m_rows.at(row);
            if (r.tier == Row::Tier::Builtin) {
                // 禁用内置 = 文件末尾新增一行 <键> ignore（随即归并进内置行，列表
                // 不出现第二条；界面不出现「覆盖」这个词）
                appendCustomRow(r.key, QStringLiteral("ignore"), QString());
            } else {
                m_conf->disableLine(r.line); // 行首加 "# "，写出与认领互逆（断言守）
            }
        }
    } else if (chosen == enableAct) {
        // 启用会删行（内置支删掉 ignore 行对象），行号随删随移：逆序处理
        QList<int> rows = sel;
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        for (int row : rows) {
            const Row &r = m_rows.at(row);
            if (r.tier == Row::Tier::Builtin) {
                // 「启用」作用在行对象上，不经过界面行号：按归一键名在模型里现算该键
                // 的生效行（归并判据的逆操作），断言守
                const QHash<QString, int> live = effectiveCustomRows();
                const int er = live.value(MpvKey::normalizedKeyName(r.key), -1);
                Q_ASSERT(er >= 0);
                if (er < 0)
                    continue;
                const int lineIdx = m_rows.at(er).line;
                Q_ASSERT(m_conf->lines().at(lineIdx).command.compare(QLatin1String("ignore"),
                                                                     Qt::CaseInsensitive) == 0);
                removeCustomRow(er);
            } else {
                m_conf->enableLine(r.line); // 删掉认领时实际剥掉的那一段
            }
        }
    } else if (chosen == deleteAct) {
        // 多选删除逆序生效（行号与行对象下标随删随移）；删被顶掉的行只影响文件
        // 整洁，同键上一条可能接棒成为新的生效行（选取规则本地复算）
        QList<int> rows = sel;
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        for (int row : rows)
            removeCustomRow(row);
    }
    m_batchUpdating = false;
    afterEdit();
}

bool MpvBindingDialog::apply() {
    if (!editable() || m_failedLock)
        return false;
    if (!isDirty())
        return true; // 无脏态：无改动可写，不弹任何框（Ok 直接收尾）

    // —— 第 0 步：不完整行校验（§3.4：只作用在用户改过的行与空白录入行上）——
    // 只填命令未填键（含键被清空的行）→ 报错；只填键未填命令 → 静默丢弃该行改动。
    // 载入时因反解失败而显示留空的行不在校验之列，Apply 按原文写回（§3.2）。
    // 报错按命令串点名该行：表格行号含内置 / 脚本档，对用户没有指向性
    const auto describe = [this](const QString &cmd) {
        return cmd.isEmpty() ? tr("(empty command)") : cmd;
    };
    QStringList keyless;
    for (int row = 0; row < m_rows.size(); ++row) {
        const Row &r = m_rows.at(row);
        if (r.tier != Row::Tier::Custom)
            continue;
        const InputConfLine &l = m_conf->lines().at(r.line);
        if (customRowDirty(r, l) && l.key.isEmpty())
            keyless << describe(l.command);
    }
    if (m_blankEditor && MpvKey::nameFromSequence(m_blankEditor->keySequence()).isEmpty() &&
        !m_blankCommand.isEmpty())
        keyless << tr("%1 (new entry)").arg(m_blankCommand);
    if (!keyless.isEmpty()) {
        QMessageBox::warning(this, tr("Missing Key"),
                             tr("These entries have a command but no key:\n%1")
                                 .arg(keyless.join(QLatin1Char('\n'))));
        return false;
    }
    // 只填键未填命令：静默丢弃该行改动（还原到打开时的内容；空白行保留已填内容）。
    // 未编辑过的行不进还原——它们本来就该逐字节回写，重组只会动到行尾空白
    for (int row = 0; row < m_rows.size(); ++row) {
        const Row &r = m_rows.at(row);
        if (r.tier != Row::Tier::Custom)
            continue;
        const InputConfLine &l = m_conf->lines().at(r.line);
        if (l.key.isEmpty() || !l.command.isEmpty() || !customRowDirty(r, l))
            continue;
        restoreCustomRow(row);
    }
    if (!isDirty())
        return true;

    // —— 第 1 步：键位冲突提示（只读、仅提示、不阻断；§4.1）——
    // 判据：本次将写盘的生效自定义行键集 ∩ 插件侧当前键位表（ActionManager 运行期
    // 真值，不是声明默认）。不读不写 [shortcuts] 节、不改 ActionManager
    QSet<QString> pluginKeys;
    QMultiHash<QString, QString> pluginNames; // 归一键名 → 动作名
    for (ActionManager::Id id : m_actions->orderedIds()) {
        const QKeySequence seq = m_actions->key(id); // 运行期真值
        if (seq.isEmpty())
            continue;
        const QString mpvName = MpvKey::nameFromSequence(seq);
        if (mpvName.isEmpty())
            continue;
        const QString norm = MpvKey::normalizedKeyName(mpvName);
        pluginKeys.insert(norm);
        pluginNames.insert(norm, m_actions->displayText(id));
    }
    QStringList conflictLines;
    const QHash<QString, int> eff = effectiveCustomRows();
    for (auto it = eff.cbegin(); it != eff.cend(); ++it) {
        if (!pluginKeys.contains(it.key()))
            continue;
        conflictLines << QStringLiteral("%1 (%2)").arg(
            it.key(), pluginNames.values(it.key()).join(QStringLiteral(", ")));
    }
    if (!conflictLines.isEmpty()) {
        // 只有一个确定按钮，点掉后照常写盘（交集为空不弹）
        QMessageBox::information(
            this, tr("Conflicting Keys"),
            tr("These keys are also claimed by the plugin, so these bindings will "
               "never trigger:\n%1\n\nThe file will be written anyway.")
                .arg(conflictLines.join(QLatin1Char('\n'))));
    }

    // —— 第 2 步：commit() 写文件（Apply 全程不向 mpv 下发任何命令）——
    if (!m_conf->commit()) {
        // 失败支（§4.1 / §7）：只弹一句通用的写出失败提示，不回滚快照（脏态与加粗
        // 标记原样保留），Apply 与 Ok 永久禁用——锁定位在本对话框生命周期内不解除
        m_failedLock = true;
        updateApplyButton();
        QMessageBox::warning(this, tr("mpv Key Bindings"),
                             tr("Writing the configuration file failed."));
        return false;
    }

    // —— 第 3 步：重启提示（成功才弹，恰一次；不做常驻状态条与待重启差分呈现）——
    QMessageBox::information(this, tr("mpv Key Bindings"),
                             tr("Changes were written to input.conf. They take effect "
                                "after restarting mpv."));
    rollSnapshotForward(); // 快照前滚：可继续编辑并再次 Apply（多次 Apply 多次弹）
    afterEdit();
    return true;
}

void MpvBindingDialog::rollSnapshotForward() {
    m_deletedLines = 0; // 已落盘，快照前滚到当前现状
    const QHash<QString, int> eff = effectiveCustomRows();
    for (Row &r : m_rows) {
        if (r.tier == Row::Tier::Custom) {
            const InputConfLine &l = m_conf->lines().at(r.line);
            r.origKey = l.key;
            r.origCommand = l.command;
            r.origComment = l.comment;
            r.origDisabled = l.disabled;
            r.appended = false;
        } else if (r.tier == Row::Tier::Builtin) {
            r.origState = builtinState(r, eff);
        }
    }
}

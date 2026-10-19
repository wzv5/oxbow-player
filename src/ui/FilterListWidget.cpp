#include "pch.h"

#include "Constants.h"
#include "FilterListWidget.h"
#include "ThemedIconEngine.h"

// ——滤镜串编译 / 解析纯函数实现——

namespace {
// %n% 引号的长度口径（唯一出处）：n 按 UTF-8 字节计——mpv 内部按 C 字符串取字
// 节，故编译侧写长度取 toUtf8().size()，扫描侧据此把字节数换算成 QString 的字
// 符跨度（钳制到串尾）。ASCII 值两侧无差异，非 ASCII 参数值才显出计数单位。
// 实测（2026-09-15 探针，值"字]幕,x" = 9 字节 / 5 字符）：只有 %9% 写入成功，
// %5%（字符长）与 %1%~%8% 全部失败，%10% 报 "Invalid length 10"——即长度既按
// 字节计、又必须精确等于值长（短于此则余下内容被当作后续条目解析，通常因未知
// 滤镜名而整体失败）；mpv 自己生成该形态时同样写字节数（读回即 %9%）
qsizetype quotedCharSpan(const QString &s, qsizetype from, qlonglong bytes) {
    const qsizetype n = s.size();
    qsizetype i = from;
    qlonglong used = 0;
    while (i < n && used < bytes) {
        const char16_t cu = s[i].unicode();
        if (QChar::isHighSurrogate(cu) && i + 1 < n && QChar::isLowSurrogate(s[i + 1].unicode())) {
            used += 4; // 代理对 = 单个码位 = UTF-8 四字节
            i += 2;
        } else {
            used += cu < 0x80 ? 1 : (cu < 0x800 ? 2 : 3);
            ++i;
        }
    }
    return i - from;
}

// 编译侧长度前缀（与 quotedCharSpan 同口径）：%<UTF-8 字节数>%
QString percentLengthPrefix(const QString &value) {
    return u'%' + QString::number(value.toUtf8().size()) + u'%';
}

// 顶层字符扫描：返回 ch 在"顶层"（不在 [ ] 内、不在 %n% 引号内）出现的下标，
// 从 from 起找；无则 -1。[ ] 深度计数只受括号字符影响；%<数字>% 仅在深度 0 处
// 被识别为引号（[ ] 内的 % 是字面量，[...] 引号已覆盖其内容，F15），引号段按
// 声明长度整段跳过（长度换算见 quotedCharSpan）。孤立的 ']' 按深度触底处理
// （不回退计数），保证扫描恒向前推进
qsizetype topLevelCharPos(const QString &s, qsizetype from, QChar ch) {
    const qsizetype n = s.size();
    int depth = 0;
    qsizetype i = from;
    while (i < n) {
        const QChar c = s[i];
        if (c == u'%' && depth == 0) {
            qsizetype d = i + 1;
            while (d < n && s[d].isDigit())
                ++d;
            if (d > i + 1 && d < n && s[d] == u'%') {
                bool ok = false;
                const qlonglong len = s.mid(i + 1, int(d - i - 1)).toLongLong(&ok);
                if (ok && len >= 0) {
                    i = d + 1 + quotedCharSpan(s, d + 1, len);
                    continue;
                }
            }
            ++i; // 非引号形态的裸 %：字面量
            continue;
        }
        if (c == u'[') {
            ++depth;
        } else if (c == u']') {
            if (depth > 0)
                --depth;
        } else if (depth == 0 && c == ch) {
            return i;
        }
        ++i;
    }
    return -1;
}

// 顶层冒号定位（编译侧插 ! 与粘贴侧剥 ! 共用，F8：! 必须写在 @label: 之后）：
// text 以 @ 开头时返回首个顶层 ':' 的下标；否则返回 -1（调用方随即把 ! 位放在段首）
qsizetype labelColonPos(const QString &text) {
    if (!text.startsWith(u'@'))
        return -1;
    return topLevelCharPos(text, 0, u':');
}

// ! 位剥离：定位规则与编译侧同源——@ 开头且含顶层冒号则在冒号之后，否则段首；
// 连续多个一并归一（与编译侧"只输出一个 !"配套，往返一致）。返回是否有剥离
bool stripBangMarks(QString *text) {
    bool stripped = false;
    const qsizetype colon = labelColonPos(*text);
    if (colon >= 0) {
        while (colon + 1 < text->size() && (*text)[colon + 1] == u'!') {
            text->remove(colon + 1, 1);
            stripped = true;
        }
    } else {
        while (text->startsWith(u'!')) {
            text->remove(0, 1);
            stripped = true;
        }
    }
    return stripped;
}

// 新建条目项（两列：勾选 + 等宽全文）
QTreeWidgetItem *makeItem(QTreeWidget *tree, bool enabled, const QString &text) {
    auto *it = new QTreeWidgetItem(tree);
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable |
                 Qt::ItemIsEditable);
    it->setCheckState(0, enabled ? Qt::Checked : Qt::Unchecked);
    it->setText(1, text);
    it->setFont(1, QFontDatabase::systemFont(QFontDatabase::FixedFont));
    return it;
}
} // namespace

QString FilterListWidget::quoteParamValue(const QString &value) {
    const bool needsQuoting = value.contains(u',') || value.contains(u'=') ||
                              value.contains(u':') || value.startsWith(u'[') ||
                              value != value.trimmed();
    if (!needsQuoting)
        return value;
    // 平衡检测：嵌套计数，中途触底（孤立 ]）或收尾非零（悬挂 [）即不配平
    int depth = 0;
    bool balanced = true;
    for (const QChar c : value) {
        if (c == u'[') {
            ++depth;
        } else if (c == u']') {
            if (depth == 0) {
                balanced = false;
                break;
            }
            --depth;
        }
    }
    if (depth != 0)
        balanced = false;
    if (balanced)
        return u'[' + value + u']';
    // 长度按 UTF-8 字节计（mpv 内部按 C 字符串解析 %n%；非 ASCII 计数待探针确认）
    return u'%' + QString::number(value.toUtf8().size()) + u'%' + value;
}

QString FilterListWidget::compileEntries(const QList<FilterEntry> &entries) {
    QStringList parts;
    for (const FilterEntry &entry : entries) {
        QString text = entry.text.trimmed(); // 空条目丢弃（F5：顶层语法错误）
        if (text.isEmpty())
            continue;
        // 手打 ! 归一进勾选位：与勾选位取或后只输出一个 !
        const bool textDisabled = stripBangMarks(&text);
        const qsizetype colon = labelColonPos(text);
        const QString bang = (!entry.enabled || textDisabled) ? QStringLiteral("!") : QString();
        if (colon >= 0)
            parts.append(text.left(colon + 1) + bang + text.mid(colon + 1));
        else
            parts.append(bang + text);
    }
    return parts.join(u',');
}

QList<FilterEntry> FilterListWidget::parseString(const QString &s) {
    QList<FilterEntry> out;
    // 顶层逗号切分复用顶层扫描（[ ] 深度与 %n% 引号的口径只此一份）；每段只剥
    // 启用位，空段保留为空条目（可见即可控，编译时才丢弃）
    auto emitSegment = [&](qsizetype start, qsizetype end) {
        FilterEntry entry;
        entry.text = s.mid(start, end - start);
        entry.enabled = !stripBangMarks(&entry.text);
        out.append(entry);
    };
    qsizetype start = 0;
    qsizetype comma = topLevelCharPos(s, start, u',');
    while (comma >= 0) {
        emitSegment(start, comma);
        start = comma + 1;
        comma = topLevelCharPos(s, start, u',');
    }
    emitSegment(start, s.size());
    return out;
}

bool FilterListWidget::hasTopLevelComma(const QString &text) {
    return topLevelCharPos(text, 0, u',') >= 0;
}

// N43：菜单显示文案，口径见头文件。标签前缀定位复用 labelColonPos（与编译侧插 !、
// 粘贴侧剥 ! 同一规则），故此处不再重复"@ 开头 + 顶层冒号"的扫描
QString FilterListWidget::entryDisplayText(const QString &text) {
    const qsizetype colon = labelColonPos(text);
    if (colon < 0)
        return text; // 无标签前缀：省略名称部分，只显示条目全文
    const QString name = text.mid(1, colon - 1);
    const QString params = text.mid(colon + 1);
    if (name.isEmpty() || params.isEmpty())
        return text; // 畸形条目（@:… / @ln:）：原样显示，不产生空段
    return name + QStringLiteral(": ") + params;
}

// ——控件——

FilterListWidget::FilterListWidget(const QString &title, const QList<FilterPreset> &presets,
                                   QWidget *parent)
    : QGroupBox(title, parent), m_presets(presets) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    // 条目树：列 0 勾选（启停位），列 1 条目全文（就地编辑，等宽字体）
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(false);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_tree->header()->resizeSection(0, 24);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *, int) {
        if (!m_blockChanged) {
            refreshDerived();
            emit changed();
        }
    });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            [this] { updateButtons(); }); // 仅启停按钮，不动条目数据，无需发 changed()
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this] { updateButtons(); }); // 当前项与选中项独立变化，两路都要刷新
    layout->addWidget(m_tree, 1);

    // 按钮行（单行）：预设插入 + 条目操作（图标按钮）+ 整串出入口
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(4);

    m_addButton = new QToolButton(this);
    m_addButton->setToolTip(tr("Add Filter"));
    m_addButton->setPopupMode(QToolButton::InstantPopup);
    QMenu *presetMenu = new QMenu(m_addButton);
    for (const FilterPreset &preset : m_presets) {
        presetMenu->addAction(
            QCoreApplication::translate("SettingsDialog", preset.label), this, [this, preset] {
                // 插入时首条自动挂 @name: 标签（翻译经 SettingsDialog 上下文，
                // 中文名称已实测可正确显示与持久化；同名标签 mpv 允许共存，
                // 实测无碍，故多条目预设也不必逐条挂——首条足以标识该预设）
                QStringList texts;
                for (int i = 0; i < preset.entries.size(); ++i) {
                    QString text = preset.entries[i];
                    if (i == 0 && preset.name && *preset.name)
                        text = u'@' + QCoreApplication::translate("SettingsDialog", preset.name) +
                               u':' + text;
                    texts.append(text);
                }
                appendEntries(texts);
            });
    }
    m_addButton->setMenu(presetMenu);
    buttons->addWidget(m_addButton);

    // 条目操作：图标按钮 + tooltip（四枚统一走图标包素材 + 运行期染色）
    m_removeButton = new QToolButton(this);
    m_removeButton->setToolTip(tr("Remove selected entries"));
    buttons->addWidget(m_removeButton);
    m_upButton = new QToolButton(this);
    m_upButton->setToolTip(tr("Move up"));
    buttons->addWidget(m_upButton);
    m_downButton = new QToolButton(this);
    m_downButton->setToolTip(tr("Move down"));
    buttons->addWidget(m_downButton);
    retintIcons(); // 四枚图标初值（与主题切换后的重染同一出口）
    buttons->addStretch(1);

    QPushButton *copyButton = new QPushButton(tr("Copy String"), this);
    QPushButton *pasteButton = new QPushButton(tr("Paste String"), this);
    buttons->addWidget(copyButton);
    buttons->addWidget(pasteButton);
    // QToolButton 默认垂直 Fixed，高度比行内 QPushButton 矮一截；改随行撑满，
    // 与右侧文本按钮视觉对齐
    for (QToolButton *tb : {m_addButton, m_removeButton, m_upButton, m_downButton})
        tb->setSizePolicy(tb->sizePolicy().horizontalPolicy(), QSizePolicy::MinimumExpanding);
    layout->addLayout(buttons);

    connect(m_removeButton, &QToolButton::clicked, this, [this] {
        m_blockChanged = true;
        for (QTreeWidgetItem *it : m_tree->selectedItems())
            delete it;
        m_blockChanged = false;
        refreshDerived();
        emit changed();
    });
    auto moveSelected = [this](int delta) {
        QTreeWidgetItem *it = m_tree->currentItem();
        if (!it)
            return;
        const int row = m_tree->indexOfTopLevelItem(it);
        const int target = row + delta;
        if (target < 0 || target >= m_tree->topLevelItemCount())
            return;
        m_blockChanged = true;
        m_tree->takeTopLevelItem(row);
        m_tree->insertTopLevelItem(target, it);
        it->setSelected(true);
        m_tree->setCurrentItem(it);
        m_blockChanged = false;
        refreshDerived();
        emit changed();
    };
    connect(m_upButton, &QToolButton::clicked, this, [moveSelected] { moveSelected(-1); });
    connect(m_downButton, &QToolButton::clicked, this, [moveSelected] { moveSelected(1); });
    connect(copyButton, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(compiledString()); });
    connect(pasteButton, &QPushButton::clicked, this, [this] {
        const QString text = QApplication::clipboard()->text();
        if (text.isEmpty())
            return; // 空剪贴板不动当前列表（清空走删除条目）
        setEntries(parseString(text));
        emit changed();
    });

    // 只读预览行：实时编译结果，便于核对与手工复制（所见即所下发）
    m_preview = new QLineEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setFont(mono);
    layout->addWidget(m_preview);

    updateButtons();
}

QList<FilterEntry> FilterListWidget::entries() const {
    QList<FilterEntry> out;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *it = m_tree->topLevelItem(i);
        FilterEntry entry;
        entry.enabled = it->checkState(0) == Qt::Checked;
        entry.text = it->text(1);
        out.append(entry);
    }
    return out;
}

void FilterListWidget::setEntries(const QList<FilterEntry> &list) {
    // 重建保持选中行（N41 §2.8）：按行下标记忆与恢复，本无选中则不强行选中
    const int keepRow = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    m_blockChanged = true;
    m_tree->clear();
    for (const FilterEntry &entry : list)
        makeItem(m_tree, entry.enabled, entry.text);
    m_blockChanged = false;
    if (keepRow >= 0 && keepRow < m_tree->topLevelItemCount()) {
        QTreeWidgetItem *it = m_tree->topLevelItem(keepRow);
        it->setSelected(true);
        m_tree->setCurrentItem(it);
    }
    refreshDerived();
}

QString FilterListWidget::compiledString() const { return compileEntries(entries()); }

void FilterListWidget::appendEntries(const QStringList &texts) {
    m_blockChanged = true;
    for (const QString &text : texts)
        makeItem(m_tree, true, text);
    m_blockChanged = false;
    const int last = m_tree->topLevelItemCount() - 1;
    if (last >= 0) {
        QTreeWidgetItem *it = m_tree->topLevelItem(last);
        it->setSelected(true);
        m_tree->setCurrentItem(it);
        m_tree->scrollToItem(it);
    }
    refreshDerived();
    emit changed();
}

void FilterListWidget::refreshDerived() {
    // 顶层逗号标红（就地提示，不阻断）：顶层逗号只能是条目分隔符，值中的逗号
    // 必须用引号包裹；其余条目恢复默认前景。前景取本控件调色板的 Text——
    // 条目由树控件绘制，但调色板事件在控件树里的传播次序不定，本控件的派发
    // 早于子控件且届时自身已解算完毕（现状两者恒同值）
    m_blockChanged = true;
    const QBrush normalText = palette().text();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_tree->topLevelItem(i);
        const QBrush brush = hasTopLevelComma(it->text(1)) ? QBrush(Qt::red) : normalText;
        it->setForeground(1, brush);
    }
    m_blockChanged = false;
    m_preview->setText(compiledString());
    updateButtons();
}

void FilterListWidget::updateButtons() {
    const bool hasSelection = !m_tree->selectedItems().isEmpty();
    QTreeWidgetItem *current = m_tree->currentItem();
    // 当前项与选中项是两个独立状态（Ctrl+点击可清空选择而 current 残留）：
    // 启停统一以"当前项在选中集内"为准，否则无选中时上移/下移仍可用
    const bool movable = current && m_tree->selectedItems().contains(current);
    const int row = current ? m_tree->indexOfTopLevelItem(current) : -1;
    const int count = m_tree->topLevelItemCount();
    m_removeButton->setEnabled(hasSelection);
    m_upButton->setEnabled(movable && row > 0);
    m_downButton->setEnabled(movable && row >= 0 && row < count - 1);
}

// 四枚按钮图标的唯一赋值点——图标内容不随调色板自绘，初值与外观事件共用此出口（幂等）
void FilterListWidget::retintIcons() {
    const QColor tint = palette().color(QPalette::WindowText);
    m_addButton->setIcon(Ui::themedIcon(App::iconPath(App::iconFilterAdd()), tint));
    m_removeButton->setIcon(Ui::themedIcon(App::iconPath(App::iconFilterRemove()), tint));
    m_upButton->setIcon(Ui::themedIcon(App::iconPath(App::iconFilterUp()), tint));
    m_downButton->setIcon(Ui::themedIcon(App::iconPath(App::iconFilterDown()), tint));
}

// 本控件在设置对话框内常驻，主题切换发生时可能仍在屏上：当场重染四枚并重算条目前景。
// m_preview 是本控件重算路径上最后建成的成员，判它即兜住控件树建成前的样式事件
void FilterListWidget::changeEvent(QEvent *event) {
    QGroupBox::changeEvent(event);
    if (event->type() != QEvent::PaletteChange && event->type() != QEvent::StyleChange)
        return;
    if (!m_preview)
        return;
    retintIcons();
    refreshDerived();
}

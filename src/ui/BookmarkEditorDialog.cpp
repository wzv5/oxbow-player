#include "BookmarkEditorDialog.h"
#include "BookmarkManager.h"
#include "Util.h"
#include "pch.h"

BookmarkEditorDialog::BookmarkEditorDialog(BookmarkManager *mgr, JumpCallback jumpCb,
                                           ListLookup listLookup,
                                           const BookmarkManager::Group &currentGroup,
                                           QWidget *parent)
    : QDialog(parent), m_mgr(mgr), m_jumpCb(std::move(jumpCb)),
      m_listLookup(std::move(listLookup)) {
    setWindowTitle(tr("Bookmark Editor"));
    resize(640, 420);
    auto *layout = new QVBoxLayout(this);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_tree, 1);

    auto *btnRow = new QHBoxLayout;
    m_jumpBtn = new QPushButton(tr("Jump"), this);
    m_removeSelBtn = new QPushButton(tr("Remove Selected"), this);
    m_removeInvalidBtn = new QPushButton(tr("Remove Invalid"), this);
    m_clearBtn = new QPushButton(tr("Clear"), this);
    btnRow->addWidget(m_jumpBtn);
    btnRow->addWidget(m_removeSelBtn);
    btnRow->addWidget(m_removeInvalidBtn);
    btnRow->addWidget(m_clearBtn);
    btnRow->addStretch(1);
    layout->addLayout(btnRow);

    // N16：编辑行（置于四按钮行之下）
    auto *editRow = new QHBoxLayout;
    m_timeLabel = new QLabel(tr("Time"), this);
    m_timeEdit = new QLineEdit(this);
    m_titleLabel = new QLabel(tr("Title"), this);
    m_titleEdit = new QLineEdit(this);
    m_applyBtn = new QPushButton(tr("Apply"), this);
    editRow->addWidget(m_timeLabel);
    editRow->addWidget(m_timeEdit, 1);
    editRow->addWidget(m_titleLabel);
    editRow->addWidget(m_titleEdit, 2);
    editRow->addWidget(m_applyBtn);
    layout->addLayout(editRow);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &BookmarkEditorDialog::onSelectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            &BookmarkEditorDialog::onItemDoubleClicked);
    connect(m_jumpBtn, &QPushButton::clicked, this, &BookmarkEditorDialog::onJump);
    connect(m_removeSelBtn, &QPushButton::clicked, this, &BookmarkEditorDialog::onRemoveSelected);
    connect(m_removeInvalidBtn, &QPushButton::clicked, this,
            &BookmarkEditorDialog::onRemoveInvalid);
    connect(m_clearBtn, &QPushButton::clicked, this, &BookmarkEditorDialog::onClearAll);
    connect(m_applyBtn, &QPushButton::clicked, this, &BookmarkEditorDialog::onApply);

    // N16：打开即聚焦当前播放文件所在的组（若有书签）
    if (!currentGroup.key.isEmpty())
        m_focusGroup = currentGroup;
    refresh();
    onSelectionChanged();
}

// N77 §2.3：从条目数据还原组标识——UserRole 存 key 之外加 uuid 字面串分量
// （UserRole+1，字面形态与落盘一致，QUuid::fromString 还原）
BookmarkManager::Group BookmarkEditorDialog::groupOfItem(QTreeWidgetItem *item) const {
    return {QUuid::fromString(item->data(0, Qt::UserRole + 1).toString()),
            item->data(0, Qt::UserRole).toString()};
}

// N54：根节点呈现 = "[列表名] 文件显示路径"。列表名按组身份运行期现查（不落盘，
// 改名即刻生效）；查不到前缀显示 Unknown——两类成因：旧 INI / 损坏数据带来的无身份
// 书签，或来源列表已被删除（N57 §2.5 后默认 tab 出生的书签不再落"无身份"，它记
// 默认列表的保留身份）——维护者裁决保留占位：编辑器里"没有归属"要显式可见，才能与
// "能回原列表"的项一眼分开（N54 变更记录 6）。显示路径仍取 displayPath（B15：网络
// 书签不额外 toNativeSeparators）
QString BookmarkEditorDialog::fileItemText(const BookmarkManager::Group &group) const {
    const BookmarkManager::Origin origin = m_mgr->bookmarkOrigin(group);
    QString listName;
    if (origin.isValid() && m_listLookup)
        listName = m_listLookup(origin.listUuid);
    if (listName.isEmpty())
        listName = tr("Unknown");
    return QStringLiteral("[%1] %2").arg(listName, m_mgr->displayPath(group));
}

void BookmarkEditorDialog::refresh() {
    m_tree->clear();
    // N77 §2.3：逐组陈列，同文件多组各占一行（合并豁免只作用读侧显示，编辑器不合并）
    const QList<BookmarkManager::Group> groups = m_mgr->allFiles();
    for (const BookmarkManager::Group &group : groups) {
        auto *root = new QTreeWidgetItem(m_tree);
        root->setText(0, fileItemText(group));
        root->setData(0, Qt::UserRole, group.key);
        root->setData(0, Qt::UserRole + 1,
                      group.listUuid.toString(QUuid::StringFormat::WithoutBraces));
        // N16：仅展开焦点组（m_focusGroup 匹配），其余一律收起
        root->setExpanded(group == m_focusGroup);
        const auto list = m_mgr->bookmarks(group);
        for (int i = 0; i < list.size(); ++i) {
            const auto &bm = list[i];
            auto *leaf = new QTreeWidgetItem(root);
            leaf->setText(0,
                          QStringLiteral("[%1] %2").arg(Util::formatTimeHMSms(bm.time), bm.title));
            leaf->setData(0, Qt::UserRole, group.key);
            leaf->setData(0, Qt::UserRole + 1,
                          group.listUuid.toString(QUuid::StringFormat::WithoutBraces));
            leaf->setData(0, Qt::UserRole + 2, i);
            leaf->setData(0, Qt::UserRole + 3, bm.time);
        }
    }
    // N16：重建后聚焦指定组父节点（打开聚焦当前播放组 / 编辑后聚焦上一次编辑组）
    if (!m_focusGroup.key.isEmpty()) {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *root = m_tree->topLevelItem(i);
            if (groupOfItem(root) == m_focusGroup) {
                root->setExpanded(true);
                m_tree->setCurrentItem(root);
                break;
            }
        }
        m_focusGroup = {};
    }
    onSelectionChanged();
}

void BookmarkEditorDialog::onSelectionChanged() {
    auto *cur = m_tree->currentItem();
    const bool hasSel = cur != nullptr;
    bool isLeaf = cur && cur->parent() != nullptr;
    m_jumpBtn->setEnabled(isLeaf);
    m_removeSelBtn->setEnabled(hasSel);
    // N16：编辑行仅在选中叶项时启用并回填；其余（文件根/无选中）禁用清空
    if (isLeaf) {
        m_selGroup = groupOfItem(cur);
        m_selIndex = cur->data(0, Qt::UserRole + 2).toInt();
        const double t = cur->data(0, Qt::UserRole + 3).toDouble();
        m_timeEdit->setText(Util::formatTimeHMSms(t));
        // 标题从 BookmarkManager 取原始值回填（leaf 文本为 "[time] title" 合成，不直接拆）
        const auto list = m_mgr->bookmarks(m_selGroup);
        if (m_selIndex >= 0 && m_selIndex < list.size())
            m_titleEdit->setText(list[m_selIndex].title);
        m_timeEdit->setEnabled(true);
        m_titleEdit->setEnabled(true);
        m_applyBtn->setEnabled(true);
    } else {
        m_selGroup = {};
        m_selIndex = -1;
        m_timeEdit->clear();
        m_titleEdit->clear();
        m_timeEdit->setEnabled(false);
        m_titleEdit->setEnabled(false);
        m_applyBtn->setEnabled(false);
    }
}

void BookmarkEditorDialog::onApply() {
    const QString timeText = m_timeEdit->text();
    double t = 0.0;
    if (!Util::parseTimeHMSms(timeText, &t)) {
        QMessageBox::warning(
            this, tr("Invalid Time Format"),
            tr("Time must be hh:mm:ss.zzz (e.g. 00:01:02.345); minutes and seconds must be "
               "less than 60, milliseconds exactly three digits."));
        return;
    }
    const QString title = m_titleEdit->text();
    if (title.isEmpty()) {
        QMessageBox::warning(this, tr("Empty Title"), tr("Bookmark title cannot be empty."));
        return;
    }
    if (title.contains(QLatin1Char('&'))) {
        QMessageBox::warning(this, tr("Title Contains Invalid Characters"),
                             tr("Title must not contain \"&\" (conflicts with chapter menu "
                                "mnemonics)."));
        return;
    }
    // 改写后整表重建并聚焦本组父节点（不还原叶选中）
    m_focusGroup = m_selGroup;
    m_mgr->updateBookmark(m_selGroup, m_selIndex, t, title);
}

void BookmarkEditorDialog::onJump() {
    auto *cur = m_tree->currentItem();
    if (!cur || !cur->parent())
        return;
    // N77 §2.3：按组定位——跳转需原始显示路径与该组来源（同文件多组各回各的承诺）
    const BookmarkManager::Group group = groupOfItem(cur);
    const QString path = m_mgr->displayPath(group);
    const double t = cur->data(0, Qt::UserRole + 3).toDouble();
    if (m_jumpCb)
        m_jumpCb(path, t, m_mgr->bookmarkOrigin(group)); // N54：来源随跳转一起交给主窗口
}

void BookmarkEditorDialog::onItemDoubleClicked(QTreeWidgetItem *item, int column) {
    Q_UNUSED(column);
    if (!item || !item->parent())
        return;
    const BookmarkManager::Group group = groupOfItem(item);
    const QString path = m_mgr->displayPath(group);
    const double t = item->data(0, Qt::UserRole + 3).toDouble();
    if (m_jumpCb)
        m_jumpCb(path, t, m_mgr->bookmarkOrigin(group));
}

void BookmarkEditorDialog::onRemoveSelected() {
    auto *cur = m_tree->currentItem();
    if (!cur)
        return;
    // 数据必须先抄录、后调用：Manager 变更经 changed 信号（同步直连）触发
    // 主窗口对本对话框的 refresh() 级联，clear() 会销毁全部条目使 cur
    // 悬空——mgr 调用之后不得再解引用 cur
    const bool isFileRoot = (cur->parent() == nullptr);
    const BookmarkManager::Group group = groupOfItem(cur); // 叶与根均存组
    const int idx = isFileRoot ? -1 : cur->data(0, Qt::UserRole + 2).toInt();
    if (isFileRoot)
        m_mgr->removeFile(group);
    else
        m_mgr->removeBookmark(group, idx);
    // 刷新统一走 changed → 主窗口级联，此处不直接调用
}

void BookmarkEditorDialog::onRemoveInvalid() {
    // 有删除必发 changed（无删除早退不发），刷新经主窗口级联。
    // N54：判据两半——本地项看磁盘、远端项看归属，后者要问"列表还在不在"，本对话框
    // 不认识播放列表，故问注入的那一次按身份直查。
    // N57 §2.6：存活判据由名字查询就地派生——"查得到名字"与"列表还在"是同一次直查的
    // 两面（前提"空名列表不存在"由注册表一侧保证），不再要调用方传第二份同源判据。
    // N77：谓参逐组以其自身 uuid 调用（组标识的 uuid 分量与来源身份恒等），本侧无感
    m_mgr->removeInvalid([this](const QUuid &listUuid) {
        return m_listLookup && !m_listLookup(listUuid).isEmpty();
    });
}

void BookmarkEditorDialog::onClearAll() {
    if (QMessageBox::question(this, tr("Confirm"), tr("Clear all bookmarks?")) != QMessageBox::Yes)
        return;
    // 非空必发 changed，刷新经主窗口级联
    m_mgr->clearAll();
}

#include "BookmarkEditorDialog.h"
#include "BookmarkManager.h"
#include "Util.h"
#include "pch.h"

BookmarkEditorDialog::BookmarkEditorDialog(BookmarkManager *mgr, JumpCallback jumpCb,
                                           ListNameLookup nameLookup, ListAliveLookup listAlive,
                                           const QString &currentPath, QWidget *parent)
    : QDialog(parent), m_mgr(mgr), m_jumpCb(std::move(jumpCb)), m_nameLookup(std::move(nameLookup)),
      m_listAlive(std::move(listAlive)) {
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

    // N16：打开即聚焦当前播放文件（若有书签）
    if (!currentPath.isEmpty())
        m_focusFile = currentPath;
    refresh();
    onSelectionChanged();
}

// N54：根节点呈现 = "[列表名] 文件显示路径"。列表名按来源 uuid 运行期现查（不落盘，
// 改名即刻生效）；查不到（无身份书签、来源列表已删、或查名回调未接）前缀显示 Unknown
// ——维护者裁决保留占位：编辑器里"没有归属"要显式可见，才能与"能回原列表"的项一眼
// 分开（N54 变更记录 6）。显示路径仍取 displayPath（归一化小写键经它恢复原样；
// B15：网络书签不额外 toNativeSeparators）
QString BookmarkEditorDialog::fileItemText(const QString &key) const {
    const BookmarkManager::Origin origin = m_mgr->bookmarkOrigin(key);
    QString listName;
    if (origin.isValid() && m_nameLookup)
        listName = m_nameLookup(origin.listUuid);
    if (listName.isEmpty())
        listName = tr("Unknown");
    return QStringLiteral("[%1] %2").arg(listName, m_mgr->displayPath(key));
}

void BookmarkEditorDialog::refresh() {
    m_tree->clear();
    const QStringList files = m_mgr->allFiles();
    for (const QString &path : files) {
        auto *root = new QTreeWidgetItem(m_tree);
        root->setText(0, fileItemText(path));
        root->setData(0, Qt::UserRole, path);
        // N16：仅展开焦点文件（m_focusFile 匹配），其余一律收起
        root->setExpanded(path == m_focusFile);
        const auto list = m_mgr->bookmarks(path);
        for (int i = 0; i < list.size(); ++i) {
            const auto &bm = list[i];
            auto *leaf = new QTreeWidgetItem(root);
            leaf->setText(0,
                          QStringLiteral("[%1] %2").arg(Util::formatTimeHMSms(bm.time), bm.title));
            leaf->setData(0, Qt::UserRole, path);
            leaf->setData(0, Qt::UserRole + 2, i);
            leaf->setData(0, Qt::UserRole + 3, bm.time);
        }
    }
    // N16：重建后聚焦指定文件父节点（打开聚焦当前播放文件 / 编辑后聚焦上一次编辑文件）
    if (!m_focusFile.isEmpty()) {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *root = m_tree->topLevelItem(i);
            if (root->data(0, Qt::UserRole).toString() == m_focusFile) {
                root->setExpanded(true);
                m_tree->setCurrentItem(root);
                break;
            }
        }
        m_focusFile.clear();
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
        m_selPath = cur->data(0, Qt::UserRole).toString();
        m_selIndex = cur->data(0, Qt::UserRole + 2).toInt();
        const double t = cur->data(0, Qt::UserRole + 3).toDouble();
        m_timeEdit->setText(Util::formatTimeHMSms(t));
        // 标题从 BookmarkManager 取原始值回填（leaf 文本为 "[time] title" 合成，不直接拆）
        const auto list = m_mgr->bookmarks(m_selPath);
        if (m_selIndex >= 0 && m_selIndex < list.size())
            m_titleEdit->setText(list[m_selIndex].title);
        m_timeEdit->setEnabled(true);
        m_titleEdit->setEnabled(true);
        m_applyBtn->setEnabled(true);
    } else {
        m_selPath.clear();
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
    // 改写后整表重建并聚焦本文件父节点（不还原叶选中）
    m_focusFile = m_selPath;
    m_mgr->updateBookmark(m_selPath, m_selIndex, t, title);
}

void BookmarkEditorDialog::onJump() {
    auto *cur = m_tree->currentItem();
    if (!cur || !cur->parent())
        return;
    // UserRole 存归一化键；跳转需原始显示路径（大小写/分隔符原样，B15）
    const QString key = cur->data(0, Qt::UserRole).toString();
    const QString path = m_mgr->displayPath(key);
    const double t = cur->data(0, Qt::UserRole + 3).toDouble();
    if (m_jumpCb)
        m_jumpCb(path, t, m_mgr->bookmarkOrigin(key)); // N54：来源随跳转一起交给主窗口
}

void BookmarkEditorDialog::onItemDoubleClicked(QTreeWidgetItem *item, int column) {
    Q_UNUSED(column);
    if (!item || !item->parent())
        return;
    const QString key = item->data(0, Qt::UserRole).toString();
    const QString path = m_mgr->displayPath(key);
    const double t = item->data(0, Qt::UserRole + 3).toDouble();
    if (m_jumpCb)
        m_jumpCb(path, t, m_mgr->bookmarkOrigin(key));
}

void BookmarkEditorDialog::onRemoveSelected() {
    auto *cur = m_tree->currentItem();
    if (!cur)
        return;
    // 数据必须先抄录、后调用：Manager 变更经 changed 信号（同步直连）触发
    // 主窗口对本对话框的 refresh() 级联，clear() 会销毁全部条目使 cur
    // 悬空——mgr 调用之后不得再解引用 cur
    const bool isFileRoot = (cur->parent() == nullptr);
    const QString path = cur->data(0, Qt::UserRole).toString(); // 叶与根均存路径
    const int idx = isFileRoot ? -1 : cur->data(0, Qt::UserRole + 2).toInt();
    if (isFileRoot)
        m_mgr->removeFile(path);
    else
        m_mgr->removeBookmark(path, idx);
    // 刷新统一走 changed → 主窗口级联，此处不直接调用
}

void BookmarkEditorDialog::onRemoveInvalid() {
    // 有删除必发 changed（无删除早退不发），刷新经主窗口级联。
    // N54：判据两半——本地项看磁盘、远端项看归属，后者要问"列表还在不在"，本对话框
    // 不认识播放列表，故经构造期注入的 m_listAlive 现问主窗口
    m_mgr->removeInvalid(m_listAlive);
}

void BookmarkEditorDialog::onClearAll() {
    if (QMessageBox::question(this, tr("Confirm"), tr("Clear all bookmarks?")) != QMessageBox::Yes)
        return;
    // 非空必发 changed，刷新经主窗口级联
    m_mgr->clearAll();
}

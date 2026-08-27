#include "pch.h"
#include "BookmarkEditorDialog.h"
#include "BookmarkManager.h"
#include "Util.h"

BookmarkEditorDialog::BookmarkEditorDialog(BookmarkManager *mgr, JumpCallback jumpCb, QWidget *parent)
    : QDialog(parent), m_mgr(mgr), m_jumpCb(std::move(jumpCb))
{
    setWindowTitle(tr("书签编辑器"));
    resize(640, 420);
    auto *layout = new QVBoxLayout(this);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_tree, 1);

    auto *btnRow = new QHBoxLayout;
    m_jumpBtn = new QPushButton(tr("跳转"), this);
    m_removeSelBtn = new QPushButton(tr("移除选中"), this);
    m_removeInvalidBtn = new QPushButton(tr("移除失效项"), this);
    m_clearBtn = new QPushButton(tr("清空"), this);
    btnRow->addWidget(m_jumpBtn);
    btnRow->addWidget(m_removeSelBtn);
    btnRow->addWidget(m_removeInvalidBtn);
    btnRow->addWidget(m_clearBtn);
    btnRow->addStretch(1);
    layout->addLayout(btnRow);

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &BookmarkEditorDialog::onSelectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &BookmarkEditorDialog::onItemDoubleClicked);
    connect(m_jumpBtn, &QPushButton::clicked, this, &BookmarkEditorDialog::onJump);
    connect(m_removeSelBtn, &QPushButton::clicked, this, &BookmarkEditorDialog::onRemoveSelected);
    connect(m_removeInvalidBtn, &QPushButton::clicked, this, &BookmarkEditorDialog::onRemoveInvalid);
    connect(m_clearBtn, &QPushButton::clicked, this, &BookmarkEditorDialog::onClearAll);

    refresh();
    onSelectionChanged();
}

void BookmarkEditorDialog::refresh()
{
    m_tree->clear();
    const QStringList files = m_mgr->allFiles();
    for (const QString &path : files) {
        auto *root = new QTreeWidgetItem(m_tree);
        // N15 实测修正：展示用原始路径（键为归一化小写，经 displayPath 恢复原样）
        root->setText(0, QDir::toNativeSeparators(m_mgr->displayPath(path)));
        root->setData(0, Qt::UserRole, path);
        root->setExpanded(true);
        const auto list = m_mgr->bookmarks(path);
        for (int i = 0; i < list.size(); ++i) {
            const auto &bm = list[i];
            auto *leaf = new QTreeWidgetItem(root);
            leaf->setText(0, QStringLiteral("[%1] %2").arg(Util::formatTimeHMSms(bm.time), bm.title));
            leaf->setData(0, Qt::UserRole, path);
            leaf->setData(0, Qt::UserRole + 2, i);
            leaf->setData(0, Qt::UserRole + 3, bm.time);
        }
    }
    onSelectionChanged();
}

void BookmarkEditorDialog::onSelectionChanged()
{
    auto *cur = m_tree->currentItem();
    const bool hasSel = cur != nullptr;
    bool isLeaf = cur && cur->parent() != nullptr;
    m_jumpBtn->setEnabled(isLeaf);
    m_removeSelBtn->setEnabled(hasSel);
}

void BookmarkEditorDialog::onJump()
{
    auto *cur = m_tree->currentItem();
    if (!cur || !cur->parent())
        return;
    const QString path = cur->data(0, Qt::UserRole).toString();
    const double t = cur->data(0, Qt::UserRole + 3).toDouble();
    if (m_jumpCb)
        m_jumpCb(path, t);
}

void BookmarkEditorDialog::onItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item || !item->parent())
        return;
    const QString path = item->data(0, Qt::UserRole).toString();
    const double t = item->data(0, Qt::UserRole + 3).toDouble();
    if (m_jumpCb)
        m_jumpCb(path, t);
}

void BookmarkEditorDialog::onRemoveSelected()
{
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

void BookmarkEditorDialog::onRemoveInvalid()
{
    // 有删除必发 changed（无删除早退不发），刷新经主窗口级联
    m_mgr->removeInvalid();
}

void BookmarkEditorDialog::onClearAll()
{
    if (QMessageBox::question(this, tr("确认"), tr("确定清空所有书签？")) != QMessageBox::Yes)
        return;
    // 非空必发 changed，刷新经主窗口级联
    m_mgr->clearAll();
}

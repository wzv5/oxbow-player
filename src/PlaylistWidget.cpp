#include "PlaylistWidget.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>

PlaylistWidget::PlaylistWidget(QWidget *parent)
    : QTabWidget(parent)
{
    setAcceptDrops(true);
    setDocumentMode(true);
    setMinimumWidth(220);
    addTab(createList(), tr("默认播放列表"));
}

QListWidget *PlaylistWidget::createList()
{
    auto *list = new QListWidget;
    // 子列表不接受拖放，事件会上溯到本 QTabWidget 统一处理
    list->setAcceptDrops(false);
    connect(list, &QListWidget::itemDoubleClicked, this, &PlaylistWidget::onItemDoubleClicked);
    return list;
}

QListWidget *PlaylistWidget::listAt(int index) const
{
    return qobject_cast<QListWidget *>(widget(index));
}

QStringList PlaylistWidget::filesOf(int tabIndex) const
{
    QStringList files;
    if (QListWidget *list = listAt(tabIndex)) {
        for (int i = 0; i < list->count(); ++i)
            files.append(list->item(i)->data(Qt::UserRole).toString());
    }
    return files;
}

void PlaylistWidget::setPlaylist(const QVariantList &items)
{
    QListWidget *list = listAt(m_playingTab);
    if (!list)
        return;

    list->clear();
    for (const QVariant &v : items) {
        const QVariantMap map = v.toMap();
        const QString filename = map.value(QStringLiteral("filename")).toString();
        QString title = map.value(QStringLiteral("title")).toString();
        if (title.isEmpty())
            title = QFileInfo(filename).fileName();

        auto *item = new QListWidgetItem(title, list);
        item->setData(Qt::UserRole, filename);
        item->setToolTip(filename);
        if (map.value(QStringLiteral("current")).toBool()) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
            list->setCurrentItem(item);
        }
    }
}

void PlaylistWidget::addFiles(const QStringList &files)
{
    QListWidget *list = listAt(currentIndex());
    if (!list || files.isEmpty())
        return;

    for (const QString &file : files) {
        auto *item = new QListWidgetItem(QFileInfo(file).fileName(), list);
        item->setData(Qt::UserRole, file);
        item->setToolTip(file);
    }
    emit filesAdded(files);
}

void PlaylistWidget::onItemDoubleClicked(QListWidgetItem *item)
{
    QListWidget *list = item->listWidget();
    const int tab = indexOf(list);
    if (!list || tab < 0)
        return;

    setPlayingTab(tab);
    emit playRequested(filesOf(tab), list->row(item));
}

void PlaylistWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void PlaylistWidget::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void PlaylistWidget::dropEvent(QDropEvent *event)
{
    QStringList files;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            files.append(url.toLocalFile());
    }
    if (!files.isEmpty()) {
        addFiles(files);
        event->acceptProposedAction();
    }
}

void PlaylistWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QListWidget *list = listAt(currentIndex());

    QMenu menu(this);
    QAction *addAction = menu.addAction(tr("添加文件…"));
    QAction *newTabAction = menu.addAction(tr("新建播放列表"));
    menu.addSeparator();
    QAction *removeAction = menu.addAction(tr("移除选中项"));
    removeAction->setEnabled(list && !list->selectedItems().isEmpty());
    QAction *clearAction = menu.addAction(tr("清空当前列表"));
    clearAction->setEnabled(list && list->count() > 0);
    menu.addSeparator();
    QAction *renameAction = menu.addAction(tr("重命名当前列表"));
    QAction *closeTabAction = menu.addAction(tr("删除当前列表"));
    closeTabAction->setEnabled(count() > 1);

    QAction *chosen = menu.exec(event->globalPos());
    if (!chosen)
        return;

    if (chosen == addAction) {
        const QStringList files = QFileDialog::getOpenFileNames(
            this, tr("添加文件"), QString(),
            tr("媒体文件 (*.mp4 *.mkv *.avi *.mov *.wmv *.flv *.webm *.ts *.mp3 *.flac *.aac *.ogg *.wav *.m4a);;所有文件 (*)"));
        addFiles(files);
    } else if (chosen == newTabAction) {
        addTab(createList(), tr("播放列表 %1").arg(count() + 1));
        setCurrentIndex(count() - 1);
    } else if (chosen == removeAction && list) {
        if (currentIsPlayingTab()) {
            // 与 mpv 播放列表同步的 tab：发命令给 mpv，由属性同步回刷界面
            QList<int> rows;
            for (const QListWidgetItem *item : list->selectedItems())
                rows.append(list->row(item));
            emit removeRequested(rows);
        } else {
            qDeleteAll(list->selectedItems());
        }
    } else if (chosen == clearAction && list) {
        if (currentIsPlayingTab())
            emit clearRequested();
        else
            list->clear();
    } else if (chosen == renameAction) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("重命名播放列表"), tr("名称："),
                                                   QLineEdit::Normal, tabText(currentIndex()), &ok);
        if (ok && !name.isEmpty())
            setTabText(currentIndex(), name);
    } else if (chosen == closeTabAction) {
        const int idx = currentIndex();
        if (m_playingTab == idx)
            m_playingTab = 0;
        else if (m_playingTab > idx)
            --m_playingTab;
        removeTab(idx);
    }
}

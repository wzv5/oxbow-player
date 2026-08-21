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
#include <QSettings>

PlaylistWidget::PlaylistWidget(const QString &configFile, QWidget *parent)
    : QTabWidget(parent), m_configFile(configFile)
{
    setAcceptDrops(true);
    setDocumentMode(true);
    setMinimumWidth(220);
    addTab(createList(), tr("默认"));
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

    // 非默认、非正在播放的 tab 是本地列表，添加后持久化
    if (currentIndex() != 0 && !currentIsPlayingTab())
        savePlaylists();
}

void PlaylistWidget::loadPlaylists()
{
    if (m_configFile.isEmpty())
        return;

    QSettings settings(m_configFile, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Playlists"));
    const int savedCount = settings.value(QStringLiteral("count"), 0).toInt();
    settings.endGroup();

    for (int i = 0; i < savedCount; ++i) {
        const QString section = QStringLiteral("Playlist_%1").arg(i);
        settings.beginGroup(section);
        const QString name = settings.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || name == tr("默认")) {
            settings.endGroup();
            continue;
        }

        auto *list = createList();
        const QStringList keys = settings.childKeys();
        for (const QString &key : keys) {
            if (key == QLatin1String("name"))
                continue;
            const QString file = settings.value(key).toString();
            if (!file.isEmpty()) {
                auto *item = new QListWidgetItem(QFileInfo(file).fileName(), list);
                item->setData(Qt::UserRole, file);
                item->setToolTip(file);
            }
        }
        settings.endGroup();
        addTab(list, name);
    }
}

void PlaylistWidget::savePlaylists()
{
    if (m_configFile.isEmpty())
        return;

    QSettings settings(m_configFile, QSettings::IniFormat);
    // 清理旧的播放列表节
    for (const QString &g : settings.childGroups()) {
        if (g.startsWith(QStringLiteral("Playlist_")) || g == QLatin1String("Playlists"))
            settings.remove(g);
    }

    int savedIdx = 0;
    for (int i = 0; i < count(); ++i) {
        // 不保存"默认"tab（索引 0）和"正在播放"tab（其内容由 mpv 驱动）
        if (i == 0 || i == m_playingTab)
            continue;

        const QStringList files = filesOf(i);
        const QString section = QStringLiteral("Playlist_%1").arg(savedIdx);
        settings.beginGroup(section);
        settings.setValue(QStringLiteral("name"), tabText(i));
        for (int j = 0; j < files.size(); ++j)
            settings.setValue(QString::number(j), files[j]);
        settings.endGroup();
        ++savedIdx;
    }

    settings.beginGroup(QStringLiteral("Playlists"));
    settings.setValue(QStringLiteral("count"), savedIdx);
    settings.endGroup();
}

void PlaylistWidget::selectItem(int tabIndex, int row)
{
    if (tabIndex < 0 || tabIndex >= count())
        return;
    setCurrentIndex(tabIndex);
    QListWidget *list = listAt(tabIndex);
    if (list && row >= 0 && row < list->count()) {
        list->setCurrentRow(row);
        list->scrollToItem(list->currentItem());
    }
}

int PlaylistWidget::currentSelectedRow() const
{
    const QListWidget *list = listAt(currentIndex());
    if (!list || list->selectedItems().isEmpty())
        return -1;
    return list->currentRow();
}

void PlaylistWidget::onItemDoubleClicked(QListWidgetItem *item)
{
    QListWidget *list = item->listWidget();
    const int tab = indexOf(list);
    if (!list || tab < 0)
        return;

    markPlayingTab(tab);
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
    const int tab = currentIndex();
    const bool isPlayingTab = (tab == m_playingTab);

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
    renameAction->setEnabled(tab != 0); // "默认"tab 不可改名
    QAction *closeTabAction = menu.addAction(tr("删除当前列表"));
    closeTabAction->setEnabled(tab != 0); // "默认"tab 不可删除

    QAction *chosen = menu.exec(event->globalPos());
    if (!chosen)
        return;

    if (chosen == addAction) {
        const QStringList files = QFileDialog::getOpenFileNames(
            this, tr("添加文件"), QString(),
            tr("媒体文件 (*.mp4 *.mkv *.avi *.mov *.wmv *.flv *.webm *.ts *.mp3 *.flac *.aac *.ogg *.wav *.m4a);;所有文件 (*)"));
        if (!files.isEmpty()) {
            addFiles(files);
            return; // addFiles 内部已处理持久化
        }
    } else if (chosen == newTabAction) {
        // 找到已有"播放列表 N"的最大序号，+1 作为新名称
        int maxN = 0;
        for (int i = 0; i < count(); ++i) {
            const QString &name = tabText(i);
            if (name.startsWith(tr("播放列表 "))) {
                bool ok = false;
                int n = name.mid(QString(tr("播放列表 ")).length()).toInt(&ok);
                if (ok && n > maxN)
                    maxN = n;
            }
        }
        addTab(createList(), tr("播放列表 %1").arg(maxN + 1));
        setCurrentIndex(count() - 1);
        savePlaylists();
    } else if (chosen == removeAction && list) {
        if (isPlayingTab) {
            // 与 mpv 播放列表同步的 tab：发命令给 mpv
            QList<int> rows;
            for (const QListWidgetItem *item : list->selectedItems())
                rows.append(list->row(item));
            emit removeRequested(rows);
        } else {
            qDeleteAll(list->selectedItems());
            if (tab != 0)
                savePlaylists();
        }
    } else if (chosen == clearAction && list) {
        if (isPlayingTab) {
            emit clearRequested();
        } else {
            list->clear();
            if (tab != 0)
                savePlaylists();
        }
    } else if (chosen == renameAction) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("重命名播放列表"), tr("名称："),
                                                   QLineEdit::Normal, tabText(tab), &ok);
        if (ok && !name.isEmpty()) {
            setTabText(tab, name);
            if (tab != 0 && tab != m_playingTab)
                savePlaylists();
        }
    } else if (chosen == closeTabAction) {
        const int idx = currentIndex();
        if (m_playingTab == idx)
            m_playingTab = 0;
        else if (m_playingTab > idx)
            --m_playingTab;
        removeTab(idx);
        savePlaylists();
    }
}

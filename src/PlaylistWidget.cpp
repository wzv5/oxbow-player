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
#include <QMessageBox>
#include <QSettings>

PlaylistWidget::PlaylistWidget(const QString &configFile, QWidget *parent)
    : QTabWidget(parent), m_configFile(configFile)
{
    setAcceptDrops(true);
    setDocumentMode(true);
    setMinimumWidth(220);
    addTab(createList(), tr("默认"));
    connect(this, &QTabWidget::currentChanged, this, &PlaylistWidget::onTabChanged);
}

QListWidget *PlaylistWidget::createList()
{
    auto *list = new QListWidget;
    // 子列表不接受拖放，事件会上溯到本 QTabWidget 统一处理
    list->setAcceptDrops(false);
    // 播放列表不允许多选（显式声明，锁定约束）
    list->setSelectionMode(QAbstractItemView::SingleSelection);
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

    // 记录重建前的选中行（单选）：停止后 mpv 列表无 current 项，
    // 不恢复的话整体重建会把界面上的选中清空（仅显示问题）
    const int prevRow = list->currentRow();

    list->clear();
    QListWidgetItem *newCurrent = nullptr;
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
            newCurrent = item;
        }
    }

    if (newCurrent)
        list->setCurrentItem(newCurrent);
    else if (prevRow >= 0 && prevRow < list->count())
        list->setCurrentRow(prevRow);
}

void PlaylistWidget::addFiles(const QStringList &files)
{
    QListWidget *list = listAt(currentIndex());
    if (!list || files.isEmpty())
        return;

    // 正在播放 tab 的内容以 mpv 回刷为准，本地不重复插入（C4：消除双刷新闪烁）
    if (!currentIsPlayingTab()) {
        for (const QString &file : files) {
            auto *item = new QListWidgetItem(QFileInfo(file).fileName(), list);
            item->setData(Qt::UserRole, file);
            item->setToolTip(file);
        }
        // 非默认本地 tab 添加后持久化
        if (currentIndex() != 0)
            savePlaylists();
    }
    emit filesAdded(files);
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
        // 仅跳过无名节（C3）：默认 tab 从不落盘（索引 0 即默认），文本比较
        // 在 i18n 下脆弱、且会误伤被改名为“默认”的正常列表
        if (name.isEmpty()) {
            settings.endGroup();
            continue;
        }

        auto *list = createList();
        const int lastPos = settings.value(QStringLiteral("lastPos"), -1).toInt();
        const double lastTimePos = settings.value(QStringLiteral("lastTimePos"), -1.0).toDouble();
        // B6：按显式计数字段逐条读取。childKeys() 顺序未定义，键多时呈字典序
        // （"0","1","10","100","11"...），百项级列表会乱序呈现，不可用
        const int n = settings.value(QStringLiteral("itemCount"), 0).toInt();
        for (int j = 0; j < n; ++j) {
            const QString file = settings.value(QString::number(j)).toString();
            if (!file.isEmpty()) {
                auto *item = new QListWidgetItem(QFileInfo(file).fileName(), list);
                item->setData(Qt::UserRole, file);
                item->setToolTip(file);
            }
        }
        settings.endGroup();
        addTab(list, name);
        if (lastPos >= 0)
            m_memory[list] = { lastPos, lastTimePos };
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
        // 仅不保存"默认"tab（索引 0）；非默认 tab 即使是播放目标也持久化内容与记忆
        if (i == 0)
            continue;

        const QStringList files = filesOf(i);
        const QString section = QStringLiteral("Playlist_%1").arg(savedIdx);
        settings.beginGroup(section);
        settings.setValue(QStringLiteral("name"), tabText(i));
        settings.setValue(QStringLiteral("itemCount"), files.size()); // B6：显式计数，读取按序号遍历
        const TabMemory mem = m_memory.value(listAt(i));
        if (mem.lastPos >= 0) {
            settings.setValue(QStringLiteral("lastPos"), mem.lastPos);
            settings.setValue(QStringLiteral("lastTimePos"), mem.lastTimePos);
        }
        for (int j = 0; j < files.size(); ++j)
            settings.setValue(QString::number(j), files[j]);
        settings.endGroup();
        ++savedIdx;
    }

    settings.beginGroup(QStringLiteral("Playlists"));
    settings.setValue(QStringLiteral("count"), savedIdx);
    settings.endGroup();
}

void PlaylistWidget::recordPlayback(int tabIndex, int pos, double seconds)
{
    if (tabIndex < 0 || pos < 0) // 负索引无效；默认 tab 允许进程内记忆（不持久化）
        return;
    QListWidget *list = listAt(tabIndex);
    if (!list)
        return;
    TabMemory &mem = m_memory[list];
    mem.lastPos = pos;
    mem.lastTimePos = seconds;
}

int PlaylistWidget::lastPosOf(int tabIndex) const
{
    const QListWidget *list = listAt(tabIndex);
    return list ? m_memory.value(list).lastPos : -1;
}

double PlaylistWidget::lastTimePosOf(int tabIndex) const
{
    const QListWidget *list = listAt(tabIndex);
    return list ? m_memory.value(list).lastTimePos : -1.0;
}

int PlaylistWidget::indexOfName(const QString &name) const
{
    for (int i = 0; i < count(); ++i) {
        if (tabText(i) == name)
            return i;
    }
    return -1;
}

void PlaylistWidget::onTabChanged(int index)
{
    // 切换到非播放 tab 时自动选中该 tab 记忆的最后播放项；
    // 播放 tab 的选中项由 mpv 同步高亮驱动，跳过以避免互相覆盖
    if (index < 0 || index == m_playingTab)
        return;
    QListWidget *list = listAt(index);
    const int row = lastPosOf(index);
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
            // 重命名守卫（C3）：与默认 tab 同名或与其他 tab 重名，
            // 都会破坏基于 indexOfName 的会话恢复，予以拒绝
            if (name == tabText(0)) {
                QMessageBox::warning(this, tr("重命名播放列表"), tr("不能使用默认列表的名称。"));
                return;
            }
            bool duplicated = false;
            for (int i = count() - 1; i >= 0; --i) {
                if (i != tab && tabText(i) == name) {
                    duplicated = true;
                    break;
                }
            }
            if (duplicated) {
                QMessageBox::warning(this, tr("重命名播放列表"), tr("该名称已被其他列表使用。"));
                return;
            }
            setTabText(tab, name);
            if (tab != 0 && tab != m_playingTab)
                savePlaylists();
        }
    } else if (chosen == closeTabAction) {
        const int idx = currentIndex();
        // 先清理被关闭 tab 的播放记忆，防悬垂指针键
        if (QListWidget *closing = listAt(idx))
            m_memory.remove(closing);
        if (m_playingTab == idx)
            m_playingTab = 0;
        else if (m_playingTab > idx)
            --m_playingTab;
        removeTab(idx);
        savePlaylists();
    }
}

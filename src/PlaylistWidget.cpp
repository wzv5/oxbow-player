#include "pch.h"

#include "PlaylistWidget.h"

#include "Constants.h"
#include "ListSettingsDialog.h"
#include "MpvController.h"
#include "PlaylistDelegate.h"
#include "StaticProvider.h"
#include "Util.h"

namespace {

// 规范化判重键：优先 canonicalFilePath（解析符号链接与大小写真实形态），
// 失败退化为清理后的绝对路径；Windows 大小写不敏感，统一转小写后比较
QString canonicalKey(const QString &path)
{
    const QFileInfo fi(path);
    QString key = fi.canonicalFilePath();
    if (key.isEmpty())
        key = QDir::cleanPath(fi.absoluteFilePath());
    return key.toLower();
}

// 发行时间解析：ISO 8601 优先，兼容常见格式；全败返回空
QDateTime parseReleaseDate(const QString &s)
{
    if (s.isEmpty())
        return {};
    const QDateTime dt = QDateTime::fromString(s, Qt::ISODate);
    if (dt.isValid())
        return dt;
    const QDate d = QDate::fromString(s.left(10), Qt::ISODate);
    if (d.isValid())
        return {d.startOfDay()};
    const QStringList formats{
        QStringLiteral("yyyy-MM-dd"), QStringLiteral("yyyy/MM/dd"),
        QStringLiteral("yyyy-MM"), QStringLiteral("yyyy"),
    };
    for (const QString &fmt : formats) {
        const QDate d2 = QDate::fromString(s.left(fmt.length()), fmt);
        if (d2.isValid())
            return {d2.startOfDay()};
    }
    return {};
}

// 元数据标签不敏感取值：mpv 的键大小写因封装格式而异
QString tagValue(const QVariantMap &tags, std::initializer_list<const char *> names)
{
    for (const char *want : names) {
        for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
            if (it.key().compare(QLatin1String(want), Qt::CaseInsensitive) == 0)
                return it.value().toString();
        }
    }
    return {};
}

} // namespace

PlaylistWidget::PlaylistWidget(const QString &configFile, QWidget *parent)
    : QTabWidget(parent), m_configFile(configFile)
{
    setAcceptDrops(true);
    setDocumentMode(true);
    setMinimumWidth(220);
    addTab(createList(), tr("默认"));
    connect(this, &QTabWidget::currentChanged, this, &PlaylistWidget::onTabChanged);
}

QString PlaylistWidget::normalizePath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QListWidget *PlaylistWidget::createList()
{
    auto *list = new QListWidget;
    // 子列表不接受拖放，事件会上溯到本 QTabWidget 统一处理
    list->setAcceptDrops(false);
    // N5：列表集中创建点统一安装过滤器（ShortcutOverride 认领导航键）
    list->installEventFilter(this);
    // 播放列表不允许多选（显式声明，锁定约束）
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    // N4：委托绘制（时长经路径查全局元数据仓）
    list->setItemDelegate(new PlaylistDelegate(&m_meta, list));
    // 序号位数随总数自适应（绘制时实时计算）；增删行后强制整表重绘，
    // 避免未受影响行保留旧的位数宽度造成混排
    {
        auto *model = list->model();
        auto touch = [list]() { list->viewport()->update(); };
        connect(model, &QAbstractItemModel::rowsInserted, list, touch);
        connect(model, &QAbstractItemModel::rowsRemoved, list, touch);
        connect(model, &QAbstractItemModel::modelReset, list, touch);
    }
    connect(list, &QListWidget::itemDoubleClicked, this, &PlaylistWidget::onItemDoubleClicked);
    // N9：每个 tab 出生即配静态提供者（默认 tab 同样，仅持久化侧跳过）
    m_providerOf.emplace(list, std::make_unique<StaticProvider>());
    return list;
}

QListWidget *PlaylistWidget::listAt(int index) const
{
    return qobject_cast<QListWidget *>(widget(index));
}

// N5：列表持焦时认领全部导航键，抑制全局音量动作（↑/↓）；
// 认领集合含 ←→/PgUp/PgDn/Home/End，为后续列表键盘功能预留。
// accept 后按键作为普通键盘事件送达列表；过滤器须返回 false 放行事件本身
bool PlaylistWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride
        && qobject_cast<QListWidget *>(watched)) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
            event->accept();
            break;
        default:
            break;
        }
    }
    return QTabWidget::eventFilter(watched, event);
}

PlaylistItem &PlaylistWidget::materializeByRaw(const QString &rawPath)
{
    PlaylistItem &item = materialize(normalizePath(rawPath));
    // 原始路径首次写入者为准，统一转为 Windows 原生分隔符供 UI 显示
    if (item.path.isEmpty())
        item.path = QDir::toNativeSeparators(rawPath);
    return item;
}

PlaylistItem &PlaylistWidget::materialize(const QString &path)
{
    auto it = m_meta.find(path);
    if (it != m_meta.end())
        return it.value();

    PlaylistItem item;
    const QFileInfo fi(path);
    item.fileSize = qMax<qint64>(0, fi.size());
    item.created = fi.birthTime();
    item.modified = fi.lastModified();
    item.icon = fi.exists() ? m_iconProvider.icon(fi) : m_iconProvider.icon(QFileIconProvider::File);

    return m_meta.insert(path, item).value();
}

void PlaylistWidget::materializeList(QListWidget *list)
{
    if (!list)
        return;
    for (int i = 0; i < list->count(); ++i)
        materialize(list->item(i)->data(PathRole).toString());
}

// 统一的条目构造：显示文件名（含扩展名）、图标取自仓、tooltip 为原始路径。
// 内部匹配键经 normalizePath 规范化；UI 展示用 path 字段保留原生形态
QListWidgetItem *PlaylistWidget::makeItem(QListWidget *list, const QString &rawPath)
{
    const PlaylistItem &meta = materializeByRaw(rawPath);
    auto *item = new QListWidgetItem(list);
    item->setData(Qt::DisplayRole, QFileInfo(meta.path).fileName());
    item->setData(Qt::DecorationRole, meta.icon);
    item->setData(PathRole, normalizePath(rawPath));
    item->setToolTip(meta.path);
    return item;
}

QStringList PlaylistWidget::filesOf(int tabIndex) const
{
    QStringList files;
    if (QListWidget *list = listAt(tabIndex)) {
        for (int i = 0; i < list->count(); ++i)
            files.append(list->item(i)->data(PathRole).toString());
    }
    return files;
}

int PlaylistWidget::playingRowOf(const QListWidget *list) const
{
    if (!list)
        return -1;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(PlayingRole).toBool())
            return i;
    }
    return -1;
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
        QListWidgetItem *item = makeItem(list,filename);
        // 正在播放标记写入视图条目专用角色（会话层信息不入元数据仓）
        if (map.value(QStringLiteral("current")).toBool()) {
            item->setData(PlayingRole, true);
            newCurrent = item;
        }
    }

    if (newCurrent) {
        list->setCurrentItem(newCurrent);
        // 跟随滚动（N4）：正播项变动时最小位移滚入可见范围
        list->scrollToItem(newCurrent, QListWidget::EnsureVisible);
    } else if (prevRow >= 0 && prevRow < list->count()) {
        list->setCurrentRow(prevRow);
    }
}

void PlaylistWidget::addFiles(const QStringList &files)
{
    QListWidget *list = listAt(currentIndex());
    if (!list || files.isEmpty())
        return;

    // N9：动态 tab 内容来自枚举，忽略手工添加（拖放与菜单同守卫）。
    // 静态 tab 增删即回无序态：复位排序规则（保留方向）
    if (isDynamicTab(currentIndex()))
        return;
    if (auto *p = providerAt(currentIndex()))
        p->setSortRule(int(SortKey::None), p->sortAscending());

    // 正在播放 tab 的内容以 mpv 回刷为准，本地不重复插入（C4：消除双刷新闪烁）
    if (!currentIsPlayingTab()) {
        for (const QString &file : files)
            makeItem(list,file);
        // 非默认本地 tab 添加后持久化
        if (currentIndex() != 0)
            savePlaylists();
    }
    emit filesAdded(files);
}

// N11：主菜单"打开文件"固定落点——整体替换默认 tab 内容并作废其续播记忆
// （清空列表隐含记忆失效，Q3 裁决）。默认 tab 永不持久化（N9/N4 既定），
// 也不发 filesAdded：立即播放由 MainWindow 经统一入口 onPlayRequested 驱动，
// 避免误入播放中 append 分支
void PlaylistWidget::setDefaultTabFiles(const QStringList &files)
{
    QListWidget *list = listAt(0);
    if (!list || files.isEmpty())
        return;
    m_memory.remove(list);
    list->clear();
    for (const QString &file : files)
        makeItem(list, file);
}

// ——元数据回填（N4）：按路径覆写仓中字段并触发相关行重绘——

void PlaylistWidget::repaintAllLists()
{
    for (int i = 0; i < count(); ++i) {
        if (QListWidget *l = listAt(i))
            l->viewport()->update();
    }
}

void PlaylistWidget::applyDuration(const QString &rawPath, double seconds)
{
    if (rawPath.isEmpty() || !(seconds > 0))
        return;
    materializeByRaw(rawPath).duration = seconds;
    repaintAllLists();
}

void PlaylistWidget::applyMediaMetadata(const QString &rawPath, const QVariantMap &tags)
{
    PlaylistItem &item = materializeByRaw(rawPath);
    const QString title = tagValue(tags, {"title"});
    const QString album = tagValue(tags, {"album"});
    const QString artist = tagValue(tags, {"artist", "performer"});
    const QString date = tagValue(tags, {"date", "original_date", "year"});
    if (!title.isEmpty())
        item.title = title;
    if (!album.isEmpty())
        item.album = album;
    if (!artist.isEmpty())
        item.artist = artist;
    const QDateTime rd = parseReleaseDate(date);
    if (rd.isValid())
        item.releaseDate = rd;
    repaintAllLists();
}

void PlaylistWidget::applyStreamInfo(const QString &rawPath, const QVariantMap &info)
{
    if (info.isEmpty())
        return;
    PlaylistItem &item = materializeByRaw(rawPath);

    // 文件格式：file-format 为 libavformat 解封装器短名
    if (info.contains(QStringLiteral("fileFormat")))
        item.fileFormat = info.value(QStringLiteral("fileFormat")).toString();

    if (info.contains(QStringLiteral("videoCodec")))
        item.videoCodec = info.value(QStringLiteral("videoCodec")).toString();
    if (info.contains(QStringLiteral("videoW")) && info.contains(QStringLiteral("videoH"))) {
        item.videoResolution = QSize(info.value(QStringLiteral("videoW")).toInt(),
                                     info.value(QStringLiteral("videoH")).toInt());
    }
    if (info.contains(QStringLiteral("fps")))
        item.videoFps = info.value(QStringLiteral("fps")).toDouble();

    if (info.contains(QStringLiteral("audioCodec")))
        item.audioCodec = info.value(QStringLiteral("audioCodec")).toString();
    if (info.contains(QStringLiteral("channels")))
        item.audioChannels = info.value(QStringLiteral("channels")).toInt();
    if (info.contains(QStringLiteral("sampleRate")))
        item.audioSampleRate = info.value(QStringLiteral("sampleRate")).toInt();
    repaintAllLists();
}

// ——续播记忆三步维护法（N4）——

QString PlaylistWidget::captureMemoryPath(QListWidget *list) const
{
    const TabMemory mem = m_memory.value(list);
    if (mem.lastPos < 0 || mem.lastPos >= list->count())
        return {};
    return list->item(mem.lastPos)->data(PathRole).toString();
}

void PlaylistWidget::restoreMemoryPath(QListWidget *list, const QString &captured, bool hadMemory)
{
    if (!hadMemory)
        return;
    TabMemory mem = m_memory.value(list);
    if (!captured.isEmpty()) {
        for (int r = 0; r < list->count(); ++r) {
            if (list->item(r)->data(PathRole).toString() == captured) {
                mem.lastPos = r;
                m_memory[list] = mem;
                return;
            }
        }
    }
    mem.lastPos = -1; // 记忆条目已在本次变更中被删除
    m_memory[list] = mem;
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
        TabMemory mem;
        mem.lastPos = settings.value(QStringLiteral("lastPos"), -1).toInt();
        mem.lastTimePos = settings.value(QStringLiteral("lastTimePos"), -1.0).toDouble();
        mem.lastPath = settings.value(QStringLiteral("lastPath")).toString();

        // N9：提供者反序列化完全自治（静态回填条目、目录读参数与排序键），
        // 内容统一经 enumerate 取得
        std::unique_ptr<PlaylistProvider> provider = PlaylistProvider::load(settings);
        settings.endGroup();
        addTab(list, name);
        // insert_or_assign：createList 已预置静态默认，此处必须覆盖
        m_providerOf.insert_or_assign(list, std::move(provider));
        if (mem.lastPos >= 0 || !mem.lastPath.isEmpty())
            m_memory[list] = mem;
        fillFromProvider(list); // 静态取所持条目、目录枚举绑定源
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

        QListWidget *list = listAt(i);
        auto *provider = providerAt(i);
        // N9：静态列表内容先回写提供者（内容跟提供者走），序列化统一走 saveSettings。
        // 按类型分支下探，不依赖 RTTI
        if (provider->type() == PlaylistProvider::Type::Static)
            static_cast<StaticProvider *>(provider)->setItems(filesOf(i));
        const QString section = QStringLiteral("Playlist_%1").arg(savedIdx);
        settings.beginGroup(section);
        settings.setValue(QStringLiteral("name"), tabText(i));
        if (provider)
            provider->saveSettings(settings); // 静态：类型+条目；目录：类型+参数
        const TabMemory mem = m_memory.value(list);
        // N9：记忆三字段（路径+序号+进度）静态/动态均持久化
        if (mem.lastPos >= 0 || !mem.lastPath.isEmpty()) {
            settings.setValue(QStringLiteral("lastPos"), mem.lastPos);
            settings.setValue(QStringLiteral("lastTimePos"), mem.lastTimePos);
            settings.setValue(QStringLiteral("lastPath"), mem.lastPath);
        }
        // N9：排序规则已随提供者序列化（saveSettings 内部），此处无额外处理
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
    // N9：路径随写入点采集（路径+序号双存）
    mem.lastPath = (pos < list->count()) ? list->item(pos)->data(PathRole).toString() : QString();
}

void PlaylistWidget::clearResumeProgress(int tabIndex)
{
    QListWidget *list = listAt(tabIndex);
    if (!list)
        return;
    auto it = m_memory.find(list);
    if (it != m_memory.end())
        it.value().lastTimePos = -1.0;
}

int PlaylistWidget::resolveResumeRow(int tabIndex) const
{
    const QListWidget *list = listAt(tabIndex);
    if (!list)
        return -1;
    const TabMemory mem = m_memory.value(list);
    if (mem.lastPos < 0 && mem.lastPath.isEmpty())
        return -1;
    // ① 序号处路径校验：一致则信任序号（含重复项场景的精确恢复）
    if (mem.lastPos >= 0 && mem.lastPos < list->count()
        && list->item(mem.lastPos)->data(PathRole).toString() == mem.lastPath)
        return mem.lastPos;
    // ② 序号失效：按路径取第一个命中
    if (!mem.lastPath.isEmpty()) {
        for (int r = 0; r < list->count(); ++r) {
            if (list->item(r)->data(PathRole).toString() == mem.lastPath)
                return r;
        }
    }
    return -1;
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
    if (index < 0)
        return;
    // 切入正在播放的列表：居中滚动到正播项。
    // 选中态由 mpv 同步高亮驱动，此处只滚不改选
    if (index == m_playingTab) {
        QTimer::singleShot(0, this, [this, index]() {
            if (QListWidget *l = listAt(index)) {
                const int pr = playingRowOf(l);
                if (pr >= 0)
                    l->scrollToItem(l->item(pr), QListWidget::PositionAtCenter);
            }
        });
        return;
    }
    // 切换到非播放 tab 时自动选中该 tab 记忆的最后播放项（N9：路径+序号解析）
    QListWidget *list = listAt(index);
    const int row = resolveResumeRow(index);
    if (list && row >= 0 && row < list->count()) {
        materializeList(list); // 惰性物化：首次访问该 tab 时补齐元数据
        list->setCurrentRow(row);
        // 跟随滚动（N4）：currentChanged 到达时新 tab 尚未完成首次布局，
        // 立即滚动会打在零尺寸视口上无效，推迟到事件循环下一拍
        QTimer::singleShot(0, this, [this, index]() {
            if (QListWidget *l = listAt(index)) {
                const int r = resolveResumeRow(index);
                if (r >= 0 && r < l->count() && l->currentRow() == r)
                    l->scrollToItem(l->currentItem(), QListWidget::PositionAtCenter);
            }
        });
    }
}

int PlaylistWidget::currentSelectedRow() const
{
    const QListWidget *list = listAt(currentIndex());
    if (!list || list->selectedItems().isEmpty())
        return -1;
    return list->currentRow();
}

// B10：同步目标转移。同 tab 早退（播放中双击本 tab 条目时防标记闪烁）；
// 异 tab 转移先清空原 tab 全部行的播放标记——setPlaylist 仅回刷新播放 tab，
// 旧 tab 标记若不在此清理将永久残留
void PlaylistWidget::markPlayingTab(int tabIndex)
{
    if (tabIndex == m_playingTab)
        return;
    if (QListWidget *old = listAt(m_playingTab)) {
        for (int i = 0; i < old->count(); ++i)
            old->item(i)->setData(PlayingRole, false);
    }
    m_playingTab = tabIndex;
}

void PlaylistWidget::onItemDoubleClicked(QListWidgetItem *item)
{
    QListWidget *list = item->listWidget();
    const int tab = indexOf(list);
    if (!list || tab < 0)
        return;

    markPlayingTab(tab);
    emit playRequested(filesOf(tab), list->row(item), -1.0);
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

// ——N9 提供者——

PlaylistProvider *PlaylistWidget::providerAt(int tabIndex) const
{
    const auto it = m_providerOf.find(listAt(tabIndex));
    return it == m_providerOf.end() ? nullptr : it->second.get();
}

bool PlaylistWidget::isDynamicTab(int tabIndex) const
{
    auto *p = providerAt(tabIndex);
    return p && p->type() == PlaylistProvider::Type::Dir;
}

// 视图内容统一从提供者取得：静态取所持条目，目录扫描绑定源。
// 填充后按该 tab 排序规则重排（None=保持枚举序）
void PlaylistWidget::fillFromProvider(QListWidget *list)
{
    if (!list)
        return;
    const auto it = m_providerOf.find(list);
    if (it == m_providerOf.end())
        return;
    list->clear();
    const QStringList entries = it->second->enumerate();
    for (const QString &path : entries)
        makeItem(list, path);
    const auto key = SortKey(it->second->sortKey());
    if (key != SortKey::None)
        sortListItems(list, key, it->second->sortAscending());
}

// 条目重排核心：行指针摘除 → 稳定排序 → 回填。无停止/恢复副作用，
// 四步式流程（sortCurrent）与加载填充共用
void PlaylistWidget::sortListItems(QListWidget *list, SortKey key, bool ascending)
{
    if (!list || key == SortKey::None || list->count() < 2)
        return;

    const int n = list->count();
    QVector<QString> paths;
    paths.reserve(n);
    for (int i = 0; i < n; ++i)
        paths.append(list->item(i)->data(PathRole).toString());

    QVector<QList<Util::NameSegment>> numericSegs;
    if (key == SortKey::FileNameNumeric) {
        numericSegs.resize(n);
        for (int i = 0; i < n; ++i)
            numericSegs[i] = Util::segmentName(QFileInfo(paths[i]).fileName());
    }

    auto lessByPath = [this, key, ascending, &paths, &numericSegs](int x, int y) -> bool {
        const QString &pa = paths[x];
        const QString &pb = paths[y];
        const PlaylistItem &a = materialize(pa);
        const PlaylistItem &b = materialize(pb);
        auto flip = [ascending](int c) { return ascending ? c < 0 : c > 0; };
        switch (key) {
        case SortKey::None:
            return false;
        case SortKey::FileName:
            return flip(QString::compare(QFileInfo(pa).fileName(), QFileInfo(pb).fileName(),
                                         Qt::CaseInsensitive));
        case SortKey::FileNameNumeric:
            return flip(Util::naturalCompare(numericSegs[x], numericSegs[y]));
        case SortKey::Extension:
            return flip(QString::compare(QFileInfo(pa).suffix(), QFileInfo(pb).suffix(),
                                         Qt::CaseInsensitive));
        case SortKey::Created:
            return flip(a.created < b.created ? -1 : (b.created < a.created ? 1 : 0));
        case SortKey::Modified:
            return flip(a.modified < b.modified ? -1 : (b.modified < a.modified ? 1 : 0));
        case SortKey::FileSize:
            return flip(a.fileSize < b.fileSize ? -1 : (b.fileSize < a.fileSize ? 1 : 0));
        case SortKey::Duration:
        case SortKey::ReleaseDate: {
            // 未知值恒归末尾组、组内原序、不受方向影响
            const bool ha = (key == SortKey::Duration) ? a.duration.has_value()
                                                       : a.releaseDate.has_value();
            const bool hb = (key == SortKey::Duration) ? b.duration.has_value()
                                                       : b.releaseDate.has_value();
            if (!ha || !hb)
                return ha && !hb; // 已知在前、未知在后；双未知视为等键
            if (key == SortKey::Duration) {
                return flip(*a.duration < *b.duration ? -1 : (*b.duration < *a.duration ? 1 : 0));
            }
            return flip(*a.releaseDate < *b.releaseDate ? -1
                                                        : (*b.releaseDate < *a.releaseDate ? 1 : 0));
        }
        }
        return false;
    };

    QVector<int> order(n);
    for (int i = 0; i < n; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
        return lessByPath(x, y);
    });

    QList<QListWidgetItem *> taken;
    taken.reserve(n);
    for (int i = 0; i < n; ++i)
        taken.append(list->takeItem(0)); // 逐个摘除，转移所有权
    for (int idx : order)
        list->addItem(taken.at(idx));
}

// ——排序（N4 第五节）——

void PlaylistWidget::sortCurrent(SortKey key, bool ascending)
{
    QListWidget *list = listAt(currentIndex());
    const int tab = currentIndex();
    if (!list || tab < 0 || key == SortKey::None)
        return;

    // N9：排序规则落位于提供者（随其持久化）
    if (auto *p = providerAt(tab))
        p->setSortRule(int(key), ascending);
    const bool playingTab = (tab == m_playingTab);
    const bool hadMemory = m_memory.contains(list) && m_memory.value(list).lastPos >= 0;
    const QString captured = captureMemoryPath(list);

    // 四步式第 1 步：记录正播路径 P 与当前播放进度（停止前读取）
    QString resumePath;
    double resumeSec = -1.0;
    if (playingTab) {
        const int pr = playingRowOf(list);
        if (pr >= 0) {
            resumePath = list->item(pr)->data(PathRole).toString();
            resumeSec = m_mpv ? m_mpv->propertyDouble(QStringLiteral("time-pos"), -1.0) : -1.0;
        }
    }
    // 第 2 步：停止
    if (playingTab && !resumePath.isEmpty())
        emit stopRequested();

    // 第 3 步：重排本 tab（核心抽取至 sortListItems，加载填充共用）
    sortListItems(list, key, ascending);

    // 第 4 步：推送并恢复（重复项取首现位置；停止前有进度则从原进度续播）
    if (playingTab && !resumePath.isEmpty()) {
        const QStringList files = filesOf(tab);
        const int idx = files.indexOf(resumePath);
        if (idx >= 0)
            emit playRequested(files, idx, resumeSec);
        // idx < 0 理论不可达（排序不删行）；保守起见不恢复
    }

    restoreMemoryPath(list, captured, hadMemory);
}

// ——清理操作（N4 第六节）——

void PlaylistWidget::executeRemoval(const QList<int> &rows)
{
    QListWidget *list = listAt(currentIndex());
    const int tab = currentIndex();
    if (!list || rows.isEmpty() || tab < 0)
        return;

    const bool playingTab = (tab == m_playingTab);
    const bool hadMemory = m_memory.contains(list) && m_memory.value(list).lastPos >= 0;
    const QString captured = captureMemoryPath(list);

    QString resumePath;
    double resumeSec = -1.0;
    if (playingTab) {
        const int pr = playingRowOf(list);
        if (pr >= 0) {
            resumePath = list->item(pr)->data(PathRole).toString();
            resumeSec = m_mpv ? m_mpv->propertyDouble(QStringLiteral("time-pos"), -1.0) : -1.0;
        }
    }
    if (playingTab && !resumePath.isEmpty())
        emit stopRequested();

    // 从大到小删除避免行号漂移
    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int row : sorted) {
        if (row >= 0 && row < list->count())
            delete list->takeItem(row);
    }

    if (playingTab && !resumePath.isEmpty()) {
        const QStringList files = filesOf(tab);
        const int idx = files.indexOf(resumePath);
        // P 仍在列表：从原进度恢复；P 自身已失效被清则保持停止
        if (idx >= 0)
            emit playRequested(files, idx, resumeSec);
    } else if (tab != 0) {
        savePlaylists();
    }

    restoreMemoryPath(list, captured, hadMemory);
}

void PlaylistWidget::removeInvalidItems()
{
    QListWidget *list = listAt(currentIndex());
    if (!list)
        return;
    QList<int> rows;
    for (int i = 0; i < list->count(); ++i) {
        if (!QFileInfo::exists(list->item(i)->data(PathRole).toString()))
            rows.append(i);
    }
    executeRemoval(rows);
}

void PlaylistWidget::removeDuplicateItems()
{
    QListWidget *list = listAt(currentIndex());
    if (!list)
        return;
    QSet<QString> seen;
    QList<int> rows;
    for (int i = 0; i < list->count(); ++i) {
        const QString key = canonicalKey(list->item(i)->data(PathRole).toString());
        if (!seen.contains(key))
            seen.insert(key);
        else
            rows.append(i); // 保留首现，其余为待删重复
    }
    executeRemoval(rows);
}

// ——右键菜单动作（N4 第七节）——

void PlaylistWidget::refreshCurrentTab()
{
    QListWidget *list = listAt(currentIndex());
    if (!list)
        return;
    // N9：动态 tab 刷新＝重新枚举（播放中 tab 走四步式保进度恢复）
    if (isDynamicTab(currentIndex())) {
        const bool playingTab = (currentIndex() == m_playingTab);
        const bool hadMemory = m_memory.contains(list) && m_memory.value(list).lastPos >= 0;
        const QString captured = captureMemoryPath(list);
        QString resumePath;
        double resumeSec = -1.0;
        if (playingTab) {
            const int pr = playingRowOf(list);
            if (pr >= 0) {
                resumePath = list->item(pr)->data(PathRole).toString();
                resumeSec = m_mpv ? m_mpv->propertyDouble(QStringLiteral("time-pos"), -1.0) : -1.0;
            }
        }
        if (playingTab && !resumePath.isEmpty())
            emit stopRequested();

        fillFromProvider(list);

        if (playingTab && !resumePath.isEmpty()) {
            const QStringList files = filesOf(currentIndex());
            const int idx = files.indexOf(resumePath);
            if (idx >= 0)
                emit playRequested(files, idx, resumeSec);
        } else if (currentIndex() != 0) {
            savePlaylists();
        }
        restoreMemoryPath(list, captured, hadMemory);
        return;
    }

    m_meta.clear();          // 清空全局仓
    materializeList(list);   // 当前 tab 立即重新物化（图标/大小/时间自取）
    repaintAllLists();       // 其他 tab 待访问时惰性补齐
}

void PlaylistWidget::copyPathToClipboard(const QString &path)
{
    QApplication::clipboard()->setText(path);
}

void PlaylistWidget::showPropertiesDialog(const QString &path)
{
    if (path.isEmpty())
        return;
    const PlaylistItem &item = materialize(path);

    auto timeText = [](const QDateTime &dt) {
        return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString();
    };

    struct Row { QString prop; QString value; };

    // 只读文本框呈现（维护者裁决）：'\t' 制表对齐两列、整篇可划选复制；
    // 不自动换行，超宽由水平滚动条承接。分组以小号灰色标题行区隔
    QDialog dialog(this);
    dialog.setWindowTitle(tr("属性"));
    auto *view = new QTextEdit(&dialog);
    view->setReadOnly(true);
    view->setFrameShape(QFrame::NoFrame);
    view->setLineWrapMode(QTextEdit::NoWrap);
    {
        // 键列统一停靠宽度（最长键为四个汉字加冒号），保证值列纵向对齐
        QTextOption opt = view->document()->defaultTextOption();
        opt.setTabStopDistance(100.0);
        view->document()->setDefaultTextOption(opt);
    }

    QTextCursor cur(view->document());
    // 配色与播放列表三色体系对齐（维护者指定）：
    // 属性名=正播项蓝、属性值=普通未选灰、组标题保持浅灰
    QTextCharFormat nameFmt;
    nameFmt.setForeground(QColor(0x19, 0x76, 0xD2));
    QTextCharFormat valueFmt;
    valueFmt.setForeground(QColor(0x60, 0x60, 0x60));
    QTextCharFormat headFmt;  // 分组标题：浅灰
    headFmt.setForeground(QColor(0x90, 0x90, 0x90));

    auto appendRow = [&cur, &nameFmt, &valueFmt](const Row &row) {
        cur.setCharFormat(nameFmt);
        cur.insertText(row.prop + QStringLiteral(":"));
        cur.setCharFormat(valueFmt);
        cur.insertText(QStringLiteral("\t"));
        cur.insertText(row.value);
        cur.insertBlock();
    };
    auto appendGroup = [&](const QString &title, const QList<Row> &groupRows) {
        if (groupRows.isEmpty())
            return;
        if (!cur.atStart())
            cur.insertBlock(); // 组间空行
        cur.setCharFormat(headFmt);
        cur.insertText(title, headFmt);
        cur.insertBlock();
        for (const Row &r : groupRows)
            appendRow(r);
    };

    // 常规组
    QList<Row> general;
    general.append({tr("文件路径"), item.path.isEmpty() ? path : item.path});
    if (item.fileFormat)
        general.append({tr("文件格式"), *item.fileFormat});
    if (item.fileSize > 0)
        general.append({tr("文件大小"), Util::formatFileSize(item.fileSize)});
    if (timeText(item.created).size())
        general.append({tr("创建时间"), timeText(item.created)});
    if (timeText(item.modified).size())
        general.append({tr("修改时间"), timeText(item.modified)});
    if (item.duration)
        general.append({tr("总时长"), QStringLiteral("%1:%2:%3")
                                          .arg(qint64(*item.duration) / 3600)
                                          .arg((qint64(*item.duration) % 3600) / 60, 2, 10, QLatin1Char('0'))
                                          .arg(qint64(*item.duration) % 60, 2, 10, QLatin1Char('0'))});

    // 视频组：仅当前实际播放的轨
    QList<Row> video;
    if (item.videoCodec)
        video.append({tr("视频编码"), *item.videoCodec});
    if (item.videoResolution && item.videoResolution->isValid())
        video.append({tr("分辨率"), QStringLiteral("%1 x %2").arg(item.videoResolution->width())
                                                    .arg(item.videoResolution->height())});
    if (item.videoFps && *item.videoFps > 0)
        video.append({tr("帧率"), QString::number(*item.videoFps, 'f', 2)});

    // 音频组：同上
    QList<Row> audio;
    if (item.audioCodec)
        audio.append({tr("音频编码"), *item.audioCodec});
    if (item.audioChannels)
        audio.append({tr("通道数"), QString::number(*item.audioChannels)});
    if (item.audioSampleRate && *item.audioSampleRate > 0)
        audio.append({tr("采样率"), QStringLiteral("%1 kHz").arg(*item.audioSampleRate / 1000.0, 0, 'f', 1)});

    // 标签组
    QList<Row> tags;
    if (item.title)
        tags.append({tr("标题"), *item.title});
    if (item.album)
        tags.append({tr("专辑"), *item.album});
    if (item.artist)
        tags.append({tr("作者"), *item.artist});
    if (item.releaseDate)
        tags.append({tr("发行时间"), timeText(*item.releaseDate)});

    appendGroup(tr("常规"), general);
    appendGroup(tr("视频"), video);
    appendGroup(tr("音频"), audio);
    appendGroup(tr("标签"), tags);

    // 光标归位到文档开头，避免打开时选区残留
    view->setTextCursor(QTextCursor(view->document()));

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->addWidget(view);
    const int lineCount = general.size() + video.size() + audio.size() + tags.size() + 8;
    dialog.resize(520, qMin(lineCount * 22 + 40, 600));
    dialog.exec();
}

void PlaylistWidget::contextMenuEvent(QContextMenuEvent *event)
{
    // N9：按右键落点分区——tab 表头与列表主体各自独立菜单。
    // 区域判定用几何（覆盖表头空白区与两侧滚动箭头）：箭头控件是 QTabBar 的
    // 子按钮，按控件类型判定会误入主体菜单分支
    QWidget *underMouse = childAt(event->pos());
    if (tabBar()->geometry().contains(event->pos())) {
        // 动作作用于右键实际命中的 tab；hitTab=-1（空白区/滚动箭头）＝无作用对象，
        // 此时菜单仅"新建播放列表"可用
        const QPoint barPos = tabBar()->mapFrom(this, event->pos());
        int hitTab = tabBar()->tabAt(barPos);
        // 滚动箭头是表头的子按钮且叠压边缘表头：命中按钮即视为空白区，
        // 否则 tabAt 的纯几何查询会返回其下方被遮盖的表头
        if (hitTab >= 0) {
            for (QObject *c : tabBar()->children()) {
                if (auto *b = qobject_cast<QToolButton *>(c);
                    b && b->isVisible() && b->geometry().contains(barPos)) {
                    hitTab = -1;
                    break;
                }
            }
        }
        QMenu menu(this);
        QAction *newTabAction = menu.addAction(tr("新建播放列表"));
        QAction *closeTabAction = menu.addAction(tr("删除当前列表"));
        QAction *settingsAction = menu.addAction(tr("列表设置…"));
        closeTabAction->setEnabled(hitTab > 0);
        settingsAction->setEnabled(hitTab > 0); // 默认 tab 与空白区均无删除/设置

        QAction *chosen = menu.exec(event->globalPos());
        if (!chosen)
            return;
        if (chosen == newTabAction)
            createNewList();
        else if (chosen == closeTabAction)
            removeTabAt(hitTab);
        else if (chosen == settingsAction)
            openListSettings(hitTab);
        return;
    }

    // ——列表主体菜单（原全量菜单剔除新建/删除/重命名三项）——
    QListWidget *list = listAt(currentIndex());
    const int tab = currentIndex();
    const bool isPlayingTab = (tab == m_playingTab);
    const bool dynamic = isDynamicTab(tab);

    // 右键落点命中检测：条目级操作以命中的行为作用对象。
    // 注意 childAt 命中的是 QListWidget 的 viewport（孙级），需回溯一级
    m_menuTargetList = nullptr;
    m_menuTargetRow = -1;
    QListWidget *hitList = underMouse ? qobject_cast<QListWidget *>(underMouse) : nullptr;
    if (!hitList && underMouse)
        hitList = qobject_cast<QListWidget *>(underMouse->parentWidget());
    if (hitList) {
        const int row =
            hitList->indexAt(hitList->viewport()->mapFromGlobal(event->globalPos())).row();
        if (row >= 0) {
            m_menuTargetList = hitList;
            m_menuTargetRow = row;
        }
    }
    const QString targetPath =
        m_menuTargetList ? m_menuTargetList->item(m_menuTargetRow)->data(PathRole).toString() : QString();

    QMenu menu(this);

    QAction *addAction = menu.addAction(tr("添加文件…"));
    menu.addSeparator();

    QMenu *sortMenu = menu.addMenu(tr("排序"));
    QAction *sortActions[8];
    sortActions[0] = sortMenu->addAction(tr("文件名"));
    sortActions[1] = sortMenu->addAction(tr("文件名（数字化）"));
    sortActions[2] = sortMenu->addAction(tr("扩展名"));
    sortActions[3] = sortMenu->addAction(tr("创建时间"));
    sortActions[4] = sortMenu->addAction(tr("修改时间"));
    sortActions[5] = sortMenu->addAction(tr("总时长"));
    sortActions[6] = sortMenu->addAction(tr("文件大小"));
    sortActions[7] = sortMenu->addAction(tr("发行时间"));
    sortMenu->addSeparator();
    // N9：规则为单选勾选组（与持久化配套），方向互斥勾选且切向即时重排
    for (QAction *a : sortActions)
        a->setCheckable(true);
    QActionGroup *ruleGroup = new QActionGroup(sortMenu);
    auto *curProvider = providerAt(tab);
    const auto curKey = curProvider ? curProvider->sortKey() : int(SortKey::None);
    const bool curAsc = curProvider ? curProvider->sortAscending() : true;
    for (QAction *a : sortActions)
        ruleGroup->addAction(a);
    if (curKey != int(SortKey::None))
        sortActions[int(curKey) - 1]->setChecked(true);

    QAction *ascAction = sortMenu->addAction(tr("正序"));
    QAction *descAction = sortMenu->addAction(tr("反序"));
    for (QAction *a : {ascAction, descAction})
        a->setCheckable(true);
    QActionGroup *dirGroup = new QActionGroup(sortMenu);
    dirGroup->addAction(ascAction);
    dirGroup->addAction(descAction);
    ascAction->setChecked(curAsc);
    descAction->setChecked(!curAsc);

    QMenu *removeMenu = menu.addMenu(tr("移除"));
    QAction *removeSelAction = removeMenu->addAction(tr("移除选中项"));
    QAction *clearAction = removeMenu->addAction(tr("清空当前列表"));
    removeMenu->addSeparator();
    QAction *removeInvalidAction = removeMenu->addAction(tr("移除无效项"));
    QAction *removeDupAction = removeMenu->addAction(tr("移除重复项"));

    QAction *refreshAction = menu.addAction(tr("刷新"));
    menu.addSeparator();

    const QString targetNative =
        m_menuTargetList ? materialize(targetPath).path : QString();
    QAction *openLocationAction = menu.addAction(tr("打开文件位置"));
    QAction *copyPathAction = menu.addAction(tr("复制文件路径"));
    QAction *propsAction = menu.addAction(tr("属性"));

    // ——启用条件——
    const bool hasList = list != nullptr;
    const int itemCount = hasList ? list->count() : 0;
    addAction->setEnabled(!dynamic); // 动态 tab 内容来自枚举，禁手工添加
    auto *menuProvider = providerAt(tab); // 排序能力遵从提供者（单项目录亦可持久化规则）
    sortMenu->setEnabled(hasList && menuProvider && menuProvider->supportsSort());
    refreshAction->setEnabled(dynamic || itemCount > 0); // 动态刷新不受空内容限制
    // N9：动态 tab 移除子菜单整组禁用（失效清理走刷新）
    removeMenu->setEnabled(hasList && !dynamic);
    removeSelAction->setEnabled(hasList && !list->selectedItems().isEmpty());
    clearAction->setEnabled(itemCount > 0);

    const bool hasTarget = m_menuTargetList != nullptr;
    openLocationAction->setEnabled(hasTarget && !targetPath.isEmpty()
                                   && QFileInfo::exists(targetPath));
    copyPathAction->setEnabled(hasTarget);
    propsAction->setEnabled(hasTarget);

    QAction *chosen = menu.exec(event->globalPos());
    if (!chosen)
        return;

    if (chosen == addAction) {
        QStringList patterns;
        for (const QString &ext : App::defaultMediaExtensions())
            patterns << QStringLiteral("*.") + ext;
        const QStringList files = QFileDialog::getOpenFileNames(
            this, tr("添加文件"), QString(),
            tr("媒体文件 (%1);;所有文件 (*)").arg(patterns.join(QLatin1Char(' '))));
        if (!files.isEmpty())
            addFiles(files);
    } else if (chosen == refreshAction) {
        refreshCurrentTab();
    } else if (chosen == openLocationAction) {
        Util::openFileLocation(targetNative.isEmpty() ? targetPath : targetNative, this);
    } else if (chosen == copyPathAction) {
        copyPathToClipboard(targetNative.isEmpty() ? targetPath : targetNative);
    } else if (chosen == propsAction) {
        showPropertiesDialog(targetPath);
    } else if (chosen == removeSelAction && list && !dynamic) {
        QList<int> rows;
        for (QListWidgetItem *sel : list->selectedItems())
            rows.append(list->row(sel));
        if (isPlayingTab) {
            // 播放 tab 的移除仍走既有同步通道（逐行 playlist-remove）
            emit removeRequested(rows);
        } else {
            executeRemoval(rows);
        }
    } else if (chosen == clearAction && list && !dynamic) {
        if (isPlayingTab) {
            emit clearRequested();
        } else {
            QList<int> all;
            for (int i = 0; i < list->count(); ++i)
                all.append(i);
            executeRemoval(all);
        }
    } else if (chosen == removeInvalidAction && !dynamic) {
        removeInvalidItems();
    } else if (chosen == removeDupAction && !dynamic) {
        removeDuplicateItems();
    } else if (chosen == ascAction || chosen == descAction) {
        // 切向即时重排：有生效规则立即按新方向执行；无规则仅记录方向
        const bool ascending = (chosen == ascAction);
        if (auto *p = providerAt(tab)) {
            p->setSortRule(p->sortKey(), ascending);
            if (p->sortKey() != int(SortKey::None))
                sortCurrent(SortKey(p->sortKey()), ascending);
        }
    } else {
        // 排序规则命中：按当前方向执行一次并迁移勾选（sortCurrent 落位状态）
        for (int k = 0; k < 8; ++k) {
            if (chosen == sortActions[k]) {
                sortCurrent(static_cast<SortKey>(k + 1), curAsc);
                break;
            }
        }
    }
}

// N9：删除指定 tab（表头菜单，作用于右键命中的 tab）。关闭正在播放的列表：
// 先停播并清空 mpv 侧，同步目标置 -1 悬空，防空列表回刷误清其他 tab；
// 下次双击经 markPlayingTab 重建
void PlaylistWidget::removeTabAt(int tabIndex)
{
    const int idx = tabIndex;
    if (idx <= 0 || idx >= count())
        return;
    if (idx == m_playingTab) {
        emit stopRequested();
        emit clearRequested();
        m_playingTab = -1;
    }
    if (QListWidget *closing = listAt(idx)) {
        m_providerOf.erase(closing); // unique_ptr 自动回收提供者（含排序规则）
        m_memory.remove(closing);
    }
    if (m_playingTab > idx)
        --m_playingTab;
    removeTab(idx);
    savePlaylists();
}

// N9：新建播放列表——弹列表设置对话框，确定后建 tab；动态提供者立即枚举填充
void PlaylistWidget::createNewList()
{
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

    QStringList occupied;
    for (int i = 0; i < count(); ++i)
        occupied << tabText(i);
    ListSettingsDialog dlg(this, tr("播放列表 %1").arg(maxN + 1), nullptr, occupied);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.listName();

    auto *list = createList();
    // 新建必产出提供者（静态为空表、目录即枚举结果）；排序默认按类型施加
    m_providerOf.insert_or_assign(list, std::move(dlg.takeProvider().value()));
    auto *p = m_providerOf[list].get();
    p->setSortRule(isDynamicTab(count() - 1) ? int(SortKey::Modified) : int(SortKey::None),
                   true);
    fillFromProvider(list);
    addTab(list, name);
    setCurrentIndex(count() - 1);
    savePlaylists();
}

// N9：存量列表设置——改名/换提供者。非法参数由对话框校验报错留在窗口；
// 合法后：仅改名直接生效；提供者有变则静默清空重建，作用对象为播放中 tab 时
// 停止并选中首项不播（悬空同步目标防回刷竞态）
void PlaylistWidget::openListSettings(int tabIndex)
{
    if (tabIndex <= 0)
        return;
    QListWidget *list = listAt(tabIndex);
    if (!list)
        return;

    QStringList occupied;
    for (int i = 0; i < count(); ++i) {
        if (i != tabIndex)
            occupied << tabText(i);
    }
    ListSettingsDialog dlg(this, tabText(tabIndex), providerAt(tabIndex), occupied);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.listName();
    auto result = dlg.takeProvider();

    setTabText(tabIndex, name);
    if (!result.has_value()) {
        // 仅改名（对话框判定提供者未变）：内容与提供者不动
        savePlaylists();
        return;
    }

    const bool wasPlaying = (tabIndex == m_playingTab);
    if (wasPlaying) {
        emit stopRequested(); // 纯 stop 清空 mpv 侧列表（AGENTS 陷阱条目）
        m_playingTab = -1;    // 悬空：挡掉随后的空列表回刷误清新填充内容
    }

    m_providerOf[list] = std::move(*result);

    // 原记忆随源失效一并清除；排序规则重置为新提供者类型默认
    m_memory.remove(list);
    if (auto *np = providerAt(tabIndex))
        np->setSortRule(isDynamicTab(tabIndex) ? int(SortKey::Modified) : int(SortKey::None),
                        true);

    list->clear();
    fillFromProvider(list); // 静态取所持条目（切换后为空），目录枚举绑定源
    if (list->count() > 0)
        list->setCurrentRow(0); // 选中第一项，不开始播放
    savePlaylists();
}

#include "pch.h"

#include "PlaylistWidget.h"

#include "Constants.h"
#include "DirProvider.h"
#include "ListSettingsDialog.h"
#include "LoadingOverlay.h"
#include "MediaInfoHelper.h"
#include "MpvController.h"
#include "PlaylistDelegate.h"
#include "StaticProvider.h"
#include "Util.h"

PlaylistWidget::PlaylistWidget(const QString &configFile, QWidget *parent)
    : QTabWidget(parent), m_configFile(configFile) {
    setAcceptDrops(true);
    setDocumentMode(true);
    setMinimumWidth(220);
    auto *defTab = createList();
    addTab(defTab, tr("Default"));
    // N19：默认 tab 内容不走提供者枚举（setDefaultTabFiles/回刷直写），恒视为已加载，
    // 否则 sortCurrent 的 entriesLoaded 守卫会误拦默认 tab 的元数据排序
    m_loadState[defTab].entriesLoaded = true;
    connect(this, &QTabWidget::currentChanged, this, &PlaylistWidget::onTabChanged);
    // N19：遮罩最后创建并置顶；延迟显示定时器可取消
    m_overlay = new LoadingOverlay(this);
    m_overlay->raise();
    m_overlayTimer.setSingleShot(true);
    connect(&m_overlayTimer, &QTimer::timeout, this,
            [this] { m_overlay->begin(m_overlayMode, m_overlayTotal); });
    connect(m_overlay, &LoadingOverlay::cancelRequested, this,
            [this] { m_metaCancel = true; });
}

// N19：在途枚举线程收尾。先置存活旗（工作线程检查后不再投递收尾），再 wait；
// 超时放弃后即便线程仍在跑，也只会看到存活旗为 false 而直接返回，不触碰任何成员。
// 预读为主线程循环，随析构自然终止
PlaylistWidget::~PlaylistWidget() {
    *m_alive = false;
    if (m_loadThread && m_loadThread->isRunning())
        m_loadThread->wait(2000);
}

QListWidget *PlaylistWidget::createList() {
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

QListWidget *PlaylistWidget::listAt(int index) const {
    return qobject_cast<QListWidget *>(widget(index));
}

// N5：列表持焦时认领全部导航键，抑制全局音量动作（↑/↓）；
// 认领集合含 ←→/PgUp/PgDn/Home/End，为后续列表键盘功能预留。
// accept 后按键作为普通键盘事件送达列表；过滤器须返回 false 放行事件本身
bool PlaylistWidget::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::ShortcutOverride && qobject_cast<QListWidget *>(watched)) {
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

PlaylistItem &PlaylistWidget::materialize(const QString &path) {
    // 仓键统一规范化；path 约定为入口处已加工的显示路径（Qt 源经 nativePath、mpv 源经
    // resolveLocalPath），原样存入 item.path 供 UI/回传 mpv/持久化，不再二次改写，
    // 以免网络路径被 toNativeSeparators 破坏。
    const QString key = Util::normalizePathForMatch(path);
    auto it = m_meta.find(key);
    if (it != m_meta.end())
        return it.value();

    PlaylistItem item;
    item.path = path;
    const QFileInfo fi(path);
    if (Util::isNetworkStreamPath(path) || !fi.exists()) {
        item.icon = m_iconProvider.icon(QFileIconProvider::File);
    } else {
        item.fileSize = qMax<qint64>(0, fi.size());
        item.created = fi.birthTime();
        item.modified = fi.lastModified();
        item.icon = m_iconProvider.icon(fi);
    }

    return m_meta.insert(key, item).value();
}

void PlaylistWidget::materializeList(QListWidget *list) {
    if (!list)
        return;
    for (int i = 0; i < list->count(); ++i) {
        if (isPlaceholderRow(list->item(i)))
            continue; // N19：占位行无路径，不入元数据仓
        materialize(list->item(i)->data(PathRole).toString());
    }
}

// 统一的条目构造：显示文件名（含扩展名）、图标取自仓、tooltip 为原始路径。
// PathRole 存显示路径原样（来源处已加工，用于 UI/回传 mpv/持久化）；
// 内部匹配键经 normalizePathForMatch 规范化后索引 m_meta 仓。
QListWidgetItem *PlaylistWidget::makeItem(QListWidget *list, const QString &rawPath) {
    const PlaylistItem &meta = materialize(rawPath);
    auto *item = new QListWidgetItem(list);
    item->setData(Qt::DisplayRole, QFileInfo(meta.path).fileName());
    item->setData(Qt::DecorationRole, meta.icon);
    item->setData(PathRole, rawPath);
    item->setToolTip(meta.path);
    return item;
}

QStringList PlaylistWidget::filesOf(int tabIndex) const {
    QStringList files;
    if (QListWidget *list = listAt(tabIndex)) {
        for (int i = 0; i < list->count(); ++i) {
            if (isPlaceholderRow(list->item(i)))
                continue; // N19：占位行不是可播文件
            files.append(list->item(i)->data(PathRole).toString());
        }
    }
    return files;
}

int PlaylistWidget::playingRowOf(const QListWidget *list) const {
    if (!list)
        return -1;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(PlayingRole).toBool())
            return i;
    }
    return -1;
}

void PlaylistWidget::setPlaylist(const QVariantList &items) {
    QListWidget *list = listAt(m_playingTab);
    if (!list)
        return;

    // N19：回刷为权威内容——占位存在即视为变化，重建时一并清除
    const bool hadPlaceholder = clearPlaceholder(list) || !m_loadState[list].loadError.isEmpty();
    m_loadState[list].loadError.clear();

    // N19：先比对，完全一致静默忽略（无重建、无闪烁、无选中丢失）
    bool identical = !hadPlaceholder && list->count() == items.size();
    if (identical) {
        for (int i = 0; i < list->count(); ++i) {
            const QString disk =
                Util::resolveLocalPath(items[i].toMap().value(QStringLiteral("filename")).toString());
            if (list->item(i)->data(PathRole).toString() != disk) {
                identical = false;
                break;
            }
        }
        if (identical) {
            // 播放标记跟随 current 变化：只动翻转的行（防整表 dataChanged 重绘），
            // 正播行变化才迁移选中+滚动（与旧整体重建的可见行为一致）
            const int oldPlaying = playingRowOf(list);
            int newPlaying = -1;
            for (int i = 0; i < list->count(); ++i) {
                const bool cur = items[i].toMap().value(QStringLiteral("current")).toBool();
                if (list->item(i)->data(PlayingRole).toBool() != cur)
                    list->item(i)->setData(PlayingRole, cur);
                if (cur)
                    newPlaying = i;
            }
            if (newPlaying >= 0 && newPlaying != oldPlaying) {
                list->setCurrentItem(list->item(newPlaying));
                list->scrollToItem(list->item(newPlaying), QListWidget::EnsureVisible);
            }
            return;
        }
    }

    // 记录重建前的选中行（单选）：停止后 mpv 列表无 current 项，
    // 不恢复的话整体重建会把界面上的选中清空（仅显示问题）
    const int prevRow = list->currentRow();
    list->clear();
    // N19：内容真变，已读元数据不再可信；仅同 tab 在途加载需丢弃收尾
    m_loadState[list].mediaInfoLoaded = false;
    if (list == m_loadingList)
        ++m_loadGen;

    QListWidgetItem *newCurrent = nullptr;
    for (const QVariant &v : items) {
        const QVariantMap map = v.toMap();
        const QString filename = map.value(QStringLiteral("filename")).toString();
        // mpv 来源路径：本地 cleanPath+原生分隔符，网络/流保持原样（B15）
        QListWidgetItem *item = makeItem(list, Util::resolveLocalPath(filename));
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

void PlaylistWidget::addFiles(const QStringList &files) {
    QListWidget *list = listAt(currentIndex());
    if (!list || files.isEmpty() || m_busy)
        return; // N19：忙时不接受手工增补（预读快照防污染）

    // N9：动态 tab 内容来自枚举，忽略手工添加（拖放与菜单同守卫）。
    // 静态 tab 增删即回无序态：复位排序规则（保留方向）
    if (isDynamicTab(currentIndex()))
        return;
    if (auto *p = providerAt(currentIndex()))
        p->setSortRule(int(SortKey::None), p->sortAscending());
    m_loadState[list].mediaInfoLoaded = false; // N19：条目集合变化，已读元数据不可信

    // 正在播放 tab 的内容以 mpv 回刷为准，本地不重复插入（C4：消除双刷新闪烁）
    if (!currentIsPlayingTab()) {
        for (const QString &file : files)
            makeItem(list, file);
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
void PlaylistWidget::setDefaultTabFiles(const QStringList &files) {
    QListWidget *list = listAt(0);
    if (!list || files.isEmpty())
        return;
    m_memory.remove(list);
    list->clear();
    for (const QString &file : files)
        makeItem(list, file);
}

// ——元数据回填（N4）：按路径覆写仓中字段并触发相关行重绘——

void PlaylistWidget::repaintAllLists() {
    for (int i = 0; i < count(); ++i) {
        if (QListWidget *l = listAt(i))
            l->viewport()->update();
    }
}

// B18：读取失败（seconds<=0）即清空该条目时长——N18 后本函数是 PlaylistItem
// 唯一保留的 mpv 写入点；卸载路径不发 0.0（PropDuration 跳过 NONE），
// 档案不会因此被误清
void PlaylistWidget::applyDuration(const QString &rawPath, double seconds) {
    if (rawPath.isEmpty())
        return;
    if (seconds > 0)
        materialize(rawPath).duration = seconds;
    else
        materialize(rawPath).duration.reset();
    repaintAllLists();
}

// N18：MediaInfo 缓存惰性查询。dll 不可用返 nullptr；命中即返回，
// 未命中则同步 parse 一次写入缓存（含解析失败写空 Data 防重解析同坏文件）。
// 调用点仅限排序比较器与 showPropertiesDialog 入口，禁止进入绘制路径
const MediaInfoHelper::Data *PlaylistWidget::mediaInfoOf(const QString &path) {
    if (!MediaInfoHelper::available())
        return nullptr;
    const QString key = Util::normalizePathForMatch(path);
    auto it = m_mediaInfo.find(key);
    if (it == m_mediaInfo.end()) {
        MediaInfoHelper::Data parsed = MediaInfoHelper::parse(path);
        // N18：未播放文件首次解析时把 MediaInfo 的总时长回填到 PlaylistItem，
        // 让列表"总时长"列立即显示（不必等 mpv 播过）；mpv 后续写入优先
        PlaylistItem &item = materialize(path);
        bool filledDuration = false;
        if (parsed.duration.has_value() && !item.duration.has_value()) {
            item.duration = *parsed.duration;
            filledDuration = true;
        }
        it = m_mediaInfo.insert(key, parsed);
        // N19：预读期间抑制逐条重绘，结束后统一一次
        if (filledDuration && !m_scanSuppressRepaint)
            repaintAllLists();
    }
    return &it.value();
}

// ——续播记忆三步维护法（N4）——

QString PlaylistWidget::captureMemoryPath(QListWidget *list) const {
    const TabMemory mem = m_memory.value(list);
    if (mem.lastPos < 0 || mem.lastPos >= list->count())
        return {};
    return list->item(mem.lastPos)->data(PathRole).toString();
}

void PlaylistWidget::restoreMemoryPath(QListWidget *list, const QString &captured, bool hadMemory) {
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

void PlaylistWidget::loadPlaylists() {
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
        if (m_providerOf[list]->type() == PlaylistProvider::Type::Dir) {
            m_loadState[list].entriesLoaded = false; // N19：动态 tab 延迟到首次激活
        } else {
            fillFromProvider(list); // 静态取所持条目
            m_loadState[list].entriesLoaded = true;
        }
    }
}

void PlaylistWidget::savePlaylists() {
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

void PlaylistWidget::recordPlayback(int tabIndex, int pos, double seconds) {
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

void PlaylistWidget::clearResumeProgress(int tabIndex) {
    QListWidget *list = listAt(tabIndex);
    if (!list)
        return;
    auto it = m_memory.find(list);
    if (it != m_memory.end())
        it.value().lastTimePos = -1.0;
}

int PlaylistWidget::resolveResumeRow(int tabIndex) const {
    const QListWidget *list = listAt(tabIndex);
    if (!list)
        return -1;
    const TabMemory mem = m_memory.value(list);
    if (mem.lastPos < 0 && mem.lastPath.isEmpty())
        return -1;
    // ① 序号处路径校验：一致则信任序号（含重复项场景的精确恢复）
    if (mem.lastPos >= 0 && mem.lastPos < list->count() &&
        list->item(mem.lastPos)->data(PathRole).toString() == mem.lastPath)
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

double PlaylistWidget::lastTimePosOf(int tabIndex) const {
    const QListWidget *list = listAt(tabIndex);
    return list ? m_memory.value(list).lastTimePos : -1.0;
}

int PlaylistWidget::indexOfName(const QString &name) const {
    for (int i = 0; i < count(); ++i) {
        if (tabText(i) == name)
            return i;
    }
    return -1;
}

void PlaylistWidget::onTabChanged(int index) {
    if (index < 0)
        return;
    // N19：未加载的动态 tab 走首次激活链（选中/滚动在 finishLoad 收尾）
    if (QListWidget *fresh = listAt(index);
        fresh && !m_loadState[fresh].entriesLoaded && isDynamicTab(index)) {
        startLoadAsync(fresh);
        return;
    }
    // N19：已加载但元数据失效（他 tab 刷新清全局缓存/上次预读取消/回刷变更），
    // 且本 tab 排序依赖元数据 → 切入即重读后排序（含取消后切入重读语义）
    if (QListWidget *cur = listAt(index); cur && needsMetaRescan(index)) {
        reactivateRescan(cur, index);
        return;
    }
    activateTab(index);
}

bool PlaylistWidget::needsMetaRescan(int tabIndex) const {
    if (m_busy || !MediaInfoHelper::available())
        return false;
    const QListWidget *list = listAt(tabIndex);
    const TabLoadState st = m_loadState.value(list);
    if (!list || !st.entriesLoaded || st.mediaInfoLoaded || !st.loadError.isEmpty())
        return false; // 占位 tab（loadError 非空）走失败重试，不走预读
    const PlaylistProvider *p = providerAt(tabIndex);
    if (!p)
        return false;
    const int k = p->sortKey();
    return k == int(SortKey::Duration) || k == int(SortKey::ReleaseDate);
}

void PlaylistWidget::reactivateRescan(QListWidget *list, int tabIndex) {
    if (!list)
        return;
    setBusy(true);
    const bool ok = runMetaPrescan(list);
    if (ok) {
        if (const PlaylistProvider *p = providerAt(tabIndex))
            applySort(list, SortKey(p->sortKey()), p->sortAscending(), false);
    }
    // 选中跟随最终顺序（播放 tab 由 mpv 驱动选中，此处只滚不动选）
    if (tabIndex == currentIndex() && tabIndex != m_playingTab) {
        if (const int row = resolveResumeRow(tabIndex); row >= 0 && row < list->count())
            list->setCurrentRow(row);
    }
    if (tabIndex == currentIndex() && tabIndex == m_playingTab)
        activateTab(tabIndex); // 正播行滚动
    setBusy(false);
}

// N19：原 onTabChanged 体。切入正在播放的列表：居中滚动到正播项。
// 选中态由 mpv 同步高亮驱动，此处只滚不改选
void PlaylistWidget::activateTab(int index) {
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

int PlaylistWidget::currentSelectedRow() const {
    const QListWidget *list = listAt(currentIndex());
    if (!list || list->selectedItems().isEmpty())
        return -1;
    return list->currentRow();
}

// B10：同步目标转移。同 tab 早退（播放中双击本 tab 条目时防标记闪烁）；
// 异 tab 转移先清空原 tab 全部行的播放标记——setPlaylist 仅回刷新播放 tab，
// 旧 tab 标记若不在此清理将永久残留
void PlaylistWidget::markPlayingTab(int tabIndex) {
    if (tabIndex == m_playingTab)
        return;
    if (QListWidget *old = listAt(m_playingTab)) {
        for (int i = 0; i < old->count(); ++i)
            old->item(i)->setData(PlayingRole, false);
    }
    m_playingTab = tabIndex;
}

void PlaylistWidget::onItemDoubleClicked(QListWidgetItem *item) {
    if (!item || isPlaceholderRow(item))
        return; // N19：失败占位行不可播放
    QListWidget *list = item->listWidget();
    const int tab = indexOf(list);
    if (!list || tab < 0)
        return;

    markPlayingTab(tab);
    emit playRequested(filesOf(tab), list->row(item), -1.0);
}

// N19：程序化切 tab 统一入口。忙时目标非当前则排队，空闲后补执行
void PlaylistWidget::requestTab(int index) {
    if (index < 0 || index >= count())
        return;
    if (!m_busy || index == currentIndex()) {
        setCurrentIndex(index);
        return;
    }
    m_pendingTab = index; // last-wins
}

void PlaylistWidget::flushPendingTab() {
    if (m_pendingTab < 0 || m_pendingTab >= count()) {
        m_pendingTab = -1;
        return;
    }
    const int idx = m_pendingTab;
    m_pendingTab = -1;
    setCurrentIndex(idx);
}

void PlaylistWidget::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void PlaylistWidget::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void PlaylistWidget::dropEvent(QDropEvent *event) {
    QStringList files;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            files.append(Util::nativePath(url.toLocalFile()));
    }
    if (!files.isEmpty()) {
        addFiles(files);
        event->acceptProposedAction();
    }
}

// ——N9 提供者——

PlaylistProvider *PlaylistWidget::providerAt(int tabIndex) const {
    const auto it = m_providerOf.find(listAt(tabIndex));
    return it == m_providerOf.end() ? nullptr : it->second.get();
}

bool PlaylistWidget::isDynamicTab(int tabIndex) const {
    auto *p = providerAt(tabIndex);
    return p && p->type() == PlaylistProvider::Type::Dir;
}

// 同步填充（静态 tab 路径）：enumerate 解包后走统一填充
void PlaylistWidget::fillFromProvider(QListWidget *list) {
    if (!list)
        return;
    const auto it = m_providerOf.find(list);
    if (it == m_providerOf.end())
        return;
    if (const auto r = it->second->enumerate(); r.has_value())
        fillListFromEntries(list, r.value());
    else
        showPlaceholder(list, r.error());
}

// N19：GUI 线程填充——清空 → 逐条 materialize + makeItem → 应用排序规则。
// 元数据键（Duration/ReleaseDate）且 MediaInfo 可用时跳过重排，由预读后的
// applySort 执行（否则比较器会在此全表同步解析）
void PlaylistWidget::fillListFromEntries(QListWidget *list, const QStringList &entries) {
    if (!list)
        return;
    list->clear();
    for (const QString &path : entries)
        makeItem(list, path);
    const auto it = m_providerOf.find(list);
    if (it == m_providerOf.end())
        return;
    const auto key = SortKey(it->second->sortKey());
    if (key == SortKey::None)
        return;
    if ((key == SortKey::Duration || key == SortKey::ReleaseDate) && MediaInfoHelper::available())
        return; // 预读链收尾排序
    sortListItems(list, key, it->second->sortAscending());
}

// N19：动态 tab 首次激活——快照参数进线程，不碰 provider 对象。
// 跨线程只带裸指针值 + 共享存活旗：QPointer（含其拷贝）在工作线程读写是数据竞争，
// 有效性一律由 GUI 收尾按 m_loadingList（仅 GUI 线程读写）+ m_loadGen 判定
void PlaylistWidget::startLoadAsync(QListWidget *list) {
    if (!list)
        return;
    QString root;
    bool recursive = false;
    QStringList exts;
    if (const auto it = m_providerOf.find(list);
        it != m_providerOf.end() && it->second->type() == PlaylistProvider::Type::Dir) {
        const auto *d = static_cast<const DirProvider *>(it->second.get());
        root = d->rootPath();
        recursive = d->recursive();
        exts = d->extensions();
    } else {
        return; // 防御：非目录源不走线程（调用方均已判定，此为兜底）
    }
    clearPlaceholder(list); // 失败重试先清占位
    PlaylistWidget *rawSelf = this;
    QListWidget *rawList = list;
    auto alive = m_alive;
    const quint64 gen = ++m_loadGen;
    m_loadingList = list;
    setBusy(true);
    armOverlay(LoadingOverlay::Mode::Loading, 0);
    auto *thread = QThread::create([rawSelf, rawList, root, recursive, exts, gen, alive] {
        std::expected<QStringList, QString> r =
            DirProvider::enumerateDir(root, recursive, exts); // 工作线程：仅此一步
        if (!alive->load())
            return; // 析构已开始，不触碰任何成员
        QMetaObject::invokeMethod(
            rawSelf,
            [rawSelf, rawList, r, gen] {
                // 上下文机制：rawSelf 若在投递前销毁，本调用被 Qt 丢弃；
                // 旧批次或目标已换（删 tab/同 tab 回刷重建）即弃
                if (gen != rawSelf->m_loadGen || rawSelf->m_loadingList != rawList)
                    return;
                rawSelf->finishLoad(rawList, r);
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread] {
        if (m_loadThread == thread)
            m_loadThread = nullptr;
    });
    m_loadThread = thread;
    thread->start();
}

// N19：异步收尾（GUI 线程）——成功填充 + 记忆恢复 + 按需预读 + 排序；
// 失败占位并保持 entriesLoaded=false（下次切入重试）
void PlaylistWidget::finishLoad(QListWidget *list, const std::expected<QStringList, QString> &r) {
    // m_loadingList 刻意保留到各出口：预读期的 processEvents 可能派发同 tab 回刷重建，
    // 届时 setPlaylist 递增 generation，此处二次校验才能发现（入口处已由调用方验过一次）
    PendingLoad pend;
    if (m_pendingLoad.list == list) {
        pend = m_pendingLoad;
        m_pendingLoad = PendingLoad{};
    }
    const quint64 genAtEntry = m_loadGen;
    if (!list) {
        m_loadingList = nullptr;
        setBusy(false);
        return;
    }
    clearPlaceholder(list);
    if (!r.has_value()) {
        m_loadState[list].loadError = r.error();
        showPlaceholder(list, r.error());
        m_loadingList = nullptr;
        emit contentsChanged(); // 占位行计入行数，导航启停需刷新
        setBusy(false);
        return;
    }
    TabLoadState &st = m_loadState[list];
    st.loadError.clear();
    st.entriesLoaded = true;
    // 刷新场景：重建前抄录记忆行（首载空表跳过，防 INI 记忆被空快照误清）
    const bool hadRows = list->count() > 0;
    const QString captured = hadRows ? captureMemoryPath(list) : QString{};
    const bool hadMem =
        hadRows && m_memory.contains(list) && m_memory.value(list).lastPos >= 0;
    fillListFromEntries(list, r.value());

    const int tab = indexOf(list);
    if (pend.selectFirst) {
        if (list->count() > 0)
            list->setCurrentRow(0); // 列表设置换源：选中首项，不播放
    } else if (tab >= 0 && tab == currentIndex()) {
        activateTab(tab); // 记忆行选中/播放行滚动（原 onTabChanged 逻辑）
    }

    // 按需预读 + 排序（规则已随提供者持久化，此处不重写）
    if (tab >= 0) {
        if (const auto *p = providerAt(tab)) {
            const auto key = SortKey(p->sortKey());
            const bool needScan = MediaInfoHelper::available() && !st.mediaInfoLoaded &&
                                  (key == SortKey::Duration || key == SortKey::ReleaseDate);
            if (needScan) {
                const bool ok = runMetaPrescan(list);
                if (genAtEntry != m_loadGen) {
                    // 预读期间同 tab 被回刷重建：行已是权威内容，收尾直接丢弃
                    m_loadingList = nullptr;
                    setBusy(false);
                    return;
                }
                if (!ok) {
                    // 取消：记忆复位后一切不动（含排序规则不写；条目已填充，刷新导航）
                    if (tab >= 0)
                        restoreMemoryPath(list, captured, hadMem);
                    m_loadingList = nullptr;
                    emit contentsChanged();
                    setBusy(false);
                    return;
                }
                applySort(list, key, p->sortAscending(), false);
            }
        }
    }

    // 动态刷新播放中 tab 的递延四步式：预读/排序完成后 stop → 推送恢复
    if (!pend.resumePath.isEmpty()) {
        // 用户可能在加载期间已手动停止/EOF：idle 则不恢复
        const bool idle = m_mpv ? m_mpv->propertyFlag(QStringLiteral("idle-active"), false) : true;
        if (!idle) {
            // 加载期间可能已切曲：优先 mpv 当前实际文件（须仍在新列表中），否则用快照
            QString target = pend.resumePath;
            const QStringList files = filesOf(tab);
            if (m_mpv) {
                const QString normCur =
                    Util::normalizePathForMatch(m_mpv->propertyString(QStringLiteral("path")));
                if (!normCur.isEmpty()) {
                    for (const QString &f : files) {
                        if (Util::normalizePathForMatch(f) == normCur) {
                            target = f;
                            break;
                        }
                    }
                }
            }
            const double resumeSec =
                m_mpv ? m_mpv->propertyDouble(QStringLiteral("time-pos"), -1.0) : -1.0;
            emit stopRequested();
            if (const int idx = files.indexOf(target); idx >= 0)
                emit playRequested(files, idx, resumeSec);
            // 目标已不在新列表：保持停止（与旧同步语义一致）
        }
    } else if (pend.saveAfter && tab > 0) {
        savePlaylists();
    }
    // 记忆复位 + 视图选中跟随最终顺序（首载 INI 记忆 / 刷新快照；播放 tab 选中由 mpv 驱动）
    if (hadMem)
        restoreMemoryPath(list, captured, true);
    if (!pend.selectFirst && tab >= 0 && tab == currentIndex() && tab != m_playingTab) {
        if (const int row = resolveResumeRow(tab); row >= 0 && row < list->count())
            list->setCurrentRow(row);
    }

    m_loadingList = nullptr;
    emit contentsChanged(); // 条目数已变，刷新导航启停（A16）
    setBusy(false);
}

// 条目重排核心：行指针摘除 → 稳定排序 → 回填。无停止/恢复副作用，
// 四步式流程（sortCurrent）与加载填充共用
void PlaylistWidget::sortListItems(QListWidget *list, SortKey key, bool ascending) {
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
            // N18：改读 MediaInfo 缓存；dll 缺席/未读到按"未知"处理
            const MediaInfoHelper::Data *ma = mediaInfoOf(pa);
            const MediaInfoHelper::Data *mb = mediaInfoOf(pb);
            const bool ha = ma && (key == SortKey::Duration ? ma->duration.has_value()
                                                            : ma->recordedDate.isValid());
            const bool hb = mb && (key == SortKey::Duration ? mb->duration.has_value()
                                                            : mb->recordedDate.isValid());
            if (!ha || !hb)
                return ha && !hb; // 已知在前、未知在后；双未知视为等键
            if (key == SortKey::Duration) {
                return flip(
                    *ma->duration < *mb->duration ? -1 : (*mb->duration < *ma->duration ? 1 : 0));
            }
            return flip(ma->recordedDate < mb->recordedDate
                            ? -1
                            : (mb->recordedDate < ma->recordedDate ? 1 : 0));
        }
        }
        return false;
    };

    QVector<int> order(n);
    for (int i = 0; i < n; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return lessByPath(x, y); });

    QList<QListWidgetItem *> taken;
    taken.reserve(n);
    for (int i = 0; i < n; ++i)
        taken.append(list->takeItem(0)); // 逐个摘除，转移所有权
    for (int idx : order)
        list->addItem(taken.at(idx));
}

// ——排序（N4 第五节）——

void PlaylistWidget::sortCurrent(SortKey key, bool ascending) {
    QListWidget *list = listAt(currentIndex());
    const int tab = currentIndex();
    // N19：忙时（菜单已禁用，此为兜底）与未加载/占位 tab 不排序
    if (!list || tab < 0 || key == SortKey::None || m_busy)
        return;
    if (!m_loadState[list].entriesLoaded)
        return;

    // N19：元数据键走预读流程——先全表预读再排序，取消则规则不写、一切不动
    if (MediaInfoHelper::available() &&
        (key == SortKey::Duration || key == SortKey::ReleaseDate)) {
        setBusy(true); // 忙时即重入守卫（预读期菜单/tab 栏已冻结）
        const bool ok = runMetaPrescan(list);
        if (!ok) {
            setBusy(false); // 取消：恢复启用、隐藏遮罩、一切不动
            return;
        }
        applySort(list, key, ascending, true);
        setBusy(false);
        return;
    }

    applySort(list, key, ascending, true);
}

// N19：预读公共骨架（不碰 m_busy，调用方负责 setBusy 包裹）
bool PlaylistWidget::runMetaPrescan(QListWidget *list) {
    if (!list)
        return false;
    armOverlay(LoadingOverlay::Mode::MetaScan, list->count());
    m_metaCancel = false;
    m_scanSuppressRepaint = true;
    const bool ok = runMetaScan(list, list->count());
    m_scanSuppressRepaint = false;
    repaintAllLists();
    if (ok)
        m_loadState[list].mediaInfoLoaded = true;
    return ok;
}

// N19：排序执行体（无预读）——规则落位 + 非播放直排 / 播放四步式
void PlaylistWidget::applySort(QListWidget *list, SortKey key, bool ascending, bool writeRule) {
    if (!list)
        return;
    const int tab = indexOf(list);
    if (tab < 0)
        return;

    // N9：排序规则落位于提供者（随其持久化）
    if (writeRule) {
        if (auto *p = providerAt(tab))
            p->setSortRule(int(key), ascending);
    }
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

// N19：主线程元数据预读循环。total = 条目总数；返回 false = 用户取消或销毁
bool PlaylistWidget::runMetaScan(QPointer<QListWidget> list, int total) {
    QPointer<PlaylistWidget> self(this);
    const QStringList paths = allPathsOf(list); // 快照：循环期间行重建不影响
    // 起点 = 缓存命中数（命中不计 IO 但计入分子，进度不跳变）
    m_scanDone = 0;
    for (const QString &path : paths) {
        if (m_mediaInfo.contains(Util::normalizePathForMatch(path)))
            ++m_scanDone;
    }
    m_overlay->setProgress(m_scanDone, total);
    QElapsedTimer throttle;
    throttle.start();
    for (const QString &path : paths) {
        if (!self || !list)
            return false;
        const bool hit = m_mediaInfo.contains(Util::normalizePathForMatch(path));
        mediaInfoOf(path); // 未命中即同步 parse 一次入缓存（含失败防重解析）
        if (hit)
            continue;
        ++m_scanDone;
        if (throttle.elapsed() < 50)
            continue; // 节流：每 ~50ms 刷新一次进度
        throttle.restart();
        m_overlay->setProgress(m_scanDone, total);
        QCoreApplication::processEvents(); // 驱动遮罩重绘、进度、取消按钮、延迟定时器
        if (m_metaCancel)
            return false;
    }
    m_overlay->setProgress(total, total);
    if (!self || !list || m_metaCancel)
        return false;
    return true;
}

QStringList PlaylistWidget::allPathsOf(QListWidget *list) const {
    QStringList paths;
    if (!list)
        return paths;
    for (int i = 0; i < list->count(); ++i) {
        if (isPlaceholderRow(list->item(i)))
            continue;
        paths.append(list->item(i)->data(PathRole).toString());
    }
    return paths;
}

// ——清理操作（N4 第六节）——

void PlaylistWidget::executeRemoval(const QList<int> &rows) {
    QListWidget *list = listAt(currentIndex());
    const int tab = currentIndex();
    if (!list || rows.isEmpty() || tab < 0 || m_busy)
        return; // N19：忙时菜单已禁用，此为兜底

    m_loadState[list].mediaInfoLoaded = false; // N19：条目集合变化
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
    emit contentsChanged(); // A17：本地删/清条目数已变，刷新导航启停
}

void PlaylistWidget::removeInvalidItems() {
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

void PlaylistWidget::removeDuplicateItems() {
    QListWidget *list = listAt(currentIndex());
    if (!list)
        return;
    QSet<QString> seen;
    QList<int> rows;
    for (int i = 0; i < list->count(); ++i) {
        const QString key = Util::normalizePathForMatch(list->item(i)->data(PathRole).toString());
        if (!seen.contains(key))
            seen.insert(key);
        else
            rows.append(i); // 保留首现，其余为待删重复
    }
    executeRemoval(rows);
}

// ——右键菜单动作（N4 第七节）——

void PlaylistWidget::refreshCurrentTab() {
    QListWidget *list = listAt(currentIndex());
    if (!list || m_busy)
        return; // N19：忙时菜单已禁用，此为兜底
    // N19：动态 tab 刷新＝异步重枚举；播放中 tab 的停止/恢复递延到收尾
    //（枚举与预读期间不断播，取消路径零副作用）。
    // 任一 tab 刷新都清空全局元数据缓存（静态/动态同规则），依赖元数据的各 tab
    // 下次切入重读
    if (isDynamicTab(currentIndex())) {
        clearGlobalMeta();
        TabLoadState &st = m_loadState[list];
        st.entriesLoaded = false;
        st.loadError.clear();
        m_pendingLoad = PendingLoad{};
        m_pendingLoad.list = list;
        if (currentIndex() == m_playingTab) {
            // 收尾 fill 会清 PlayingRole 标记，发起时（行还在）抄录正播路径
            if (const int pr = playingRowOf(list); pr >= 0)
                m_pendingLoad.resumePath = list->item(pr)->data(PathRole).toString();
        } else if (currentIndex() != 0) {
            m_pendingLoad.saveAfter = true;
        }
        startLoadAsync(list);
        return;
    }

    clearGlobalMeta(); // N19：任一 tab 刷新都清空全局缓存（N18 遗留遗漏一并补清）
    materializeList(list); // 当前 tab 立即重新物化（图标/大小/时间自取）
    repaintAllLists();     // 其他 tab 待访问时惰性补齐
    // 统一重排：按持久化规则重排（None 跳过；播放中 tab 走四步式保续播，与手动排序一致）
    if (const PlaylistProvider *p = providerAt(currentIndex())) {
        const auto key = SortKey(p->sortKey());
        if (key != SortKey::None) {
            if ((key == SortKey::Duration || key == SortKey::ReleaseDate) &&
                MediaInfoHelper::available()) {
                // 元数据键自动重读+重排（可取消，取消则时长列空、规则保留）
                if (needsMetaRescan(currentIndex()))
                    reactivateRescan(list, currentIndex());
            } else {
                applySort(list, key, p->sortAscending(), false);
                // 选中跟随最终顺序（播放 tab 只滚不动选）
                if (currentIndex() != m_playingTab) {
                    if (const int row = resolveResumeRow(currentIndex());
                        row >= 0 && row < list->count())
                        list->setCurrentRow(row);
                } else {
                    activateTab(currentIndex());
                }
            }
        }
    }
}

// N19：全局元数据缓存清空 + 所有 tab 已读标志重置。任一 tab 刷新共用；
// 排序依赖元数据的各 tab 下次切入经 needsMetaRescan 重读
void PlaylistWidget::clearGlobalMeta() {
    m_meta.clear();      // 清空全局仓
    m_mediaInfo.clear(); // 补清 MediaInfo 缓存
    for (auto it = m_loadState.begin(); it != m_loadState.end(); ++it)
        it.value().mediaInfoLoaded = false;
}

// ——N19 忙时与遮罩——

void PlaylistWidget::setBusy(bool on) {
    m_busy = on;
    setListsEnabled(!on);
    if (!on) {
        m_overlayTimer.stop();
        m_overlay->finish();
        flushPendingTab();
    }
}

void PlaylistWidget::setListsEnabled(bool on) {
    tabBar()->setEnabled(on); // protected 成员，子类可用
    for (int i = 0; i < count(); ++i)
        if (QListWidget *l = listAt(i))
            l->setEnabled(on);
}

void PlaylistWidget::armOverlay(LoadingOverlay::Mode mode, int total) {
    m_overlayMode = mode;
    m_overlayTotal = total;
    if (m_overlay->isVisible())
        m_overlay->begin(mode, total); // 加载转预读：直接切换三行
    else if (!m_overlayTimer.isActive())
        m_overlayTimer.start(300);
}

void PlaylistWidget::resizeEvent(QResizeEvent *event) {
    QTabWidget::resizeEvent(event);
    if (m_overlay)
        m_overlay->setGeometry(rect()); // 遮罩贴合播放列表全域（含 tab 栏）
}

// ——N19 失败占位——

bool PlaylistWidget::isPlaceholderRow(const QListWidgetItem *item) {
    return item && item->data(PlaceholderRole).toBool();
}

void PlaylistWidget::showPlaceholder(QListWidget *list, const QString &error) {
    if (!list)
        return;
    clearPlaceholder(list);
    auto *item = new QListWidgetItem(list);
    item->setData(Qt::DisplayRole, tr("<Load failed>"));
    item->setData(PathRole, QString());
    item->setData(PlaceholderRole, true);
    item->setToolTip(error);
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable); // 不可选中，防选中态残留
}

bool PlaylistWidget::clearPlaceholder(QListWidget *list) {
    if (!list)
        return false;
    bool removed = false;
    for (int i = list->count() - 1; i >= 0; --i) {
        if (isPlaceholderRow(list->item(i))) {
            delete list->takeItem(i);
            removed = true;
        }
    }
    return removed;
}

void PlaylistWidget::copyPathToClipboard(const QString &path) {
    QApplication::clipboard()->setText(path);
}

void PlaylistWidget::showPropertiesDialog(const QString &path) {
    if (path.isEmpty())
        return;
    // N18：N4 阶段 m_meta 仅剩文件系统固有属性 + duration；本对话框
    // 文件系统四行（路径 / 大小 / 创建 / 修改）读 PlaylistItem，
    // 其余媒体字段（格式 / 总时长 / 视频 / 音频 / 字幕 / 标签 / 章节）
    // 改读 MediaInfo 缓存——本函数首部即调 mediaInfoOf 触发惰性解析。
    // 单 Get 5s 超时由 MediaInfoHelper 守卫；二次访问走缓存
    const PlaylistItem &item = materialize(path);
    const MediaInfoHelper::Data *mi = mediaInfoOf(path);

    auto timeText = [](const QDateTime &dt) {
        return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString();
    };

    struct Row {
        QString prop;
        QString value;
    };

    // 只读文本框呈现（维护者裁决）：'\t' 制表对齐两列、整篇可划选复制；
    // 不自动换行，超宽由水平滚动条承接。分组以小号灰色标题行区隔
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Properties"));
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
    QTextCharFormat headFmt; // 分组标题：浅灰
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

    // ——N18 格式化辅助：空格版（与旧版同形态，UI 不变）——
    auto formatKHzSpaced = [](int sr) -> QString {
        QString s = QString::number(sr / 1000.0, 'f', 1);
        if (s.contains(QLatin1Char('.'))) {
            while (s.endsWith(QLatin1Char('0')))
                s.chop(1);
            if (s.endsWith(QLatin1Char('.')))
                s.chop(1);
        }
        return s + QStringLiteral(" kHz");
    };
    auto formatFpsSpaced = [](double fps) -> QString {
        QString s = QString::number(fps, 'f', 2);
        if (s.contains(QLatin1Char('.'))) {
            while (s.endsWith(QLatin1Char('0')))
                s.chop(1);
            if (s.endsWith(QLatin1Char('.')))
                s.chop(1);
        }
        return s + QStringLiteral(" fps");
    };
    auto formatKbpsSpaced = [](qint64 bps) -> QString {
        return QString::number((bps + 500) / 1000) + QStringLiteral(" kbps");
    };
    auto formatHMS = [](double s) -> QString {
        return QStringLiteral("%1:%2:%3")
            .arg(qint64(s) / 3600)
            .arg((qint64(s) % 3600) / 60, 2, 10, QLatin1Char('0'))
            .arg(qint64(s) % 60, 2, 10, QLatin1Char('0'));
    };

    // ——常规组：路径 / 大小 / 创建 / 修改 走 PlaylistItem（文件系统）；
    // 文件格式 / 总时长 走 MediaInfo
    QList<Row> general;
    general.append({tr("File Path"), item.path.isEmpty() ? path : item.path});
    if (mi && !mi->format.isEmpty())
        general.append({tr("Format"), mi->format});
    if (item.fileSize > 0)
        general.append({tr("File Size"), Util::formatFileSize(item.fileSize)});
    if (timeText(item.created).size())
        general.append({tr("Created"), timeText(item.created)});
    if (timeText(item.modified).size())
        general.append({tr("Modified"), timeText(item.modified)});
    if (mi && mi->duration.has_value())
        general.append({tr("Duration"), formatHMS(*mi->duration)});

    // 流"大小"格式化："1.23 GB (82%)"，百分比相对 mi->fileSize
    auto formatStreamSize = [&](qint64 sz, qint64 filesize) -> QString {
        if (sz <= 0)
            return {};
        const QString sizeStr = Util::formatFileSize(sz);
        if (filesize <= 0)
            return sizeStr;
        const int pct = int(double(sz) / double(filesize) * 100.0 + 0.5);
        return QStringLiteral("%1 (%2%)").arg(sizeStr).arg(pct);
    };

    // ——流分组：依次遍历 MediaInfo::streams，按 kind 分桶
    auto buildVideoRows = [&](const MediaInfoHelper::Stream &t) -> QList<Row> {
        QList<Row> rows;
        if (t.isDefault)
            rows.append({tr("Default"), tr("Yes")});
        if (!t.title.isEmpty())
            rows.append({tr("Title"), t.title});
        if (!t.language.isEmpty())
            rows.append({tr("Language"), t.language});
        if (!t.codec.isEmpty())
            rows.append({tr("Codec"), t.codec});
        if (t.width > 0 && t.height > 0)
            rows.append({tr("Resolution"), QStringLiteral("%1 x %2").arg(t.width).arg(t.height)});
        if (t.frameRate > 0.0)
            rows.append({tr("Frame Rate"), formatFpsSpaced(t.frameRate)});
        if (t.bitrate > 0)
            rows.append({tr("Bitrate"), formatKbpsSpaced(t.bitrate)});
        const QString sizeStr = formatStreamSize(t.streamSize, item.fileSize);
        if (!sizeStr.isEmpty())
            rows.append({tr("Size"), sizeStr});
        return rows;
    };
    auto buildAudioRows = [&](const MediaInfoHelper::Stream &t) -> QList<Row> {
        QList<Row> rows;
        if (t.isDefault)
            rows.append({tr("Default"), tr("Yes")});
        if (!t.title.isEmpty())
            rows.append({tr("Title"), t.title});
        if (!t.language.isEmpty())
            rows.append({tr("Language"), t.language});
        if (!t.codec.isEmpty())
            rows.append({tr("Codec"), t.codec});
        if (t.channelCount > 0)
            rows.append({tr("Channels"), QString::number(t.channelCount)});
        if (t.sampleRate > 0)
            rows.append({tr("Sample Rate"), formatKHzSpaced(t.sampleRate)});
        if (t.bitrate > 0)
            rows.append({tr("Bitrate"), formatKbpsSpaced(t.bitrate)});
        const QString sizeStr = formatStreamSize(t.streamSize, item.fileSize);
        if (!sizeStr.isEmpty())
            rows.append({tr("Size"), sizeStr});
        return rows;
    };
    auto buildTextRows = [&](const MediaInfoHelper::Stream &t) -> QList<Row> {
        QList<Row> rows;
        if (t.isDefault)
            rows.append({tr("Default"), tr("Yes")});
        if (!t.title.isEmpty())
            rows.append({tr("Title"), t.title});
        if (!t.language.isEmpty())
            rows.append({tr("Language"), t.language});
        if (!t.codec.isEmpty())
            rows.append({tr("Codec"), t.codec});
        const QString sizeStr = formatStreamSize(t.streamSize, item.fileSize);
        if (!sizeStr.isEmpty())
            rows.append({tr("Size"), sizeStr});
        return rows;
    };

    QList<QList<Row>> videos, audios, texts;
    if (mi) {
        for (const auto &s : mi->streams) {
            switch (s.kind) {
            case MediaInfoHelper::Stream::Kind::Video:
                videos.append(buildVideoRows(s));
                break;
            case MediaInfoHelper::Stream::Kind::Audio:
                audios.append(buildAudioRows(s));
                break;
            case MediaInfoHelper::Stream::Kind::Text:
                texts.append(buildTextRows(s));
                break;
            default:
                break;
            }
        }
    }

    // ——标签组：标题 / 专辑 / 作者 / 发行时间
    QList<Row> tags;
    if (mi && !mi->title.isEmpty())
        tags.append({tr("Title"), mi->title});
    if (mi && !mi->album.isEmpty())
        tags.append({tr("Album"), mi->album});
    if (mi && !mi->performer.isEmpty())
        tags.append({tr("Author"), mi->performer});
    if (mi && mi->recordedDate.isValid())
        tags.append({tr("Released"), timeText(mi->recordedDate)});

    appendGroup(tr("General"), general);
    for (int i = 0; i < videos.size(); ++i)
        appendGroup(tr("Video #%1").arg(i + 1), videos[i]);
    for (int i = 0; i < audios.size(); ++i)
        appendGroup(tr("Audio #%1").arg(i + 1), audios[i]);
    for (int i = 0; i < texts.size(); ++i)
        appendGroup(tr("Subtitle #%1").arg(i + 1), texts[i]);
    appendGroup(tr("Tags"), tags);

    // ——章节组：每章节一行 `[hh:mm:ss.zzz] 标题`；媒体信息不可用时不显示
    if (mi && !mi->chapters.isEmpty()) {
        if (!cur.atStart())
            cur.insertBlock();
        cur.setCharFormat(headFmt);
        cur.insertText(tr("Chapters"), headFmt);
        cur.insertBlock();
        for (const auto &c : mi->chapters) {
            cur.setCharFormat(valueFmt);
            cur.insertText(QStringLiteral("["));
            cur.setCharFormat(nameFmt);
            cur.insertText(Util::formatTimeHMSms(c.timeSeconds));
            cur.setCharFormat(valueFmt);
            cur.insertText(QStringLiteral("]\t") + c.title);
            cur.insertBlock();
        }
    }

    // 光标归位到文档开头，避免打开时选区残留
    view->setTextCursor(QTextCursor(view->document()));

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->addWidget(view);
    int lineCount = general.size() + videos.size() + audios.size() + texts.size() + tags.size() + 8;
    for (const auto &g : videos)
        lineCount += g.size() + 2;
    for (const auto &g : audios)
        lineCount += g.size() + 2;
    for (const auto &g : texts)
        lineCount += g.size() + 2;
    if (mi && !mi->chapters.isEmpty())
        lineCount += mi->chapters.size() + 1; // 组标题 + 每章节一行
    dialog.resize(520, qMin(lineCount * 22 + 40, 600));
    dialog.exec();
}

void PlaylistWidget::contextMenuEvent(QContextMenuEvent *event) {
    if (m_busy)
        return; // N19：忙时右键菜单禁用（表头/主体同守卫）
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
        QAction *newTabAction = menu.addAction(tr("New Playlist"));
        QAction *closeTabAction = menu.addAction(tr("Delete Current List"));
        QAction *settingsAction = menu.addAction(tr("List Settings…"));
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
    // N19：占位行不是文件——命中即视为空目标（复制/属性/定位全禁用，仅保留刷新与排序）
    if (m_menuTargetList && m_menuTargetRow >= 0 &&
        isPlaceholderRow(m_menuTargetList->item(m_menuTargetRow))) {
        m_menuTargetList = nullptr;
        m_menuTargetRow = -1;
    }
    const QString targetPath =
        m_menuTargetList ? m_menuTargetList->item(m_menuTargetRow)->data(PathRole).toString()
                         : QString();

    QMenu menu(this);

    QAction *addAction = menu.addAction(tr("Add Files…"));
    menu.addSeparator();

    QMenu *sortMenu = menu.addMenu(tr("Sort"));
    QAction *sortActions[8];
    sortActions[0] = sortMenu->addAction(tr("File Name"));
    sortActions[1] = sortMenu->addAction(tr("File Name (Numeric)"));
    sortActions[2] = sortMenu->addAction(tr("Extension"));
    sortActions[3] = sortMenu->addAction(tr("Created"));
    sortActions[4] = sortMenu->addAction(tr("Modified"));
    sortActions[5] = sortMenu->addAction(tr("File Size"));
    sortActions[6] = sortMenu->addAction(tr("Duration"));
    sortActions[7] = sortMenu->addAction(tr("Released"));
    // N18：菜单 action 显式携带 SortKey，解除"菜单数组下标 +1 == 枚举值"的脆弱不变式
    sortActions[0]->setData(int(SortKey::FileName));
    sortActions[1]->setData(int(SortKey::FileNameNumeric));
    sortActions[2]->setData(int(SortKey::Extension));
    sortActions[3]->setData(int(SortKey::Created));
    sortActions[4]->setData(int(SortKey::Modified));
    sortActions[5]->setData(int(SortKey::FileSize));
    sortActions[6]->setData(int(SortKey::Duration));
    sortActions[7]->setData(int(SortKey::ReleaseDate));
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
    if (curKey != int(SortKey::None)) {
        // N18：从 action data 反查 SortKey 对应 action，避免下标耦合
        for (QAction *a : sortActions) {
            if (a->data().toInt() == curKey) {
                a->setChecked(true);
                break;
            }
        }
    }

    QAction *ascAction = sortMenu->addAction(tr("Ascending"));
    QAction *descAction = sortMenu->addAction(tr("Descending"));
    for (QAction *a : {ascAction, descAction})
        a->setCheckable(true);
    QActionGroup *dirGroup = new QActionGroup(sortMenu);
    dirGroup->addAction(ascAction);
    dirGroup->addAction(descAction);
    ascAction->setChecked(curAsc);
    descAction->setChecked(!curAsc);

    QMenu *removeMenu = menu.addMenu(tr("Remove"));
    QAction *removeSelAction = removeMenu->addAction(tr("Remove Selected"));
    QAction *clearAction = removeMenu->addAction(tr("Clear Current List"));
    removeMenu->addSeparator();
    QAction *removeInvalidAction = removeMenu->addAction(tr("Remove Invalid"));
    QAction *removeDupAction = removeMenu->addAction(tr("Remove Duplicates"));

    QAction *refreshAction = menu.addAction(tr("Refresh"));
    menu.addSeparator();

    const QString targetNative = m_menuTargetList ? materialize(targetPath).path : QString();
    QAction *openLocationAction = menu.addAction(tr("Open File Location"));
    QAction *copyPathAction = menu.addAction(tr("Copy File Path"));
    QAction *propsAction = menu.addAction(tr("Properties"));

    // ——启用条件——
    const bool hasList = list != nullptr;
    const int itemCount = hasList ? list->count() : 0;
    // N19：枚举失败占位态仅启用刷新与排序（排序只改持久化规则，不执行）
    const bool loadFailed = hasList && !m_loadState.value(list).loadError.isEmpty();
    addAction->setEnabled(!dynamic && !loadFailed); // 动态 tab 内容来自枚举，禁手工添加
    auto *menuProvider = providerAt(tab); // 排序能力遵从提供者（单项目录亦可持久化规则）
    sortMenu->setEnabled(hasList && menuProvider && menuProvider->supportsSort());
    refreshAction->setEnabled(dynamic || itemCount > 0); // 动态刷新不受空内容限制
    // N9：动态 tab 移除子菜单整组禁用（失效清理走刷新）
    removeMenu->setEnabled(hasList && !dynamic && !loadFailed);
    removeSelAction->setEnabled(hasList && !list->selectedItems().isEmpty());
    clearAction->setEnabled(itemCount > 0);

    const bool hasTarget = m_menuTargetList != nullptr;
    openLocationAction->setEnabled(hasTarget && !targetPath.isEmpty() &&
                                   QFileInfo::exists(targetPath));
    copyPathAction->setEnabled(hasTarget);
    propsAction->setEnabled(hasTarget);

    // N18：依赖 MediaInfo 缓存的两项排序在 dll 不可用时禁用；其余排序项不受影响
    if (!MediaInfoHelper::available()) {
        for (QAction *a : sortActions) {
            const int k = a->data().toInt();
            if (k == int(SortKey::Duration) || k == int(SortKey::ReleaseDate))
                a->setEnabled(false);
        }
    }

    QAction *chosen = menu.exec(event->globalPos());
    if (!chosen)
        return;

    if (chosen == addAction) {
        QStringList patterns;
        for (const QString &ext : App::defaultMediaExtensions())
            patterns << QStringLiteral("*.") + ext;
        const QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Add Files"), QString(),
            tr("Media files (%1);;All files (*)").arg(patterns.join(QLatin1Char(' '))));
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
        // 切向即时重排：有生效规则立即按新方向执行；无规则仅记录方向；
        // N19：失败占位态只写规则不执行（无内容可排，下次加载成功后生效）
        const bool ascending = (chosen == ascAction);
        if (auto *p = providerAt(tab)) {
            p->setSortRule(p->sortKey(), ascending);
            if (!loadFailed && p->sortKey() != int(SortKey::None))
                sortCurrent(SortKey(p->sortKey()), ascending);
        }
    } else {
        // 排序规则命中：按当前方向执行一次并迁移勾选（sortCurrent 落位状态）
        // N18：从 action data 取 SortKey 而非数组下标 +1（避免菜单顺序与枚举序耦合）
        const int keyInt = chosen->data().toInt();
        if (keyInt > int(SortKey::None)) {
            if (loadFailed) {
                if (auto *p = providerAt(tab))
                    p->setSortRule(keyInt, curAsc); // 仅持久化记忆，随下次成功加载生效
            } else {
                sortCurrent(static_cast<SortKey>(keyInt), curAsc);
            }
        }
    }
}

// N9：删除指定 tab（表头菜单，作用于右键命中的 tab）。关闭正在播放的列表：
// 先停播并清空 mpv 侧，同步目标置 -1 悬空，防空列表回刷误清其他 tab；
// 下次双击经 markPlayingTab 重建
void PlaylistWidget::removeTabAt(int tabIndex) {
    const int idx = tabIndex;
    if (idx <= 0 || idx >= count() || m_busy)
        return; // N19：忙时表头菜单已禁用，此为兜底
    if (idx == m_playingTab) {
        emit stopRequested();
        emit clearRequested();
        m_playingTab = -1;
    }
    if (QListWidget *closing = listAt(idx)) {
        m_providerOf.erase(closing); // unique_ptr 自动回收提供者（含排序规则）
        m_memory.remove(closing);
        m_loadState.remove(closing); // N19：清理加载状态（含 loadError）
        if (m_pendingLoad.list == closing)
            m_pendingLoad = PendingLoad{};
        if (m_loadingList == closing) {
            m_loadingList = nullptr;
            ++m_loadGen; // 在途收尾按 generation 丢弃
        }
    }
    if (m_playingTab > idx)
        --m_playingTab;
    removeTab(idx);
    savePlaylists();
}

// N19：忙时表头菜单已禁用，此为兜底
// N9：新建播放列表——弹列表设置对话框，确定后建 tab；动态提供者经激活链异步填充
void PlaylistWidget::createNewList() {
    if (m_busy)
        return;
    int maxN = 0;
    for (int i = 0; i < count(); ++i) {
        const QString &name = tabText(i);
        if (name.startsWith(tr("Playlist "))) {
            bool ok = false;
            int n = name.mid(QString(tr("Playlist ")).length()).toInt(&ok);
            if (ok && n > maxN)
                maxN = n;
        }
    }

    QStringList occupied;
    for (int i = 0; i < count(); ++i)
        occupied << tabText(i);
    ListSettingsDialog dlg(this, tr("Playlist %1").arg(maxN + 1), nullptr, occupied);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.listName();

    auto *list = createList();
    // 新建必产出提供者；排序默认按类型施加（目录默认按修改时间）
    m_providerOf.insert_or_assign(list, std::move(dlg.takeProvider().value()));
    auto *p = m_providerOf[list].get();
    const bool dynamic = p->type() == PlaylistProvider::Type::Dir;
    p->setSortRule(dynamic ? int(SortKey::Modified) : int(SortKey::None), true);
    addTab(list, name);
    if (!dynamic) {
        fillFromProvider(list); // 静态同步填充（恒成功）
        m_loadState[list].entriesLoaded = true;
    }
    // N19：动态 tab 经切入首次激活链异步加载（修正旧 `isDynamicTab(count()-1)` 取错 tab）
    setCurrentIndex(count() - 1);
    savePlaylists();
}

// N9：存量列表设置——改名/换提供者。非法参数由对话框校验报错留在窗口；
// 合法后：仅改名直接生效；提供者有变则静默清空重建，作用对象为播放中 tab 时
// 停止并选中首项不播（悬空同步目标防回刷竞态）
void PlaylistWidget::openListSettings(int tabIndex) {
    if (tabIndex <= 0 || m_busy)
        return; // N19：忙时表头菜单已禁用，此为兜底
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
        np->setSortRule(isDynamicTab(tabIndex) ? int(SortKey::Modified) : int(SortKey::None), true);

    TabLoadState &st = m_loadState[list];
    st.entriesLoaded = false;
    st.mediaInfoLoaded = false;
    st.loadError.clear();
    list->clear();
    if (isDynamicTab(tabIndex)) {
        // N19：目录源走异步加载，完成后选中首项（不播放）
        m_pendingLoad = PendingLoad{};
        m_pendingLoad.list = list;
        m_pendingLoad.selectFirst = true;
        startLoadAsync(list);
    } else {
        fillFromProvider(list); // 静态取所持条目（切换后为空）
        st.entriesLoaded = true;
        if (list->count() > 0)
            list->setCurrentRow(0); // 选中第一项，不开始播放
        emit contentsChanged(); // A17：静态重建条目数可能变化，刷新导航启停
    }
    savePlaylists();
}

#include "pch.h"

#include "PlaylistWidget.h"

#include "Constants.h"
#include "ListSettingsDialog.h"
#include "LoadingOverlay.h"
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
    connect(m_overlay, &LoadingOverlay::cancelRequested, this, [this] { m_metaCancel = true; });
}

// N19：在途枚举线程收尾。先置存活旗（工作线程检查后不再投递收尾），再 wait；
// 超时放弃后即便线程仍在跑，也只会看到存活旗为 false 而直接返回，不触碰任何成员。
// N26：工作线程可能在 provider->enumerate() 内部（线程触碰提供者对象，不再只持参数
// 快照）——wait 超时后成员析构会连提供者一并释放，构成 use-after-free；故先把在途
// 提供者以一次性泄漏保活（进程收尾期，无实害），N28 改 shared_ptr 捕获后根治。
// 预读为主线程循环，随析构自然终止
PlaylistWidget::~PlaylistWidget() {
    *m_alive = false;
    if (m_loadThread && m_loadThread->isRunning()) {
        if (QListWidget *loading = m_loadingList.data()) {
            const auto it = m_providerOf.find(loading);
            if (it != m_providerOf.end())
                it->second.release();
        }
        m_loadThread->wait(2000);
    }
}

QListWidget *PlaylistWidget::createList() {
    auto *list = new QListWidget;
    // 子列表不接受拖放，事件会上溯到本 QTabWidget 统一处理
    list->setAcceptDrops(false);
    // N5：列表集中创建点统一安装过滤器（ShortcutOverride 认领导航键）
    list->installEventFilter(this);
    // 播放列表不允许多选（显式声明，锁定约束）
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    // N26：委托绘制（时长经实体 key 查时长仓，兜底实体缓存元数据）
    list->setItemDelegate(new PlaylistDelegate(&m_duration, list));
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

// ——N26 实体所有权——

// 缺省插入空表；调用点均伴随建行 / 回传
PlaylistEntity::List &PlaylistWidget::entitiesOf(QListWidget *list) { return m_entities[list]; }

// 行实体读取：占位行 / 无实体行返回 nullptr
const PlaylistEntity *PlaylistWidget::entityFromItem(const QListWidgetItem *item) {
    if (!item)
        return nullptr;
    const QVariant v = item->data(EntityRole);
    if (!v.isValid())
        return nullptr;
    return reinterpret_cast<const PlaylistEntity *>(v.value<quintptr>());
}

// 行实体的共享计数副本：跨嵌套事件循环（右键菜单 exec）持有，防收编重建致悬空。
// 容器序 == 行序定位容器下标，并与行 EntityRole 交叉校验（不变式被破坏即拒绝）
PlaylistEntity::Ptr PlaylistWidget::entityPtrAt(QListWidget *list, int row) const {
    if (!list || row < 0 || row >= list->count())
        return nullptr;
    const auto it = m_entities.constFind(list);
    if (it == m_entities.end())
        return nullptr;
    int idx = 0;
    for (int i = 0; i < row; ++i) {
        if (!isPlaceholderRow(list->item(i)))
            ++idx;
    }
    const auto &entities = it.value();
    if (idx >= int(entities.size()))
        return nullptr;
    const auto &candidate = entities.at(idx);
    if (candidate.get() != entityFromItem(list->item(row)))
        return nullptr; // 容器/行序不变式被破坏：宁可拒绝也不返回错乱实体
    return candidate;
}

// 按行序批量删除实体（rows 必须降序；须在删行前调用）。
// 单遍自顶向下计非占位行数定位各行容器下标（容器序 == 行序，占位行不占位），
// 随后按容器下标降序批量 erase——避免逐行删除时 O(n·k) 的前缀重扫
void PlaylistWidget::eraseEntitiesAt(QListWidget *list, const QList<int> &rows) {
    auto it = m_entities.find(list);
    if (it == m_entities.end() || rows.isEmpty())
        return;
    auto &entities = it.value();
    QList<int> ascRows = rows;
    std::sort(ascRows.begin(), ascRows.end()); // 升序扫描，容器下标随之升序
    QList<int> idxs;
    int cursor = 0;
    int nonPlaceholder = 0;
    for (int i = 0; i < list->count(); ++i) {
        if (isPlaceholderRow(list->item(i)))
            continue;
        while (cursor < ascRows.size() && ascRows.at(cursor) < i)
            ++cursor; // 目标行是占位行（无实体，防御）：跳过
        if (cursor < ascRows.size() && ascRows.at(cursor) == i) {
            idxs.append(nonPlaceholder);
            ++cursor;
        }
        ++nonPlaceholder;
    }
    for (int k = idxs.size() - 1; k >= 0; --k)
        entities.erase(entities.begin() + idxs.at(k));
}

// 统一的条目构造：显示名 / 图标 / tooltip 全部取自实体；
// EntityRole 存实体裸指针（quintptr），实体所有权随行同步 push 进容器
QListWidgetItem *PlaylistWidget::makeItem(QListWidget *list, PlaylistEntity::Ptr entity) {
    auto *item = new QListWidgetItem(list);
    item->setData(Qt::DisplayRole, entity->title());
    item->setData(Qt::DecorationRole, entity->icon());
    item->setData(EntityRole, QVariant::fromValue<quintptr>(quintptr(entity.get())));
    item->setToolTip(entity->playUrl());
    entitiesOf(list).push_back(std::move(entity)); // 所有权随行同步入仓（统一经 entitiesOf）
    return item;
}

QStringList PlaylistWidget::filesOf(int tabIndex) const {
    QStringList files;
    if (QListWidget *list = listAt(tabIndex)) {
        for (int i = 0; i < list->count(); ++i) {
            if (isPlaceholderRow(list->item(i)))
                continue; // N19：占位行不是可播文件
            if (const PlaylistEntity *entity = entityFromItem(list->item(i)))
                files.append(entity->playUrl());
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

// N25：回刷策略——本地在途回显 → 空列表忽略 → 一致静默 → 占位/枚举失败忽略 → 差异收编默认 tab。
// 回刷不再重建正在播放的 tab：本地发起的变更一律本地先行，回刷因此恒为一致而静默；
// 真正的外部变更统一收编到默认 tab（mpv 会话镜像：临时、不持久化、可随时被替换）
void PlaylistWidget::setPlaylist(const QVariantList &items) {
    QStringList pushed;
    QList<bool> current;
    pushed.reserve(items.size());
    current.reserve(items.size());
    for (const QVariant &v : items) {
        const QVariantMap map = v.toMap();
        // mpv 来源路径：本地 cleanPath+原生分隔符，网络/流保持原样（B15）
        pushed.append(Util::resolveLocalPath(map.value(QStringLiteral("filename")).toString()));
        current.append(map.value(QStringLiteral("current")).toBool());
    }

    // 1. 本地在途回显：本地下发的是多命令序列，mpv 可能分批推送中间态（空列表/部分列表）。
    // 这些中间态若被判为差异会把列表搬去默认 tab，故按"同序子序列关系"识别并忽略
    if (m_localPush.pending) {
        if (pushed == m_localPush.snapshot) {
            m_localPush.pending = false; // 回显确认：继续走常规判定（翻转播放标记）
        } else if (Util::isOrderedSubsequence(pushed, m_localPush.snapshot) ||
                   Util::isOrderedSubsequence(m_localPush.snapshot, pushed)) {
            return; // 同一变更的在途中间态
        } else {
            m_localPush.pending = false; // 回显失配：放弃，按外部变更处理
        }
    }

    // 2. 空列表无正播项、无收编意义（本地 stop/clear/删 tab/换源均已本地先行），一律忽略
    if (pushed.isEmpty())
        return;

    QListWidget *list = listAt(m_playingTab);
    if (!list)
        return;

    // 3. 占位/枚举失败态：占位只应由刷新或重枚举清除，且占位行无路径会令比较恒为差异
    bool hasPlaceholder = false;
    for (int i = 0; i < list->count(); ++i) {
        if (isPlaceholderRow(list->item(i))) {
            hasPlaceholder = true;
            break;
        }
    }
    if (hasPlaceholder || !m_loadState[list].loadError.isEmpty())
        return;

    // 4. 逐行一致即静默：只翻转播放标记，正播行变化才迁移选中+滚动（无重建、无闪烁）
    if (list->count() == pushed.size()) {
        bool identical = true;
        for (int i = 0; i < list->count(); ++i) {
            const PlaylistEntity *entity = entityFromItem(list->item(i));
            if (!entity || entity->playUrl() != pushed.at(i)) {
                identical = false;
                break;
            }
        }
        if (identical) {
            const int oldPlaying = playingRowOf(list);
            int newPlaying = -1;
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->data(PlayingRole).toBool() != current.at(i))
                    list->item(i)->setData(PlayingRole, current.at(i));
                if (current.at(i))
                    newPlaying = i;
            }
            if (newPlaying >= 0 && newPlaying != oldPlaying) {
                list->setCurrentItem(list->item(newPlaying));
                list->scrollToItem(list->item(newPlaying), QListWidget::EnsureVisible);
            }
            return;
        }
    }

    // 5. 差异＝外部变更：整体收编到默认 tab，原 playing tab 一字不动
    QListWidget *def = listAt(0);
    if (!def)
        return;
    if (def == m_loadingList)
        ++m_loadGen; // 默认 tab 无异步加载，此为防御

    // 记录收编前的选中行（单选）：推送无 current 项（如停止态外部编辑）时
    // 整表替换会把界面选中清空（仅显示问题），沿用旧重建语义恢复
    const int prevRow = def->currentRow();
    def->clear();
    m_entities[def].clear();                  // 同步点：收编重建＝先清容器再随建行 push_back
    m_loadState[def].mediaInfoLoaded = false; // N19：条目集合已变，已读元数据不可信

    QListWidgetItem *newCurrent = nullptr;
    for (int i = 0; i < pushed.size(); ++i) {
        // 经工厂造实体（网络路径落 UnknownEntity）；复用已有实体的克隆方案归 N30
        QListWidgetItem *item = makeItem(def, PlaylistEntity::fromPlayUrl(pushed.at(i)));
        if (current.at(i)) {
            item->setData(PlayingRole, true);
            newCurrent = item;
        }
    }
    if (newCurrent) {
        def->setCurrentItem(newCurrent);
        // 跟随滚动（N4）：正播项变动时最小位移滚入可见范围
        def->scrollToItem(newCurrent, QListWidget::EnsureVisible);
    } else if (prevRow >= 0 && prevRow < def->count()) {
        def->setCurrentRow(prevRow);
    }
    markPlayingTab(0);         // B10：异 tab 转移清旧标记；同 tab 早退（默认 tab 自覆盖不误清）
    m_memory.remove(def);      // 内容被替换即作废续播记忆（新记忆随 mpv playlist-pos 推送重建）
    emit contentsChanged();    // A17：条目数已变，刷新导航启停
    emit playingListAdopted(); // N25：镜像已重新一致，MainWindow 刷新标题/OSD 正播名
    requestTab(0);             // 可见 tab 跟随；N19 忙时排队，枚举/预读结束后补执行
}

void PlaylistWidget::addFiles(const QStringList &files) {
    QListWidget *list = listAt(currentIndex());
    if (!list || files.isEmpty() || m_busy)
        return; // N19：忙时不接受手工增补（预读快照防污染）

    // N9/N27：不可修改的 tab 内容来自枚举，忽略手工添加（拖放与菜单同守卫）。
    // 静态 tab 增删即回无序态：复位排序规则（保留方向）
    if (!isModifiableTab(currentIndex()))
        return;
    if (auto *p = providerAt(currentIndex()))
        p->setSortRule(int(SortKey::None), p->sortAscending());
    m_loadState[list].mediaInfoLoaded = false; // N19：条目集合变化，已读元数据不可信

    // N25：本地先行——播放中 tab 也立即插行（回刷只会把"本地未改"的差异搬去默认 tab），
    // 插入后本地序列即等于 mpv 追加后的序列，回刷恒一致而静默
    for (const QString &file : files)
        makeItem(list, PlaylistEntity::fromPlayUrl(file));
    // 非默认本地 tab 添加后持久化（默认 tab 永不落盘，N9/N4 既定）
    if (currentIndex() != 0)
        savePlaylists();
    // 仅播放中 tab 的增删会同步进 mpv（MainWindow::onFilesAdded 同款守卫）：
    // 非播放 tab 不会引发推送，登记在途会留下永不回显的快照
    if (currentIsPlayingTab())
        noteLocalPush(filesOf(currentIndex()));
    emit filesAdded(files);
}

// N25：本地在途回显登记。空序列不登记——清空/删 tab 之后 mpv 侧本就变空，
// 由回刷的"空列表忽略"兜底，登记反而会让后续推送被误判为在途中间态
void PlaylistWidget::noteLocalPush(const QStringList &snapshot) {
    if (snapshot.isEmpty())
        return;
    m_localPush.snapshot = snapshot;
    m_localPush.pending = true;
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
    m_entities[list].clear(); // 同步点：重建＝先清容器再随建行 push_back
    for (const QString &file : files)
        makeItem(list, PlaylistEntity::fromPlayUrl(file));
}

void PlaylistWidget::repaintAllLists() {
    for (int i = 0; i < count(); ++i) {
        if (QListWidget *l = listAt(i))
            l->viewport()->update();
    }
}

// B18/N26：读取失败（seconds<=0）存 nullopt——时长列经取值链回落到实体缓存元数据
// （mpv 推送未知而实体已解析时显示 MediaInfo 时长，B18 口径以 N26 为准）
void PlaylistWidget::applyDuration(const QString &rawPath, double seconds) {
    if (rawPath.isEmpty())
        return;
    m_duration[Util::normalizePathForMatch(rawPath)] =
        seconds > 0 ? std::optional<double>(seconds) : std::nullopt;
    repaintAllLists();
}

// ——续播记忆三步维护法（N4）——

QString PlaylistWidget::captureMemoryPath(QListWidget *list) const {
    const TabMemory mem = m_memory.value(list);
    if (mem.lastPos < 0 || mem.lastPos >= list->count())
        return {};
    if (const PlaylistEntity *entity = entityFromItem(list->item(mem.lastPos)))
        return entity->key();
    return {};
}

void PlaylistWidget::restoreMemoryPath(QListWidget *list, const QString &captured, bool hadMemory) {
    if (!hadMemory)
        return;
    TabMemory mem = m_memory.value(list);
    if (!captured.isEmpty()) {
        for (int r = 0; r < list->count(); ++r) {
            if (const PlaylistEntity *entity = entityFromItem(list->item(r));
                entity && entity->key() == captured) {
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
        // N26：记忆存条目 key（normalizePathForMatch 归一形态）。旧 INI 存原始形态，
        // 读取侧归一桥接（新写入的 key 再归一幂等，往返无损）；空值防归一成 CWD
        if (!mem.lastPath.isEmpty())
            mem.lastPath = Util::normalizePathForMatch(mem.lastPath);

        // N9：提供者反序列化完全自治（静态回填条目、目录读参数与排序键），
        // 内容统一经 enumerate 取得
        std::unique_ptr<PlaylistProvider> provider = PlaylistProvider::load(settings);
        // N27：能力位在移交容器前就地读取（此后只经 providerAt 查表）
        const bool modifiable = provider && provider->supportsModify();
        settings.endGroup();
        addTab(list, name);
        // insert_or_assign：createList 已预置静态默认，此处必须覆盖
        m_providerOf.insert_or_assign(list, std::move(provider));
        if (mem.lastPos >= 0 || !mem.lastPath.isEmpty())
            m_memory[list] = mem;
        // N27：不可修改的提供者内容以枚举为准，延迟到首次激活；其余同步填充
        if (!modifiable) {
            m_loadState[list].entriesLoaded = false; // N19：延迟到首次激活
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
        // N26：回传实体集合（StaticProvider 从实体提取 playUrl 覆盖最小表示）；
        // N27：回传条件收口为能力位，走基类虚调用——不可修改的提供者（目录）走空实现，
        // 落盘内容恒为参数与排序键。回传与落盘是同一动作的两半，不得拆开
        if (provider && provider->supportsModify())
            provider->setItems(entitiesOf(list));
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

// N25：写入前校验 (tab, pos) 与 mpv 实际正播文件一致。外部结构变更在途时
// playlist-pos 先于 playlist 推送更新（如终端移动正播条目），旧 playing tab 配
// mpv 新索引会把别行写成"正在播放"（实测：移动后原 tab 续播记忆串位）。
// time-pos 心跳仅在文件真正播放后才带 v>0，此时 path 必为正播文件，校验恒过；
// 切条目瞬间 pos 推送若 path 尚未更新则跳过一次，随即由 time-pos 心跳自愈
void PlaylistWidget::recordPlayback(int tabIndex, int pos, double seconds) {
    if (tabIndex < 0 || pos < 0) // 负索引无效；默认 tab 允许进程内记忆（不持久化）
        return;
    QListWidget *list = listAt(tabIndex);
    if (!list || pos >= list->count())
        return;
    const QString mpvPath = Util::normalizePathForMatch(
        m_mpv ? m_mpv->propertyString(QStringLiteral("path")) : QString());
    const PlaylistEntity *entity = entityFromItem(list->item(pos));
    if (mpvPath.isEmpty() || !entity || entity->key() != mpvPath)
        return; // 索引与正播文件对不上：结构变更在途，不写本表记忆
    TabMemory &mem = m_memory[list];
    mem.lastPos = pos;
    mem.lastTimePos = seconds;
    // N9/N26：路径随写入点采集（路径+序号双存）。存条目 key（身份），比对免疫来源形态漂移
    mem.lastPath = entity->key();
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
    if (mem.lastPos >= 0 && mem.lastPos < list->count()) {
        if (const PlaylistEntity *entity = entityFromItem(list->item(mem.lastPos));
            entity && entity->key() == mem.lastPath)
            return mem.lastPos;
    }
    // ② 序号失效：按路径取第一个命中
    if (!mem.lastPath.isEmpty()) {
        for (int r = 0; r < list->count(); ++r) {
            if (const PlaylistEntity *entity = entityFromItem(list->item(r));
                entity && entity->key() == mem.lastPath)
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
    // N19：未加载的 tab 走首次激活链（选中/滚动在 finishLoad 收尾）；
    // N27：能否异步的判定交 N28，此处只看是否已枚举。依赖不变式——可修改的 tab
    // 在新建 / 换源 / 会话恢复时同步填充并置位 entriesLoaded，不会落到此分支
    if (QListWidget *fresh = listAt(index); fresh && !m_loadState[fresh].entriesLoaded) {
        startLoadAsync(fresh);
        return;
    }
    // N19：已加载但元数据失效（他 tab 刷新清共享缓存/上次预读取消/回刷变更），
    // 且本 tab 排序依赖元数据 → 切入即重读后排序（含取消后切入重读语义）
    if (QListWidget *cur = listAt(index); cur && needsMetaRescan(index)) {
        reactivateRescan(cur, index);
        return;
    }
    activateTab(index);
}

bool PlaylistWidget::needsMetaRescan(int tabIndex) const {
    if (m_busy)
        return false;
    const QListWidget *list = listAt(tabIndex);
    const TabLoadState st = m_loadState.value(list);
    if (!list || !st.entriesLoaded || st.mediaInfoLoaded || !st.loadError.isEmpty())
        return false; // 占位 tab（loadError 非空）走失败重试，不走预读
    // N26：全表无媒体属性支持者 → 切入不预读（取代 dll 全局可用性判定）
    if (!anyEntitySupportsMediaMeta(list))
        return false;
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
    emit playRequested(filesOf(tab), list->row(item),
                       -1.0); // 回显由 MainWindow 统一入口登记（N25）
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

bool PlaylistWidget::isModifiableTab(int tabIndex) const {
    const auto *p = providerAt(tabIndex);
    Q_ASSERT(listAt(tabIndex) == nullptr || p != nullptr); // I1：tab 与 provider 一一对应
    return p != nullptr && p->supportsModify();
}

// 同步填充（可修改的源路径）：enumerate 解包后走统一填充
void PlaylistWidget::fillFromProvider(QListWidget *list) {
    if (!list)
        return;
    const auto it = m_providerOf.find(list);
    if (it == m_providerOf.end())
        return;
    if (auto r = it->second->enumerate(); r.has_value())
        fillListFromEntries(list, std::move(r.value()));
    else
        showPlaceholder(list, r.error());
}

// N19/N26：GUI 线程填充——清空（行 + 实体容器）→ 逐条 makeItem → 应用排序规则。
// 元数据键（Duration/ReleaseDate）且本表有支持者时跳过重排，由预读后的
// applySort 执行（否则比较器会在此全表同步解析）
void PlaylistWidget::fillListFromEntries(QListWidget *list, PlaylistEntity::List &&entities) {
    if (!list)
        return;
    list->clear();
    m_entities[list].clear(); // 同步点：填充＝先清容器，再随建行逐条 push_back
    for (auto &entity : entities)
        makeItem(list, std::move(entity));
    const auto it = m_providerOf.find(list);
    if (it == m_providerOf.end())
        return;
    const auto key = SortKey(it->second->sortKey());
    if (key == SortKey::None)
        return;
    if ((key == SortKey::Duration || key == SortKey::ReleaseDate) &&
        anyEntitySupportsMediaMeta(list))
        return; // 预读链收尾排序
    sortListItems(list, key, it->second->sortAscending());
}

// N19：枚举类 tab 首次激活。N26：线程经裸指针调用 provider->enumerate()
// （实体构造与 fileMeta 预热都在工作线程）。
// 不变式：加载在途期间 GUI 线程不修改该提供者——目录源成员构造后只读，
// 增删/换源/删 tab/保存均被 m_busy 拦截；N28 以 shared_ptr 形式化保证生命周期。
// 跨线程只带裸指针值 + 共享存活旗：QPointer（含其拷贝）在工作线程读写是数据竞争，
// 有效性一律由 GUI 收尾按 m_loadingList（仅 GUI 线程读写）+ m_loadGen 判定
void PlaylistWidget::startLoadAsync(QListWidget *list) {
    if (!list)
        return;
    const auto it = m_providerOf.find(list);
    if (it == m_providerOf.end() || it->second->supportsModify())
        return; // 防御：可修改的提供者同步填充即够，不走线程（调用方均已判定，此为兜底）
    PlaylistProvider *provider = it->second.get();
    clearPlaceholder(list); // 失败重试先清占位
    PlaylistWidget *rawSelf = this;
    QListWidget *rawList = list;
    auto alive = m_alive;
    const quint64 gen = ++m_loadGen;
    m_loadingList = list;
    setBusy(true);
    armOverlay(LoadingOverlay::Mode::Loading, 0);
    auto *thread = QThread::create([rawSelf, rawList, provider, gen, alive] {
        std::expected<PlaylistEntity::List, QString> r =
            provider->enumerate(); // 工作线程：实体构造 + 预热仅此一步
        if (!alive->load())
            return; // 析构已开始，不触碰任何成员
        QMetaObject::invokeMethod(
            rawSelf,
            [rawSelf, rawList, r = std::move(r), gen] {
                // 上下文机制：rawSelf 若在投递前销毁，本调用被 Qt 丢弃；
                // 旧批次或目标已换（删 tab/同 tab 回刷重建）即弃
                if (gen != rawSelf->m_loadGen || rawSelf->m_loadingList != rawList)
                    return;
                rawSelf->finishLoad(rawList, std::move(r));
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
void PlaylistWidget::finishLoad(QListWidget *list, std::expected<PlaylistEntity::List, QString> r) {
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
        // N25：播放中 tab 枚举失败即停止——枚举已失败，后续切曲大概率同失败，
        // 继续播放无意义。stop 不带 keep-playlist，一并清空 mpv 播放列表
        // （随后的空列表推送由回刷"空列表忽略"吸收，不改写任何 tab）
        const int failTab = indexOf(list);
        if (failTab >= 0 && failTab == m_playingTab) {
            emit stopRequested();
            m_playingTab = 0; // 同步目标归位默认 tab（恒 >= 0，不悬空）
        }
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
    const bool hadMem = hadRows && m_memory.contains(list) && m_memory.value(list).lastPos >= 0;
    fillListFromEntries(list, std::move(r.value()));

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
            const bool needScan = anyEntitySupportsMediaMeta(list) && !st.mediaInfoLoaded &&
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
                emit playRequested(files, idx, resumeSec); // 回显由 MainWindow 统一入口登记（N25）
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

// 条目重排核心：行指针摘除 → 稳定排序 → 回填；实体容器按同一 order 重排。
// 无停止/恢复副作用，四步式流程（sortCurrent）与加载填充共用
void PlaylistWidget::sortListItems(QListWidget *list, SortKey key, bool ascending) {
    if (!list || key == SortKey::None || list->count() < 2)
        return;

    const int n = list->count();
    // 行序实体快照（容器序 == 行序）：比较器只读实体
    std::vector<const PlaylistEntity *> rows(n, nullptr);
    for (int i = 0; i < n; ++i)
        rows[i] = entityFromItem(list->item(i));

    QVector<QList<Util::NameSegment>> numericSegs;
    if (key == SortKey::FileNameNumeric) {
        numericSegs.resize(n);
        for (int i = 0; i < n; ++i)
            numericSegs[i] = Util::segmentName(rows[i] ? rows[i]->fileName() : QString());
    }

    auto lessByRow = [key, ascending, &rows, &numericSegs](int x, int y) -> bool {
        const PlaylistEntity *a = rows[x];
        const PlaylistEntity *b = rows[y];
        if (!a || !b)
            return false; // 排序场景无占位行；防御等键保序
        auto flip = [ascending](int c) { return ascending ? c < 0 : c > 0; };
        switch (key) {
        case SortKey::None:
            return false;
        case SortKey::FileName:
            return flip(QString::compare(a->fileName(), b->fileName(), Qt::CaseInsensitive));
        case SortKey::FileNameNumeric:
            return flip(Util::naturalCompare(numericSegs[x], numericSegs[y]));
        case SortKey::Extension:
            return flip(QString::compare(a->suffix(), b->suffix(), Qt::CaseInsensitive));
        case SortKey::Created:
        case SortKey::Modified: {
            // N26：文件级字段缺失按未知——已知在前、未知在后、双未知等键（与媒体键同形）
            const FileMeta &fa = a->fileMeta();
            const FileMeta &fb = b->fileMeta();
            const auto &va = key == SortKey::Created ? fa.created : fa.modified;
            const auto &vb = key == SortKey::Created ? fb.created : fb.modified;
            if (!va.has_value() || !vb.has_value())
                return va.has_value() && !vb.has_value();
            return flip(*va < *vb ? -1 : (*vb < *va ? 1 : 0));
        }
        case SortKey::FileSize: {
            const FileMeta &fa = a->fileMeta();
            const FileMeta &fb = b->fileMeta();
            if (!fa.size.has_value() || !fb.size.has_value())
                return fa.size.has_value() && !fb.size.has_value();
            return flip(*fa.size < *fb.size ? -1 : (*fb.size < *fa.size ? 1 : 0));
        }
        case SortKey::Duration:
        case SortKey::ReleaseDate: {
            // N26：读实体媒体元数据（预读链内调用；未缓存的在此同步解析）
            const auto &ma = a->mediaMeta();
            const auto &mb = b->mediaMeta();
            const bool ha =
                key == SortKey::Duration ? ma.duration.has_value() : ma.recordedDate.isValid();
            const bool hb =
                key == SortKey::Duration ? mb.duration.has_value() : mb.recordedDate.isValid();
            if (!ha || !hb)
                return ha && !hb; // 已知在前、未知在后；双未知视为等键
            if (key == SortKey::Duration)
                return flip(*ma.duration < *mb.duration ? -1
                                                        : (*mb.duration < *ma.duration ? 1 : 0));
            return flip(ma.recordedDate < mb.recordedDate
                            ? -1
                            : (mb.recordedDate < ma.recordedDate ? 1 : 0));
        }
        }
        return false;
    };

    QVector<int> order(n);
    for (int i = 0; i < n; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return lessByRow(x, y); });

    // 实体容器按同一 order 以移动语义重排后回写（容器序 == 行序）。
    // Q_ASSERT：行数与实体数失配即"容器序 == 行序"不变式被某个未同步的变更点破坏，
    // 静默跳过会让后续 eraseEntitiesAt 按行序删错实体——必须让 Debug 构建立即炸出
    auto eit = m_entities.find(list);
    Q_ASSERT(eit == m_entities.end() || int(eit.value().size()) == n);
    if (eit != m_entities.end() && int(eit.value().size()) == n) {
        PlaylistEntity::List sortedEntities;
        sortedEntities.reserve(n);
        for (int idx : order)
            sortedEntities.push_back(std::move(eit.value()[idx]));
        eit.value() = std::move(sortedEntities);
    }

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

    // N19：元数据键走预读流程——先全表预读再排序，取消则规则不写、一切不动。
    // N26：本表无支持者时退化为直排（与菜单禁用一致）
    if ((key == SortKey::Duration || key == SortKey::ReleaseDate) &&
        anyEntitySupportsMediaMeta(list)) {
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
    const bool ok = runMetaScan(list, list->count());
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
            if (const PlaylistEntity *entity = entityFromItem(list->item(pr)))
                resumePath = entity->playUrl();
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
            emit playRequested(files, idx, resumeSec); // 回显由 MainWindow 统一入口登记（N25）
        // idx < 0 理论不可达（排序不删行）；保守起见不恢复
    }

    restoreMemoryPath(list, captured, hadMemory);
}

// N19/N26：主线程元数据预读循环。total = 条目总数；返回 false = 用户取消或销毁。
// 进度按已处理行数计数——不支持媒体属性的实体永不产生缓存，
// 按缓存命中计数会使进度条永不达 100% 并可致预读卡死
bool PlaylistWidget::runMetaScan(QPointer<QListWidget> list, int total) {
    QPointer<PlaylistWidget> self(this);
    // 快照：持有共享计数，循环期间行重建不影响；实体归属容器可能被回刷替换，
    // 快照保证被解析实体存活到本次循环结束
    PlaylistEntity::List snapshot;
    if (list) {
        const auto it = m_entities.constFind(list);
        if (it != m_entities.end())
            snapshot = it.value();
    }
    m_scanDone = 0;
    m_overlay->setProgress(0, total);
    QElapsedTimer throttle;
    throttle.start();
    for (const auto &entity : snapshot) {
        if (!self || !list)
            return false;
        const bool hit = entity->cachedMediaMeta() != nullptr;
        entity->mediaMeta(); // 未命中即同步解析一次入实体缓存（含失败防重解析）
        ++m_scanDone;
        if (hit)
            continue; // 命中零成本，不进入节流/取消检查
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
            if (const PlaylistEntity *entity = entityFromItem(list->item(pr)))
                resumePath = entity->playUrl();
            resumeSec = m_mpv ? m_mpv->propertyDouble(QStringLiteral("time-pos"), -1.0) : -1.0;
        }
    }
    if (playingTab && !resumePath.isEmpty())
        emit stopRequested();

    // 从大到小删除避免行号漂移
    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    eraseEntitiesAt(list, sorted); // 先批量缩实体容器（行还在，按行序定位）
    for (int row : sorted) {
        if (row >= 0 && row < list->count())
            delete list->takeItem(row);
    }

    if (playingTab && !resumePath.isEmpty()) {
        const QStringList files = filesOf(tab);
        const int idx = files.indexOf(resumePath);
        // P 仍在列表：从原进度恢复；P 自身已失效被清则保持停止
        if (idx >= 0)
            emit playRequested(files, idx, resumeSec); // 回显由 MainWindow 统一入口登记（N25）
    } else if (tab != 0) {
        savePlaylists();
    }

    restoreMemoryPath(list, captured, hadMemory);
    emit contentsChanged(); // A17：本地删/清条目数已变，刷新导航启停
}

// N25：播放中 tab 的本地先行删行（供"删除选中"走逐行 playlist-remove 同步通道时用）。
// 不含四步式的停止/重推——删除不需要中断播放，mpv 逐个移除后推送的最终序列即本地序列
void PlaylistWidget::removeRowsLocal(QListWidget *list, const QList<int> &rows) {
    if (!list || rows.isEmpty())
        return;
    const int tab = indexOf(list);
    m_loadState[list].mediaInfoLoaded = false; // N19：条目集合变化
    const bool hadMemory = m_memory.contains(list) && m_memory.value(list).lastPos >= 0;
    const QString captured = captureMemoryPath(list);

    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    eraseEntitiesAt(list, sorted); // 先批量缩实体容器（行还在，按行序定位）
    for (int row : sorted) {
        if (row >= 0 && row < list->count())
            delete list->takeItem(row);
    }
    restoreMemoryPath(list, captured, hadMemory);
    if (tab >= 0)
        noteLocalPush(filesOf(tab));
    emit contentsChanged(); // A17：本地删行条目数已变，刷新导航启停
}

void PlaylistWidget::removeInvalidItems() {
    QListWidget *list = listAt(currentIndex());
    if (!list)
        return;
    QList<int> rows;
    for (int i = 0; i < list->count(); ++i) {
        const QListWidgetItem *item = list->item(i);
        if (isPlaceholderRow(item))
            continue; // 占位行无实体，不参与
        if (const PlaylistEntity *entity = entityFromItem(item); entity && !entity->exists())
            rows.append(i); // 未知实体 exists 恒 true → 网络/流项不参与清理
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
        const QListWidgetItem *item = list->item(i);
        if (isPlaceholderRow(item))
            continue; // 占位行无实体，不参与
        const PlaylistEntity *entity = entityFromItem(item);
        if (!entity)
            continue;
        const QString key = entity->key();
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
    // N19：不可修改的 tab 刷新＝异步重枚举；播放中 tab 的停止/恢复递延到收尾
    // （枚举与预读期间不断播，取消路径零副作用）。
    // N20：刷新只驱逐本表 key（静态/动态同规则），他表缓存不受影响
    if (!isModifiableTab(currentIndex())) {
        evictListKeys(list); // N20：条目只驱逐本表；flag 重置所有表
        TabLoadState &st = m_loadState[list];
        st.entriesLoaded = false;
        st.loadError.clear();
        m_pendingLoad = PendingLoad{};
        m_pendingLoad.list = list;
        if (currentIndex() == m_playingTab) {
            // 收尾 fill 会清 PlayingRole 标记，发起时（行还在）抄录正播路径
            if (const int pr = playingRowOf(list); pr >= 0) {
                if (const PlaylistEntity *entity = entityFromItem(list->item(pr)))
                    m_pendingLoad.resumePath = entity->playUrl();
            }
        } else if (currentIndex() != 0) {
            m_pendingLoad.saveAfter = true;
        }
        startLoadAsync(list);
        return;
    }

    evictListKeys(list);   // N20：条目只驱逐本表；flag 重置所有表
    refreshEntities(list); // N26：本表实体缓存清空并重绘（他表待访问时惰性重读）
    // 统一重排：按持久化规则重排（None 跳过；播放中 tab 走四步式保续播，与手动排序一致）
    if (const PlaylistProvider *p = providerAt(currentIndex())) {
        const auto key = SortKey(p->sortKey());
        if (key != SortKey::None) {
            if ((key == SortKey::Duration || key == SortKey::ReleaseDate) &&
                anyEntitySupportsMediaMeta(list)) {
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

// N26：静态刷新——对本表实体逐个 refresh()（清 fileMeta / mediaMeta / icon 缓存）
// 后重绘；图标 DecorationRole 在 makeItem 时快照，刷新须重写方能取新值；
// 重排由调用方按持久化规则执行
void PlaylistWidget::refreshEntities(QListWidget *list) {
    if (!list)
        return;
    const auto it = m_entities.constFind(list);
    if (it == m_entities.end())
        return;
    for (const auto &entity : it.value())
        entity->refresh();
    for (int i = 0; i < list->count(); ++i) {
        if (const PlaylistEntity *entity = entityFromItem(list->item(i)))
            list->item(i)->setData(Qt::DecorationRole, entity->icon());
    }
    repaintAllLists();
}

// N20/N26：按表精确驱逐。遍历该表全部非占位行，按实体 key 逐个删 m_duration；
// flag 重置所有表（N20 语义保留：本表驱逐可能清掉他表共享 key 的 mpv 推送时长，
// 他表切入经预读重读恢复）
void PlaylistWidget::evictListKeys(QListWidget *list) {
    if (!list)
        return;
    for (int i = 0; i < list->count(); ++i) {
        const QListWidgetItem *item = list->item(i);
        if (isPlaceholderRow(item))
            continue;
        if (const PlaylistEntity *entity = entityFromItem(item))
            m_duration.remove(entity->key());
    }
    for (auto it = m_loadState.begin(); it != m_loadState.end(); ++it)
        it.value().mediaInfoLoaded = false;
}

// ——N26 元数据能力谓词（一对，同形）：遍历本表实体，任一支持即真；
// 空表 / 未加载表为假——

bool PlaylistWidget::anyEntitySupportsMediaMeta(const QListWidget *list) const {
    if (!list)
        return false;
    const auto it = m_entities.constFind(list);
    if (it == m_entities.end())
        return false;
    for (const auto &entity : it.value()) {
        if (entity->supportsMediaMeta())
            return true;
    }
    return false;
}

bool PlaylistWidget::anyEntitySupportsFileMeta(const QListWidget *list) const {
    if (!list)
        return false;
    const auto it = m_entities.constFind(list);
    if (it == m_entities.end())
        return false;
    for (const auto &entity : it.value()) {
        if (entity->supportsFileMeta())
            return true;
    }
    return false;
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

void PlaylistWidget::showPropertiesDialog(const PlaylistEntity &entity) {
    // N26：文件系统四行（路径 / 大小 / 创建 / 修改）读实体 fileMeta，其余媒体字段
    // （格式 / 总时长 / 视频 / 音频 / 字幕 / 标签 / 章节）读实体 mediaMeta——
    // 模态内允许同步求值。supportsMediaMeta 为假（dll 缺席 / 未知实体）时
    // 媒体区各行为空；supportsFileMeta 为假时大小/创建/修改行为空（路径行恒有）。
    // 单 Get 5s 超时由 MediaInfoHelper 守卫；二次访问走实体缓存
    const FileMeta &fm = entity.fileMeta();
    const MediaInfoHelper::Data *mi = entity.supportsMediaMeta() ? &entity.mediaMeta() : nullptr;
    const qint64 totalSize = fm.size.has_value() ? *fm.size : 0;

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

    // ——常规组：路径 / 大小 / 创建 / 修改 走实体 fileMeta；
    // 文件格式 / 总时长 走实体 mediaMeta
    QList<Row> general;
    general.append({tr("File Path"), entity.playUrl()});
    if (mi && !mi->format.isEmpty())
        general.append({tr("Format"), mi->format});
    if (totalSize > 0)
        general.append({tr("File Size"), Util::formatFileSize(totalSize)});
    if (fm.created.has_value()) {
        const QString t = timeText(*fm.created);
        if (!t.isEmpty())
            general.append({tr("Created"), t});
    }
    if (fm.modified.has_value()) {
        const QString t = timeText(*fm.modified);
        if (!t.isEmpty())
            general.append({tr("Modified"), t});
    }
    if (mi && mi->duration.has_value())
        general.append({tr("Duration"), formatHMS(*mi->duration)});

    // 流"大小"格式化："1.23 GB (82%)"，百分比相对文件总大小
    auto formatStreamSize = [&](qint64 sz, qint64 filesize) -> QString {
        if (sz <= 0)
            return {};
        const QString sizeStr = Util::formatFileSize(sz);
        if (filesize <= 0)
            return sizeStr;
        const int pct = int(double(sz) / double(filesize) * 100.0 + 0.5);
        return QStringLiteral("%1 (%2%)").arg(sizeStr).arg(pct);
    };

    // ——流分组：依次遍历实体媒体元数据 streams，按 kind 分桶
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
        const QString sizeStr = formatStreamSize(t.streamSize, totalSize);
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
        const QString sizeStr = formatStreamSize(t.streamSize, totalSize);
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
        const QString sizeStr = formatStreamSize(t.streamSize, totalSize);
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
    const bool modifiable = isModifiableTab(tab);

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
    // N26：目标行实体的共享计数副本——menu.exec() 的嵌套事件循环可能处理 mpv 推送，
    // 触发收编重建默认 tab（实体被整体释放），裸指针/引用届时即悬空；
    // 副本保活到分发结束，对话框读取的是存活实体（内容与右键时刻一致）
    PlaylistEntity::Ptr targetEntity =
        m_menuTargetList ? entityPtrAt(m_menuTargetList, m_menuTargetRow) : nullptr;
    const QString targetUrl = targetEntity ? targetEntity->playUrl() : QString();

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

    QAction *openLocationAction = menu.addAction(tr("Open File Location"));
    QAction *copyPathAction = menu.addAction(tr("Copy File Path"));
    QAction *propsAction = menu.addAction(tr("Properties"));

    // ——启用条件——
    const bool hasList = list != nullptr;
    const int itemCount = hasList ? list->count() : 0;
    // N19：枚举失败占位态仅启用刷新与排序（排序只改持久化规则，不执行）
    const bool loadFailed = hasList && !m_loadState.value(list).loadError.isEmpty();
    addAction->setEnabled(modifiable && !loadFailed); // 不可修改的 tab 内容来自枚举，禁手工添加
    auto *menuProvider = providerAt(tab); // 排序能力遵从提供者（单项目录亦可持久化规则）
    sortMenu->setEnabled(hasList && menuProvider && menuProvider->supportsSort());
    refreshAction->setEnabled(!modifiable || itemCount > 0); // 枚举类刷新不受空内容限制
    // N9：不可修改的 tab 移除子菜单整组禁用（失效清理走刷新）
    removeMenu->setEnabled(hasList && modifiable && !loadFailed);
    removeSelAction->setEnabled(hasList && !list->selectedItems().isEmpty());
    clearAction->setEnabled(itemCount > 0);

    // N26：启用真值统一绑定实体（占位行已在命中检测时挡掉，此处指针即真相）
    const bool hasTarget = targetEntity != nullptr;
    // 能力位取代字符串前缀猜测——定位仅对本地存在项启用
    openLocationAction->setEnabled(targetEntity && targetEntity->isLocalFile() &&
                                   targetEntity->exists());
    copyPathAction->setEnabled(hasTarget);
    propsAction->setEnabled(hasTarget);

    // N26：依赖实体能力的排序项按本表支持者启用；其余排序项不受影响
    if (list) {
        if (!anyEntitySupportsMediaMeta(list)) {
            for (QAction *a : sortActions) {
                const int k = a->data().toInt();
                if (k == int(SortKey::Duration) || k == int(SortKey::ReleaseDate))
                    a->setEnabled(false);
            }
        }
        if (!anyEntitySupportsFileMeta(list)) {
            for (QAction *a : sortActions) {
                const int k = a->data().toInt();
                if (k == int(SortKey::Created) || k == int(SortKey::Modified) ||
                    k == int(SortKey::FileSize))
                    a->setEnabled(false);
            }
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
        Util::openFileLocation(targetUrl, this);
    } else if (chosen == copyPathAction) {
        copyPathToClipboard(targetUrl);
    } else if (chosen == propsAction) {
        showPropertiesDialog(*targetEntity);
    } else if (chosen == removeSelAction && list && modifiable) {
        QList<int> rows;
        for (QListWidgetItem *sel : list->selectedItems())
            rows.append(list->row(sel));
        if (isPlayingTab) {
            // N25：本地先行删行，再走既有同步通道（逐行 playlist-remove）——
            // mpv 逐个移除推送的中间态以本地序列为子序列，最终序列与本地一致，回刷均静默
            removeRowsLocal(list, rows);
            emit removeRequested(rows);
        } else {
            executeRemoval(rows);
        }
    } else if (chosen == clearAction && list && modifiable) {
        QList<int> all;
        for (int i = 0; i < list->count(); ++i)
            all.append(i);
        if (isPlayingTab)
            emit stopRequested(); // N25：先停（stop 不带 keep-playlist 即清空 mpv 列表），
                                  // 空列表推送被回刷忽略；随后 executeRemoval 本地清空
        executeRemoval(all);
    } else if (chosen == removeInvalidAction && modifiable) {
        removeInvalidItems();
    } else if (chosen == removeDupAction && modifiable) {
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
// 先停播并清空 mpv 侧；同步目标归位默认 tab（N25：空列表推送由回刷忽略，
// 不再需要悬空态挡刷）。下次双击经 markPlayingTab 重建
void PlaylistWidget::removeTabAt(int tabIndex) {
    const int idx = tabIndex;
    if (idx <= 0 || idx >= count() || m_busy)
        return; // N19：忙时表头菜单已禁用，此为兜底
    if (idx == m_playingTab) {
        emit stopRequested();
        emit clearRequested();
        m_playingTab = 0;
    }
    if (QListWidget *closing = listAt(idx)) {
        m_providerOf.erase(closing); // unique_ptr 自动回收提供者（含排序规则）
        m_entities.remove(closing);  // N26：实体所有权随 tab 一并回收
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
// N9：新建播放列表——弹列表设置对话框，确定后建 tab；不可修改的提供者经激活链异步填充
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
    // 新建必产出提供者；排序默认按能力位施加（不可修改的源默认按修改时间）
    const auto ins = m_providerOf.insert_or_assign(list, std::move(dlg.takeProvider().value()));
    auto *p = ins.first->second.get(); // 不经 operator[]：键缺失时会插入空 unique_ptr
    const bool modifiable = p->supportsModify();
    p->setSortRule(modifiable ? int(SortKey::None) : int(SortKey::Modified), true);
    addTab(list, name);
    if (modifiable) {
        fillFromProvider(list); // 静态同步填充（恒成功）
        m_loadState[list].entriesLoaded = true;
    }
    // N19：枚举类 tab 经切入首次激活链异步加载（修正旧 `isDynamicTab(count()-1)` 取错 tab）
    setCurrentIndex(count() - 1);
    savePlaylists();
}

// N9：存量列表设置——改名/换提供者。非法参数由对话框校验报错留在窗口；
// 合法后：仅改名直接生效；提供者有变则静默清空重建，作用对象为播放中 tab 时
// 停止（N25：同步目标归位默认 tab，空列表推送由回刷忽略）
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
        m_playingTab = 0;     // N25：同步目标归位默认 tab（空列表推送由回刷忽略，无需悬空）
    }

    // N27：同 insert_or_assign——不经 operator[]，键缺失时不静默插入空 unique_ptr
    m_providerOf.insert_or_assign(list, std::move(*result));

    // 原记忆随源失效一并清除；排序规则重置为新提供者类型默认
    m_memory.remove(list);
    if (auto *np = providerAt(tabIndex))
        np->setSortRule(np->supportsModify() ? int(SortKey::None) : int(SortKey::Modified), true);

    TabLoadState &st = m_loadState[list];
    st.entriesLoaded = false;
    st.mediaInfoLoaded = false;
    st.loadError.clear();
    list->clear();
    m_entities[list].clear(); // N26：实体所有权同步回收（行已清，防悬空 EntityRole）
    if (!isModifiableTab(tabIndex)) {
        // N19：不可修改的源走异步加载，完成后选中首项（不播放）
        m_pendingLoad = PendingLoad{};
        m_pendingLoad.list = list;
        m_pendingLoad.selectFirst = true;
        startLoadAsync(list);
    } else {
        fillFromProvider(list); // 静态取所持条目（切换后为空）
        st.entriesLoaded = true;
        if (list->count() > 0)
            list->setCurrentRow(0); // 选中第一项，不开始播放
        emit contentsChanged();     // A17：静态重建条目数可能变化，刷新导航启停
    }
    savePlaylists();
}

#include "pch.h"

#include "PlaylistWidget.h"

#include "Constants.h"
#include "HttpClient.h"
#include "ListSettingsDialog.h"
#include "LoadingOverlay.h"
#include "MediaInfoHelper.h"
#include "MpvController.h"
#include "PlaylistDelegate.h"
#include "ServiceRegistry.h"
#include "StaticProvider.h"
#include "TraversalPolicy.h"
#include "Util.h"

PlaylistWidget::PlaylistWidget(const QString &configFile, QWidget *parent)
    : QTabWidget(parent), m_configFile(configFile),
      m_registry(std::make_shared<ServiceRegistry>()) {
    // 注册表在初始化列表创建——创建是本对象的初始化职责，不归任何一个装配函数；
    // 此后 m_registry 恒非空，两个 provide 函数只做"读 ini + 覆盖内部实例"，
    // 彼此独立、可任意次序调用
    // N35：装配仍须前置——createList 出生即配 StaticProvider 并注入 registry，
    // 两个装配函数仅读 ini，不依赖任何控件
    provideHttpServices();
    provideTraversalPolicy();
    setAcceptDrops(true);
    setDocumentMode(true);
    setMinimumWidth(220);
    auto *defTab = createList();
    addTab(defTab, tr("Default"));
    // N30：默认 tab 内容不经枚举取得，由两条直写路径维护（setDefaultTabFiles / 回刷收编），
    // 恒视为已就位——否则切回默认 tab 会发起枚举，而其 StaticProvider 只在刷新回推时才
    // setItems，枚举必返回空表，把直写内容清空。刷新按 refreshCurrentTab 显式发起枚举，
    // 不受此标志拦截（N28 的"默认 tab 不参与枚举"口径随本篇改写）
    m_loadState[defTab].entriesLoaded = true;
    connect(this, &QTabWidget::currentChanged, this, &PlaylistWidget::onTabChanged);
    // N19：遮罩最后创建并置顶；延迟显示定时器可取消
    m_overlay = new LoadingOverlay(this);
    m_overlay->raise();
    m_overlayTimer.setSingleShot(true);
    connect(&m_overlayTimer, &QTimer::timeout, this,
            [this] { m_overlay->begin(m_overlayMode, m_overlayTotal); });
    // N32：取消请求经转发 lambda 落到当前槽位的 future（信号只连一次，future 随任务更换，
    // 不能直连）；转发前判本批次"是否可取消"——枚举批次不可取消，且遮罩在 Loading 模式下
    // 没有取消按钮，双重保证。N35 修订 2：取消 = 旗置位（任务体下一边界退出、在途结果
    // 丢弃）+ onAbandon 锁内搬走已算批次注入缓存（N32 缓存保留语义）+ abandonJob 立即
    // 收尾 GUI（腾槽、代际 +1、忙时与遮罩即复位），不等在途求值返回
    connect(m_overlay, &LoadingOverlay::cancelRequested, this, [this] {
        if (!m_job.cancelable || !m_job.future.isValid())
            return;
        m_job.future.cancel();
        if (m_job.onAbandon)
            m_job.onAbandon();
        abandonJob();
    });
}

// N32：析构不再等待。任务体只捕获入参副本（提供者 shared_ptr / 实体快照），不持 this；
// watcher 以本对象为 parent 与接收者上下文，析构即断线，投递不到已析构对象——
// 原 wait(2000) 的三个理由（QThread 析构约束、alive 旗 → invokeMethod 的 TOCTOU 窗口、
// 线程卡死兜底）随之消灭。此处只做一次非阻塞收尾：作废并 cancel 在途任务，尽快释放
// IO / MediaInfo 句柄、停止无谓计算，不为安全（cancel 尽力而为——卡在单条调用内时
// 最坏延后一条）。
// m_pendingTab 必须先清零：abandonJob 内的 setBusy(false) 会 flushPendingTab，
// 那会在析构中途经 onTabChanged 再起一个新任务
PlaylistWidget::~PlaylistWidget() {
    m_pendingTab = -1;
    abandonJob();
}

// ——N35 服务注册表装配——

// 读 ini 两项构建 HttpClient 缺省参数并 provide。初装配与重装配共用：
// 注册表身份恒定（初始化列表创建一次，此后恒非空），重装配只覆盖内部
// HttpClient 实例——实体持有的 registry 指针不变，下一次求值 resolve 即得新 client；
// 在途单请求不受影响（旧 client 副本随任务存活），条目粒度切换，可接受（N35 §2.3）
void PlaylistWidget::provideHttpServices() {
    int timeoutSec = App::remoteTimeoutSecDefault();
    bool ignoreSsl = App::ignoreHttpsErrorsDefault();
    if (!m_configFile.isEmpty()) {
        QSettings s(m_configFile, QSettings::IniFormat);
        bool ok = false;
        const int v = s.value(QStringLiteral("remoteTimeoutSec"), timeoutSec).toInt(&ok);
        if (ok && v >= 1 && v <= 120) // 与设置目录 range 同界，读侧自钳
            timeoutSec = v;
        ignoreSsl = s.value(QStringLiteral("ignoreHttpsErrors"), ignoreSsl).toBool();
    }
    HttpClient::Config cfg;
    cfg.timeoutMs = timeoutSec * 1000; // ini 记秒，Config 记毫秒（N34）
    cfg.ignoreSslErrors = ignoreSsl;
    m_registry->provide<HttpClient>(std::make_shared<HttpClient>(cfg));
}

// N36：遍历策略同路——读 ini 构建 TraversalPolicy 快照 provide 进同一注册表
// （m_registry 于初始化列表创建，恒非空）；取值侧在 DirProvider::enumerate 起始处 resolve
void PlaylistWidget::provideTraversalPolicy() {
    int depthLimit = App::recursiveDepthLimitDefault();
    if (!m_configFile.isEmpty()) {
        QSettings s(m_configFile, QSettings::IniFormat);
        bool ok = false;
        const int v = s.value(QStringLiteral("recursiveDepthLimit"), depthLimit).toInt(&ok);
        // 读侧自钳：上界与设置目录 range 同源（见 Constants.h）；下界恒 0
        if (ok && v >= 0 && v <= App::recursiveDepthLimitMax())
            depthLimit = v;
    }
    auto policy = std::make_shared<TraversalPolicy>();
    policy->depthLimit = depthLimit;
    m_registry->provide<TraversalPolicy>(std::move(policy));
}

void PlaylistWidget::onPluginIniKeysChanged(const QStringList &keys) {
    if (keys.contains(QStringLiteral("remoteTimeoutSec")) ||
        keys.contains(QStringLiteral("ignoreHttpsErrors")))
        provideHttpServices();
    if (keys.contains(QStringLiteral("recursiveDepthLimit")))
        provideTraversalPolicy();
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
    // N9：每个 tab 出生即配静态提供者（默认 tab 同样，仅持久化侧跳过）；
    // N35：registry 随构造注入（基类持有）；m_registry 于初始化列表创建，恒非空
    m_providerOf.emplace(list, std::make_shared<StaticProvider>(m_registry));
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

// 行与实体的角色绑定（建行与换绑共用）：显示名 / 图标 / tooltip / EntityRole
void PlaylistWidget::setItemEntity(QListWidgetItem *item, const PlaylistEntity::Ptr &entity) {
    item->setData(Qt::DisplayRole, entity->title());
    item->setData(Qt::DecorationRole, entity->icon());
    item->setData(EntityRole, QVariant::fromValue<quintptr>(quintptr(entity.get())));
    item->setToolTip(entity->playUrl());
}

// 统一的条目构造：显示名 / 图标 / tooltip 全部取自实体；
// EntityRole 存实体裸指针（quintptr），实体所有权随行同步 push 进容器
QListWidgetItem *PlaylistWidget::makeItem(QListWidget *list, PlaylistEntity::Ptr entity) {
    auto *item = new QListWidgetItem(list);
    setItemEntity(item, entity);
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
    // N32：默认 tab 亦会异步刷新，收编重建即作废在途批次（cancel + 递增代际 + 清槽位 +
    // 复位忙时与遮罩由 abandonJob 一次完成）。仅当在途批次的目标表正是本表才作废——
    // 否则会误杀他表的在途批次
    if (m_job.target == def)
        abandonJob();

    // N31：清前快照收编源实体，建 playUrl → 源实体索引（constFind 拷贝只延长源寿命，
    // 自覆盖情形清表后克隆仍可用）。同 playUrl 取首个；首个未解析而重复项已解析时
    // 复用落空无正确性代价（缺失元数据按需链自补）
    QHash<QString, PlaylistEntity::Ptr> reuse;
    if (const auto it = m_entities.constFind(list); it != m_entities.end()) {
        for (const auto &src : it.value()) {
            if (src && !reuse.contains(src->playUrl()))
                reuse.insert(src->playUrl(), src);
        }
    }

    // 记录收编前的选中行（单选）：推送无 current 项（如停止态外部编辑）时
    // 整表替换会把界面选中清空（仅显示问题），沿用旧重建语义恢复
    const int prevRow = def->currentRow();
    def->clear();
    m_entities[def].clear();                  // 同步点：收编重建＝先清容器再随建行 push_back
    m_loadState[def].mediaMetaLoaded = false; // N29：条目集合已变，两类已读元数据一并失效
    m_loadState[def].fileMetaLoaded = false;

    QListWidgetItem *newCurrent = nullptr;
    for (int i = 0; i < pushed.size(); ++i) {
        // N31：命中索引复用源实体克隆（携带已解析缓存，playUrl 与回显串逐字节相等，
        // 不破静默不变式）；未命中经工厂新建（网络路径落 UnknownEntity）
        const auto rit = reuse.constFind(pushed.at(i));
        PlaylistEntity::Ptr entity = rit != reuse.constEnd()
                                         ? rit.value()->clone()
                                         : PlaylistEntity::fromPlayUrl(pushed.at(i), m_registry);
        QListWidgetItem *item = makeItem(def, std::move(entity));
        if (current.at(i)) {
            item->setData(PlayingRole, true);
            newCurrent = item;
        }
    }
    // N31：收编内容序 == mpv 会话序，随收编复位默认 tab 排序规则为无序（保留方向，
    // 与 addFiles 同一形状）；否则存量元数据规则会让下次切入经 reactivateRescan
    // 重排并四步式重推，吃掉推送序
    if (auto *p = providerAt(0))
        p->setSortRule(int(SortKey::None), p->sortAscending());
    if (newCurrent) {
        def->setCurrentItem(newCurrent);
        // 跟随滚动（N4）：正播项变动时最小位移滚入可见范围
        def->scrollToItem(newCurrent, QListWidget::EnsureVisible);
    } else if (prevRow >= 0 && prevRow < def->count()) {
        def->setCurrentRow(prevRow);
    }
    markPlayingTab(0); // B10：异 tab 转移清旧标记；同 tab 早退（默认 tab 自覆盖不误清）
    // N30：记忆不整体作废——按正播行重建（定位不到才作废）。整体 remove 会让随后的
    // 排序 / 移除走四步式时 resolveResumeRow 无命中，表现为"操作后播放直接停掉"
    rebuildMemoryAfterAdopt(def, newCurrent ? def->row(newCurrent) : -1);
    m_loadState[def].entriesLoaded = true; // N30：收编等同直写，内容已就位
    emit contentsChanged();                // A17：条目数已变，刷新导航启停
    emit playingListAdopted();             // N25：镜像已重新一致，MainWindow 刷新标题/OSD 正播名
    requestTab(0);                         // 可见 tab 跟随；N19 忙时排队，枚举/预读结束后补执行
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
    m_loadState[list].mediaMetaLoaded = false; // N29：条目集合变化，两类已读元数据一并失效
    m_loadState[list].fileMetaLoaded = false;

    // N25：本地先行——播放中 tab 也立即插行（回刷只会把"本地未改"的差异搬去默认 tab），
    // 插入后本地序列即等于 mpv 追加后的序列，回刷恒一致而静默
    for (const QString &file : files)
        makeItem(list, PlaylistEntity::fromPlayUrl(file, m_registry));
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
    // N30：主菜单"打开文件"与书签跳转不受 m_busy 限制，此时可能有本表的刷新批次在途。
    // 不作废则枚举收尾到达后用回推的旧内容整表覆盖，视图回到旧内容而 mpv 在播新文件。
    // N32：统一走 abandonJob（仅当在途批次的目标表即本表）
    if (m_job.target == list)
        abandonJob();
    if (m_pendingLoad.list == list)
        m_pendingLoad = PendingLoad{};
    // 内容由本函数直写即已就位：不置回会让下次切入默认 tab 误触发一次枚举，
    // 用提供者里的旧 m_urls 覆盖刚打开的文件
    m_loadState[list].entriesLoaded = true;
    m_loadState[list].loadError.clear();
    m_memory.remove(list);
    list->clear();
    m_entities[list].clear();                  // 同步点：重建＝先清容器再随建行 push_back
    m_loadState[list].mediaMetaLoaded = false; // N29：条目集合变化，两类已读元数据一并失效
    m_loadState[list].fileMetaLoaded = false;
    for (const QString &file : files)
        makeItem(list, PlaylistEntity::fromPlayUrl(file, m_registry));
    // N31：内容序 == 打开顺序，随直写复位排序规则为无序（保留方向）——否则存量
    // 元数据规则会在下次切入默认 tab 时经 reactivateRescan 重排，吃掉刚打开的顺序。
    // 静态源 enumerate 不读排序键，与被作废的在途枚举批次无实际竞争
    if (auto *p = providerAt(0))
        p->setSortRule(int(SortKey::None), p->sortAscending());
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

// N30：回刷收编后的记忆重建（替代原先的整体作废）。内容虽被整体替换，mpv 仍在播
// 新表里的某一项——按正播行重建记忆，使随后的四步式（排序 / 移除）恒有续播目标。
// 优先用推送自带的 current 行；推送无 current 时退回 mpv 的 path 定位
// （与 restorePlayingState 同口径）。两者都定位不到即 mpv 未播，此时才作废记忆——
// 与"不在播就不续播"同义，不是记忆缺失
void PlaylistWidget::rebuildMemoryAfterAdopt(QListWidget *list, int playingRow) {
    if (!list)
        return;
    int row = playingRow;
    if (row < 0 && m_mpv) {
        const QString cur =
            Util::normalizePathForMatch(m_mpv->propertyString(QStringLiteral("path")));
        if (!cur.isEmpty()) {
            for (int i = 0; i < list->count(); ++i) {
                if (const PlaylistEntity *e = entityFromItem(list->item(i)); e && e->key() == cur) {
                    row = i;
                    break;
                }
            }
        }
    }
    const PlaylistEntity *entity =
        (row >= 0 && row < list->count()) ? entityFromItem(list->item(row)) : nullptr;
    if (!entity) {
        m_memory.remove(list);
        return;
    }
    TabMemory mem;
    mem.lastPos = row;
    mem.lastPath = entity->key();
    // 进度即时取一次：播放中随后会被 timePosChanged 心跳覆盖，暂停态没有心跳，
    // 不取则记忆缺进度，四步式重推后回到开头（同 recordPlayback 的写法）
    mem.lastTimePos = m_mpv ? m_mpv->propertyDouble(QStringLiteral("time-pos"), -1.0) : -1.0;
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
        // 内容统一经 enumerate 取得（N28：静态表同样延迟到首次激活）；
        // N35：registry 转交（静态持久化的 http 条目重载后仍得 HttpEntity）
        auto provider = PlaylistProvider::load(settings, m_registry);
        settings.endGroup();
        // insert_or_assign：createList 已预置静态默认，此处必须覆盖
        m_providerOf.insert_or_assign(list, std::move(provider));
        if (mem.lastPos >= 0 || !mem.lastPath.isEmpty())
            m_memory[list] = mem;
        // N28：所有 tab 内容一律经异步枚举填充，延迟到首次激活
        m_loadState[list].entriesLoaded = false;
        // N28：状态装配完毕再挂 tab。addTab 一旦触发 currentChanged，onTabChanged
        // 会拿尚未装配到位的默认提供者发起首载，把本该来自 INI 的内容填成空表；
        // 先备齐提供者 / 记忆 / 加载态再挂，根除该窗口
        addTab(list, name);
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
        // 落盘内容恒为参数与排序键。回传与落盘是同一动作的两半，不得拆开。
        // N28：回传守卫——仅对 entriesLoaded 为真的表执行：未加载表实体集合为空，
        // 回传会清空 StaticProvider::m_urls 造成永久丢数据，跳过即保住提供者自持内容。
        // 先判状态再取实体容器，避免 entitiesOf 给未加载表插入空实体条目。
        // 在途加载的目标表 entriesLoaded 恒为 false（onTabChanged 只在未加载时发起，
        // refreshCurrentTab 先清标志再发起），故 setItems 与工作线程的 enumerate()
        // 不可能并发——日后改动 entriesLoaded 置位时序须同步复核本条
        if (provider && provider->supportsModify() && m_loadState.value(list).entriesLoaded)
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
    // N19：未加载的 tab 走首次激活链（选中/滚动在枚举续作收尾）。
    // N28：所有 tab 创建 / 换源 / 会话恢复时 entriesLoaded 均为 false，
    // 内容统一在此经异步枚举填充；N30：默认 tab 内容由两条直写路径维护，该标志
    // 恒为"已就位"，故不在此发起枚举（其刷新经 refreshCurrentTab 显式发起）
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

// N29：已加载 tab 切入重读判定——外围条件自判，类别/支持者/标志位三要素
// 经共用内核（与填充跳过、首载收尾同一判据，防多点位漂移）
bool PlaylistWidget::needsMetaRescan(int tabIndex) const {
    if (m_busy)
        return false;
    const QListWidget *list = listAt(tabIndex);
    if (!list)
        return false;
    const auto lit = m_loadState.constFind(list); // 只读引用，免整结构体拷贝
    if (lit == m_loadState.end() || !lit->entriesLoaded || !lit->loadError.isEmpty())
        return false; // 占位 tab（loadError 非空）走失败重试，不走预读
    const PlaylistProvider *p = providerAt(tabIndex);
    if (!p)
        return false;
    return pendingMetaCategory(list, SortKey(p->sortKey())) != MetaCategory::None;
}

// N30/N32：切入已加载 tab 的重读——与刷新共用同一套"比对 → 三分支"，差别只在"不换入"。
// N32 起为薄壳：只组装意图上下文（旧值采集挪进推进函数，见 advanceTab），
// 预读在任务槽内异步跑，不再有"同步路径不传 generation"这条分支
void PlaylistWidget::reactivateRescan(QListWidget *list, int tabIndex) {
    if (!list)
        return;
    clearPlaceholder(list); // 与枚举收尾对称：占位行不参与比对，也不进实体序列
    JobContext ctx;
    ctx.intent = Intent::Reactivate;
    ctx.list = list;
    ctx.tab = tabIndex;
    advanceTab(ctx, false);
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
    return it == m_providerOf.end() ? nullptr : it.value().get();
}

bool PlaylistWidget::isModifiableTab(int tabIndex) const {
    const auto *p = providerAt(tabIndex);
    Q_ASSERT(listAt(tabIndex) == nullptr || p != nullptr); // I1：tab 与 provider 一一对应
    return p != nullptr && p->supportsModify();
}

// N30：行级换入——不 clear、不销毁行对象（保住 PlayingRole 与选中 / 滚动连续）。
// ① 前 min(旧行数, 新实体数) 行逐行换绑（先改行角色再替容器元素，零悬空窗口）；
// ② 新实体更多则经 makeItem 追加；③ 更少则删尾部行并同步缩容器。
// 旧实体连同 fileMeta / mediaMeta / icon 缓存全部丢弃——换绑即换新，保留任何缓存都不叫刷新。
// 调用前须已清占位行（无占位行时容器下标 == 行号）
void PlaylistWidget::applyEntitiesToRows(QListWidget *list, PlaylistEntity::List &&entities) {
    if (!list)
        return;
    const int oldRows = list->count();
    const int n = int(entities.size());
    auto &owned = entitiesOf(list);
    const int m = qMin(oldRows, n);
    if (int(owned.size()) < m)
        owned.resize(m); // 不变式兜底：容器不得短于待换绑行数，防下标越界
    for (int i = 0; i < m; ++i) {
        PlaylistEntity::Ptr fresh = std::move(entities[i]);
        setItemEntity(list->item(i), fresh);
        owned[i] = std::move(fresh); // 旧实体在此销毁：行已指向新实体
    }
    for (int i = m; i < n; ++i)
        makeItem(list, std::move(entities[i]));
    for (int i = oldRows - 1; i >= n; --i)
        delete list->takeItem(i); // 先删行（此时实体仍在），再缩容器
    owned.resize(n);              // 容器序恒等于行序
}

// ——N32 上下文采集（一律到"用到它的那一步之前一刻"才采）——

// 行序 key 序列（比对用；占位行跳过）
QStringList PlaylistWidget::keysOf(const QListWidget *list) const {
    QStringList keys;
    if (!list)
        return keys;
    for (int i = 0; i < list->count(); ++i) {
        if (isPlaceholderRow(list->item(i)))
            continue;
        if (const PlaylistEntity *e = entityFromItem(list->item(i)))
            keys << e->key();
    }
    return keys;
}

// 换入前的视图快照（换入后行号语义失效，按记忆行 key 复原）
PlaylistWidget::ViewSnapshot PlaylistWidget::captureViewSnapshot(QListWidget *list) const {
    ViewSnapshot snap;
    if (!list)
        return snap;
    const bool hadRows = list->count() > 0;
    snap.memoryPath = hadRows ? captureMemoryPath(list) : QString{};
    snap.hadMemory = hadRows && m_memory.contains(list) && m_memory.value(list).lastPos >= 0;
    return snap;
}

// N30：记忆修正 + 选中与滚动，幂等（行是否换过都可调用）。
// 播放表不在此滚动——滚动与选中由归位一次性完成，否则视觉跳两下
void PlaylistWidget::restoreViewState(QListWidget *list, int tab, const ViewSnapshot &snap) {
    if (!list)
        return;
    if (snap.selectFirst) {
        if (list->count() > 0)
            list->setCurrentRow(0); // 列表设置换源：选中首项，不播放
        return;                     // 换源已清除记忆，无记忆可复；不得被 activateTab 覆盖
    }
    restoreMemoryPath(list, snap.memoryPath, snap.hadMemory);
    if (tab >= 0 && tab == currentIndex() && tab != m_playingTab)
        activateTab(tab); // 非播放表：按记忆行选中并滚动
}

// N30：播放态归位——标记与播放表选中的唯一出口。行级换入不再 clear()，标记没有天然
// 清除路径，故对每个未走四步式的分支在收尾末尾调用一次：在播且能定位即重建在正播行，
// 否则清除本表全部标记。走了四步式的分支在此早退——stop 只是入队，此刻读到的
// path / idle-active 仍是发起前的值，按旧值判断会把"未重推"误判为在播
void PlaylistWidget::restorePlayingState(QListWidget *list, int tab) {
    if (!list || tab < 0 || tab != m_playingTab)
        return;
    const bool idle = m_mpv ? m_mpv->propertyFlag(QStringLiteral("idle-active"), false) : true;
    const QString cur =
        m_mpv ? Util::normalizePathForMatch(m_mpv->propertyString(QStringLiteral("path")))
              : QString();
    int target = -1;
    if (!idle && !cur.isEmpty()) { // path 为空即无正播 → 清除
        for (int i = 0; i < list->count(); ++i) {
            if (const PlaylistEntity *e = entityFromItem(list->item(i)); e && e->key() == cur) {
                target = i;
                break;
            }
        }
    }
    clearPlayingMarks(list); // 先全清，再按定位结果单点重建
    if (target < 0) {
        // N30：定位失败（停止态 / 正播项已不在本表）不置标记，但选中仍需归属——
        // restoreViewState 对播放表刻意不设选中（免与归位重复滚动），此处是它唯一的
        // 补偿点。不可交 activateTab：其播放表分支只滚标记行不改选，而停止态标记
        // 已清，故在此按记忆行直接设选并滚动，停止态刷新后仍可见上次播放项
        if (tab == currentIndex()) {
            if (const int row = resolveResumeRow(tab); row >= 0 && row < list->count()) {
                list->setCurrentRow(row);
                list->scrollToItem(list->item(row), QListWidget::PositionAtCenter);
            }
        }
        return;
    }
    QListWidgetItem *item = list->item(target);
    item->setData(PlayingRole, true);
    list->setCurrentItem(item);
    list->scrollToItem(item, QListWidget::EnsureVisible);
}

// N30：四步式唯一入口——pause 只有一处读取点，四处入口（刷新与切入的"其它"分支、
// applySort、executeRemoval）共用。前置校验由调用点完成；
// mutate = 停止之后、重推之前的结构变更（重排 / 删行；调用前已排好则传空）
// N30：清除本表全部播放标记。stop 已入队时 mpv 属性查询反映的是发起前的状态，
// 故此类出口一律显式清除，不按归位判断
void PlaylistWidget::clearPlayingMarks(QListWidget *list) {
    if (!list)
        return;
    for (int i = 0; i < list->count(); ++i)
        list->item(i)->setData(PlayingRole, false);
}

void PlaylistWidget::runFourStep(QListWidget *list, int tab, const std::function<void()> &mutate) {
    const bool keepPaused =
        m_mpv ? m_mpv->propertyFlag(QStringLiteral("pause"), false) : false; // stop 之前读
    emit stopRequested();
    clearPlayingMarks(list); // stop 即 mpv 列表清空，本地标记同步失效
    if (mutate)
        mutate();
    const int idx = resolveResumeRow(tab); // 记忆定位：序号校验优先，失败按路径取首命中
    if (idx >= 0)
        emit playRequested(filesOf(tab), idx, lastTimePosOf(tab), keepPaused);
    // idx < 0：记忆条目已在本次变更中消失，保持停止，标记保持清除态
}

// ——N32 任务槽（枚举与预读共用，全插件仅一份）——

// 唯一出口（六步，顺序不可交换）。投递的收尾不外乎"取判定 → 校验身份 → 腾槽 → 续作 →
// 条件复位 → 警告弹框"这一条链：任务体只算不写，一切落到视图的动作都在续作里发生。
// 忙时与遮罩的复位只在此处（另一处是 abandonJob 的显式作废），且必须晚于续作——
// 续作起的下一轮任务会重新置忙与起遮罩，先复位会取消它的忙时与遮罩并清掉它的槽位
void PlaylistWidget::runJob(const JobContext &ctx, LoadingOverlay::Mode mode, int total,
                            bool cancelable, std::function<void(QPromise<void> &)> body,
                            std::function<void(JobVerdict)> cont, std::function<void()> onAbandon) {
    Q_ASSERT(!m_job.active()); // 单飞行：全插件同一时刻至多一个在途任务
    setBusy(true);
    armOverlay(mode, total);

    m_job.token = ++m_token; // 代际前进（读旧批次的一律失配）
    m_job.target = ctx.list;
    m_job.cancelable = cancelable;
    m_job.onAbandon = std::move(onAbandon);
    auto *watcher = new QFutureWatcher<void>(this); // parent = 本对象：析构即断线
    m_job.watcher = watcher;

    // 收尾体：幂等（靠身份判定），供 finished 信号与"起跑即结束"两条路径共用。
    // 身份 = 本 watcher 仍是当前槽位项 + 代际未变；两者任一失配即"批次已作废，什么都不碰"
    const quint64 jobToken = m_job.token;
    auto finish = [this, watcher, jobToken, ctx, cont = std::move(cont)] {
        if (m_job.watcher != watcher || m_job.token != jobToken) {
            watcher->deleteLater(); // 早退不得漏销 watcher
            return;
        }
        const JobVerdict verdict = watcher->isCanceled() ? JobVerdict::Cancelled : JobVerdict::Ok;
        // ③ 腾出槽位（future 复位为默认构造，避免槽里留一个已完成的 future）
        m_job.future = QFuture<void>();
        m_job.target = nullptr;
        m_job.watcher = nullptr;
        m_job.onAbandon = nullptr;
        watcher->deleteLater();
        // ④ 续作（可能起下一轮任务并占用槽位）
        cont(verdict);
        // ⑤ 槽位仍空才复位——setBusy(false) 内含 flushPendingTab，必为最后一步
        if (!m_job.active())
            setBusy(false);
        // ⑥ N37：枚举警告弹框——落在忙时与遮罩复位之后，经零延时单发投递，
        // 不占续作的栈（模态框若开在续作内会嵌套事件循环，可能与 flushPendingTab
        // 重入）。续作起了下一轮任务时本步跳过，警告自动延后到那一轮的第⑥步；
        // 无警告零成本（Discarded 批次不累积，不会误弹）
        if (!m_enumWarnings.isEmpty() && !m_job.active()) {
            const QStringList lines = std::move(m_enumWarnings);
            m_enumWarnings.clear();
            QTimer::singleShot(0, this, [this, lines] {
                // 0ms 窗口内可能已有新任务起跑（连续刷新）：命中即放回队首，
                // 随那一轮的第⑥步再弹——弹框时机恒落空闲态
                if (m_busy || m_job.active()) {
                    m_enumWarnings = lines + m_enumWarnings;
                    return;
                }
                QString text = lines.join(QLatin1Char('\n'));
                if (lines.size() > 10) {
                    // 封顶 10 行：防上百个不可读子目录撑爆对话框
                    text = QStringList(lines.mid(0, 10)).join(QLatin1Char('\n')) +
                           QStringLiteral("\n") + tr("…and %1 more").arg(lines.size() - 10);
                }
                QMessageBox::warning(this, tr("Enumeration Warnings"), text);
            });
        }
    };
    // 只接 finished 一个信号：Qt 侧语义为取消后 future 亦以 finished 收尾
    // （RunFunctionTaskBase::run() 取消时走 reportFinished()；启动前取消则不执行任务体），
    // 取消与否由 isCanceled() 判定，无需 canceled / finished 双信号幂等
    connect(watcher, &QFutureWatcher<void>::finished, this, finish);
    // 进度：任务内按时间节流 setProgressValue，这里直连遮罩（total 由入参捕获，
    // progressValueChanged 只给值）
    connect(watcher, &QFutureWatcher<void>::progressValueChanged, this,
            [this, total](int value) { m_overlay->setProgress(value, total); });

    m_job.future = QtConcurrent::run([body = std::move(body)](QPromise<void> &promise) {
        body(promise); // 任务体：只捕获入参副本，绝不捕获 this / widget / 成员指针
    });
    watcher->setFuture(m_job.future);
    // 防御：任务可能在 setFuture 之前就跑完（池线程空闲 + 任务极短），此时 Qt 是否补发
    // finished 不在文档保证之内——本轮不依赖该语义，起跑后立刻补判一次；递延到事件循环
    // 以免在发起栈内同步跑续作。两条路径都走 finish，由槽位身份判定兜住重复
    if (m_job.future.isFinished())
        QMetaObject::invokeMethod(this, finish, Qt::QueuedConnection);
}

// N19/N26/N28/N29/N32：所有 tab 首次激活 / 刷新 / 换源统一走本函数异步枚举。
// 任务体经 shared_ptr 调用 provider->enumerate()（实体构造与提供者侧元数据注入都在
// 工作线程）——换源/删 tab 后旧提供者的销毁被自动延后到任务结束，无 use-after-free。
// 不变式：在途期间 GUI 线程不修改该提供者的可变状态——目录源成员构造后只读，
// 增删/换源/删 tab 均被 m_busy 拦截；静态源的 setItems/setSortRule 经 savePlaylists
// 的回传守卫（未加载表不回传）被跳过，不可能与任务线程的 enumerate() 并发。
// 跨线程只带 shared_ptr 与实体快照：QPointer 禁止在任务线程读写，有效性由任务槽判定
void PlaylistWidget::startLoadAsync(QListWidget *list) {
    if (!list)
        return;
    const auto it = m_providerOf.find(list);
    if (it == m_providerOf.end())
        return;                 // 防御：提供者缺失不加载
    auto provider = it.value(); // 任务持有 shared_ptr 副本：枚举期间提供者恒存活

    // 意图在发起处定死并按值随任务走（异步边界不靠成员承载）。m_pendingLoad 只作
    // 发起期意图来源：一次性消费 + 清空，失配残留一并清掉，杜绝污染后续首载
    const PendingLoad pend = (m_pendingLoad.list == list) ? m_pendingLoad : PendingLoad{};
    m_pendingLoad = PendingLoad{};

    clearPlaceholder(list); // 失败重试先清占位
    JobContext ctx;
    ctx.intent = pend.refresh ? Intent::Refresh
                              : (pend.selectFirst ? Intent::SwitchSource : Intent::InitialLoad);
    ctx.list = list;
    ctx.tab = indexOf(list);

    auto result = std::make_shared<std::expected<PlaylistProvider::Enumeration, QString>>();
    runJob(
        ctx, LoadingOverlay::Mode::Loading, 0, /*cancelable=*/false,
        [provider, result](QPromise<void> &) {
            *result = provider->enumerate(); // 工作线程：实体构造 + 元数据注入仅此一步
        },
        [this, ctx, result](JobVerdict v) { onLoadFinished(ctx, *result, v); });
}

// N30：刷新收尾的播放同步——排序后逐行比对旧 key 序列，三分支选一。
// 前缀比较已蕴含"只增不删、相对顺序未变"，mpv 的增量通道只能追加，只有这一形状可静默。
// 返回是否走了四步式（走了则由回刷重建标记，归位不介入）
bool PlaylistWidget::syncAfterRefresh(QListWidget *list, int tab, const QStringList &oldKeys) {
    const int n = oldKeys.size();
    bool prefix = list->count() >= n; // 行数更少必然不是前缀一致
    for (int i = 0; prefix && i < n; ++i) {
        const PlaylistEntity *e = entityFromItem(list->item(i));
        prefix = e && e->key() == oldKeys.at(i) && !isPlaceholderRow(list->item(i));
    }
    // 正播时长回填（三分支通用）：静默与追加不重建播放列表，正播行时长不会再有 mpv 推送补回。
    // 不前置判断——duration <= 0 由 applyDuration 记 nullopt，时长列回落到实体缓存元数据
    if (m_mpv && tab >= 0 && tab == m_playingTab) {
        const QString cur =
            Util::normalizePathForMatch(m_mpv->propertyString(QStringLiteral("path")));
        if (!cur.isEmpty()) {
            for (int i = 0; i < list->count(); ++i) {
                if (const PlaylistEntity *e = entityFromItem(list->item(i)); e && e->key() == cur) {
                    applyDuration(e->playUrl(),
                                  m_mpv->propertyDouble(QStringLiteral("duration"), -1.0));
                    break;
                }
            }
        }
    }

    if (prefix && list->count() == n)
        return false; // 完全相同：静默，mpv 不动
    // 守卫：停止态 / 非播放表 / 空表刷出内容一律只做视图更新（append-play 在空列表时会起播）
    const bool canSync =
        tab == m_playingTab && !oldKeys.isEmpty() &&
        !(m_mpv ? m_mpv->propertyFlag(QStringLiteral("idle-active"), false) : true);
    if (!canSync)
        return false;
    if (prefix) { // 末尾追加：登记在途回显后追加新项，播放零中断
        const QStringList files = filesOf(tab);
        const QStringList added = files.mid(n);
        if (!added.isEmpty()) {
            noteLocalPush(files); // 登记先于发射：避免 mpv 回刷被判为外部变更
            emit filesAdded(added);
        }
        return false;
    }
    runFourStep(list, tab, nullptr); // 收尾前已排好，mutate 为空
    return true;
}

// N32：枚举续作（GUI 线程，唯一出口的续作步之内）——换入 → 推进函数。
// 枚举失败与元数据缺失是两回事：占位 / 停播 / 同步目标归位仍是枚举专属，故失败分支
// 不进推进函数；成功分支的后半段（换入之后）一律交 advanceTab。
// 判定的来源：本批次已作废（Discarded）即行未换入、什么都不碰，忙时由唯一出口复位。
// N37：成功批次的 warnings 入 m_enumWarnings（Discarded 首行早退不写，Cancelled
// 防御路径在此返回亦不写），弹框延后到唯一出口第⑥步
void PlaylistWidget::onLoadFinished(const JobContext &ctx,
                                    std::expected<PlaylistProvider::Enumeration, QString> &r,
                                    JobVerdict v) {
    QListWidget *list = ctx.list;
    if (!list || v == JobVerdict::Discarded)
        return;
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
        clearPlayingMarks(list); // N30：旧行保留而 stop 已入队，标记须显式清除（不可按归位判断）
        emit contentsChanged();  // 占位行计入行数，导航启停需刷新
        return;                  // 忙时与遮罩由唯一出口第 ⑤ 步复位（本续作未起新任务）
    }
    if (v == JobVerdict::Cancelled) { // 枚举批次不可取消，此为 abandonJob 之外的防御路径
        finishCanceled(ctx);
        return;
    }
    TabLoadState &st = m_loadState[list];
    st.loadError.clear();
    st.entriesLoaded = true;

    // 换入前一刻才采集（§4.3）：在途期间 mpv 事件仍经 recordPlayback 推进本表记忆，
    // 发起时快照会丢掉这段进度。oldKeys 仅刷新批次需要（供比对；其余意图无旧内容可比）
    PlaylistProvider::Enumeration enumeration = std::move(r.value());
    if (!enumeration.warnings.isEmpty())
        m_enumWarnings.append(enumeration.warnings); // N37：部分失败警告随成功批次入库
    JobContext out = ctx;
    if (ctx.intent == Intent::Refresh)
        out.oldKeys = keysOf(list);
    out.snap = captureViewSnapshot(list);
    out.snap.selectFirst = (ctx.intent == Intent::SwitchSource);
    applyEntitiesToRows(list, std::move(enumeration.entities));
    advanceTab(out, false);
}

// N32 推进函数（业务状态机，唯一入口）：判"还要不要预读"——要就起预读任务并让自己
// 作为其续作（预读成功后置"本表该类已读"标志，再次进入必然落到收尾步，天然终止）；
// 不要就做收尾。fromMetaScan 为真表示本轮预读已跑过，绝不再入预读分支——取消/失配时
// "已读"标志未置位，若再判一次会立刻重起一轮，用户刚取消又被弹回遮罩。
// 收尾顺序照搬现状：排序 → 视图恢复 → 比对与播放同步（含落盘）→ 播放态归位 → contentsChanged
void PlaylistWidget::advanceTab(JobContext ctx, bool fromMetaScan) {
    QListWidget *list = ctx.list;
    if (!list)
        return;
    const int tab = indexOf(list);
    if (tab < 0)
        return;
    const auto *p = providerAt(tab);
    // 判类别不得读提供者的排序键：Sort 意图的规则写点在收尾步，取消即不写，
    // 而"先写规则才能判类别"会让取消也落盘
    const bool isSort = (ctx.intent == Intent::Sort);
    const SortKey key = isSort ? ctx.sortKey : (p ? SortKey(p->sortKey()) : SortKey::None);
    const bool asc = isSort ? ctx.ascending : (p ? p->sortAscending() : true);

    if (!fromMetaScan) {
        const MetaCategory cat = pendingMetaCategory(list, key);
        if (cat != MetaCategory::None) {
            ctx.tab = tab;
            startMetaScan(std::move(ctx), cat); // 续作 = 自己（fromMetaScan=true 再入）
            return;
        }
    }

    // 不换入类（Reactivate）的旧值到此才采集：异步后预读窗口变长，挪到续作内更准；
    // 判定不受影响——restoreMemoryPath 按 key 重新定位，四步式的续播定位同理
    if (ctx.intent == Intent::Reactivate) {
        ctx.snap = captureViewSnapshot(list);
        ctx.oldKeys = keysOf(list);
    }

    if (isSort) {
        // 排序规则写点只此一处（且取消即不写）；applySort 内部自带播放表的四步式
        applySort(list, key, asc, /*writeRule=*/true);
        emit contentsChanged();
        return;
    }

    if (key != SortKey::None)
        sortListItems(list, key, asc);
    restoreViewState(list, tab, ctx.snap);

    bool fourStep = false;
    if (ctx.intent == Intent::Refresh) {
        if (tab > 0 && tab != m_playingTab)
            savePlaylists(); // 非播放非默认 tab 落盘（播放中 tab 不落盘为既有语义）
        else if (tab == m_playingTab)
            fourStep = syncAfterRefresh(list, tab, ctx.oldKeys);
    } else if (ctx.intent == Intent::Reactivate) {
        if (tab == m_playingTab)
            fourStep = syncAfterRefresh(list, tab, ctx.oldKeys);
    }
    if (!fourStep)
        restorePlayingState(list, tab);
    emit contentsChanged(); // 末位补发一次（该信号契约幂等，接收方只刷新导航启停）
}

// N32：预读发起。任务体只算不写（经实体求值转发取回值），结果按快照实体成批交出；
// 续作把批次注入快照实体——这是实体缓存的唯一写点，故缓存（含绘制路径读的）只在 GUI 线程写。
// 快照天然正确：活动容器的行序会被换源 / 删行 / 排序改变，而快照注入按实体而非行号
void PlaylistWidget::startMetaScan(JobContext ctx, MetaCategory cat) {
    QListWidget *list = ctx.list;
    if (!list || cat == MetaCategory::None)
        return;
    // MediaInfo 进线程池的前提①：显式预载一次（shim 的模块加载惰性且无锁）。
    // 此调用必在 GUI 线程、必在起任务之前——此后任务线程对 shim 只剩只读
    if (cat == MetaCategory::Media)
        MediaInfoHelper::available();

    auto pending = std::make_shared<PlaylistEntity::List>(); // 待求值者（共享计数快照）
    int baseline = 0; // 已求值 / 不支持者：无需计算，但计入进度分母（现状口径）
    if (const auto it = m_entities.constFind(list); it != m_entities.end()) {
        for (const auto &entity : it.value()) {
            // 能力位为假的类别不进求值；已求值者（含"值未知"入缓存）也不重算——
            // 缓存判定必须在 GUI 线程做：任务只算不写，读缓存会与 GUI 侧写入构成数据竞争
            const bool supported = cat == MetaCategory::File ? entity->supportsFileMeta()
                                                             : entity->supportsMediaMeta();
            const bool cached = cat == MetaCategory::File ? entity->cachedFileMeta() != nullptr
                                                          : entity->cachedMediaMeta() != nullptr;
            if (!supported || cached) {
                ++baseline;
                continue;
            }
            pending->push_back(entity);
        }
    }
    const int total = baseline + int(pending->size()); // 分母恒为条目总数（含命中与不支持者）
    auto result = std::make_shared<MetaResult>();
    result->cat = cat;

    runJob(
        ctx, LoadingOverlay::Mode::MetaScan, total, /*cancelable=*/true,
        [pending, result, cat, baseline, total](QPromise<void> &promise) {
            promise.setProgressRange(0, total);
            promise.setProgressValue(baseline);
            QElapsedTimer throttle;
            throttle.start();
            int done = baseline;
            // N35 修订 2：同步求值（远端 HEAD 阻塞至完成/超时，无取消通道）。
            // 逐项边界检查取消：命中即不再发起后续项；在途项求值返回后其结果
            // 依 abandoned 旗静默丢弃（不注入不缓存，下次预读重试）。取消时
            // GUI 侧经 onAbandon 立即收尾（锁内搬走已算批次注入缓存），不等本任务
            for (const auto &entity : *pending) {
                if (promise.isCanceled())
                    break;
                if (cat == MetaCategory::File) {
                    const FileMeta fm = entity->evalFileMeta();
                    std::lock_guard<std::mutex> lk(result->mutex);
                    if (result->abandoned)
                        break;
                    result->entities.push_back(entity);
                    result->fileMeta.push_back(fm);
                } else {
                    const auto mm = entity->evalMediaMeta();
                    std::lock_guard<std::mutex> lk(result->mutex);
                    if (result->abandoned)
                        break;
                    result->entities.push_back(entity);
                    result->mediaMeta.push_back(mm);
                }
                ++done;
                // 进度按时间节流（不是"每 N 条"）：各条目耗时量级差异极大——本地文件
                // 毫秒级，MediaInfo / 远端请求可能每条数秒，条数节流会让界面几十秒
                // 不刷新状态，看起来像卡死。单条求值本身不可中断，该条耗时期间进度不动
                if (throttle.elapsed() >= 50) {
                    throttle.restart();
                    promise.setProgressValue(done);
                }
            }
            if (!promise.isCanceled())
                promise.setProgressValue(total); // 循环末尾补一次
        },
        [this, ctx, result](JobVerdict v) { onMetaScanFinished(ctx, result, v); },
        [this, result, cat] {
            // 遮罩取消（GUI 线程）：锁内搬走已算批次并注入缓存——N32"取消也注入
            // 已算批次"的缓存保留语义；在途项不注入（下次预读重试）。互斥锁保证
            // 与任务体的 push 互斥，搬走后任务体到界即弃
            PlaylistEntity::List entities;
            std::vector<FileMeta> fileMeta;
            std::vector<PlaylistEntity::MediaMeta> mediaMeta;
            {
                std::lock_guard<std::mutex> lk(result->mutex);
                result->abandoned = true;
                entities = std::move(result->entities);
                fileMeta = std::move(result->fileMeta);
                mediaMeta = std::move(result->mediaMeta);
            }
            const size_t n = entities.size();
            for (size_t i = 0; i < n; ++i) {
                if (cat == MetaCategory::File)
                    entities[i]->setFileMeta(fileMeta[i]); // 友元授权写点（GUI 线程）
                else
                    entities[i]->setMediaMeta(mediaMeta[i]);
            }
            if (n)
                repaintAllLists();
        });
}

// N32：预读续作——注入批次（唯一的写点）→ 置"本表该类已读"标志 → 重绘 → 收尾。
// 取消也注入：已算出的值不丢，下次预读从缓存命中处续跑（N19 §3.3 的缓存保留语义），
// 只是不置已读标志（切入时会重新预读，但全程命中缓存）、不排序、不写规则
void PlaylistWidget::onMetaScanFinished(JobContext ctx, const std::shared_ptr<MetaResult> &r,
                                        JobVerdict v) {
    QListWidget *list = ctx.list;
    if (!list || v == JobVerdict::Discarded)
        return; // 批次已作废：不注入、不排序、不动记忆
    const size_t n = r->entities.size();
    for (size_t i = 0; i < n; ++i) {
        if (r->cat == MetaCategory::File)
            r->entities[i]->setFileMeta(r->fileMeta[i]); // 友元授权：视图层唯一的写入口
        else
            r->entities[i]->setMediaMeta(r->mediaMeta[i]);
    }
    repaintAllLists(); // 批次落地后统一重绘一次（成功与取消同此一次）
    if (v == JobVerdict::Cancelled) {
        finishCanceled(ctx);
        return;
    }
    setMetaLoadedOf(list, r->cat, true);
    advanceTab(std::move(ctx), /*fromMetaScan=*/true);
}

// N32：取消收尾——按意图分支，不可一刀切。枚举 / 刷新 / 重读侧有 ViewSnapshot
// （行已换入或记忆待修正）→ 做视图恢复再归位；Sort 侧无快照 → 一律不动：
// 给 Sort 补一次视图恢复会经 restoreViewState 的 tab == currentIndex() &&
// tab != m_playingTab 分支调 activateTab，产生重新选中与滚动的可见副作用
void PlaylistWidget::finishCanceled(const JobContext &ctx) {
    QListWidget *list = ctx.list;
    if (!list || ctx.intent == Intent::Sort)
        return;
    const int tab = indexOf(list);
    if (tab < 0)
        return;
    // Reactivate 的采集点在排序前一刻，取消时尚未采集——此刻补采（在途期间本表被
    // m_busy 冻结，与"发起时采集"等价），否则记忆修正与选中跟随会整体落空
    ViewSnapshot snap = ctx.snap;
    if (ctx.intent == Intent::Reactivate)
        snap = captureViewSnapshot(list);
    restoreViewState(list, tab, snap);
    restorePlayingState(list, tab);
    emit contentsChanged(); // 末位补发一次
}

// N32：外部作废统一入口（内容被外部替换 / 重建的三处由此收口）。
// 顺序不可交换：先 cancel 在途任务（尽力取消，落到 future 上）→ 递增代际 → 清槽位
// （含目标表，future 复位为默认构造）→ 复位忙时与遮罩。cancel 不得晚于清槽位，
// 否则"尽力取消"落空。被作废批次随后的 finished 投递按身份校验早退，什么都不碰
void PlaylistWidget::abandonJob() {
    if (m_job.future.isValid())
        m_job.future.cancel();
    ++m_token;
    m_job.target = nullptr;
    m_job.future = QFuture<void>();
    m_job.watcher = nullptr; // watcher 的 finished 路径自会销它（早退分支同样 deleteLater）
    m_job.onAbandon = nullptr;
    m_pendingLoad = PendingLoad{};
    // N37：作废即丢弃待弹警告——内容可能指向已不存在 / 已换源的 tab
    m_enumWarnings.clear();
    if (m_busy)
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

    QVector<Util::SegmentedName> numericSegs;
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

// N32：手动排序链——只组装意图上下文（含判类别与写规则共用的 key/asc，不预写规则），
// 预读与收尾一律由推进函数负责。取消由 finishCanceled 按意图分支处理：Sort 侧无快照，
// 一律不动（顺序不动、规则不落盘）
void PlaylistWidget::sortCurrent(SortKey key, bool ascending) {
    QListWidget *list = listAt(currentIndex());
    const int tab = currentIndex();
    // N19：忙时（菜单已禁用，此为兜底）与未加载/占位 tab 不排序
    if (!list || tab < 0 || key == SortKey::None || m_busy)
        return;
    if (!m_loadState[list].entriesLoaded)
        return;

    JobContext ctx;
    ctx.intent = Intent::Sort;
    ctx.list = list;
    ctx.tab = tab;
    ctx.sortKey = key;
    ctx.ascending = ascending;
    advanceTab(ctx, false);
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
    const bool hadMemory = m_memory.contains(list) && m_memory.value(list).lastPos >= 0;
    const QString captured = captureMemoryPath(list);

    // N30：播放表且存在正播行则走四步式（统一入口：续播取记忆、暂停态自动保持）；
    // 非播放表直接重排。mutate 即重排本身
    if (tab == m_playingTab && playingRowOf(list) >= 0)
        runFourStep(list, tab, [&] { sortListItems(list, key, ascending); });
    else
        sortListItems(list, key, ascending);

    restoreMemoryPath(list, captured, hadMemory);
}

// ——清理操作（N4 第六节）——

void PlaylistWidget::executeRemoval(const QList<int> &rows) {
    QListWidget *list = listAt(currentIndex());
    const int tab = currentIndex();
    if (!list || rows.isEmpty() || tab < 0 || m_busy)
        return; // N19：忙时菜单已禁用，此为兜底

    m_loadState[list].mediaMetaLoaded = false; // N29：条目集合变化，两类已读元数据一并失效
    m_loadState[list].fileMetaLoaded = false;
    const bool playingTab = (tab == m_playingTab);
    const bool hadMemory = m_memory.contains(list) && m_memory.value(list).lastPos >= 0;
    const QString captured = captureMemoryPath(list);
    // 从大到小删除避免行号漂移
    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());

    // N30：播放表且存在正播行则走四步式（统一入口），mutate 即删行；否则本地直删后落盘
    if (playingTab && playingRowOf(list) >= 0) {
        runFourStep(list, tab, [&] {
            eraseEntitiesAt(list, sorted); // 先批量缩实体容器（行还在，按行序定位）
            for (int row : sorted) {
                if (row >= 0 && row < list->count())
                    delete list->takeItem(row);
            }
        });
    } else {
        eraseEntitiesAt(list, sorted);
        for (int row : sorted) {
            if (row >= 0 && row < list->count())
                delete list->takeItem(row);
        }
        if (tab != 0)
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
    m_loadState[list].mediaMetaLoaded = false; // N29：条目集合变化，两类已读元数据一并失效
    m_loadState[list].fileMetaLoaded = false;
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

// N30：静态与动态同路——一律回推后重枚举，收尾走统一的三单元 + 比对三分支。
// 静态表刷新同样吸收提供者侧变化（旧路径对本表实体逐个 refresh 后重绘，不吸收外部新增）
void PlaylistWidget::refreshCurrentTab() {
    QListWidget *list = listAt(currentIndex());
    if (!list || m_busy)
        return; // N19：忙时菜单已禁用，此为兜底
    const int tab = currentIndex();
    // ① 回推：已就位且可修改的表把当前内容写回提供者。默认 tab 同样回推——其提供者
    // 从不落盘，不回推则枚举返回空表，刷新即清空内容。回推只改内存，不落盘
    if (auto *p = providerAt(tab);
        m_loadState.value(list).entriesLoaded && p && p->supportsModify())
        p->setItems(entitiesOf(list));
    // ② 置位：须在回推之后（回推守卫读的就是这个标志）
    TabLoadState &st = m_loadState[list];
    st.entriesLoaded = false;
    st.loadError.clear();
    // ③ 驱逐：N20 条目只驱逐本表；flag 重置所有表
    evictListKeys(list);
    // ④ 登记意图 + ⑤ 发起（收尾见 onLoadFinished：换入 → 推进函数）
    m_pendingLoad = PendingLoad{};
    m_pendingLoad.list = list;
    m_pendingLoad.refresh = true;
    startLoadAsync(list);
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
    for (auto it = m_loadState.begin(); it != m_loadState.end(); ++it) {
        it.value().mediaMetaLoaded = false; // N29：两类标志位对称复位
        it.value().fileMetaLoaded = false;
    }
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

// ——N29 类别映射与判定收敛点——

// 排序键 → 预读元数据类别的唯一映射
PlaylistWidget::MetaCategory PlaylistWidget::metaCategoryOf(SortKey key) {
    switch (key) {
    case SortKey::Created:
    case SortKey::Modified:
    case SortKey::FileSize:
        return MetaCategory::File;
    case SortKey::Duration:
    case SortKey::ReleaseDate:
        return MetaCategory::Media;
    case SortKey::None:
    case SortKey::FileName:
    case SortKey::FileNameNumeric:
    case SortKey::Extension:
        return MetaCategory::None;
    }
    return MetaCategory::None;
}

// 对应类别支持者谓词：Media / File 各取其一（None 由调用方上游挡掉）
bool PlaylistWidget::anyEntitySupports(const QListWidget *list, MetaCategory cat) const {
    return cat == MetaCategory::Media ? anyEntitySupportsMediaMeta(list)
                                      : anyEntitySupportsFileMeta(list);
}

// 切入重读判定内核：该表该排序键仍需预读的类别（无需预读返回 None）。
// 注意只含"类别 / 支持者 / 标志位"三要素，busy / entriesLoaded / 占位等
// 外围条件由调用方自判
PlaylistWidget::MetaCategory PlaylistWidget::pendingMetaCategory(const QListWidget *list,
                                                                 SortKey key) const {
    const auto cat = metaCategoryOf(key);
    if (cat == MetaCategory::None)
        return MetaCategory::None;
    if (!anyEntitySupports(list, cat))
        return MetaCategory::None;
    return metaLoadedOf(list, cat) ? MetaCategory::None : cat;
}

bool PlaylistWidget::metaLoadedOf(const QListWidget *list, MetaCategory cat) const {
    const auto it = m_loadState.constFind(list); // 只读引用，免整结构体拷贝
    if (it == m_loadState.end())
        return false;
    return cat == MetaCategory::File ? it->fileMetaLoaded : it->mediaMetaLoaded;
}

void PlaylistWidget::setMetaLoadedOf(QListWidget *list, MetaCategory cat, bool on) {
    TabLoadState &st = m_loadState[list];
    if (cat == MetaCategory::File)
        st.fileMetaLoaded = on;
    else
        st.mediaMetaLoaded = on;
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

// N35 重写：判定只看能力位——需任一元数据（supportsFileMeta 或 supportsMediaMeta）
// 统一走异步；两者皆不支持只显示文件路径。刷新语义为"最新属性而非缓存"：任务线程
// 经求值转发重读（eval 即重读，不读缓存不落缓存），GUI 续作经注入口写回实体缓存后
// 用同一套行构建渲染——任务内不得直接调共享实体的 fileMeta() / mediaMeta()
// （命中前写缓存与 GUI 绘制并发读写即 data race，N32 纪律），求值转发 + GUI 写回
// 是唯一合法形状。pending 态 General 组只放文件路径行，空一行后显示"正在读取元数据"。
// N35 修订 2：同步求值 + 结果静默丢弃——关闭对话框置 closed 旗（事件非轮询），
// 对话框即刻返回，后台任务跑完结果自然蒸发。发起前 GUI 线程显式判
// MediaInfoHelper::available()（supportsMediaMeta 内含，N32 前提）
void PlaylistWidget::showPropertiesDialog(const PlaylistEntity::Ptr &entityPtr) {
    const PlaylistEntity &entity = *entityPtr;
    const bool needFileMeta = entity.supportsFileMeta();
    const bool needMediaMeta = entity.supportsMediaMeta();

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

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->addWidget(view);

    // ——渲染器：按给定值构建整篇内容（pending 时只放路径行 + 占位）——
    // 续作仅在对话框存续期（exec 嵌套事件循环内）触发，按引用捕获安全
    auto renderDocument = [&](const FileMeta &fm, const MediaInfoHelper::Data *mi, bool pending) {
        const qint64 totalSize = fm.size.has_value() ? *fm.size : 0;
        view->clear();
        { // 键列统一停靠宽度；clear 后重设（文档默认选项不随 clear 保留）
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
                rows.append(
                    {tr("Resolution"), QStringLiteral("%1 x %2").arg(t.width).arg(t.height)});
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

        // ——N35 pending 占位：空一行后显示"正在读取元数据"（完成后整篇重渲染即移除）——
        if (pending) {
            if (!cur.atStart())
                cur.insertBlock(); // 空行
            cur.setCharFormat(valueFmt);
            cur.insertText(tr("Reading metadata..."));
            cur.insertBlock();
        }

        // 光标归位到文档开头，避免打开时选区残留
        view->setTextCursor(QTextCursor(view->document()));

        // 尺寸适配按当前内容行数重估（pending 占位行数少，先小后大可接受）
        int lineCount =
            general.size() + videos.size() + audios.size() + texts.size() + tags.size() + 8;
        for (const auto &g : videos)
            lineCount += g.size() + 2;
        for (const auto &g : audios)
            lineCount += g.size() + 2;
        for (const auto &g : texts)
            lineCount += g.size() + 2;
        if (mi && !mi->chapters.isEmpty())
            lineCount += mi->chapters.size() + 1; // 组标题 + 每章节一行
        if (pending)
            lineCount += 2; // 空行 + "正在读取元数据"占位行
        dialog.resize(520, qMin(lineCount * 22 + 40, 600));
    };

    // ——分支一：两者皆不支持（未知实体）→ 只显示文件路径，同步出框——
    if (!needFileMeta && !needMediaMeta) {
        renderDocument(FileMeta{}, nullptr, /*pending=*/false);
        dialog.exec();
        return;
    }

    // ——分支二：pending 占位 → 异步求值 → 注入写回 → 重渲染（N35 §2.5）——
    renderDocument(FileMeta{}, nullptr, /*pending=*/true);

    // N35 修订 2：同步求值 + 结果静默丢弃——关闭对话框仅置关闭旗（事件，非轮询），
    // 对话框即刻返回；后台任务跑完（在途远端 HEAD ≤ 超时）结果自然蒸发，
    // 不写缓存、不触碰 GUI。任务体只捕获副本，绝不捕获 this / widget
    const auto closed = std::make_shared<std::atomic_bool>(false);
    connect(&dialog, &QDialog::finished, &dialog, [closed] { closed->store(true); });

    struct PropResult {
        FileMeta fm;
        MediaInfoHelper::Data mm;
    };
    const auto result = std::make_shared<PropResult>();
    const PlaylistEntity::Ptr ent = entityPtr; // 实体本体由共享计数副本保活

    // 续作（GUI）：经注入口写回（唯一合法写点）+ 重渲染。按引用捕获
    // renderDocument 安全：投递窗口 ⊆ exec 嵌套事件循环 ⊆ 本函数栈帧存续期
    auto finishRender = [this, &renderDocument, ent, result, closed, needFileMeta, needMediaMeta] {
        if (closed->load())
            return; // 关闭即取消：结果静默丢弃，不写回、不重渲染
        // 求值转发取回的值由 GUI 续作经注入口写回——实体缓存只在 GUI 线程写（N32）。
        // 失败（空值）同样入缓存：不再重试（N35 §2.4）
        if (needFileMeta)
            ent->setFileMeta(result->fm);
        if (needMediaMeta)
            ent->setMediaMeta(result->mm);
        repaintAllLists(); // 写回影响行绘制（时长列回落实体缓存元数据）
        renderDocument(result->fm, needMediaMeta ? &result->mm : nullptr, /*pending=*/false);
    };
    auto *watcher = new QFutureWatcher<void>(&dialog); // parent = 对话框：随框销毁
    connect(watcher, &QFutureWatcher<void>::finished, &dialog, finishRender);

    watcher->setFuture(
        QtConcurrent::run([ent, result, closed, needFileMeta, needMediaMeta](QPromise<void> &) {
            if (needFileMeta)
                result->fm = ent->evalFileMeta(); // 同步求值（关闭不中断，结果丢弃）
            if (needMediaMeta && !closed->load())
                result->mm = ent->evalMediaMeta();
        }));
    // 防御：任务可能在 setFuture 之前就跑完（池线程空闲 + 任务极短），此时 Qt 是否
    // 补发 finished 不在文档保证之内——立即补判一次，递延到事件循环执行以免在
    // 发起栈内同步跑续作（与 runJob 同款兜底；closed 守候下幂等，双跑无害）
    if (watcher->isFinished())
        QMetaObject::invokeMethod(this, finishRender, Qt::QueuedConnection);

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
        // 弹层父指针挂顶层窗口：原生视频区与 splitter 内容叠压（N3）使 Qt 把
        // splitter 隐式原生化，QMenuPrivate::transientParentWindow 会取
        // nativeParentWidget 的窗口句柄当 transient parent，挂 splitter 之下的
        // 控件时该句柄非顶层而触发 "must be a top level window" 警告
        QMenu menu(window());
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

    // 弹层父指针挂顶层窗口（原因见表头菜单分支注释）
    QMenu menu(window());

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
    // N30：统一后静态表刷新同样走枚举，"静态空表禁刷新"的前提已不成立，保持默认启用
    // （refreshAction 无额外限制，此处不调用 setEnabled）
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
        showPropertiesDialog(targetEntity); // N35：传共享计数副本（异步对话框持有实体）
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
        // N28：shared_ptr 引用计数回收（含排序规则）；若旧提供者在途，
        // 任务 lambda 仍持副本，销毁自动延后到枚举任务结束
        // N32：在途批次一并作废，统一走 abandonJob（仅当目标表即本表）
        if (m_job.target == closing)
            abandonJob();
        m_providerOf.remove(closing);
        m_entities.remove(closing); // N26：实体所有权随 tab 一并回收
        m_memory.remove(closing);
        m_loadState.remove(closing); // N19：清理加载状态（含 loadError）
        if (m_pendingLoad.list == closing)
            m_pendingLoad = PendingLoad{};
    }
    if (m_playingTab > idx)
        --m_playingTab;
    removeTab(idx);
    savePlaylists();
}

// N19：忙时表头菜单已禁用，此为兜底
// N9：新建播放列表——弹列表设置对话框，确定后建 tab；内容一律经首次激活的异步枚举
// 填充（N28：所有源同路，静态源不再同步填充）
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
    ListSettingsDialog dlg(this, tr("Playlist %1").arg(maxN + 1), nullptr, occupied, m_registry);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.listName();

    auto *list = createList();
    // 新建必产出提供者（对话框入口 m_current 为 null，不会走"未变更"分支）；
    // 断言守候不变量——空值插入会让下面的解引用直接崩，宁可 Debug 期炸出
    auto result = dlg.takeProvider();
    Q_ASSERT(result.has_value() && *result != nullptr);
    // 排序默认按能力位施加（不可修改的源默认按修改时间）。
    // N28：插入与取值拆两步——Qt 的 key_value_iterator 无 operator->，
    // 不能经 insert_or_assign 返回值直接取实例指针
    m_providerOf.insert_or_assign(list, std::move(*result));
    auto *p = m_providerOf.find(list).value().get();
    const bool modifiable = p->supportsModify();
    p->setSortRule(modifiable ? int(SortKey::None) : int(SortKey::Modified), true);
    addTab(list, name);
    // N28：静态表不再同步填充，切入（setCurrentIndex）触发首次异步加载
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
    ListSettingsDialog dlg(this, tabText(tabIndex), providerAt(tabIndex), occupied, m_registry);
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

    // N27：同 insert_or_assign——不经 operator[]，键缺失时不静默插入空值
    m_providerOf.insert_or_assign(list, std::move(*result));

    // 原记忆随源失效一并清除；排序规则重置为新提供者类型默认
    m_memory.remove(list);
    if (auto *np = providerAt(tabIndex))
        np->setSortRule(np->supportsModify() ? int(SortKey::None) : int(SortKey::Modified), true);

    TabLoadState &st = m_loadState[list];
    st.entriesLoaded = false;
    st.mediaMetaLoaded = false; // N29：换源两类标志一并复位
    st.fileMetaLoaded = false;
    st.loadError.clear();
    list->clear();
    m_entities[list].clear(); // N26：实体所有权同步回收（行已清，防悬空 EntityRole）
    // N28：换源不再区分能力位，一律异步填充；当前 tab 索引未变（onTabChanged
    // 不会触发），显式发起以保留"换源即出内容"的体验，完成后选中首项（不播放）
    m_pendingLoad = PendingLoad{};
    m_pendingLoad.list = list;
    m_pendingLoad.selectFirst = true;
    startLoadAsync(list);
    savePlaylists();
}

#include "pch.h"

#include "PlaylistWidget.h"

#include "Constants.h"
#include "ListSettingsDialog.h"
#include "LoadingOverlay.h"
#include "MediaInfoHelper.h"
#include "MpvController.h"
#include "PlaylistDelegate.h"
#include "PlaylistListView.h" // N57 §2.2：视图即状态载体，本页所有列表页由 createView 出生
#include "PlaylistRegistry.h" // N57 §2.3：默认 tab 的列表与默认名出口
#include "ServiceRegistry.h"
#include "StaticProvider.h" // N79：新表提供者经 createDefault 出生
#include "StreamTransit.h"
#include "Util.h"

PlaylistWidget::PlaylistWidget(const QString &configFile, std::shared_ptr<ServiceRegistry> registry,
                               std::shared_ptr<PlaylistRegistry> lists, QWidget *parent)
    : QTabWidget(parent), m_configFile(configFile), m_registry(std::move(registry)),
      m_lists(std::move(lists)) {
    // N56 §2.7：registry 由装配顶点（MainWindow）创建并注入，本类不再自造；
    // HttpClient / TraversalPolicy 的注册与设置变更更新同样上移装配顶点，本类
    // 只经注入的 registry 取用（提供者构造注入、实体求值现查），不做任何 provide。
    // relay 的 provide<StreamRelayIf> 同样上移（MainWindow 在构造本对象之前完成），
    // 硬顺序②"早于任何提供者出生"由装配顶点保证
    Q_ASSERT(m_registry && m_lists);
    // N35：装配仍须前置——造实体经注入的 registry，提供者由装配顶点交入注册表
    setAcceptDrops(true);
    setDocumentMode(true);
    setMinimumWidth(220);
    // N57 §2.3：tab 0 由注册表的默认列表建视图挂 tab，名取注册表自持的默认名出口
    // （本类不再自造提供者，也不再有作 tab 名的 tr("Default")）
    auto *defTab = createView(m_lists->defaultList());
    addTab(defTab, defaultListDisplayName());
    // N30：默认 tab 内容不经枚举取得，由两条直写路径维护（setDefaultTabFiles / 回刷收编），
    // 恒视为已就位——否则切回默认 tab 会发起枚举，而其 StaticProvider 只在刷新回推时才
    // setItems，枚举必返回空表，把直写内容清空。刷新按 refreshCurrentTab 显式发起枚举，
    // 不受此拦截（N28 的"默认 tab 不参与枚举"口径随本篇改写）。
    // N45：直写即层入缓存（层在缓存即"已加载"，§2.5）
    defTab->ensureLevel({}); // 直写路径的层入缓存（根层）
    connect(this, &QTabWidget::currentChanged, this, &PlaylistWidget::onTabChanged);
    // ——N49 底部工具条：面板级常驻，全 tab 共享一行，不随 tab 切换重建——
    // 创建顺序：各列表之后、遮罩之前——工具条自然压在页面栈之上，遮罩仍最后创建并置顶
    m_stack =
        findChild<QStackedWidget *>(); // 内部页面栈（QTabWidget 唯一的 QStackedWidget 子控件）
    m_bottomBar = new QWidget(this);
    // 底色取 base（与列表视口同色——工具条在视觉上并入内容区，内容区整体再与主窗口
    // 底色区分）。样式表背景即不透明底，同样盖住样式画的 tab pane 边框线（§2.1）；
    // 颜色取调色板角色以适应浅深主题，写法沿用 LoadingOverlay 的 QFrame#N19InfoPanel
    // 先例。列表自身的 StyledPanel 边框即列表与工具条之间的分隔，不再额外画线
    m_bottomBar->setObjectName(QStringLiteral("PlaylistBottomBar"));
    m_bottomBar->setStyleSheet(
        QStringLiteral("QWidget#PlaylistBottomBar { background-color: palette(base); }"));
    auto *barLayout = new QHBoxLayout(m_bottomBar);
    // 四周留 2、控件间距 2（默认边距约 9、默认间距约 6 对 16px 图标与 24px 条目行高
    // 明显过大）。整行高 = 控件高 + 4 = 28（条目行 24）
    barLayout->setContentsMargins(2, 2, 2, 2);
    barLayout->setSpacing(2);
    m_locateButton = new QToolButton(m_bottomBar);
    // 24 与条目行高相等，也与主窗口音量行的静音按钮同尺寸。**边框保留**：静音按钮可以
    // 免边框（autoRaise）是因为"音量图标即按钮"已是公认习惯，定位按钮没有这层共识，
    // 边框是它"可点"的唯一标识
    m_locateButton->setFixedSize(24, 24);
    // C2：不保留焦点（与主窗口控制行按钮同一处理）——否则点完按钮后按空格会再次触发
    // 定位，而不是走全局暂停
    m_locateButton->setFocusPolicy(Qt::NoFocus);
    // 不设 iconSize 时 QToolButton 取样式的工具栏图标度量（常为 24px 而非 16px），
    // 故钉死显示尺寸；素材是矢量，缩放不失真，这里只定观感大小（§2.1）
    m_locateButton->setIconSize(QSize(16, 16));
    m_locateButton->setIcon(QIcon(App::iconPath(App::iconLocatePlaying())));
    m_locateButton->setToolTip(tr("Locate Playing Item")); // §2.8：由被删的右键菜单项平移而来
    connect(m_locateButton, &QToolButton::clicked, this, &PlaylistWidget::locatePlayingItem);
    m_searchBox = new QLineEdit(m_bottomBar);
    m_searchBox->setPlaceholderText(tr("Search"));
    m_searchBox->setClearButtonEnabled(true);
    // 框内右侧装饰图标：Qt 为其预留位置（文字不压图、长文本滚动也不盖图），与清除
    // 按钮并存时排在清除按钮左侧。只作标识——不接 triggered、不设 tooltip、不置灰，
    // 点击无行为；半透明已烘进素材的 fill-opacity，代码不再合成（§2.2）
    m_searchBox->addAction(QIcon(App::iconPath(App::iconSearch())), QLineEdit::TrailingPosition);
    // §2.7：焦点在搜索框时认领按键（与列表同为 ShortcutOverride 认领，同一实现）
    m_searchBox->installEventFilter(this);
    connect(m_searchBox, &QLineEdit::returnPressed, this, &PlaylistWidget::searchNextInCurrentList);
    barLayout->addWidget(m_locateButton);
    barLayout->addWidget(m_searchBox, 1); // 搜索框以伸展因子占满余量
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
    reserveBottomBarSpace(); // N49：让位初算（幂等，此后由各重算点维护）
    updateBottomBarState();  // N49：按钮启用态初值
}

// N32：析构不再等待。任务体只捕获入参副本（提供者 shared_ptr / 实体快照），不持 this；
// watcher 以本对象为 parent 与接收者上下文，析构即断线，投递不到已析构对象——
// 原 wait(2000) 的三个理由（QThread 析构约束、alive 旗 → invokeMethod 的 TOCTOU 窗口、
// 线程卡死兜底）随之消灭。此处只做一次非阻塞收尾：作废并 cancel 在途任务，尽快释放
// IO / MediaInfo 句柄、停止无谓计算，不为安全（cancel 尽力而为——卡在单条调用内时
// 最坏延后一条）。
// m_pendingTabUuid 必须先清零：abandonJob 内的 setBusy(false) 会 flushPendingTab，
// 那会在析构中途经 onTabChanged 再起一个新任务。m_pendingReveal 同理——
// setBusy(false) 的延后归位消费会触碰列表；N49 的 m_pendingLocate 同族
PlaylistWidget::~PlaylistWidget() {
    m_pendingTabUuid = QUuid();
    m_pendingReveal = false;
    m_pendingLocate = false;
    abandonJob();
}

// ——N35 服务注册表装配——

// N57 §2.2：全仓唯一的列表页诞生点。所属列表在构造期一次注入（不再有"无提供者的
// 中间态"，也不再预置默认提供者——提供者由调用方交进注册表）
PlaylistListView *PlaylistWidget::createView(Playlist *list) {
    auto *view = new PlaylistListView(list);
    // 子列表不接受拖放，事件会上溯到本 QTabWidget 统一处理
    view->setAcceptDrops(false);
    // N5：列表集中创建点统一安装过滤器（ShortcutOverride 认领导航键）
    view->installEventFilter(this);
    // 播放列表不允许多选（显式声明，锁定约束）
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    // N26：委托绘制（时长经实体 key 查时长仓，兜底实体缓存元数据）。仓与委托共享，
    // 不指向面板成员——页面晚于成员销毁
    view->setItemDelegate(new PlaylistDelegate(m_duration, view));
    // 序号位数自适应的"行变更 → 整表重绘"接线归视图自身（N57 §2.2：它是渲染关注点，
    // 且连接句柄须在视图析构开头断开）
    connect(view, &QListWidget::itemDoubleClicked, this, &PlaylistWidget::onItemDoubleClicked);
    return view;
}

PlaylistListView *PlaylistWidget::viewAt(int index) const {
    return qobject_cast<PlaylistListView *>(widget(index));
}

// 不变式（§2.2）：视图非空时其列表非空、身份非 nil、提供者非空
Playlist *PlaylistWidget::listOfView(const PlaylistListView *view) const {
    const auto *listView = qobject_cast<const PlaylistListView *>(view);
    return listView ? listView->list() : nullptr;
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
    // N49 §2.7：焦点在搜索框时认领无修饰键的按键——全局单键动作（空格暂停、
    // ↑↓ 音量等）不再触发，文本输入与行内编辑照常。只认领无修饰键者是有意的：
    // 认领全部会挡掉主菜单的组合键，组合键放行也让 QLineEdit 自带的编辑键
    // （Ctrl+A / C / V）不受影响
    if (event->type() == QEvent::ShortcutOverride && watched == m_searchBox) {
        if (static_cast<QKeyEvent *>(event)->modifiers() == Qt::NoModifier)
            event->accept();
    }
    return QTabWidget::eventFilter(watched, event);
}

// N57 §2.8：层缓存与路径 / 显示层 / 记忆的定位访问器（browsePathIdsOf /
// queueRootIdsOf(list) / displayedIdsOf / findLevel / ensureLevel / levelOf /
// layerLoaded / levelErrorOf）已随状态搬进 PlaylistListView——
// 取用形态 `list->findLevel(ids)` 等，外层键不复存在（N58：entities() 改名 entries()，
// 返回层元素包装体表）

// filesOf 双出口的实体版（共用体）：层的实体序列（实体序）。旧 filesOf 读显示行
// （跳占位行），实体序与显示行在无特殊行时一致；失败层既无实体也无内容行
// （内容一律清空），读层实体与旧行读取等价。空位原样保留，调用方自判
PlaylistEntity::List PlaylistWidget::layerEntitiesOf(int tabIndex, const QStringList &ids) const {
    const PlaylistListView *view = viewAt(tabIndex);
    const Level *lvl = view ? view->findLevel(ids) : nullptr;
    if (!lvl)
        return {};
    PlaylistEntity::List out;
    out.reserve(lvl->entities.size());
    for (const auto &entry : lvl->entities)
        out.push_back(entry.entity); // 空位原样保留
    return out;
}

PlaylistEntity::List PlaylistWidget::queueEntitiesOf(int tabIndex) const {
    const PlaylistListView *view = viewAt(tabIndex);
    return view ? layerEntitiesOf(tabIndex, view->queueRootIds()) : PlaylistEntity::List{};
}

PlaylistEntity::List PlaylistWidget::currentLayerEntitiesOf(int tabIndex) const {
    const PlaylistListView *view = viewAt(tabIndex);
    return view ? layerEntitiesOf(tabIndex, view->browsePathIds()) : PlaylistEntity::List{};
}

// N56 §2.7：正播实体——队列根层第 entityIndex 项（mpv playlist-pos 语义）。
// MainWindow 的一切"以当前内容为对象"取串点经此取实体，不再读 mpv 属性
PlaylistEntity::Ptr PlaylistWidget::playingEntityOf(int tabIndex, int entityIndex) const {
    if (tabIndex < 0 || entityIndex < 0)
        return nullptr;
    const PlaylistListView *view = viewAt(tabIndex);
    const Level *lvl = view ? view->findLevel(view->queueRootIds()) : nullptr;
    if (!lvl || entityIndex >= int(lvl->entities.size()))
        return nullptr;
    return lvl->entities.at(entityIndex).entity;
}

// N48/R：实体表 → 喂 mpv 的播放形态。N52：走基类非虚 playTarget 模板
// 方法——usesTransit() 为假走 directPlayUrl（直发形态），为真铸 oxbow:// 转接
// URI 并随串交出 cache=yes（N65：申报落点在铸造点）；
// 本出口由此成为全仓唯一能取得"直接形态"的生产调用点（directPlayUrl 已 protected）。
// 凭据只在此处与 PROPFIND 请求两个使用点注入；本出口只服务下发 mpv 的 loadfile
// 循环，收编索引与回刷判据和它同源同形（N56 §2.5）。
// N57 §2.2：整表映射归列表对象（逐实体传它自身身份），本出口只是薄适配——
// 提供者恒非空后不再有"无提供者按恒等兜底"那一支，越界（无表可寻）按下发空集
QVector<PlayTarget> PlaylistWidget::playTargets(int tabIndex,
                                                const PlaylistEntity::List &entities) const {
    const Playlist *list = listOfView(viewAt(tabIndex));
    return list ? list->playTargets(entities) : QVector<PlayTarget>{};
}

// ——N54 列表身份只读出口（书签按身份回跳的查询面）——
// N57 §2.6：原 listUuidOf 删除——身份经 listOf(tab)->uuid() 直查；本出口只读，
// 不得经它直写（改名换源走 openListSettings 等显式方法）

const Playlist *PlaylistWidget::listOf(int tabIndex) const { return listOfView(viewAt(tabIndex)); }

QStringList PlaylistWidget::queueRootIdsOf(int tabIndex) const {
    const PlaylistListView *view = viewAt(tabIndex);
    return view ? view->queueRootIds() : QStringList{};
}

// 按列表身份找 tab：判等全在代码侧（QUuid 比较，N57 §2.4 删去两侧 toLower 归一，
// 手改大小写与带花括号自本版起归一命中）。不跳过 tab 0——默认列表带恒定保留
// 身份（§2.5），命中即 0；nil 入参恒不命中
int PlaylistWidget::tabIndexOfList(const QUuid &uuid) const {
    if (uuid.isNull())
        return -1;
    for (int i = 0; i < count(); ++i)
        if (const Playlist *list = listOfView(viewAt(i)); list && list->uuid() == uuid)
            return i;
    return -1;
}

// N57 §2.8：播放目标改持身份，坐标现算（个位数条目的一次扫描）。它恒可解析——初值与
// 各归位点都取 kDefaultListUuid，而默认列表恒定且不可摘除（§2.5 的咬合关系在此）
int PlaylistWidget::playingTabIndex() const { return tabIndexOfList(m_playingUuid); }

// 层级定位的目标换算：身份 → 页面。换算不出坐标即空指针，是 ListGone 的唯一判据；
// 坐标只在需要动作的那一刻现取，不进待办状态（§1.5）
PlaylistListView *PlaylistWidget::jumpTarget(const LevelJump &req) const {
    return viewAt(tabIndexOfList(req.listUuid));
}

// N57 §2.6：原 listNameOfUuid 退场——名字属列表，按身份直查注册表
// （`PlaylistRegistry::nameOf`，前提"空名列表不存在"由 readTabGroup 跳无名节与
// `append` 侧的重名校验共同保证）

// ——N45 §2.4 双坐标系换算与三类行谓词——

bool PlaylistWidget::isContainerRow(const QListWidgetItem *item) {
    return item && !item->data(ContainerRole).toString().isEmpty();
}

bool PlaylistWidget::isParentLinkRow(const QListWidgetItem *item) {
    return item && item->data(ParentLinkRole).toBool();
}

// 实体行 = EntityRole 非空。占位行 / 容器行 / "上一级"行均无实体
bool PlaylistWidget::isEntityRow(const QListWidgetItem *item) {
    return entityFromItem(item) != nullptr;
}

// 实体序 → 行序：扫描行，数到第 entityIndex 个实体行即其行号
int PlaylistWidget::entityIndexToRow(const PlaylistListView *list, int entityIndex) const {
    if (!list || entityIndex < 0)
        return -1;
    int idx = 0;
    for (int r = 0; r < list->count(); ++r) {
        if (!isEntityRow(list->item(r)))
            continue;
        if (idx == entityIndex)
            return r;
        ++idx;
    }
    return -1;
}

// 行序 → 实体序：非实体行（占位 / 容器 / 上一级）返回 -1
int PlaylistWidget::rowToEntityIndex(const PlaylistListView *list, int row) const {
    if (!list || row < 0 || row >= list->count() || !isEntityRow(list->item(row)))
        return -1;
    int idx = 0;
    for (int i = 0; i < row; ++i)
        if (isEntityRow(list->item(i)))
            ++idx;
    return idx;
}

// ——N45 §2.4/§2.5 导航与换入——

// 实体行构造（视图侧孪生，不入仓）：所有权已在 Level 的实体经此建行
QListWidgetItem *PlaylistWidget::makeEntityRow(PlaylistListView *list,
                                               const PlaylistEntity::Ptr &entity) {
    auto *item = new QListWidgetItem(list);
    setItemEntity(item, entity);
    return item;
}

// 相对路径 tooltip（§2.4.3）：目标层各段标题以 / 拼接，不带根前缀；
// 目标为根层（空序列）返回空串——调用点不设 tooltip
static QString relativePathText(QStringList titles) { return titles.join(QLatin1Char('/')); }

// 前导特殊行构建：上一级行占第 0 行（非根层），容器行随后——
// 模型不变式：行序 = [上一级行][容器行…][实体行…]。
// 上一级行（视图合成，§2.4.1）：文案固定为 ".."（维护者裁决：文件系统惯例符号，
// 不经 tr）、双击语义为导航、tooltip = 父层相对路径；容器行：ContainerRole 存 id、
// tooltip = 目标层相对路径
void PlaylistWidget::buildSpecialRows(PlaylistListView *list, const QStringList &ids,
                                      const QList<PlaylistProvider::Container> &containers) {
    const QList<PathSeg> path = list->browsePath();
    Q_ASSERT(ids == list->browsePathIds()); // 特殊行的标题列取自浏览路径，二者必须同源
    QStringList titlesSoFar;
    for (const PathSeg &seg : path)
        titlesSoFar << seg.title;

    int insertPos = 0;
    if (!ids.isEmpty()) {
        auto *item = new QListWidgetItem(list);
        item->setData(Qt::DisplayRole, QStringLiteral(".."));
        item->setData(Qt::DecorationRole,
                      QApplication::style()->standardIcon(QStyle::SP_ArrowBack));
        item->setData(ParentLinkRole, true);
        // 父层相对路径 = 当前路径去掉末段；目标为根层时为空，不设 tooltip
        const QString rel = relativePathText(titlesSoFar.mid(0, qMax(0, titlesSoFar.size() - 1)));
        if (!rel.isEmpty())
            item->setToolTip(rel);
        list->insertItem(insertPos++, item);
    }
    for (const PlaylistProvider::Container &c : containers) {
        auto *item = new QListWidgetItem(list);
        item->setData(Qt::DisplayRole, c.title);
        item->setData(Qt::DecorationRole, QApplication::style()->standardIcon(QStyle::SP_DirIcon));
        item->setData(ContainerRole, c.id);
        const QString rel = relativePathText(titlesSoFar + QStringList{c.title});
        if (!rel.isEmpty())
            item->setToolTip(rel);
        list->insertItem(insertPos++, item);
    }
}

// 整层行重建（视图侧）：行只引用 Level 持有的实体，所有权不动。
// 播放标记不在此点亮——由调用方按 tab 角色走 restorePlayingState / 记忆选中
void PlaylistWidget::rebuildRowsForLevel(PlaylistListView *list, const Level &lvl,
                                         const QStringList &ids) {
    clearPlaceholder(list);
    list->clear();
    QList<PlaylistProvider::Container> containers; // N58 §2.2：调用点拆包一次
    containers.reserve(lvl.containers.size());
    for (const auto &entry : lvl.containers)
        containers.push_back(entry.container);
    buildSpecialRows(list, ids, containers);
    for (const auto &entry : lvl.entities)
        makeEntityRow(list, entry.entity);
    list->setDisplayedIds(ids);
}

void PlaylistWidget::saveLayerSelection(PlaylistListView *list, const QStringList &ids) {
    Level *lvl = list->findLevel(ids); // 可写重载：只改本层簿记字段，不插新层
    if (!lvl)
        return;
    const int row = list->currentRow();
    if (row < 0 || row >= list->count())
        return;
    if (isContainerRow(list->item(row)))
        lvl->lastSelectedKey = list->item(row)->data(ContainerRole).toString();
    else if (const PlaylistEntity *entity = entityFromItem(list->item(row)))
        lvl->lastSelectedKey = entity->key();
}

void PlaylistWidget::restoreLayerSelection(PlaylistListView *list, const Level &lvl) {
    if (lvl.lastSelectedKey.isEmpty())
        return;
    for (int r = 0; r < list->count(); ++r) {
        QListWidgetItem *item = list->item(r);
        bool hit = false;
        if (isContainerRow(item))
            hit = item->data(ContainerRole).toString() == lvl.lastSelectedKey;
        else if (const PlaylistEntity *entity = entityFromItem(item))
            hit = entity->key() == lvl.lastSelectedKey;
        if (hit) {
            list->setCurrentItem(item);
            list->scrollToItem(item, QListWidget::EnsureVisible);
            return;
        }
    }
}

// 切层统一入口（§2.5）：显示层 != 浏览层时按层换入。命中缓存同步重建
// （不起任务、无遮罩）；失败层只剩骨架与占位行（切入不自动重试，N53 变更
// 记录 V1）；未加载层异步枚举
void PlaylistWidget::showLayer(PlaylistListView *list) {
    if (!list)
        return;
    const QStringList ids = list->browsePathIds();
    if (list->displayedIds() == ids)
        return; // 行已对齐，无需换入
    const Level *lvl = list->findLevel(ids);
    if (lvl && lvl->loadError.isEmpty()) {
        rebuildRowsForLevel(list, *lvl, ids);
        if (indexOf(list) == playingTabIndex() && ids == list->queueRootIds())
            restorePlayingState(list, indexOf(list)); // 队列根层：标记与选中由归位权威出口重建
        else
            restoreLayerSelection(list, *lvl);
    } else if (lvl) {
        // 失败层：只剩骨架与占位行。切入失败层的常规渲染路径（切入不自动重试，
        // 变更记录 V1）；离开该层（回到上一级）即删条目，再钻进来按未加载重举
        rebuildRowsForLevel(list, *lvl, ids);
        showPlaceholder(list, lvl->loadError);
        restoreLayerSelection(list, *lvl);
    } else if (!ids.isEmpty() && wholeTreeMissingLevel(list, ids)) {
        // N64：整树形态进入子层永不枚举（§2.3）——缺层违约已按空层处置（行已重建）
    } else {
        startEnumAsync(list, Intent::InitialLoad, ids); // 未加载：遮罩 + 完成后换入
    }
}

void PlaylistWidget::navigateDown(PlaylistListView *list, QListWidgetItem *containerRow) {
    if (!list || !containerRow || m_busy)
        return;
    saveLayerSelection(list, list->browsePathIds());
    PathSeg seg;
    seg.id = containerRow->data(ContainerRole).toString();
    seg.title = containerRow->data(Qt::DisplayRole).toString();
    QList<PathSeg> down = list->browsePath();
    down.append(seg);
    list->setBrowsePath(down);
    showLayer(list);
    reactivateIfStale(list);
}

void PlaylistWidget::navigateUp(PlaylistListView *list) {
    if (!list || m_busy)
        return;
    const QStringList ids = list->browsePathIds(); // 离开层（删失败层条目与存选中共用）
    saveLayerSelection(list, ids);
    QList<PathSeg> up = list->browsePath();
    if (!up.isEmpty()) {
        // 失败层不长期驻留缓存，回到上一级即删（N53 §2.4）——清空后条目只剩一句
        // loadError，钻取重进即走异步重举（服务端已恢复时不必再等手动刷新）。删除点
        // 只此一处且不得铺开：切入失败层按缓存渲染零请求（变更记录 V1），删层若挂
        // 在切 tab 等高频路径上才构成反复打服务端。失败当场不删：条目须在缓存承载
        // loadError（占位行文案来源 + 手动刷新的 ensureLevel 清文案落点）。不连带删
        // 子孙——刷新发起时已按前缀整枝逐出，能留在缓存里的只可能是它自己
        list->eraseFailedLevel(ids);
        up.removeLast();
        list->setBrowsePath(up);
    }
    showLayer(list);
    reactivateIfStale(list);
}

// 缓存命中换入后的预读判定——与切 tab（onTabChanged）同判同路。缓存命中换入
// 不重枚举，但该层元数据未预读且排序规则需要时，必须预读后重排：规则变更时的
// 全层重排是"只读缓存"的（sortLevelData 零求值），未预读层当时只能退化为提供者
// 初始序，若不在显示时补排，缓存命中会永久呈现旧序（违反 §2.6"每一层施加同一
// 条规则"）。播放中 tab 的队列根层经 reactivateRescan 的 syncAfterRefresh 三分
// 支自愈（重排后与 mpv 序不一致即四步式重推）
void PlaylistWidget::reactivateIfStale(PlaylistListView *list) {
    if (m_busy)
        return;
    const int tab = indexOf(list);
    if (tab < 0)
        return;
    // needsMetaRescan 内部含忙时与已加载守卫，判定对象是当前显示层（浏览路径层）
    if (needsMetaRescan(tab))
        reactivateRescan(list, tab);
}

// ——N64 整树形态——

// 缺层守候（showLayer 与层级定位共用）：整树交付完备，进入子层永不枚举（缓存命中
// 即换入）——缺层只可能是提供者或视图的 bug：Debug 断言显影；Release 按空层处置
// （ensureLevel 插入空层即宣告已加载，与"调用方违约降级返回空结果"先例同味），
// 调用方按已加载空层继续、不得再对该层发起枚举。根层缺失不在此列（整树未初载，
// 根层调用合法），由调用方以 ids 非空 gate。
// 有意不镜像 showLayer 命中分支的播放态还原（restorePlayingState）：守候只在层缺于
// 缓存时进入，而"播放 tab 的队列根层缺于缓存"不可达——播放基准层随播放恒在缓存，
// 其逐出必与停播 / 锚点作废 / 换源同点同步（N53 §2.2）；故此处只恢复层选中
bool PlaylistWidget::wholeTreeMissingLevel(PlaylistListView *list, const QStringList &ids) {
    const auto *p = providerOfView(list);
    if (!p || !p->enumeratesWholeTree())
        return false;
    Q_ASSERT(false); // 缺层违约：整树交付是完备的
    Level &lvl = list->ensureLevel(ids);
    rebuildRowsForLevel(list, lvl, ids);
    restoreLayerSelection(list, lvl);
    return true;
}

// 整树权威重建（onLoadFinished 换入后调用，§2.3 唯一的新动作）。显示序住 LevelData
// 的两条列表、descendants 的键只是寻址（QMap 字典序不承载显示语义）；结构合法性由
// 前缀校验闭合。三步：校验 → 先删 → 后写。线程口径不变：全在 GUI 续作里，工作线程
// 只产数据、不碰任何视图结构
void PlaylistWidget::rebuildWholeTree(PlaylistListView *list, Level &rootLvl,
                                      PlaylistProvider::Enumeration &enumeration) {
    const auto *p = providerOfView(list);
    const SortRule rule = p ? p->sortRule() : SortRule{};

    auto containerIdsOf = [](const QList<PlaylistProvider::Container> &containers) {
        QSet<QString> ids;
        for (const auto &c : containers)
            ids.insert(c.id);
        return ids;
    };
    // 根层容器取 rootLvl——enumeration.containers 已在换入时被 move 走（Qt 的 QList
    // 移动赋值是 move-and-swap，源必被清空），从这里取恒得空集。
    // N58：rootLvl 持包装体，校验面与下方的 rootIds 采集都消费裸 Container，
    // 此处拆包一次（必须无条件编译：rootIds 采集在 Debug 预处理块之外消费它）
    QList<PlaylistProvider::Container> rootContainers;
    rootContainers.reserve(rootLvl.containers.size());
    for (const auto &entry : rootLvl.containers)
        rootContainers.push_back(entry.container);
#ifndef QT_NO_DEBUG
    // 义务 3 的唯一性半边：同层容器 id 不得重复（QMap 会静默后者覆盖前者、丢一整层）
    auto checkUniqueIds = [](const QList<PlaylistProvider::Container> &containers) {
        QSet<QString> seen;
        for (const auto &c : containers) {
            Q_ASSERT(!seen.contains(c.id));
            seen.insert(c.id);
        }
    };
    checkUniqueIds(rootContainers);
    for (auto it = enumeration.descendants.constBegin(); it != enumeration.descendants.constEnd();
         ++it)
        checkUniqueIds(it.value().containers);
#endif

    // ——校验（义务 3 前缀一致性）：键非空（调用层自身与根层不交付）、首段 ∈ 根层
    // containers、逐段往下落在交付层的 containers。QMap 字典序保证短前缀键先于长键
    // 到达，逐键校验时祖先层必已入验收集；链断即整枝拒绝（其子孙因祖先缺席同样被拒）。
    // Debug 断言显影；Release 逐项忽略——权威语义下错键会删层，校验是权威写的
    // 正确性要件。根层容器 id 集取 rootLvl（换入落点），不是 enumeration（已搬空）
    const QSet<QString> rootIds = containerIdsOf(rootContainers);
    // 验收集：键 → 交付层节点指针（QMap 节点稳定，校验期无增删，指针不悬空）
    QMap<QStringList, PlaylistProvider::LevelData *> accepted;
    for (auto it = enumeration.descendants.begin(); it != enumeration.descendants.end(); ++it) {
        const QStringList &key = it.key();
        bool valid = !key.isEmpty();
        if (valid) {
            const QStringList parentKey = key.mid(0, key.size() - 1);
            if (parentKey.isEmpty())
                valid = rootIds.contains(key.last());
            else {
                const auto ancestor = enumeration.descendants.constFind(parentKey);
                valid = accepted.contains(parentKey) &&
                        ancestor != enumeration.descendants.constEnd() &&
                        containerIdsOf(ancestor.value().containers).contains(key.last());
            }
        }
        Q_ASSERT(valid); // 键前缀一致性违约
        if (valid)
            accepted.insert(key, &it.value());
    }

    // ——先删（权威语义：缺席即删除，消失的组不可再进、不留幽灵入口）。根层（空键）
    // 与验收层之外的缓存层一律移出；此时换入已完成、显示层即根层，删除不触及任何行。
    // 被删层携带的 lastSelectedKey / 元数据标志随层一并消失（整树是重建不是合并）
    auto &levels = list->levels();
    for (auto it = levels.begin(); it != levels.end();) {
        if (!it.key().isEmpty() && !accepted.contains(it.key()))
            it = levels.erase(it);
        else
            ++it;
    }

    // ——后写：逐层 ensureLevel 写入 + 入列盖章（N55 §2.2：入列即盖章，与根层换入
    // 共用同一次盖章家族、不开第二处注入口）+ 簿记重置（lastSelectedKey / loadError /
    // 元数据标志：被覆盖层从头来过；视图级续播记忆不属层簿记，不在此触碰）+ 当场按
    // 提供者当前规则 sortLevelData——showLayer 命中已加载层只重建行不排序，不当场排
    // 对无元数据能力的来源有一段"显示出来是未排序的"窗口；零 I/O，与"换入时排"同形
    for (auto it = accepted.begin(); it != accepted.end(); ++it) {
        Level &lvl = list->ensureLevel(it.key());
        // N58 §2.2：LevelData 的裸容器 / 裸实体表换入即包成层元素（序号待收尾重编号）
        lvl.containers.clear();
        lvl.containers.reserve(it.value()->containers.size());
        for (auto &c : it.value()->containers)
            lvl.containers.push_back({std::move(c), 0});
        for (auto &e : it.value()->entities)
            stampOwnership(*e, list->list());
        lvl.entities.clear();
        lvl.entities.reserve(it.value()->entities.size());
        for (auto &e : it.value()->entities)
            lvl.entities.push_back({std::move(e), 0});
        lvl.lastSelectedKey.clear();
        lvl.loadError.clear();
        lvl.fileMetaLoaded = false;
        lvl.mediaMetaLoaded = false;
        // N58 §2.3 表第 2 行：每层写完即重编号，必须早于同函数内当场的 sortLevelData
        PlaylistListView::renumberEnumerationOrder(lvl);
        sortLevelData(lvl, rule);
    }
}

// N45 §2.9 归位：浏览路径置为队列根层并滚动到正播项（restorePlayingState 的层级版）。
// 播放中读队列根层时它恒在缓存（逐出必与归位同步，N53 §2.2），归位是同步的；
// 层未在缓存（会话恢复早期）则仅归位状态，其载入完成即换入（onLoadFinished 的收尾补换入）
void PlaylistWidget::revealQueueRoot(PlaylistListView *list) {
    if (!list)
        return;
    list->setBrowsePath(list->queueRoot());
    const QStringList ids = list->queueRootIds();
    if (list->displayedIds() == ids)
        return;
    const Level *lvl = list->findLevel(ids);
    if (lvl && lvl->loadError.isEmpty()) {
        rebuildRowsForLevel(list, *lvl, ids);
        restorePlayingState(list, indexOf(list)); // 标记 + 选中 + 滚动一次完成
        reactivateIfStale(list);                  // 队列根层元数据未预读时补排（与导航换入同判）
    } else if (lvl) {
        // 失败层：兜底渲染骨架与占位行（仅当刚失败且仍停在层上时可达——刚失败时
        // 行已对齐会首行早退，此处为防御保留，N53 §2.4）
        rebuildRowsForLevel(list, *lvl, ids);
        showPlaceholder(list, lvl->loadError);
    }
    // 层不在缓存：防御兜底，正常流程不可达（mpv 在播 ⇒ 队列根层在缓存——
    // 双击/停止态起播/收编/书签各锚点都在已加载层上锚定，见 markPlayingTab；
    // 层被逐出的那一步必同步归位播放目标并作废锚点，N53 §2.2，此后不再有指向
    // 队列根层的归位读取）；仅归位状态，不发起任务
}

// N45 §2.9：归位统一作用播放 tab——手动"定位到正在播放"（右键菜单启用条件已
// 限定当前即播放 tab，对菜单行为无差）与自动切曲归位共用；用户停留他 tab 时
// 播放 tab 的浏览路径同样归位，切回即见队列根层。
// 忙时延后：在途期浏览路径不得被搬移（§2.10"不写入任何层"的前提），置
// m_pendingReveal 由 setBusy(false) 补执行——与 requestTab 同属忙时延后家族
void PlaylistWidget::revealPlayingItem() {
    if (m_busy) {
        m_pendingReveal = true; // last-wins：归位幂等，多次触发一次执行
        return;
    }
    PlaylistListView *list = viewAt(playingTabIndex());
    if (!list)
        return;
    revealQueueRoot(list);
}

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
// 实体序 == 实体行序定位容器下标，并与行 EntityRole 交叉校验（不变式被破坏即拒绝）
PlaylistEntity::Ptr PlaylistWidget::entityPtrAt(PlaylistListView *list, int row) const {
    if (!list || row < 0 || row >= list->count())
        return nullptr;
    const Level *lvl = list->findLevel(list->browsePathIds());
    if (!lvl)
        return nullptr;
    int idx = 0;
    for (int i = 0; i < row; ++i) {
        if (isEntityRow(list->item(i)))
            ++idx;
    }
    const auto &entities = lvl->entities;
    if (idx >= int(entities.size()))
        return nullptr;
    const auto &candidate = entities.at(idx);
    if (candidate.entity.get() != entityFromItem(list->item(row)))
        return nullptr; // 实体/行序不变式被破坏：宁可拒绝也不返回错乱实体
    return candidate.entity;
}

// N47：行容器的按值副本——容器行按"容器序 == 容器行序"定位容器下标（占位 /
// 上一级行不占位，与 entityPtrAt 同形），并与行 ContainerRole 交叉校验
// （不变式被破坏即拒绝）；按值返回，跨菜单嵌套事件循环持有安全
std::optional<PlaylistProvider::Container> PlaylistWidget::containerAt(PlaylistListView *list,
                                                                       int row) const {
    if (!list || row < 0 || row >= list->count())
        return std::nullopt;
    const Level *lvl = list->findLevel(list->browsePathIds());
    if (!lvl)
        return std::nullopt;
    int idx = 0;
    for (int i = 0; i < row; ++i) {
        if (isContainerRow(list->item(i)))
            ++idx;
    }
    const auto &containers = lvl->containers;
    if (idx >= int(containers.size()))
        return std::nullopt;
    const auto &candidate = containers.at(idx).container;
    if (candidate.id != list->item(row)->data(ContainerRole).toString())
        return std::nullopt; // 容器/行序不变式被破坏：宁可拒绝也不返回错乱容器
    return candidate;
}

// 按行序批量删除实体（rows 必须降序；须在删行前调用）。
// 单遍自顶向下计实体行数定位各行容器下标（实体序 == 实体行序，占位 / 容器 /
// 上一级行不占位——N45 §2.4.3），随后按容器下标降序批量 erase
void PlaylistWidget::eraseEntitiesAt(PlaylistListView *list, const QList<int> &rows) {
    if (rows.isEmpty())
        return;
    Level *lvl = list->findLevel(list->browsePathIds()); // 只取不插（删实体不造层）
    if (!lvl)
        return;
    auto &entities = lvl->entities;
    QList<int> ascRows = rows;
    std::sort(ascRows.begin(), ascRows.end()); // 升序扫描，容器下标随之升序
    QList<int> idxs;
    int cursor = 0;
    int entityCount = 0;
    for (int i = 0; i < list->count(); ++i) {
        if (!isEntityRow(list->item(i)))
            continue;
        while (cursor < ascRows.size() && ascRows.at(cursor) < i)
            ++cursor; // 目标行是非实体行（防御）：跳过
        if (cursor < ascRows.size() && ascRows.at(cursor) == i) {
            idxs.append(entityCount);
            ++cursor;
        }
        ++entityCount;
    }
    for (int k = idxs.size() - 1; k >= 0; --k)
        entities.erase(entities.begin() + idxs.at(k));
    // N58 §2.3 表第 6 行：删除族的重编号单点收在本函数（只抹平跳号、不改相对序，
    // 但统一走一条纪律）
    PlaylistListView::renumberEnumerationOrder(*lvl);
}

// 行与实体的角色绑定（建行与换绑共用）：显示名 / 图标 / tooltip / EntityRole
void PlaylistWidget::setItemEntity(QListWidgetItem *item, const PlaylistEntity::Ptr &entity) {
    item->setData(Qt::DisplayRole, entity->title());
    item->setData(Qt::DecorationRole, entity->icon());
    item->setData(EntityRole, QVariant::fromValue<quintptr>(quintptr(entity.get())));
    // N56 §2.4：tooltip 第一行走显示出口（剥凭据），凭据不再明文可见。
    // N70：第二行给文件名（名称类排序键在读的那个串）——显示名与文件名分家后，外部表与
    // Jellyfin 表上两者不再相同，这一行让用户一眼对照；文件名为空则只给一行
    QString tip = entity->displayUrl();
    if (!entity->fileName().isEmpty())
        tip += QLatin1Char('\n') + entity->fileName();
    item->setToolTip(tip);
}

// ——N55 §2.2：盖章落点共用——归属 = 列表身份 + 该列表当前提供者的弱引用。
// 实体入列（进入层缓存或行）即盖章是归属不变式的入口；显式赋盖而非"空则补盖"——
// 盖与不盖由每个入列落点各自决定（收编 clone() 携带原归属的落点不调本函数）。
// owner 恒非空：各列表的 list() 与 defaultList() 都有"表必在"不变式兜着；万一为空，
// 与实体侧同调——不静默造一份"半盖章"（提供者空而身份非空那种组合根本无人能答形态），
// 早退回未盖章态，由 playUrl() 的断言把违约显影在真正的消费点上
void PlaylistWidget::stampOwnership(PlaylistEntity &entity, const Playlist *owner) {
    Q_ASSERT(owner);
    if (!owner)
        return;
    entity.setOwnership(owner->uuid(), owner->provider());
}

// 统一的条目构造：显示名 / 图标 / tooltip 全部取自实体；
// EntityRole 存实体裸指针（quintptr），实体所有权随行同步 push 进容器
QListWidgetItem *PlaylistWidget::makeItem(PlaylistListView *list, PlaylistEntity::Ptr entity) {
    QListWidgetItem *item = makeEntityRow(list, entity);
    // 所有权随行同步入仓；序号随所在路径的收尾重编号归正（N58 §2.3）
    list->entries().push_back({std::move(entity), 0});
    return item;
}

int PlaylistWidget::playingRowOf(const PlaylistListView *list) const {
    if (!list)
        return -1;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(PlayingRole).toBool())
            return i;
    }
    return -1;
}

// N25 + N60：回刷策略——吞域（推送会话在册）→ 空列表忽略 → 一致静默 → 占位/枚举失败
// 忽略 → 差异收编默认 tab。回刷不再重建正在播放的 tab：本地发起的变更一律本地先行，
// 回刷因此恒为一致而静默；真正的外部变更统一收编到默认 tab（mpv 会话镜像：临时、
// 不持久化、可随时被替换）。
// N60 §2.2：第 1 步从"内容域比对在途中间态"换成**因果域吞域**——本地批次执行期内
// 的回显无条件忽略，终点由屏障 reply 自带终值对账（onPushBarrier），无声吞掉成为
// 结构不可达。其余各步的判据不动
// N56 §2.5 判据 R + N61 §2.1：回显**先过一道整形**再进判据（未定形者补形态，已定形者
// 逐字节原样——"视图零加工"这句随整形进表即失效），实体侧按所在 tab 的提供者
// **现算**它会下发的形态（playTargets 的同源产物），逐字节比对——语义是"mpv 手里
// 那条串，正是我此刻会推的那条"，仅对未定形项退为"两者所指同一文件"（N61 §2.2 的
// 让步）。reuse 索引键与判据**同源同形**（同为整形后的这份形态），
// 否则"命中即克隆、克隆形态 ≠ 回显形态"会让下一次回显又判不一致而重复收编永续
void PlaylistWidget::setPlaylist(const QVariantList &items) {
    // N49：回刷入口是正播态的最新真相（覆盖停止 / 起播 / 外部变更收编），在此刷新按钮态
    updateBottomBarState();

    // 1. N60 §2.2 吞域：会话在册（本地批次执行期）的一切 playlist 回显**无条件忽略**，
    // 不做任何内容比对——"这条回显是不是我自己那批命令的中间态"是因果问题，旧的
    // 子序列判据属内容域、与因果无必然联系（命中即静默吞掉真外部改动）。批次终点由
    // 屏障 reply 自带终值在 onPushBarrier 对账兜底，故吞掉的都补得回来
    if (!m_openPushGens.isEmpty())
        return;

    QStringList pushed;
    QList<bool> current;
    pushed.reserve(items.size());
    current.reserve(items.size());
    for (const QVariant &v : items) {
        const QVariantMap map = v.toMap();
        // R（依据按 N61 §一 2 实测更正）：mpv 从不改写下发的串——相对进、相对出，
        // 绝对化只发生在 path 属性（它按环境变量 PWD 拼，可指向并不存在的路径，不作
        // 身份来源）。故每一项先过一道回显整形：未定形者补形态，已定形者逐字节原样。
        // 此后第 2–5 步只看得见整形后的串，不再有任何一处各自定形——判据侧仍属零加工，
        // 全表唯一的定形加工点就是本行（N56 删掉的那两处定形不复活）。第 5 步那一句
        // StreamTransit::unwrap 是剥包装、不是定形，两句分工不同（见该处注释）
        pushed.append(
            Util::Url::identityFromMpvEcho(map.value(QStringLiteral("filename")).toString()));
        current.append(map.value(QStringLiteral("current")).toBool());
    }

    // 2. 空列表无正播项、无收编意义（本地 stop/clear/删 tab/换源均已本地先行），一律忽略
    if (pushed.isEmpty())
        return;

    PlaylistListView *list = viewAt(playingTabIndex());
    if (!list)
        return;

    // 3. 占位/枚举失败态：N45 §2.9——比对基准 = 队列根层的实体序列（播放中读它时
    // 层恒在缓存：逐出必与归位同步，N53 §2.2）；显示层的占位行与之无关；
    // 队列根层枚举失败（loadError 非空）维持"忽略"现状
    if (!list->levelErrorOf(list->queueRootIds()).isEmpty())
        return;

    // 4. 逐行一致即静默：只翻转播放标记，正播行变化才迁移选中+滚动（无重建、无闪烁）。
    // 比对与标记都作用于队列根层；显示层非队列根层时不触碰任何行（标记随层状态，
    // 下次渲染队列根层时由归位权威出口按 mpv 现状重建）。
    // R：实体侧现算形态 = 归属提供者现算——N55 §2.3 归属化后与 playTargets 的下发路径
    // 是同一个出口 entity->playUrl()，判据与 reuse 键同形自动成立；"无表按恒等兜底"
    // 的分岔随归属化删除（归属不变式保证显示中实体恒有归属，无表异常态不再可达）
    const Level *ql = list->findLevel(list->queueRootIds());
    const bool sameAsQueueRoot =
        ql && int(ql->entities.size()) == pushed.size() && [ql, &pushed]() {
            for (int i = 0; i < int(ql->entities.size()); ++i) {
                if (!ql->entities.at(i).entity)
                    return false;
                if (ql->entities.at(i).entity->playUrl() != pushed.at(i))
                    return false;
            }
            return true;
        }();
    if (sameAsQueueRoot) {
        if (list->displayedIds() == list->queueRootIds()) {
            const int oldPlaying = playingRowOf(list);
            int newPlaying = -1;
            int entityIdx = 0; // current 按队列序（实体序），行按序对位到实体行
            for (int r = 0; r < list->count(); ++r) {
                QListWidgetItem *item = list->item(r);
                if (!isEntityRow(item))
                    continue;
                const bool isCur = entityIdx < current.size() && current.at(entityIdx);
                if (item->data(PlayingRole).toBool() != isCur)
                    item->setData(PlayingRole, isCur);
                if (isCur)
                    newPlaying = r;
                ++entityIdx;
            }
            if (newPlaying >= 0 && newPlaying != oldPlaying) {
                list->setCurrentItem(list->item(newPlaying));
                list->scrollToItem(list->item(newPlaying), QListWidget::EnsureVisible);
            }
        }
        return;
    }

    // 5. 差异＝外部变更：整体收编到默认 tab，原 playing tab 一字不动
    PlaylistListView *def = viewAt(0);
    if (!def)
        return;
    // N32：默认 tab 亦会异步刷新，收编重建即作废在途批次（cancel + 递增代际 + 清槽位 +
    // 复位忙时与遮罩由 abandonJob 一次完成）。仅当在途批次的目标表正是本表才作废——
    // 否则会误杀他表的在途批次
    if (m_job.target == def)
        abandonJob();

    // N55 §3.5：收编告知判据就地记账——"跨列表"（playingTabIndex() != 0）必须在
    // markPlayingTab(0) 之前取值：playingListAdopted 发射时 playingTabIndex() 已是 0，
    // 判据带不出来。"存在远端项"在收编处逐条对回显串判 Util::Url::isLocal（下方
    // 循环内累计）。两样都在本步内算好记账，playingListAdopted 签名不变、不加载荷
    const int prevPlayingTab = playingTabIndex();
    bool hasRemote = false;

    // N31：清前快照收编源实体，建"下发形态 → 源实体"索引（constFind 拷贝只延长源寿命，
    // 自覆盖情形清表后克隆仍可用）。同形态取首个；首个未解析而重复项已解析时
    // 复用落空无正确性代价（缺失元数据按需链自补）。
    // N45：收编源 = 正在播放 tab 队列根层的实体序列（§2.9）。
    // R：索引键与判据**同源同形**——N55 §2.3 归属化后同为唯一出口 entity->playUrl()。
    // 命中支按形态分流（N76 §2.2）：转接命中原样克隆（携带源实体原身份、缓存与原
    // 归属，归属提供者现算重铸同一形态）；直接命中以播放 URL 重造默认归属实体
    // （身份 = 含凭据的下发串，判据同样逐字节闭合）——两条路的下一次回显都必静默
    // （自愈不变式）
    QHash<QString, PlaylistEntity::Ptr> reuse;
    if (const Level *srcLvl = list->findLevel(list->queueRootIds())) {
        for (const auto &src : srcLvl->entities) {
            if (!src.entity)
                continue;
            const QString key = src.entity->playUrl();
            if (!reuse.contains(key))
                reuse.insert(key, src.entity);
        }
    }

    // N55 §3.6：未命中的转接回显＝异常数据。合法的 oxbow:// 只有两条来路——插件自己
    // 从归属真列表的实体现算下发（必命中 reuse，索引键就是同一个函数），或收编项经
    // clone() 带着原归属进默认 tab（同样命中）。故"转接形态且未命中"当且仅当外部把
    // 本仓私有协议的串喂进了 mpv 队列（IPC loadfile / append，或清单文件里写着它）。
    // 不按串里的 uuid 折给那个列表——那等于把私有协议当对外承诺的入口，拿真列表的
    // 凭据与端点替来路不明的串背书。判据只认 StreamTransit::isTransitUri（不越面），
    // 命中 reuse 的合法转接串（收编项所在队列被再次回显）不判异常
    bool hasAnomaly = false;
    for (const QString &echo : pushed) {
        if (StreamTransit::isTransitUri(echo) && !reuse.contains(echo))
            hasAnomaly = true;
    }
    if (hasAnomaly) {
        // 处置 1：停播 + 清正播表标记——异常项就在正播队列里，这份队列已不可信，
        // 不逐条挑。先停后剔不可调换：先剔后停会留一个"表已短而队列仍在"的窗口；
        // 先 stop 则 mpv 侧队列当场清空，随后的空列表回显被第 2 步忽略，对位关系
        // 已作废，剔除才安全
        emit stopRequested();
        clearPlayingMarks(list);
    }

    // 记录收编前的选中行（单选）：推送无 current 项（如停止态外部编辑）时
    // 整表替换会把界面选中清空（仅显示问题），沿用旧重建语义恢复
    const int prevRow = def->currentRow();
    def->clear();
    // N45：收编重建＝直写默认 tab 队列根层（层在缓存即"已加载"，§2.5）：
    // 先清实体仓再随建行 push_back；条目集合已变，两类已读元数据一并失效
    Level &defLvl = def->ensureLevel(def->queueRootIds());
    defLvl.entities.clear();
    defLvl.loadError.clear();
    defLvl.mediaMetaLoaded = false; // N29
    defLvl.fileMetaLoaded = false;  // N29

    QListWidgetItem *newCurrent = nullptr;
    for (int i = 0; i < pushed.size(); ++i) {
        // N55 §3.6 处置 2：异常行就地剔除——不进默认 tab（不盖归属也不留表），剩余项
        // 照常走收编；判据在未命中支之前逐行施判
        if (hasAnomaly && StreamTransit::isTransitUri(pushed.at(i)) &&
            !reuse.contains(pushed.at(i)))
            continue;
        // 未命中支（N31 / N61 不变）：回显是"播放形态"，先经 StreamTransit::unwrap
        // 折回身份再进工厂（N56 §2.6：工厂只收已定形身份）。unwrap 是纯解码（非
        // oxbow:// 原样返回），这一句不得省——漏折即让转接项以 oxbow:// 落进实体身份，
        // 破 N52 / N56 的折叠纪律（整形只补形态、不拆包装：oxbow:// 含 "://" 属已定形，
        // 逐字节放行）。工厂走回显专用入口 fromMpvEcho——补不出形态的串（空 filename、
        // 裸 file:）不撞 fromPath 那句断言，原样落 UnknownEntity 保行。
        // 两条路不得互换：索引键若改用折叠键（小写归一）就与判据不同形，命中的克隆会被
        // 下次回显判成"外部改动"而重复收编永续；建实体若改用折叠键，则展示形态被
        // 小写化（破 B15）。未命中项盖默认列表归属（N55 §2.2）——默认列表是收编、
        // 命令行与手工添加项的归属来源。含补不出形态落占位的 UnknownEntity：
        // isLocalFile() 为假 ⇒ StaticProvider 恒等下发 ⇒ 判据两侧同为那份原串 ⇒
        // 静默，不落重复收编
        //
        // N76 §2.2：命中支按形态分流——
        // 转接命中原样收编：clone() 携带源实体原身份、缓存与原归属，归属提供者现算
        // 重铸同一形态，判据闭合（N55 自愈不变式）；书签侧对这类项维持拒收（N76 §2.1）。
        // 直接命中当场以播放 URL 重造新实体：fromPath 严格入口即可（命中即该串与源
        // 实体 playUrl() 逐字节相等，必已定形，不撞断言），身份 = 含凭据的下发串、
        // 归属盖默认列表——项从此自含可播（与命令行 / 手工远端串同类），不随来源列表
        // 退役被清扫（applySourceRetired 的实际作用面收窄为只剩转接收编项）。判据
        // 闭合性保持：新实体 playUrl() = StaticProvider 恒等 = path() = 回显串，逐字节
        // 相等。显示面与缓存经友元注入口从源实体拷贝（N76 §2.3 定案）：N70 注入的
        // title / fileName 与 N65 playOptions 不因重造退化，已解析元数据顺路带走
        // （cached*() 有值才拷；icon 无注入口，惰性重算）
        const auto rit = reuse.constFind(pushed.at(i));
        const bool hit = rit != reuse.constEnd();
        PlaylistEntity::Ptr entity;
        if (hit && StreamTransit::isTransitUri(pushed.at(i))) {
            entity = rit.value()->clone();
        } else if (hit) {
            entity = PlaylistEntity::fromPath(pushed.at(i), m_registry);
            stampOwnership(*entity, m_lists->defaultList());
            const PlaylistEntity::Ptr &src = rit.value();
            entity->setFilename(src->fileName());
            entity->setTitle(src->title());
            entity->setPlayOptions(src->playOptions());
            if (const auto *fm = src->cachedFileMeta())
                entity->setFileMeta(*fm);
            if (const auto *mm = src->cachedMediaMeta())
                entity->setMediaMeta(*mm);
        } else {
            entity = PlaylistEntity::fromMpvEcho(StreamTransit::unwrap(pushed.at(i)), m_registry);
            stampOwnership(*entity, m_lists->defaultList());
        }
        QListWidgetItem *item = makeItem(def, std::move(entity));
        // N55 §3.6 处置 2：异常支的重建不写 PlayingRole——队列即将作废，回显里的
        // current 是停播前的旧值，照抄会挂出一个 mpv 已不持有的正播行（与 runFourStep
        // 里"stop 即标记同步失效"同一口径）
        if (!hasAnomaly && current.at(i)) {
            item->setData(PlayingRole, true);
            newCurrent = item;
        }
        // N55 §3.5："存在远端项"对回显串逐条判 isLocal（被收编项集合；N61 占位行
        // 对空串误报为远端，按规格接受——多余的模态告知，方向上"宁可多告知"）
        if (!Util::Url::isLocal(pushed.at(i)))
            hasRemote = true;
    }
    // N58 §2.3 表第 5 行：收编重建完成后重编号
    PlaylistListView::renumberEnumerationOrder(defLvl);
    // N45：收编重建即根层显示——显式落显示层记录，不依赖"默认 tab 恒平铺"不变式
    def->setDisplayedIds(def->queueRootIds());
    // N31：收编内容序 == mpv 会话序，随收编复位默认 tab 排序规则为无序（保留方向，
    // 与 addFiles 同一形状）；否则存量元数据规则会让下次切入经 reactivateRescan
    // 重排并四步式重推，吃掉推送序
    if (auto *p = providerOfView(viewAt(0)))
        p->setSortRule({SortKey::None, p->sortRule().ascending}); // N57 §2.9：值对象复位
    if (newCurrent) {
        def->setCurrentItem(newCurrent);
        // 跟随滚动（N4）：正播项变动时最小位移滚入可见范围
        def->scrollToItem(newCurrent, QListWidget::EnsureVisible);
    } else if (!hasAnomaly && prevRow >= 0 && prevRow < def->count()) {
        // 异常支不按旧位置设选：那一步剔过行，prevRow 这个下标已不指着收编前的同一条
        // 目，按它设选等于把选中随手落到别处；而队列即将作废，此处也没有"接着播这一
        // 行"的语义要护。非异常支的按位恢复是既有语义，不动
        def->setCurrentRow(prevRow);
    }
    markPlayingTab(0); // B10：异 tab 转移清旧标记；同 tab 早退（默认 tab 自覆盖不误清）
    // N30：记忆不整体作废——按正播行重建（定位不到才作废）。整体 remove 会让随后的
    // 排序 / 移除走四步式时 resolveResumeRow 无命中，表现为"操作后播放直接停掉"。
    // N45：重建结果落在队列根层的实体序上（rebuildMemoryAfterAdopt 内部换算）
    rebuildMemoryAfterAdopt(def, newCurrent ? def->row(newCurrent) : -1);
    emit contentsChanged();    // A17：条目数已变，刷新导航启停
    emit playingListAdopted(); // N25：镜像已重新一致，MainWindow 刷新标题/OSD 正播名
    // N55 §3.5：收编告知——跨列表收编且被收编项中存在远端项才弹（全本地收编免告知；
    // 命令行启动 prevPlayingTab 恒为 0，不告知）。文案只述"远端项已收编、可能含访问
    // 凭据"，不做逐项断言、不提项数与逐项清单
    if (prevPlayingTab != 0 && hasRemote)
        queueDeferredNotice(tr("The playlist you were playing was changed externally. Remote "
                               "entries have been adopted into the default playlist and may "
                               "contain access credentials."));
    // N55 §3.6 处置 4：异常数据告知（与收编告知合用出口；同一次第 5 步既判出异常又
    // 触发收编告知时，两条文案并入同一次弹窗）。不点列表名——那条串的来源本就不可信，
    // 也不得按它读 uuid。不恢复播放：用户要继续，从默认 tab 手动双击剩余项
    if (hasAnomaly)
        queueDeferredNotice(tr("An invalid playback address was detected and removed. "
                               "Playback has been stopped."));
    requestTab(0); // 可见 tab 跟随；N19 忙时排队，枚举/预读结束后补执行
}

void PlaylistWidget::addFiles(const QStringList &files) {
    PlaylistListView *list = viewAt(currentIndex());
    if (!list || files.isEmpty() || m_busy)
        return; // N19：忙时不接受手工增补（预读快照防污染）

    // N9/N27：不可修改的 tab 内容来自枚举，忽略手工添加（拖放与菜单同守卫）。
    // 静态 tab 增删即回无序态：复位排序规则（保留方向）
    if (!isModifiableTab(currentIndex()))
        return;
    if (auto *p = providerOfView(viewAt(currentIndex())))
        p->setSortRule({SortKey::None, p->sortRule().ascending}); // N57 §2.9：值对象复位
    // N29：条目集合变化，两类已读元数据一并失效（N45：落在当前显示层）
    {
        Level &lvl = list->levelOf();
        lvl.mediaMetaLoaded = false;
        lvl.fileMetaLoaded = false;
    }

    // N25：本地先行——播放中 tab 也立即插行（回刷只会把"本地未改"的差异搬去默认 tab），
    // 插入后本地序列即等于 mpv 追加后的序列，回刷恒一致而静默。
    // N56 §2.7：建好的实体表随 filesAdded 信号交 MainWindow（喂 mpv 前最后一刻再映射形态）
    PlaylistEntity::List created;
    created.reserve(files.size());
    for (const QString &file : files) {
        auto entity = PlaylistEntity::fromPath(file, m_registry);
        stampOwnership(*entity, list->list()); // N55 §2.2：入列即盖章（归属 = 当前列表）
        created.push_back(entity);
        makeItem(list, std::move(entity));
    }
    // N58 §2.3 表第 4 行：追加完成后重编号
    PlaylistListView::renumberEnumerationOrder(list->levelOf());
    // 非默认本地 tab 添加后持久化（默认 tab 永不落盘，N9/N4 既定）
    if (currentIndex() != 0)
        savePlaylists();
    // N60 §2.4：会话只在**喂入侧**开——播放中 tab 的追加经 filesAdded 落到
    // MainWindow::onFilesAdded，那里才是喂 mpv 的地方，守卫也在那里
    emit filesAdded(std::move(created));
}

// N60 §2.2：推送会话的开与闭。**只在喂入侧成对出现**（§2.4：四个喂 mpv 列表内容的
// 入口各持一个 PushSession 局部对象），配对由 RAII 保证、不靠纪律——世代泄漏等价
// 于永久全静默吞域（集合非空则一切回显被吞），比原缺陷更重（§2.3 第 3 条）
quint64 PlaylistWidget::beginLocalPush() {
    const quint64 gen = ++m_pushGenCounter; // 自 1 起，0 留作"非世代"防御值
    m_openPushGens.insert(gen);
    return gen;
}

void PlaylistWidget::endLocalPush(quint64 gen) {
    // 关批发屏障读：reply 自带队列终值，是本次会话的终点（§2.1——喂入全同步，
    // 此刻 mpv 队列已是本批终态，故该读取到的即终态）
    if (m_mpv && m_mpv->pushBarrier(gen))
        return;
    // 发射失败（或无控制器）不会产生 reply，世代号就地摘除——批内变更的迟投回显
    // 会自然到达并走正常判定。残余见规格 §2.2：该路径只在 mpv 正在退出 / 参数非法
    // 时出现，下一轮回显即自愈，不为此加超时兜底（否则世代泄漏成永久吞域）
    if (!m_mpv)
        qDebug() << "[oxbow] push barrier skipped, no controller: gen" << gen; // 装配异常才到这
    m_openPushGens.remove(gen);
}

// N60 §2.2 / §2.3：屏障 reply 的终点对账。世代递减只发生在 GUI 线程的本槽内
// （§2.3 第 1 条：部分态回显在迟投规则下排在 reply 之前，若先减再处理会被误收编）；
// 对账在本槽内同步完成，不得延后（§2.3 第 2 条：延后可能落进下一个会话的吞域）
void PlaylistWidget::onPushBarrier(quint64 gen, bool ok, const QVariantList &items) {
    if (!m_openPushGens.remove(gen))
        return; // 未命中即忽略（防御未来新增 async 请求者）

    // 暂存按世代覆盖、与 ok 无关（§2.3 第 4 条——async 之间亦无到达顺序保证，
    // 取"已收到 reply 中的最大世代"，不取"最后到达的那条"）
    if (gen >= m_lastBarrier.gen)
        m_lastBarrier = Barrier{gen, ok, items};

    // 观测点（§四.3）：摘除后在册规模须在下面对账**之前**取——对账的收编会就地
    // 再起会话，事后读到的集合规模不是这一瞬的
    const int stillOpen = int(m_openPushGens.size());
    qDebug() << "[oxbow] push barrier gen" << gen << "ok" << ok << "still open" << stillOpen;
    if (stillOpen > 0)
        return;

    const Barrier settled = m_lastBarrier;
    m_lastBarrier = Barrier();
    // 合成回显：走与原生回显完全相同的一条管线（含 current 标记，勾选标记随既有
    // 比对校正）。ok 为假（读失败 / 载荷非法，mpv 正在退出）时不判、也不回落次新
    // 世代的 ok 载荷——只递减返回（§2.3 第 2 条）
    if (settled.ok)
        setPlaylist(settled.items);
    qDebug() << "[oxbow] push barrier settled with gen" << settled.gen << "ok" << settled.ok;
}

// N11：主菜单"打开文件"固定落点——整体替换默认 tab 内容并作废其续播记忆
// （清空列表隐含记忆失效，Q3 裁决）。默认 tab 永不持久化（N9/N4 既定），
// 也不发 filesAdded：立即播放由 MainWindow 经统一入口 onPlayRequested 驱动，
// 避免误入播放中 append 分支
PlaylistEntity::List PlaylistWidget::setDefaultTabFiles(const QStringList &files) {
    PlaylistListView *list = viewAt(0);
    if (!list || files.isEmpty())
        return {};
    // N30：主菜单"打开文件"与书签跳转不受 m_busy 限制，此时可能有本表的刷新批次在途。
    // 不作废则枚举收尾到达后用回推的旧内容整表覆盖，视图回到旧内容而 mpv 在播新文件。
    // N32：统一走 abandonJob（仅当在途批次的目标表即本表）
    if (m_job.target == list)
        abandonJob();
    if (m_pendingLoad.list == list)
        m_pendingLoad = PendingLoad{};
    // 内容由本函数直写即已就位：不置回会让下次切入默认 tab 误触发一次枚举，
    // 用提供者里的旧 m_urls 覆盖刚打开的文件。
    // N45：直写即层入缓存（根层；层在缓存即"已加载"，§2.5）
    Level &lvl = list->ensureLevel(list->queueRootIds());
    lvl.loadError.clear();
    list->clearMemory(); // N11：直写作废续播记忆
    list->clear();
    lvl.entities.clear();        // 同步点：重建＝先清实体仓再随建行 push_back
    lvl.mediaMetaLoaded = false; // N29：条目集合变化，两类已读元数据一并失效
    lvl.fileMetaLoaded = false;
    PlaylistEntity::List created;
    created.reserve(files.size());
    for (const QString &file : files) {
        auto entity = PlaylistEntity::fromPath(file, m_registry);
        // N55 §2.2：入列即盖章——归属 = 默认列表（经 PlaylistRegistry::defaultList()
        // 取得，与 providerOfView(viewAt(0)) 同一实例；默认列表跨会话恒定、不可删
        // 不可换源，以它为归属的实体来源永不过期）
        stampOwnership(*entity, m_lists->defaultList());
        created.push_back(entity);
        makeItem(list, std::move(entity));
    }
    // N58 §2.3 表第 3 行：清仓重建后重编号
    PlaylistListView::renumberEnumerationOrder(lvl);
    // N45：直写重建即根层显示（与收编同款，显式落显示层记录）
    list->setDisplayedIds(list->queueRootIds());
    // N31：内容序 == 打开顺序，随直写复位排序规则为无序（保留方向）——否则存量
    // 元数据规则会在下次切入默认 tab 时经 reactivateRescan 重排，吃掉刚打开的顺序。
    // 静态源 enumerate 不读排序键，与被作废的在途枚举批次无实际竞争
    if (auto *p = providerOfView(viewAt(0)))
        p->setSortRule({SortKey::None, p->sortRule().ascending}); // N57 §2.9：值对象复位
    // N56 §2.7：回传造好的实体表，调用方据此驱动统一播放入口
    return created;
}

void PlaylistWidget::repaintAllLists() {
    for (int i = 0; i < count(); ++i) {
        if (PlaylistListView *l = viewAt(i))
            l->viewport()->update();
    }
}

// B18/N26：读取失败（seconds<=0）存 nullopt——时长列经取值链回落到实体缓存元数据
// （mpv 推送未知而实体已解析时显示 MediaInfo 时长，B18 口径以 N26 为准）。
// N56 §2.7：键由调用方取正播实体的 key() 传入，与 m_duration 仓的读取侧（实体 key）同形
void PlaylistWidget::applyDuration(const QString &key, double seconds) {
    if (key.isEmpty())
        return;
    (*m_duration)[key] = seconds > 0 ? std::optional<double>(seconds) : std::nullopt;
    repaintAllLists();
}

// ——续播记忆三步维护法（N4）——
// N45 §2.4.2/§2.8：mem.lastPos 是队列根层内的实体序下标（恰等于 mpv 的
// playlist-pos），不受"当前显示的是哪一层"影响。以下读写一律在队列根层的
// 实体序列上进行，不再触碰显示行

QString PlaylistWidget::captureMemoryPath(PlaylistListView *list) const {
    const Memory mem = list->memory();
    const Level *lvl = list->findLevel(list->queueRootIds());
    if (!lvl || mem.lastPos < 0 || mem.lastPos >= int(lvl->entities.size()))
        return {};
    if (const PlaylistEntity *entity = lvl->entities.at(mem.lastPos).entity.get())
        return entity->key();
    return {};
}

void PlaylistWidget::restoreMemoryPath(PlaylistListView *list, const QString &captured,
                                       bool hadMemory) {
    if (!hadMemory)
        return;
    Memory mem = list->memory();
    const Level *lvl = list->findLevel(list->queueRootIds());
    const int n = lvl ? int(lvl->entities.size()) : 0;
    if (!captured.isEmpty()) {
        for (int i = 0; i < n; ++i) {
            if (const PlaylistEntity *entity = lvl->entities.at(i).entity.get();
                entity && entity->key() == captured) {
                mem.lastPos = i;
                list->setMemory(mem);
                return;
            }
        }
    }
    mem.lastPos = -1; // 记忆条目已在本次变更中被删除
    list->setMemory(mem);
}

// N30：回刷收编后的记忆重建（替代原先的整体作废）。内容虽被整体替换，mpv 仍在播
// 新表里的某一项——按正播行重建记忆，使随后的四步式（排序 / 移除）恒有续播目标。
// 优先用推送自带的 current 行；推送无 current 时退回正播路径出口（N61 §2.3 的
// MpvController::currentPlayingPath，不再裸读 mpv 的 path 属性）定位
// （与 restorePlayingState 同口径，经 matches 键级判等）。两者都定位不到即 mpv
// 未播，此时才作废记忆——与"不在播就不续播"同义，不是记忆缺失
void PlaylistWidget::rebuildMemoryAfterAdopt(PlaylistListView *list, int playingRow) {
    if (!list)
        return;
    const Level *lvl = list->findLevel(list->queueRootIds());
    // N45：入参 playingRow 是行序，记忆 lastPos 是队列根层实体序，先换算
    int idx = rowToEntityIndex(list, playingRow);
    if (idx < 0 && m_mpv) {
        const QString cur = m_mpv->currentPlayingPath();
        if (!cur.isEmpty() && lvl) {
            for (int i = 0; i < int(lvl->entities.size()); ++i) {
                if (const PlaylistEntity *e = lvl->entities.at(i).entity.get();
                    e && e->matches(cur)) {
                    idx = i;
                    break;
                }
            }
        }
    }
    const PlaylistEntity *entity = (lvl && idx >= 0 && idx < int(lvl->entities.size()))
                                       ? lvl->entities.at(idx).entity.get()
                                       : nullptr;
    if (!entity) {
        list->clearMemory();
        return;
    }
    Memory mem;
    mem.lastPos = idx;
    mem.lastPath = entity->key();
    // 进度即时取一次：播放中随后会被 timePosChanged 心跳覆盖，暂停态没有心跳，
    // 不取则记忆缺进度，四步式重推后回到开头（同 recordPlayback 的写法）
    mem.lastTimePos = m_mpv ? m_mpv->propertyDouble(QStringLiteral("time-pos"), -1.0) : -1.0;
    list->setMemory(mem);
}

void PlaylistWidget::loadPlaylists() {
    if (m_configFile.isEmpty())
        return;

    QSettings settings(m_configFile, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Playlists"));
    const int savedCount = settings.value(QStringLiteral("count"), 0).toInt();
    settings.endGroup();

    for (int i = 0; i < savedCount; ++i)
        readTabGroup(settings, i);
}

// N57 §2.7 读序：逐节读三段（名字与身份 / 提供者参数 / 记忆与队列根）→ 登记进注册表
// （接回存量身份或撞车新铸）→ 建视图 → 装视图态 → 状态齐备**后**才挂 tab。
// N28：addTab 一旦触发 currentChanged，onTabChanged 会拿尚未装配到位的提供者发起首载，
// 把本该来自 INI 的内容填成空表——先备齐再挂，根除该窗口
void PlaylistWidget::readTabGroup(QSettings &settings, int index) {
    settings.beginGroup(QStringLiteral("Playlist_%1").arg(index));
    const QString name = settings.value(QStringLiteral("name")).toString();
    // 仅跳过无名节（C3）：默认 tab 从不落盘（结构上不在持久列表里），文本比较
    // 在 i18n 下脆弱、且会误伤被改名为“默认”的正常列表
    if (name.isEmpty()) {
        settings.endGroup();
        return;
    }
    // N57 §2.4：列表身份的读回在编排点——提供者不认识身份。fromString 对空串与
    // 非法串一律返回 nil ⇒ 按缺键另铸新值（不迁移旧形态）；INI 里手改大小写或带
    // 花括号的存量值自此归一命中（可观察差分②）
    const QUuid storedUuid = QUuid::fromString(settings.value(QStringLiteral("uuid")).toString());

    Memory mem;
    mem.lastPos = settings.value(QStringLiteral("lastPos"), -1).toInt();
    mem.lastTimePos = settings.value(QStringLiteral("lastTimePos"), -1.0).toDouble();
    mem.lastPath = settings.value(QStringLiteral("lastPath")).toString();
    // N26：记忆存条目 key（Util::Url::key 归一形态）。旧 INI 存原始形态，
    // 读取侧归一桥接（新写入的 key 再归一幂等，往返无损）；空值防归一成 CWD
    if (!mem.lastPath.isEmpty())
        mem.lastPath = Util::Url::key(mem.lastPath);

    // N9：提供者反序列化完全自治（静态回填条目、目录读参数与排序键），
    // 内容统一经 enumerate 取得（N28：静态表同样延迟到首次激活）；
    // N35：registry 转交（静态持久化的 http 条目重载后仍得 HttpEntity）
    auto provider = PlaylistProvider::load(settings, m_registry);
    // N45 §2.8：队列根恢复——重启后浏览路径直接取 queueRoot（§2.8：与续播记忆
    // 会打架的另一份浏览位置不持久化）。两列各读一个数组键，尺寸失配降级为
    // id 自兼标题（不维护旧键映射）
    const QStringList qrIds = settings.value(QStringLiteral("queueRootIds")).toStringList();
    const QStringList qrTitles = settings.value(QStringLiteral("queueRootTitles")).toStringList();
    QList<PathSeg> queueRoot;
    queueRoot.reserve(qrIds.size());
    for (int k = 0; k < qrIds.size(); ++k)
        queueRoot.append({qrIds.at(k), k < qrTitles.size() ? qrTitles.at(k) : qrIds.at(k)});
    settings.endGroup();

    PlaylistListView *list = createView(m_lists->append(name, std::move(provider), storedUuid));
    if (!queueRoot.isEmpty()) {
        // N45 §2.8：重启后浏览路径直接取队列根（另一份浏览位置不持久化）
        list->setQueueRoot(queueRoot);
        list->setBrowsePath(queueRoot);
    }
    if (mem.lastPos >= 0 || !mem.lastPath.isEmpty())
        list->setMemory(mem);
    // N28：所有 tab 内容一律经异步枚举填充，延迟到首次激活
    // （N45：层不在缓存即未加载，无独立标志可置）
    addTab(list, name);
}

void PlaylistWidget::savePlaylists() {
    if (m_configFile.isEmpty())
        return;

    QSettings settings(m_configFile, QSettings::IniFormat);
    // 清理旧的播放列表节（节名的第三处知识点）
    for (const QString &g : settings.childGroups()) {
        if (g.startsWith(QStringLiteral("Playlist_")) || g == QLatin1String("Playlists"))
            settings.remove(g);
    }

    // N57 §2.7：节序权威源是注册表的有序 vector，遍历只走 persistentAt——
    // 结构上不含默认列表，无需"跳过索引 0"那一类位置判据
    for (int i = 0; i < m_lists->persistentCount(); ++i)
        writeTabGroup(settings, m_lists->persistentAt(i), i);

    settings.beginGroup(QStringLiteral("Playlists"));
    settings.setValue(QStringLiteral("count"), m_lists->persistentCount());
    settings.endGroup();
}

// N57 §2.7 写序：静态条目的回传 → 名字与身份（`Playlist::save`）→ 提供者
// （类型键、转接偏好、自身参数、排序两键）→ 视图写记忆与队列根。
// **回传必须与落盘同处**（N27）：守卫条件与回传形态归视图，拆开会静默丢内容。
// N28 回传守卫：仅对已加载的表执行——未加载表实体集合为空，回传会清空
// StaticProvider 自持内容造成永久丢数据。先判状态再取实体容器，避免给未加载层
// 插入幽灵"已加载"层。在途加载的目标层在成功前不在缓存（或带 loadError），故
// setItems 与工作线程的 enumerate() 不可能并发——日后改动加载置位时序须复核本条
void PlaylistWidget::writeTabGroup(QSettings &settings, Playlist *list, int index) {
    // N62 §2.3：回传守卫收成 backfillItems 单点，判据一字不变（非行为变化）
    backfillItems(list);
    PlaylistListView *view = viewAt(tabIndexOfList(list->uuid()));

    settings.beginGroup(QStringLiteral("Playlist_%1").arg(index));
    list->save(settings);                     // name + uuid
    list->provider()->saveSettings(settings); // 静态：类型+条目；目录：类型+参数
    if (view) {
        const Memory mem = view->memory();
        // N9：记忆三字段（路径+序号+进度）静态/动态均持久化
        if (mem.lastPos >= 0 || !mem.lastPath.isEmpty()) {
            settings.setValue(QStringLiteral("lastPos"), mem.lastPos);
            settings.setValue(QStringLiteral("lastTimePos"), mem.lastTimePos);
            settings.setValue(QStringLiteral("lastPath"), mem.lastPath);
        }
        // N45 §2.8：队列根持久化——id 与标题两列各写一个数组键（QSettings 原生
        // QStringList，不自造分隔符）。重启后浏览路径直接取它：落在记忆项所在层，
        // 且拼"上一级"相对路径的祖先标题现成
        const QList<PathSeg> queueRoot = view->queueRoot();
        if (!queueRoot.isEmpty()) {
            QStringList ids, titles;
            ids.reserve(queueRoot.size());
            titles.reserve(queueRoot.size());
            for (const PathSeg &seg : queueRoot) {
                ids << seg.id;
                titles << seg.title;
            }
            settings.setValue(QStringLiteral("queueRootIds"), ids);
            settings.setValue(QStringLiteral("queueRootTitles"), titles);
        }
    }
    // N9：排序规则已随提供者序列化（saveSettings 内部），此处无额外处理
    settings.endGroup();
}

// N25：写入前校验 (tab, pos) 与 mpv 实际正播文件一致。外部结构变更在途时
// playlist-pos 先于 playlist 推送更新（如终端移动正播条目），旧 playing tab 配
// mpv 新索引会把别行写成"正在播放"（实测：移动后原 tab 续播记忆串位）。
// time-pos 心跳仅在文件真正播放后才带 v>0；N61 §2.3 换源后本校验读的是队列第 pos 条
// 的 filename（经正播路径出口整形），与镜像第 pos 项比对——守住的是"mpv 队列 ↔ 本地
// 镜像"的在途结构变更，不再顺带拦"pos 推送与正播文件的错位"：该错位改由 MainWindow
// 的 switchedItem ? 0.0 : m_timePos 那道既有守卫承载（N61 §2.3 记在案）
void PlaylistWidget::recordPlayback(int tabIndex, int pos, double seconds) {
    if (tabIndex < 0 || pos < 0) // 负索引无效；默认 tab 允许进程内记忆（不持久化）
        return;
    PlaylistListView *list = viewAt(tabIndex);
    if (!list)
        return;
    // N45 §2.4.2：pos 是 mpv 的 playlist-pos，即队列根层内的实体序下标——
    // 不再当显示行行号用（层级化后"浏览别层期间切曲"会取到别层的行、key 比对
    // 失败、静默早退）。队列根层被逐出必与播放目标归位同步（N53 §2.2），
    // 播放中读它时恒在缓存，未在缓存即无记忆可写
    const Level *lvl = list->findLevel(list->queueRootIds());
    if (!lvl || pos >= int(lvl->entities.size()))
        return;
    const QString mpvPath = m_mpv ? m_mpv->currentPlayingPath() : QString();
    const PlaylistEntity *entity = lvl->entities.at(pos).entity.get();
    // N56 §2.5：键级判等经 matches（整形后的回显进判据，归一在实体内部完成）
    if (mpvPath.isEmpty() || !entity || !entity->matches(mpvPath))
        return; // 索引与正播文件对不上：结构变更在途，不写本表记忆
    Memory &mem = list->memory();
    mem.lastPos = pos;
    mem.lastTimePos = seconds;
    // N9/N26：路径随写入点采集（路径+序号双存）。存条目 key（身份），比对免疫来源形态漂移
    mem.lastPath = entity->key();
}

void PlaylistWidget::clearResumeProgress(int tabIndex) {
    if (PlaylistListView *list = viewAt(tabIndex))
        list->clearResumeProgress(); // B9：清进度、留最后播放项
}

int PlaylistWidget::resolveResumeRow(int tabIndex) const {
    // N45 §2.8：寻位一律在队列根层的实体序上做、再换算行序返回（本函数返回行序，
    // 供选中与滚动；队列语义的调用点直接用 resumeEntityIndexOf 的实体序）
    const int idx = resumeEntityIndexOf(tabIndex);
    return idx < 0 ? -1 : entityIndexToRow(viewAt(tabIndex), idx);
}

// N45：记忆定位内核——序号校验优先（含重复项场景的精确恢复），序号失效按路径
// 取首命中；一律在队列根层的实体序列上进行。队列根层不在缓存（未加载）返回 -1
int PlaylistWidget::resumeEntityIndexOf(int tabIndex) const {
    const PlaylistListView *list = viewAt(tabIndex);
    if (!list)
        return -1;
    const Memory mem = list->memory();
    if (mem.lastPos < 0 && mem.lastPath.isEmpty())
        return -1;
    const Level *lvl = list->findLevel(list->queueRootIds());
    if (!lvl)
        return -1;
    const int n = int(lvl->entities.size());
    // ① 序号处路径校验：一致则信任序号（含重复项场景的精确恢复）
    if (mem.lastPos >= 0 && mem.lastPos < n) {
        if (const PlaylistEntity *entity = lvl->entities.at(mem.lastPos).entity.get();
            entity && entity->key() == mem.lastPath)
            return mem.lastPos;
    }
    // ② 序号失效：按路径取第一个命中
    if (!mem.lastPath.isEmpty()) {
        for (int i = 0; i < n; ++i) {
            if (const PlaylistEntity *entity = lvl->entities.at(i).entity.get();
                entity && entity->key() == mem.lastPath)
                return i;
        }
    }
    return -1;
}

double PlaylistWidget::lastTimePosOf(int tabIndex) const {
    const PlaylistListView *list = viewAt(tabIndex);
    return list ? list->memory().lastTimePos : -1.0;
}

// N57 §2.6：原 indexOfName 退场——按名反查的唯一调用点（会话恢复）已改按身份直查

void PlaylistWidget::onTabChanged(int index) {
    if (index < 0)
        return;
    updateBottomBarState(); // N49：切 tab 后刷新定位按钮启用态
    PlaylistListView *list = viewAt(index);
    if (!list)
        return;
    // N62 §2.2：停留身份守卫——判据从"tab 序号变了就重激活"校正为"停留的表变了才重
    // 激活"。同表重入（索引变了、页面没变，如改序移动当前表触发的 currentChanged）只
    // 保留上方按钮态刷新即早退：不重选记忆行、不滚动、不起第二次枚举
    const QUuid uuid = list->list()->uuid();
    if (uuid == m_currentListUuid)
        return;
    m_currentListUuid = uuid;
    // N19：未加载的 tab 走首次激活链（选中/滚动在枚举续作收尾）。
    // N28：所有 tab 创建 / 换源 / 会话恢复时层不在缓存，内容统一在此经异步枚举填充；
    // N30：默认 tab 内容由两条直写路径维护，层恒在缓存，故不在此发起枚举
    // （其刷新经 refreshCurrentTab 显式发起）。
    // N45：已加载 = 层在缓存且无失败文案（§2.5）。判定对象是本 tab 的当前显示层
    // （浏览路径层）。
    // N53 变更记录 V1：失败层（在缓存且带 loadError）切入不自动重试——落到下方
    // showLayer 按缓存渲染骨架与占位行（行未对齐才换入，对齐即原样保留），切入
    // 零请求，重试入口只剩手动刷新；层不在缓存（从未加载）才走首载
    if (!list->layerLoaded(list->browsePathIds()) &&
        list->levelErrorOf(list->browsePathIds()).isEmpty()) {
        startLoadAsync(list);
        return;
    }
    // N45 §2.5：切 tab 按该 tab 的浏览路径换入对应层——命中缓存同步换入
    // （显示行已对齐则无操作，不重建、不重新枚举）
    showLayer(list);
    // N19：已加载但元数据失效（他 tab 刷新清共享缓存/上次预读取消/回刷变更），
    // 且本 tab 排序依赖元数据 → 切入即重读后排序（含取消后切入重读语义）
    if (needsMetaRescan(index)) {
        reactivateRescan(list, index);
        return;
    }
    activateTab(index);
}

// N29：已加载 tab 切入重读判定——外围条件自判，类别/支持者/标志位三要素
// 经共用内核（与填充跳过、首载收尾同一判据，防多点位漂移）。
// N45：判定对象是本 tab 的当前显示层
bool PlaylistWidget::needsMetaRescan(int tabIndex) const {
    if (m_busy)
        return false;
    const PlaylistListView *list = viewAt(tabIndex);
    if (!list)
        return false;
    if (!list->layerLoaded(list->browsePathIds()))
        return false; // 未加载层走首载；占位层（loadError 非空）切入只渲染缓存，均不走预读
    const PlaylistProvider *p = providerOfView(viewAt(tabIndex));
    if (!p)
        return false;
    return pendingMetaCategory(list, list->browsePathIds(), p->sortRule()) != MetaCategory::None;
}

// N30/N32：切入已加载 tab 的重读——与刷新共用同一套"比对 → 三分支"，差别只在"不换入"。
// N32 起为薄壳：只组装意图上下文（旧值采集挪进推进函数，见 advanceTab），
// 预读在任务槽内异步跑，不再有"同步路径不传 generation"这条分支
void PlaylistWidget::reactivateRescan(PlaylistListView *list, int tabIndex) {
    if (!list)
        return;
    clearPlaceholder(list); // 与枚举收尾对称：占位行不参与比对，也不进实体序列
    JobContext ctx;
    ctx.intent = Intent::Reactivate;
    ctx.list = list;
    ctx.parent = list->browsePathIds(); // N45：目标层 = 当前显示层
    advanceTab(ctx, false);
}

// N19：原 onTabChanged 体。切入正在播放的列表：居中滚动到正播项。
// 选中态由 mpv 同步高亮驱动，此处只滚不改选。
// N62 §2.7 A 档：两处延后滚动改捕**列表身份**——投到下一拍前 tab 结构可能已变
// （同拍 createNewList / removeTabAt 即能命中），回调开头经 tabIndexOfList 现换算，
// 换算不到即放弃，不再按旧索引打在别表上
void PlaylistWidget::activateTab(int index) {
    PlaylistListView *list = viewAt(index);
    if (!list)
        return;
    const QUuid uuid = list->list()->uuid();
    if (index == playingTabIndex()) {
        QTimer::singleShot(0, this, [this, uuid]() {
            const int tab = tabIndexOfList(uuid);
            if (tab < 0)
                return;
            if (PlaylistListView *l = viewAt(tab)) {
                const int pr = playingRowOf(l);
                if (pr >= 0)
                    l->scrollToItem(l->item(pr), QListWidget::PositionAtCenter);
            }
        });
        return;
    }
    // 切换到非播放 tab 时自动选中该 tab 记忆的最后播放项（N9：路径+序号解析）。
    // N45 §2.8：记忆行只在"显示层 == 队列根层"时参与选中与滚动，其余情形由该层
    // 自己的 lastSelectedKey 负责恢复（showLayer 换入时已完成）
    const bool displayedIsQueueRoot = list->browsePathIds() == list->queueRootIds();
    const int row = displayedIsQueueRoot ? resolveResumeRow(index) : -1;
    if (row >= 0 && row < list->count()) {
        list->setCurrentRow(row);
        // 跟随滚动（N4）：currentChanged 到达时新 tab 尚未完成首次布局，
        // 立即滚动会打在零尺寸视口上无效，推迟到事件循环下一拍
        QTimer::singleShot(0, this, [this, uuid]() {
            const int tab = tabIndexOfList(uuid);
            if (tab < 0)
                return;
            if (PlaylistListView *l = viewAt(tab)) {
                const int r = resolveResumeRow(tab);
                if (r >= 0 && r < l->count() && l->currentRow() == r)
                    l->scrollToItem(l->currentItem(), QListWidget::PositionAtCenter);
            }
        });
    }
}

int PlaylistWidget::currentSelectedRow() const {
    const PlaylistListView *list = viewAt(currentIndex());
    if (!list || list->selectedItems().isEmpty())
        return -1;
    return list->currentRow();
}

// N45：当前选中行的实体序——停止态起播的选中行回退用（mpv 侧语义一律实体序）
int PlaylistWidget::currentSelectedEntityIndex() const {
    PlaylistListView *list = viewAt(currentIndex());
    if (!list || list->selectedItems().isEmpty())
        return -1;
    return rowToEntityIndex(list, list->currentRow());
}

// B10：同步目标转移。同 tab 早退（播放中双击本 tab 条目时防标记闪烁）；
// 异 tab 转移先清空原 tab 全部行的播放标记——setPlaylist 仅回刷新播放 tab，
// 旧 tab 标记若不在此清理将永久残留。
// N45 §2.1：同步目标转移即"要播的东西 = 该 tab 当前浏览层"——队列根在此重锚
// （早退前执行：播放中 tab 换层双击也要换队列根）
void PlaylistWidget::markPlayingTab(int tabIndex) {
    updateBottomBarState(); // N49：同步目标转移后刷新定位按钮启用态
    PlaylistListView *list = viewAt(tabIndex);
    if (list)
        list->setQueueRoot(list->browsePath()); // 队列根 = 该 tab 当前浏览层（N45 §2.1）
    // N57 §2.8：转移播放目标 = 换身份。表必在（不变式：tab ⇔ 视图 ⇔ 列表），越界兜回
    // 默认列表身份——恒可解析由 §2.5 的保留身份承载，不再有"索引悬空"这一说
    const QUuid targetUuid = list && list->list() ? list->list()->uuid() : kDefaultListUuid;
    // N59 §3.2：主动设定在此单点记账，且必须在同 tab 早退之前——正播 X 时表内双击、以及
    // 播放目标本已是 0 时的收编都走早退支，放之后就漏记。默认身份即复位
    m_lastPlayingTargetUuid = targetUuid;
    if (tabIndex == playingTabIndex())
        return;
    if (PlaylistListView *old = viewAt(playingTabIndex())) {
        for (int i = 0; i < old->count(); ++i)
            old->item(i)->setData(PlayingRole, false);
    }
    m_playingUuid = targetUuid;
}

void PlaylistWidget::onItemDoubleClicked(QListWidgetItem *item) {
    if (!item)
        return;
    // 行必出自本类出生的页面（createView 是全仓唯一列表页诞生点），故 cast 恒命中
    PlaylistListView *list = qobject_cast<PlaylistListView *>(item->listWidget());
    const int tab = indexOf(list);
    if (!list || tab < 0)
        return;
    // N45 §2.4.3：双击分派——上一级行导航返回、容器行进入该节点、实体行整层入队
    if (isPlaceholderRow(item))
        return; // N19：失败占位行不可播放
    if (isParentLinkRow(item)) {
        navigateUp(list);
        return;
    }
    if (isContainerRow(item)) {
        navigateDown(list, item);
        return;
    }
    markPlayingTab(tab);
    // N45 §2.8：双击入队是唯一"当前层"语义——入队单位即当前层（markPlayingTab
    // 已把队列根锚定到该层）；index 由行序换算为实体序。
    // N56 §2.7：播放链路传实体表，下发形态由 MainWindow 在喂 mpv 前一刻映射
    emit playRequested(currentLayerEntitiesOf(tab), tab, rowToEntityIndex(list, list->row(item)),
                       -1.0); // 回显由 MainWindow 统一入口登记（N25）
}

// N19：程序化切 tab 统一入口。忙时目标非当前则排队（last-wins），空闲后补执行。
// N62 §2.7 A 档：待办改持列表身份，入参索引在入口当场换算（同族其余待办均不携坐标）
void PlaylistWidget::requestTab(int index) {
    if (index < 0 || index >= count())
        return;
    if (!m_busy || index == currentIndex()) {
        setCurrentIndex(index);
        return;
    }
    const Playlist *list = listOf(index);
    m_pendingTabUuid = list ? list->uuid() : QUuid(); // last-wins；换算不出即无待办
}

void PlaylistWidget::flushPendingTab() {
    if (m_pendingTabUuid.isNull())
        return;
    const QUuid uuid = m_pendingTabUuid;
    m_pendingTabUuid = QUuid();
    const int idx = tabIndexOfList(uuid);
    if (idx < 0)
        return; // 目标已不在视图（被删 / 身份失效）：与旧版越界早退等价
    setCurrentIndex(idx);
}

// ——N54 层级定位（书签按列表身份回跳的机制侧：切列表 + 换层 + 定位，不播任何东西）——

// 整段替换某表的浏览路径。标题列不进层缓存键（N45 §2.5）、只服务"上一级"行的
// tooltip 相对路径，故按 loadPlaylists 的同一降级形态取 id 自兼（书签来源不另存
// 标题列）。替换前保存离开层的选中，与钻取 / 返回同一纪律
void PlaylistWidget::setBrowsePath(PlaylistListView *list, const QStringList &ids) {
    if (!list || list->browsePathIds() == ids)
        return;
    saveLayerSelection(list, list->browsePathIds());
    QList<PathSeg> path;
    path.reserve(ids.size());
    for (const QString &id : ids)
        path.append({id, id});
    list->setBrowsePath(path);
}

// N57 §2.8：原 eraseFailedLevel 转发函数退场——"删单层不连带子孙"的形态已在
// PlaylistListView 上，调用点直接 `list->eraseFailedLevel(ids)`（N54 §2.4 的显式
// 重试入口，与 navigateUp 同一实现）

// 目标层内定位：返回首个 matches(itemKey) 的**实体序**下标（即 mpv playlist-pos 语义，
// 非行序，N45 §2.4.2）。入参是书签存下的键（Util::Url::key 产出，N56 §2.5）——
// 键级判等经实体自己的 matches 出口，外部不假设键长什么样
int PlaylistWidget::entityIndexInLevel(const PlaylistListView *list, const QStringList &ids,
                                       const QString &itemKey) const {
    const Level *lvl = list->findLevel(ids);
    if (!lvl)
        return -1;
    for (int i = 0; i < int(lvl->entities.size()); ++i)
        if (lvl->entities[i].entity && lvl->entities[i].entity->matches(itemKey))
            return i;
    return -1;
}

void PlaylistWidget::requestLevelItemJump(const LevelJump &req) {
    if (!jumpTarget(req)) { // 当场失败：调用方只有一条失败出口，不设返回值分支
        emit levelItemFailed(req, JumpFailure::ListGone, QString());
        return;
    }
    m_pendingJump = PendingJump{req, false, true}; // last-wins：新的显式意图取代旧的
    if (m_busy)
        return; // 忙时不进行任何列表相关操作（连浏览路径都不写），等 busy 出口消费
    advanceLevelJump();
}

// 唯一推进口：每次重判目标层三态（已加载 / 失败 / 未加载），不假设上一轮消费已把
// 该层取回——一轮可能只是发起了枚举。至多两轮出结果，第三轮不可达（enumerated 闸门）
void PlaylistWidget::advanceLevelJump() {
    if (!m_pendingJump.active || m_busy)
        return;
    const LevelJump req = m_pendingJump.req; // 副本：出结果即作废，不残留待办意图
    // N57 §2.8：待办里只有身份，页面与坐标每次推进都现换算——延后期间目标 tab 被关闭
    // 就落 ListGone，不会按旧坐标漂移误命中别的表
    PlaylistListView *list = jumpTarget(req);
    if (!list) {
        m_pendingJump = PendingJump{};
        emit levelItemFailed(req, JumpFailure::ListGone, QString());
        return;
    }
    const Level *lvl = list->findLevel(req.levelIds);
    if (lvl && !lvl->loadError.isEmpty()) {
        if (m_pendingJump.enumerated) { // 已重试过一轮仍失败：出结果
            m_pendingJump = PendingJump{};
            emit levelItemFailed(req, JumpFailure::LevelLoadFailed, lvl->loadError);
            return;
        }
        list->eraseFailedLevel(req.levelIds); // 显式重试一次，落回下面的未加载分支
        lvl = nullptr;
    }
    if (lvl) {
        // 已加载：先同步换入（换入自身可能起一轮预读——切入已加载层时 needsMetaRescan
        // 判定通过），再判能否立刻出结果
        setBrowsePath(list, req.levelIds);
        requestTab(indexOf(list)); // 非忙即切，currentChanged 走 onTabChanged 的换入路径
        showLayer(list); // 目标 tab 本就是当前 tab 时无回调，此处补换入（缓存命中同步重建）
        m_pendingJump.enumerated = true; // 目标层已在手，绝不再起第二轮枚举
        if (m_busy)
            // 预读在途：实体序仍会被排序改动，此刻定位会把索引打在旧序上，且重推与
            // 预读收尾的四步式互相打断——留给下一次 busy 出口消费（至多一次，预读标志
            // 置位后 needsMetaRescan 不再复触发）
            return;
        m_pendingJump = PendingJump{};
        const int idx = entityIndexInLevel(list, req.levelIds, req.itemKey);
        if (idx >= 0)
            emit levelItemReady(req, indexOf(list), idx); // 坐标在发射时刻现取
        else
            emit levelItemFailed(req, JumpFailure::ItemNotFound, QString());
        return;
    }
    if (m_pendingJump.enumerated) {
        // 起过枚举而层仍不在缓存 = 该轮任务被作废 / 取消：静默作废，两个信号都不发
        // （取消即用户意图，不弹错也不回退，N54 §2.3）
        m_pendingJump = PendingJump{};
        return;
    }
    m_pendingJump.enumerated = true;
    setBrowsePath(list, req.levelIds); // 先落路径：离开层选中保存与守候的行重建都按当前浏览路径
    // N64 整树形态：缺层守候——空层处置不需要任何取源（空层必无命中，落
    // ItemNotFound）；须在落路径之后（rebuildRowsForLevel 的特殊行构建断言
    // ids == browsePathIds），根层缺失不在此列（整树未初载，走下方枚举）
    const bool wholeTreeMissing =
        !req.levelIds.isEmpty() && wholeTreeMissingLevel(list, req.levelIds);
    requestTab(indexOf(list));
    if (wholeTreeMissing) {
        if (!m_busy) { // 忙时不抢：pendingJump 在册，busy 出口重推进（层已入缓存必命中）
            m_pendingJump = PendingJump{};
            emit levelItemFailed(req, JumpFailure::ItemNotFound, QString());
        }
        return;
    }
    // 不经 showLayer 起枚举：第二次跳转到同一失败层时，行内容已与该层"对齐"（残留的
    // 骨架 + 占位行），showLayer 首行即按 displayedIds == ids 早退，于是整次跳转静默
    // 无事发生——既不重举也不回报。失败层条目刚被 eraseFailedLevel 删掉，此处必然
    // 不在缓存，直接按目标层发起枚举（startEnumAsync 自带清占位 / 置忙 / 遮罩）
    if (!m_busy)
        startEnumAsync(list, Intent::InitialLoad, req.levelIds);
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
            files.append(Util::Url::toNative(url.toLocalFile()));
    }
    if (!files.isEmpty()) {
        addFiles(files);
        event->acceptProposedAction();
    }
}

// ——N9 提供者——

PlaylistProvider *PlaylistWidget::providerOfView(const PlaylistListView *view) const {
    Playlist *list = listOfView(view);
    return list ? list->provider().get() : nullptr;
}

bool PlaylistWidget::isModifiableTab(int tabIndex) const {
    const auto *p = providerOfView(viewAt(tabIndex));
    return p != nullptr && p->supportsModify();
}

// N30：行级换入——不 clear、不销毁行对象（保住 PlayingRole 与选中 / 滚动连续）。
// ① 前导特殊行（上一级 + 容器行）按当前层重建——上一级行让开第 0 行（N45 §2.4.3），
//    容器组恒在实体组之前；② 前 min(旧行数, 新实体数) 行逐行换绑（先改行角色再替容器
//    元素，零悬空窗口）；③ 新实体更多则经 makeItem 追加；④ 更少则删尾部行并同步缩容器。
// 旧实体连同 fileMeta / mediaMeta / icon 缓存全部丢弃——换绑即换新，保留任何缓存都不叫刷新。
// 调用前提：目标层即显示层，Level.containers 已由调用方先行写入
void PlaylistWidget::applyEntitiesToRows(PlaylistListView *list, PlaylistEntity::List &&entities) {
    if (!list)
        return;
    // ① 摘除既有特殊行（恒居行序前缀——[上一级行][容器行…]）
    int head = 0;
    while (head < list->count() &&
           (isParentLinkRow(list->item(head)) || isContainerRow(list->item(head))))
        delete list->takeItem(head);
    // ② 实体行换绑（此时行全为实体行，容器下标 == 行号）
    const int oldRows = list->count();
    const int n = int(entities.size());
    auto &owned = list->entries();
    const int m = qMin(oldRows, n);
    if (int(owned.size()) < m)
        owned.resize(m); // 不变式兜底：容器不得短于待换绑行数，防下标越界
    for (int i = 0; i < m; ++i) {
        PlaylistEntity::Ptr fresh = std::move(entities[i]);
        setItemEntity(list->item(i), fresh);
        owned[i].entity = std::move(fresh); // 旧实体在此销毁：行已指向新实体（序号沿用原位旧值）
    }
    for (int i = m; i < n; ++i)
        makeItem(list, std::move(entities[i]));
    for (int i = oldRows - 1; i >= n; --i)
        delete list->takeItem(i); // 先删行（此时实体仍在），再缩容器
    owned.resize(n);              // 容器序恒等于实体行序（新增元素序号 0，收尾重编号归正）
    // ③ 前导特殊行重建（上一级行 + 容器行插回顶部）
    QList<PlaylistProvider::Container> containers; // N58 §2.2：调用点拆包一次
    containers.reserve(list->levelOf().containers.size());
    for (const auto &entry : list->levelOf().containers)
        containers.push_back(entry.container);
    buildSpecialRows(list, list->browsePathIds(), containers);
    list->setDisplayedIds(list->browsePathIds());
}

// ——N32 上下文采集（一律到"用到它的那一步之前一刻"才采）——

// 行序 key 序列（比对用；占位行跳过）
QStringList PlaylistWidget::keysOf(const PlaylistListView *list) const {
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
PlaylistWidget::ViewSnapshot PlaylistWidget::captureViewSnapshot(PlaylistListView *list) const {
    ViewSnapshot snap;
    if (!list)
        return snap;
    const bool hadRows = list->count() > 0;
    snap.memoryPath = hadRows ? captureMemoryPath(list) : QString{};
    snap.hadMemory = hadRows && list->memory().lastPos >= 0;
    return snap;
}

// N30：记忆修正 + 选中与滚动，幂等（行是否换过都可调用）。
// 播放表不在此滚动——滚动与选中由归位一次性完成，否则视觉跳两下
void PlaylistWidget::restoreViewState(PlaylistListView *list, int tab, const ViewSnapshot &snap) {
    if (!list)
        return;
    if (snap.selectFirst) {
        if (list->count() > 0)
            list->setCurrentRow(0); // 列表设置换源：选中首项，不播放
        return;                     // 换源已清除记忆，无记忆可复；不得被 activateTab 覆盖
    }
    restoreMemoryPath(list, snap.memoryPath, snap.hadMemory);
    if (tab >= 0 && tab == currentIndex() && tab != playingTabIndex())
        activateTab(tab); // 非播放表：按记忆行选中并滚动
}

// N30：播放态归位——标记与播放表选中的唯一出口。行级换入不再 clear()，标记没有天然
// 清除路径，故对每个未走四步式的分支在收尾末尾调用一次：在播且能定位即重建在正播行，
// 否则清除本表全部标记。走了四步式的分支在此早退——stop 只是入队，此刻读到的
// 正播路径与 idle-active 仍是发起前的值，按旧值判断会把"未重推"误判为在播
// （正播路径自 N61 §2.3 起一律经 MpvController::currentPlayingPath，不再裸读 path）
void PlaylistWidget::restorePlayingState(PlaylistListView *list, int tab) {
    if (!list || tab < 0 || tab != playingTabIndex())
        return;
    // N45 §2.9：播放标记随层状态归属队列根层——显示层非队列根层时行上本无标记
    // 可重建（下次渲染队列根层时由本函数按 mpv 现状点亮），选中与滚动亦不归它管
    if (list->browsePathIds() != list->queueRootIds())
        return;
    const bool idle = m_mpv ? m_mpv->propertyFlag(QStringLiteral("idle-active"), false) : true;
    const QString cur = m_mpv ? m_mpv->currentPlayingPath() : QString();
    int target = -1;
    if (!idle && !cur.isEmpty()) { // 正播路径为空即无正播 → 清除
        for (int i = 0; i < list->count(); ++i) {
            // N56 §2.5：键级判等经 matches（整形后的回显进判据，归一在实体内部完成）
            if (const PlaylistEntity *e = entityFromItem(list->item(i)); e && e->matches(cur)) {
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
void PlaylistWidget::clearPlayingMarks(PlaylistListView *list) {
    if (!list)
        return;
    for (int i = 0; i < list->count(); ++i)
        list->item(i)->setData(PlayingRole, false);
}

// N55 §3.5：单一延后告知出口。判断处只记账（文案入待发队列；队列已有完全相同的
// 文案则不重复入队——收编文案恒同串，天然合并），QTimer::singleShot(0) 把投递锚在
// 事件循环顶层：两处判断栈（setPlaylist / 清扫所在的设置对话框 accept() 栈）都不能
// 自己 exec()——清扫侧从 accept() 栈内调度，定时器只在设置对话框 exec() 退出、控制权
// 交还外层后才触发；收编侧同理离开 mpv 事件栈。模态期间事件照常流动是特性：弹窗
// 自身的事件循环照常分派 mpv 回显，setPlaylist 若再入那是全新栈帧的新调用
void PlaylistWidget::queueDeferredNotice(const QString &text) {
    if (m_pendingNotices.contains(text))
        return;
    m_pendingNotices.append(text);
    QTimer::singleShot(0, this, [this] { pumpDeferredNotices(); });
}

// 投递步：待发队列合成一次模态弹窗（仅确认、分段显示、不可取消）后清队列。
// singleShot 在弹窗自己的嵌套循环里照常触发，故"弹窗在幕"时必须只记账不叠窗——
// 一窗压一窗既读不出有几条，也把第二窗顶到不易察觉的位置；本扇关掉后再自排下一轮，
// 队列不丢（早退时队列未动，尾部那次重排是唯一的续命点）
void PlaylistWidget::pumpDeferredNotices() {
    if (m_noticeDialogUp || m_pendingNotices.isEmpty())
        return;
    const QStringList pending = std::move(m_pendingNotices);
    m_pendingNotices.clear();
    m_noticeDialogUp = true;
    QMessageBox box(QMessageBox::Warning, tr("Playlist Notice"), pending.join(QLatin1Char('\n')),
                    QMessageBox::Ok, this);
    box.exec();
    m_noticeDialogUp = false;
    if (!m_pendingNotices.isEmpty())
        QTimer::singleShot(0, this, [this] { pumpDeferredNotices(); });
}

// N55 §2.6：来源失效清扫——保证"弱引用能 lock 到的那个实例，永远是当前唯一有资格
// 代言这个身份的实例"。换源侧：旧提供者实例若仍被在途任务的 shared_ptr 吊着，归属
// U 的显示中实体还能 lock 到旧实例，新旧同身份向 StreamRelay 同槽位 publish 争用；
// 删列表侧：列表死后弱引用同样能临时 lock 成功。两触发点（removeTabAt / openListSettings）
// 各接一处，均与触发动作同栈同步调用；retiredName 须在摘除前采集（摘除后 nameOf 即
// 空返回）。清扫面封闭：归属 U 的显示中实体只可能在默认 tab 的收编层——其余列表的
// 实体归属即自身列表，随列表同生死；默认列表归属的提供者永生且 U 恒非保留身份。
// N76 §2.2：实际作用面收窄——收编直接形态项重造后归属默认列表（身份 = 自含凭据串，
// 自含可播，非"能看不能播"的僵尸项），不再被本清扫移除；本清扫只余转接收编项。
// 行面依赖"默认 tab 恒平铺"：默认列表的提供者是 StaticProvider、无容器 ⇒ 显示层 ==
// 队列根层，removeRowsLocal 作用的 levelOf() 即队列根层，清扫不必先归位显示层
// （若将来给默认列表配可钻取的提供者，须显式先 setDisplayedIds(queueRootIds())）
void PlaylistWidget::applySourceRetired(const QUuid &retiredUuid, const QString &retiredName,
                                        bool stopAlreadyDone) {
    PlaylistListView *def = viewAt(0);
    if (!def)
        return;
    QList<int> rows;
    for (int r = 0; r < def->count(); ++r) {
        // 归属不参与判等的另一面：按"归属身份 == U"过滤是归属唯一的读取点
        if (const PlaylistEntity *e = entityFromItem(def->item(r));
            e && e->m_ownerUuid == retiredUuid)
            rows.append(r);
    }
    if (rows.isEmpty())
        return; // 无命中项即无连带：不停播、不弹窗（列表自身的摘除/换身是用户自知动作）

    // 处置 2：停播判据 = 正播是 U（触发点的既有停止语义已停，stopAlreadyDone）或
    // 正播在默认 tab（此处补停——mpv 队列的内容就是 tab 0 的实体序，清掉任何一批行
    // 都令队列与界面失配，下一次回显必判差异再把刚清掉的项捞回来）。正播在第三个
    // 列表时不停播——待清的行全在 tab 0，与那个队列没有一条对位关系
    const bool playingDefault = playingTabIndex() == 0;
    if (playingDefault && !stopAlreadyDone) {
        emit stopRequested();   // stop 即 mpv 列表清空（AGENTS 陷阱条目）
        clearPlayingMarks(def); // 本地标记同步失效（与 runFourStep 同型）
        // 不写 m_playingUuid：playingTabIndex() == 0 的定义就是"该身份解析到 tab 0"，
        // 而 tab 0 恒为默认列表（N57 §2.8 的身份↔坐标不变式），此处本就恒为保留
        // 身份——N55 §2.6 处置 2 明写"不需再归位"
    }
    // 处置 3 + 4：行删除走既有本地路径，只删界面；不得跟着 emit removeRequested——
    // 判据命中时 mpv 侧已被 stop 清空，再喂逐行 playlist-remove 打的是空队列
    removeRowsLocal(def, rows);
    // 处置 1：延后弹窗告知，不可取消。文案只点名列表、不给计数（项数与逐项清单对
    // 用户没有可操作价值）；判据命中而停播则加一句"播放已停止"。去处写明"默认列表"
    // （变更记录 1，维护者裁决）：收编项的常住地是默认 tab，正播在第三个列表时，
    // 只说"来自列表 X 的条目已失效并被移除"读不出从哪里移除，报错会显得莫名其妙
    QString text = tr("Entries from playlist %1 are no longer valid and have been removed "
                      "from the default playlist.")
                       .arg(retiredName);
    if (stopAlreadyDone || playingDefault)
        text += QLatin1Char('\n') + tr("Playback has been stopped.");
    queueDeferredNotice(text);
}

void PlaylistWidget::runFourStep(PlaylistListView *list, int tab,
                                 const std::function<void()> &mutate) {
    const bool keepPaused =
        m_mpv ? m_mpv->propertyFlag(QStringLiteral("pause"), false) : false; // stop 之前读
    emit stopRequested();
    clearPlayingMarks(list); // stop 即 mpv 列表清空，本地标记同步失效
    if (mutate)
        mutate();
    // N45 §2.8：重推的是 mpv 队列本身——entities 取队列根层出口，idx 为队列根层
    // 实体序（resumeEntityIndexOf 内部完成序号校验与路径寻位）。
    // N56 §2.7：播放链路传实体表
    const int idx = resumeEntityIndexOf(tab);
    if (idx >= 0)
        emit playRequested(queueEntitiesOf(tab), tab, idx, lastTimePosOf(tab), keepPaused);
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

// N19/N26/N28/N29/N32：所有 tab 首次激活 / 刷新 / 换源统一走异步枚举。
// 任务体经 shared_ptr 调用 provider->enumerate(parent)（实体构造与提供者侧元数据
// 注入都在工作线程）——换源/删 tab 后旧提供者的销毁被自动延后到任务结束，
// 无 use-after-free。不变式：在途期间 GUI 线程不修改该提供者的可变状态——
// 目录源成员构造后只读，增删/换源/删 tab 均被 m_busy 拦截；静态源的
// setItems/setSortRule 经 savePlaylists 的回传守卫（未加载表不回传）被跳过，
// 不可能与任务线程的 enumerate() 并发。跨线程只带 shared_ptr 与实体快照：
// QPointer 禁止在任务线程读写，有效性由任务槽判定。
// N45 §2.10：枚举目标 = (list, parent) 二维——任务体与续作一律按 parent 读写层缓存
void PlaylistWidget::startLoadAsync(PlaylistListView *list) {
    if (!list)
        return;
    // 意图在发起处定死并按值随任务走（异步边界不靠成员承载）。m_pendingLoad 只作
    // 发起期意图来源：一次性消费 + 清空，失配残留一并清掉，杜绝污染后续首载
    const PendingLoad pend = (m_pendingLoad.list == list) ? m_pendingLoad : PendingLoad{};
    m_pendingLoad = PendingLoad{};
    const Intent intent = pend.refresh
                              ? Intent::Refresh
                              : (pend.selectFirst ? Intent::SwitchSource : Intent::InitialLoad);
    // N64：整树形态恒对根层发起（契约义务②：初载与刷新都落根层，保证是视图层的
    // 义务）——浏览路径可能停在持久化的深层队列根（会话恢复）或旧深层，在此钉根
    // 即覆盖首载 / 换源 / 刷新全部常规通路；收尾按形态整树重建并归位根层
    const auto *p = providerOfView(list);
    startEnumAsync(list, intent,
                   p && p->enumeratesWholeTree() ? QStringList{} : list->browsePathIds());
}

void PlaylistWidget::startEnumAsync(PlaylistListView *list, Intent intent,
                                    const QStringList &parent) {
    if (!list)
        return;
    Playlist *owning = listOfView(list);
    if (!owning)
        return; // 防御：视图未配列表（提供者随之缺失）不加载
    // 任务持有 shared_ptr 副本：换源 / 删 tab 后在途提供者的销毁自动延后到线程结束（N28）
    auto provider = owning->provider();

    if (parent == list->browsePathIds())
        clearPlaceholder(list); // 失败重试先清占位（仅显示层——非显示层重建不动显示行）
    JobContext ctx;
    ctx.intent = intent;
    ctx.list = list;
    ctx.parent = parent;
    // N64：整树形态随批次声明（现查提供者，不存模式位）——收尾据此放宽目标层判定
    // 并做整树权威重建；逐层批次恒为 false，行为一字不变
    ctx.wholeTree = provider && provider->enumeratesWholeTree();

    auto result = std::make_shared<std::expected<PlaylistProvider::Enumeration, QString>>();
    runJob(
        ctx, LoadingOverlay::Mode::Loading, 0, /*cancelable=*/false,
        [provider, result, parent](QPromise<void> &) {
            *result = provider->enumerate(parent); // 工作线程：实体构造 + 元数据注入仅此一步
        },
        [this, ctx, result](JobVerdict v) { onLoadFinished(ctx, *result, v); });
}

// N30：刷新收尾的播放同步——排序后逐行比对旧 key 序列，三分支选一。
// 前缀比较已蕴含"只增不删、相对顺序未变"，mpv 的增量通道只能追加，只有这一形状可静默。
// 返回是否走了四步式（走了则由回刷重建标记，归位不介入）。
// N45 §2.9：显示层非队列根层时，刷新纯粹是该层视图重建——比对与播放同步无意义
// （mpv 队列的内容基准是队列根层；该情形已由 N53 §2.2 在刷新发起时整枝清除
// 前置处置，本函数不再轮得到执行）
bool PlaylistWidget::syncAfterRefresh(PlaylistListView *list, int tab, const QStringList &oldKeys) {
    if (list->browsePathIds() != list->queueRootIds())
        return false;
    const int n = oldKeys.size();
    bool prefix = list->count() >= n; // 行数更少必然不是前缀一致
    for (int i = 0; prefix && i < n; ++i) {
        const PlaylistEntity *e = entityFromItem(list->item(i));
        prefix = e && e->key() == oldKeys.at(i) && !isPlaceholderRow(list->item(i));
    }
    // 正播时长回填（三分支通用）：静默与追加不重建播放列表，正播行时长不会再有 mpv 推送补回。
    // 不前置判断——duration <= 0 由 applyDuration 记 nullopt，时长列回落到实体缓存元数据。
    // N56 §2.5：定位正播行经 matches（整形后的回显进判据）；落仓键取实体 key
    if (m_mpv && tab >= 0 && tab == playingTabIndex()) {
        const QString cur = m_mpv->currentPlayingPath();
        if (!cur.isEmpty()) {
            for (int i = 0; i < list->count(); ++i) {
                if (const PlaylistEntity *e = entityFromItem(list->item(i)); e && e->matches(cur)) {
                    applyDuration(e->key(),
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
        tab == playingTabIndex() && !oldKeys.isEmpty() &&
        !(m_mpv ? m_mpv->propertyFlag(QStringLiteral("idle-active"), false) : true);
    if (!canSync)
        return false;
    if (prefix) { // 末尾追加：只把新项交给喂入侧，播放零中断
        // N45 §2.8：比对与追加的基准是 mpv 队列 → 队列根层出口。
        // N56 §2.7：追加段以实体表随 filesAdded 信号交 MainWindow
        const PlaylistEntity::List entities = queueEntitiesOf(tab);
        PlaylistEntity::List added;
        for (int i = n; i < int(entities.size()); ++i)
            added.push_back(entities.at(i));
        if (!added.empty())
            // N60 §2.4：不再在这里登记在途回显——会话由喂入侧（MainWindow::onFilesAdded）
            // 的守卫开闭；守卫不通过而不喂的情形见规格 §2.4 的等价性论证
            emit filesAdded(std::move(added));
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
    PlaylistListView *list = ctx.list;
    if (!list || v == JobVerdict::Discarded)
        return;
    // N45 §2.4.1/§2.10：失败与换入的处置按层分派——显示层（行上可见）与队列根层
    // （播放基准）各有专属语义
    const bool targetsDisplayed = (ctx.parent == list->browsePathIds());
    const bool targetsQueueRoot = (ctx.parent == list->queueRootIds());
    // N64 整树批次：目标层恒为根层，浏览路径可能停在深层（整树刷新发起、会话恢复的
    // 持久化队列根）——按"目标层 = 收尾将落地的层"放宽 targetsDisplayed：收尾先
    // 归位根层，随后的换入 / 重建行 / 失败占位落点即根层（成功与失败同口径，已裁决）
    const bool wholeTreeLanding = ctx.wholeTree && !targetsDisplayed;
    if (wholeTreeLanding)
        setBrowsePath(list, {});
    if (targetsDisplayed)
        clearPlaceholder(list);
    if (!r.has_value()) {
        // 失败层内容可信度归零：先毁行、再清内容、后重建骨架（N53 §2.1）。清空不分
        // 显示层与否——枚举没成功，保留只会在下次渲染时复现陈旧行。毁行前置于清仓：
        // 行上 EntityRole 是实体裸指针，先清仓会让现有行悬空。
        // N64：整树批次按放宽后的落点（根层）收口
        const bool landsDisplayed = targetsDisplayed || wholeTreeLanding;
        Level &lvl = list->ensureLevel(ctx.parent);
        lvl.loadError = r.error();
        if (landsDisplayed)
            list->clear();
        lvl.containers.clear();
        lvl.entities.clear();
        if (landsDisplayed) {
            // "上一级"行是渲染骨架的一部分，不依赖内容是否加载成功：首钻失败
            // （显示行仍是旧层内容、浏览路径已指向失败层）也按失败层重建行骨架，
            // 否则持续失败即回不到上一级，且旧层残留行的双击会在失败层路径上
            // 继续推段。清空后重建只产出骨架，占位行追加于其后
            rebuildRowsForLevel(list, lvl, ctx.parent);
            showPlaceholder(list, r.error());
        }
        // N25/N45：枚举失败即停止的处置只适用于队列根层失败且正在播放（N53 §2.3——
        // 该层是 mpv 队列的内容基准，后续切曲大概率同失败；停在队列根层的子层失败时
        // 队列内容仍有效，不停播）。stop 不带 keep-playlist，一并清空 mpv 播放列表
        // （随后的空列表推送由回刷"空列表忽略"吸收）。行已重建、天然无播放标记
        const int failTab = indexOf(list);
        if (targetsQueueRoot && failTab >= 0 && failTab == playingTabIndex()) {
            emit stopRequested();
            m_playingUuid = kDefaultListUuid; // 同步目标归位默认列表（身份恒定，恒可解析）
        }
        emit contentsChanged(); // 占位行计入行数，导航启停需刷新
        return;                 // 忙时与遮罩由唯一出口第 ⑤ 步复位（本续作未起新任务）
    }
    if (v == JobVerdict::Cancelled) { // 枚举批次不可取消，此为 abandonJob 之外的防御路径
        finishCanceled(ctx);
        return;
    }
    // N45 §2.5：层入缓存即"已加载"——覆盖旧层（刷新的收尾就在此完成）。
    // 失败文案清空；元数据标志不动（刷新路径已由 evictListKeys 全层复位，
    // 首载 / 换源的新层缺省即 false，与旧 entriesLoaded 语义一致）
    Level &lvl = list->ensureLevel(ctx.parent);
    lvl.loadError.clear();

    // N64：整树落地批次（浏览路径深于根层）的播放同步基线按根层旧内容取（V6）——
    // oldKeys 常规按行采集，但落地根层的批次此刻行还是深层旧行，拿去比根层新行会
    // 误判（无谓四步式重推 / 前缀巧合误入 append 分支重推已有项）；根层旧实体仓
    // 此刻尚未被下方覆盖，按层采集即与逐层根层刷新同基线。②'' 已停播的批次不受
    // 此影响（canSync 恒假），仅 Refresh 需要基线（其余意图无旧内容可比）
    QStringList rootOldKeys;
    if (wholeTreeLanding && ctx.intent == Intent::Refresh) {
        rootOldKeys.reserve(lvl.entities.size());
        for (const auto &e : lvl.entities)
            if (e.entity)
                rootOldKeys.push_back(e.entity->key());
    }

    PlaylistProvider::Enumeration enumeration = std::move(r.value());
    // N64 义务 1 的 Debug 守候：逐层批次不得带 descendants。Release 侧由"仅整树
    // 路径读 descendants"自然丢弃（随局部变量析构），此处只补显影，行为零变化
    Q_ASSERT(ctx.wholeTree || enumeration.descendants.isEmpty());
    if (!enumeration.warnings.isEmpty())
        m_enumWarnings.append(enumeration.warnings); // N37：部分失败警告随成功批次入库
    // N58 §2.2：枚举产出的裸容器表换入即包成层元素（序号待收尾重编号）
    lvl.containers.clear();
    lvl.containers.reserve(enumeration.containers.size());
    for (auto &c : enumeration.containers)
        lvl.containers.push_back({std::move(c), 0});

    // N45 §2.10：归位已并入忙时延后家族（revealPlayingItem），在途期浏览路径不被
    // 搬移——批次恒以目标层即显示层完成，此处必然换入。
    // N64：整树批次放宽为"目标层 = 收尾将落地的层"（浏览路径已在上方归根）
    Q_ASSERT(targetsDisplayed || wholeTreeLanding);

    // N55 §2.2：实体入列即盖章（归属 = view 所在列表的身份与当前提供者）。成功批次
    // 的两支换入（跨层写 lvl.entities / 同层走 applyEntitiesToRows）共用同一次盖章
    // ——每个实体恰落其中一支；显式赋盖（异步任务只产未盖章实体表，归属由 GUI
    // 线程在入列时盖）
    // 逐元素不判空：三提供者的 enumerate 与两个工厂恒不产 null 实体（工厂"恒返回
    // 实体"是契约，落不了已知类型也有 UnknownEntity 占位）。setPlaylist 第 4 / 5 步
    // 那两处判空不是同一回事——它们是判据侧的保守写法（判不出即不一致 → 收编、
    // 不索引），不是"表里会有 null"的存在断言，两者不构成本循环该判空的理由
    for (auto &e : enumeration.entities)
        stampOwnership(*e, list->list());

    // 换入前一刻才采集（§4.3）：在途期间 mpv 事件仍经 recordPlayback 推进本表记忆，
    // 发起时快照会丢掉这段进度。oldKeys 仅刷新批次需要（供比对；其余意图无旧内容可比）
    JobContext out = ctx;
    if (ctx.intent == Intent::Refresh)
        out.oldKeys = wholeTreeLanding ? std::move(rootOldKeys) : keysOf(list);
    out.snap = captureViewSnapshot(list);
    out.snap.selectFirst = (ctx.intent == Intent::SwitchSource);
    // N30 行级换入（applyEntitiesToRows）只适用于同层换入：它保住的是本层行的选中
    // 与滚动连续。跨层换入（首钻成功，行对象原属别层）连续性不存在——紧随的
    // restoreLayerSelection 即按本层记忆改选，行级换入的唯一收益当场作废，只剩
    // 副作用：行上挂的 PlayingRole 随换绑漏到本层某一行（父层正播行的标记平移到
    // 本层同实体序位置），而本层非队列根层，restorePlayingState 按 §2.9 守卫不清它
    // ——表现为子层级把某个不相干文件标成正在播放
    // N45：跨层换入按层 clear 重建；同层换入不动（行级换绑已保住选中，恢复反而打扰）
    const bool rowsWereElsewhere = (list->displayedIds() != ctx.parent);
    if (rowsWereElsewhere) {
        list->clear(); // 毁行在前：行上 EntityRole 是实体裸指针，先动实体仓即悬空
        // N58 §2.2：枚举产出的裸实体表换入即包成层元素（序号待收尾重编号）
        lvl.entities.clear();
        lvl.entities.reserve(enumeration.entities.size());
        for (auto &e : enumeration.entities)
            lvl.entities.push_back({std::move(e), 0});
        rebuildRowsForLevel(list, lvl, ctx.parent); // 内部 clear 幂等，并落显示层记录
        restoreLayerSelection(list, lvl);
    } else {
        applyEntitiesToRows(list, std::move(enumeration.entities));
    }
    // N58 §2.3 表第 1 行：两支换入（跨层 / 同层）共用的收尾重编号——必须早于
    // rebuildWholeTree 内的当场排序与 advanceTab 对显示层的排序，否则刚换入的
    // 枚举序被改写成排序序
    PlaylistListView::renumberEnumerationOrder(lvl);
    // N64：整树形态——换入后整树权威重建（缺席即删、交付即真，簿记随新层重置；
    // 只发生在"刚刚完成一次整树枚举"的收尾里，不新增触发点）
    if (ctx.wholeTree)
        rebuildWholeTree(list, lvl, enumeration);
    advanceTab(out, false);
}

// N32 推进函数（业务状态机，唯一入口）：判"还要不要预读"——要就起预读任务并让自己
// 作为其续作（预读成功后置"本表该类已读"标志，再次进入必然落到收尾步，天然终止）；
// 不要就做收尾。fromMetaScan 为真表示本轮预读已跑过，绝不再入预读分支——取消/失配时
// "已读"标志未置位，若再判一次会立刻重起一轮，用户刚取消又被弹回遮罩。
// 收尾顺序照搬现状：排序 → 视图恢复 → 比对与播放同步（含落盘）→ 播放态归位 → contentsChanged
void PlaylistWidget::advanceTab(JobContext ctx, bool fromMetaScan) {
    PlaylistListView *list = ctx.list;
    if (!list)
        return;
    const int tab = indexOf(list);
    if (tab < 0)
        return;
    const auto *p = providerOfView(viewAt(tab));
    // 判类别不得读提供者的排序键：Sort 意图的规则写点在收尾步，取消即不写，
    // 而"先写规则才能判类别"会让取消也落盘
    const bool isSort = (ctx.intent == Intent::Sort);
    const SortRule rule = isSort ? ctx.rule : (p ? p->sortRule() : SortRule{});

    if (!fromMetaScan) {
        const MetaCategory cat = pendingMetaCategory(list, ctx.parent, rule);
        if (cat != MetaCategory::None) {
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
        applySort(list, rule, /*writeRule=*/true);
        emit contentsChanged();
        return;
    }

    if (rule.key != SortKey::None)
        sortListItems(list, rule);
    restoreViewState(list, tab, ctx.snap);

    bool fourStep = false;
    if (ctx.intent == Intent::Refresh) {
        if (tab > 0 && tab != playingTabIndex())
            savePlaylists(); // 非播放非默认 tab 落盘（播放中 tab 不落盘为既有语义）
        else if (tab == playingTabIndex())
            fourStep = syncAfterRefresh(list, tab, ctx.oldKeys);
    } else if (ctx.intent == Intent::Reactivate) {
        if (tab == playingTabIndex())
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
    PlaylistListView *list = ctx.list;
    if (!list || cat == MetaCategory::None)
        return;
    // MediaInfo 进线程池的前提①：显式预载一次（shim 的模块加载惰性且无锁）。
    // 此调用必在 GUI 线程、必在起任务之前——此后任务线程对 shim 只剩只读
    if (cat == MetaCategory::Media)
        MediaInfoHelper::available();

    auto pending = std::make_shared<PlaylistEntity::List>(); // 待求值者（共享计数快照）
    int baseline = 0; // 已求值 / 不支持者：无需计算，但计入进度分母（现状口径）
    // N45 §2.10：预读快照限于任务目标层（ctx.parent）的实体，不再整表采集
    if (const Level *lvl = list->findLevel(ctx.parent)) {
        for (const auto &entry : lvl->entities) {
            const PlaylistEntity::Ptr &entity = entry.entity;
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
    PlaylistListView *list = ctx.list;
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
    setMetaLoadedOf(list, ctx.parent, r->cat, true); // N45：标志写在任务目标层
    advanceTab(std::move(ctx), /*fromMetaScan=*/true);
}

// N32：取消收尾——按意图分支，不可一刀切。枚举 / 刷新 / 重读侧有 ViewSnapshot
// （行已换入或记忆待修正）→ 做视图恢复再归位；Sort 侧无快照 → 一律不动：
// 给 Sort 补一次视图恢复会经 restoreViewState 的 tab == currentIndex() &&
// tab != playingTabIndex() 分支调 activateTab，产生重新选中与滚动的可见副作用
void PlaylistWidget::finishCanceled(const JobContext &ctx) {
    PlaylistListView *list = ctx.list;
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

// ——N58 §2.4 两个共用比较器（原四处内联比较器副本的收敛）——
// 形参收层元素包装体：序号与 null 实体都是"层内元素"这一级的事实，收进包装体才能
// 让新增键只加一处、判空只有一份。三条由既有代码带来的语义逐字保留：
// 1. "未知值恒归末尾"不参与方向翻转——故 ascending 必须进函数内部，不由调用方外层
//    flip 包装（外层包装会把"未知归末尾"一起翻掉）；
// 2. 两个数字化键（FileNameNumeric / N71 的 DisplayName）的分段序列只在需要时由
//    调用方提供（sortListItems 预切分一次，sortLevelData 现算；其余键传空占位）；
// 3. null 实体按等键保序（applyEntitiesToRows 的 resize 兜底会造出 null 槽位）。
// 新键 EnumerationOrder 比包装体序号：两侧恒有值、不碰元数据、不属"未知归末尾"档，
// ascending 照常生效（降序即枚举序反转，与其余键同形）

// 元数据取用档：Evaluated 走惰性 getter（sortListItems——显示层已预读，求值即命中
// 缓存）；Cached 只读缓存（sortLevelData——作用于任意缓存层，绝不触发同步求值）
enum class MetaAccess { Evaluated, Cached };

static const Util::SegmentedName s_emptySeg; // 其余键的分段序列空占位

// N71：走数字化对比的两个键——判据与比较串取法收在这一对符号里，四个调用点
// （两个执行面各备序列一处）共用，不各写一遍而漂移。DisplayName 只有一档
// （不平行提供纯字典序档），它与 FileNameNumeric 的差别只在于读 title() 还是
// fileName()。容器侧两键同读 Container::title（容器无显示名 / 文件名之分），
// 故取名不需经此对——直接用 container.title
static bool isNaturalNameKey(SortKey key) {
    return key == SortKey::FileNameNumeric || key == SortKey::DisplayName;
}

static QString naturalNameOf(SortKey key, const PlaylistEntity *e) {
    if (!e)
        return QString();
    return key == SortKey::DisplayName ? e->title() : e->fileName();
}

static bool containerLess(SortKey key, const PlaylistListView::Level::ContainerEntry &a,
                          const PlaylistListView::Level::ContainerEntry &b, bool ascending,
                          const Util::SegmentedName &segA, const Util::SegmentedName &segB) {
    const auto flip = [ascending](int c) { return ascending ? c < 0 : c > 0; };
    const PlaylistProvider::Container &ca = a.container;
    const PlaylistProvider::Container &cb = b.container;
    switch (key) {
    case SortKey::EnumerationOrder:
        return flip(a.enumerationIndex < b.enumerationIndex
                        ? -1
                        : (b.enumerationIndex < a.enumerationIndex ? 1 : 0));
    case SortKey::FileName:
        return flip(QString::compare(ca.title, cb.title, Qt::CaseInsensitive));
    case SortKey::FileNameNumeric:
    case SortKey::DisplayName: // N71：与 FileNameNumeric 同读 Container::title，仅数字化与否不同
        return flip(Util::naturalCompare(segA, segB));
    case SortKey::Extension: {
        const int da = ca.title.lastIndexOf(QLatin1Char('.'));
        const int db = cb.title.lastIndexOf(QLatin1Char('.'));
        const QString ea = da < 0 ? QString() : ca.title.mid(da + 1);
        const QString eb = db < 0 ? QString() : cb.title.mid(db + 1);
        return flip(QString::compare(ea, eb, Qt::CaseInsensitive));
    }
    case SortKey::Created:
    case SortKey::Modified: {
        const auto &va = ca.meta ? (key == SortKey::Created ? ca.meta->created : ca.meta->modified)
                                 : std::optional<QDateTime>{};
        const auto &vb = cb.meta ? (key == SortKey::Created ? cb.meta->created : cb.meta->modified)
                                 : std::optional<QDateTime>{};
        if (!va || !vb)
            return va.has_value() && !vb.has_value(); // 未知恒归末尾，不随方向翻转
        return flip(*va < *vb ? -1 : (*vb < *va ? 1 : 0));
    }
    case SortKey::FileSize: {
        const auto sa = ca.meta ? ca.meta->size : std::optional<qint64>{};
        const auto sb = cb.meta ? cb.meta->size : std::optional<qint64>{};
        if (!sa || !sb)
            return sa.has_value() && !sb.has_value();
        return flip(*sa < *sb ? -1 : (*sb < *sa ? 1 : 0));
    }
    case SortKey::None:
    case SortKey::Duration:
    case SortKey::ReleaseDate:
        return false; // 对容器恒未知 / 无规则：等键保序
    }
    return false;
}

static bool entityLess(SortKey key, const PlaylistListView::Level::EntityEntry &a,
                       const PlaylistListView::Level::EntityEntry &b, MetaAccess access,
                       bool ascending, const Util::SegmentedName &segA,
                       const Util::SegmentedName &segB) {
    const PlaylistEntity *pa = a.entity.get();
    const PlaylistEntity *pb = b.entity.get();
    if (!pa || !pb)
        return false; // 防御等键保序（既有判空原样进函数，不得省）
    const auto flip = [ascending](int c) { return ascending ? c < 0 : c > 0; };
    auto fileMetaOf = [access](const PlaylistEntity *e) -> const FileMeta * {
        return access == MetaAccess::Evaluated ? &e->fileMeta() : e->cachedFileMeta();
    };
    auto mediaMetaOf = [access](const PlaylistEntity *e) -> const PlaylistEntity::MediaMeta * {
        return access == MetaAccess::Evaluated ? &e->mediaMeta() : e->cachedMediaMeta();
    };
    switch (key) {
    case SortKey::None:
        return false;
    case SortKey::EnumerationOrder:
        return flip(a.enumerationIndex < b.enumerationIndex
                        ? -1
                        : (b.enumerationIndex < a.enumerationIndex ? 1 : 0));
    case SortKey::FileName:
        return flip(QString::compare(pa->fileName(), pb->fileName(), Qt::CaseInsensitive));
    case SortKey::FileNameNumeric:
    case SortKey::DisplayName: // N71：分段序列由调用方按本键取好（naturalNameOf），此处只比
        return flip(Util::naturalCompare(segA, segB));
    case SortKey::Extension:
        return flip(QString::compare(pa->suffix(), pb->suffix(), Qt::CaseInsensitive));
    case SortKey::Created:
    case SortKey::Modified: {
        const FileMeta *fa = fileMetaOf(pa);
        const FileMeta *fb = fileMetaOf(pb);
        const std::optional<QDateTime> va =
            fa ? (key == SortKey::Created ? fa->created : fa->modified)
               : std::optional<QDateTime>{};
        const std::optional<QDateTime> vb =
            fb ? (key == SortKey::Created ? fb->created : fb->modified)
               : std::optional<QDateTime>{};
        if (!va || !vb)
            return va.has_value() && !vb.has_value();
        return flip(*va < *vb ? -1 : (*vb < *va ? 1 : 0));
    }
    case SortKey::FileSize: {
        const FileMeta *fa = fileMetaOf(pa);
        const FileMeta *fb = fileMetaOf(pb);
        const std::optional<qint64> sa = fa ? fa->size : std::optional<qint64>{};
        const std::optional<qint64> sb = fb ? fb->size : std::optional<qint64>{};
        if (!sa || !sb)
            return sa.has_value() && !sb.has_value();
        return flip(*sa < *sb ? -1 : (*sb < *sa ? 1 : 0));
    }
    case SortKey::Duration:
    case SortKey::ReleaseDate: {
        const PlaylistEntity::MediaMeta *ma = mediaMetaOf(pa);
        const PlaylistEntity::MediaMeta *mb = mediaMetaOf(pb);
        const bool ha = ma && (key == SortKey::Duration ? ma->duration.has_value()
                                                        : ma->recordedDate.isValid());
        const bool hb = mb && (key == SortKey::Duration ? mb->duration.has_value()
                                                        : mb->recordedDate.isValid());
        if (!ha || !hb)
            return ha && !hb;
        if (key == SortKey::Duration)
            return flip(*ma->duration < *mb->duration ? -1
                                                      : (*mb->duration < *ma->duration ? 1 : 0));
        return flip(ma->recordedDate < mb->recordedDate
                        ? -1
                        : (mb->recordedDate < ma->recordedDate ? 1 : 0));
    }
    }
    return false;
}

// 条目重排核心：行指针摘除 → 稳定排序 → 回填；实体容器按同一 order 重排。
// 无停止/恢复副作用，四步式流程（sortCurrent）与加载填充共用。
// N45 §2.6：行序 = [上一级行][容器行…][实体行…]——上一级行恒第 0 行不动；
// 排序施加顺序 = 容器组内排 → 实体组内排（两组分界即模型不变式）；
// 取值面对容器扩为 title 顶 fileName/suffix（N71：DisplayName 档对容器亦读
// title，容器无显示名 / 文件名之分）、元数据取 FileMeta；"未知值恒归末尾组"
// 只在组内成立（容器组内部全为未知值时退化为保持提供者初始序，稳定排序保序）
void PlaylistWidget::sortListItems(PlaylistListView *list, SortRule rule) {
    const SortKey key = rule.key; // N57 §2.9：入参值对象化，函数体不动
    const bool ascending = rule.ascending;
    Level *lvl = list ? list->findLevel(list->browsePathIds()) : nullptr; // 只取不插
    if (!list || key == SortKey::None || !lvl || list->count() < 2)
        return;

    // 行分区：容器行 / 实体行 / 占位行；上一级行不收集（重装时按序原样回置）。
    // 排序场景无占位行（调用点均有已加载守卫），此处显式分流只为与 Q_ASSERT
    // 的计数口径自洽（N45 §2.4.3：占位行不参与排序）
    QList<QListWidgetItem *> containerRows, entityRows, placeholderRows;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        if (isContainerRow(item))
            containerRows << item;
        else if (isParentLinkRow(item))
            continue;
        else if (isPlaceholderRow(item))
            placeholderRows << item;
        else
            entityRows << item;
    }
    Q_ASSERT(containerRows.size() == int(lvl->containers.size()));
    Q_ASSERT(entityRows.size() == int(lvl->entities.size()));

    // ——容器组：比较逻辑在共用比较器内（含翻转与未知归末尾），此处只备分段序列——
    // N71：数字化键两档（FileNameNumeric / DisplayName）共用此备序列，容器侧两键同串
    QVector<Util::SegmentedName> containerSegs;
    if (isNaturalNameKey(key)) {
        containerSegs.reserve(containerRows.size());
        for (const auto &entry : lvl->containers)
            containerSegs.push_back(Util::segmentName(entry.container.title));
    }
    auto containerSegAt = [&containerSegs](int i) -> const Util::SegmentedName & {
        return containerSegs.isEmpty() ? s_emptySeg : containerSegs.at(i);
    };
    std::vector<int> cOrd(containerRows.size());
    for (int i = 0; i < int(cOrd.size()); ++i)
        cOrd[i] = i;
    std::stable_sort(cOrd.begin(), cOrd.end(), [&](int a, int b) {
        return containerLess(key, lvl->containers[a], lvl->containers[b], ascending,
                             containerSegAt(a), containerSegAt(b));
    });

    // ——实体组：元数据取用档 = Evaluated（显示层已预读，求值即命中缓存）——
    const int ne = int(lvl->entities.size());
    QVector<Util::SegmentedName> entitySegs;
    if (isNaturalNameKey(key)) {
        entitySegs.resize(ne);
        for (int i = 0; i < ne; ++i)
            entitySegs[i] = Util::segmentName(naturalNameOf(key, lvl->entities[i].entity.get()));
    }
    auto entitySegAt = [&entitySegs](int i) -> const Util::SegmentedName & {
        return entitySegs.isEmpty() ? s_emptySeg : entitySegs.at(i);
    };
    std::vector<int> eOrd(ne);
    for (int i = 0; i < ne; ++i)
        eOrd[i] = i;
    std::stable_sort(eOrd.begin(), eOrd.end(), [&](int ia, int ib) {
        return entityLess(key, lvl->entities[ia], lvl->entities[ib], MetaAccess::Evaluated,
                          ascending, entitySegAt(ia), entitySegAt(ib));
    });

    // ——Level 数组按新序重排（移动语义）——
    {
        QList<Level::ContainerEntry> sortedContainers;
        sortedContainers.reserve(cOrd.size());
        for (int idx : cOrd)
            sortedContainers.push_back(std::move(lvl->containers[idx]));
        lvl->containers = std::move(sortedContainers);
        QList<Level::EntityEntry> sortedEntities;
        sortedEntities.reserve(ne);
        for (int idx : eOrd)
            sortedEntities.push_back(std::move(lvl->entities[idx]));
        lvl->entities = std::move(sortedEntities);
    }

    // ——行重装：[上一级行][容器行·新序][实体行·新序]——
    // 行对象复用（takeItem 转移所有权后按新序 addItem）
    const int total = list->count();
    QList<QListWidgetItem *> taken;
    taken.reserve(total);
    for (int i = 0; i < total; ++i)
        taken.append(list->takeItem(0));
    for (QListWidgetItem *item : taken)
        if (isParentLinkRow(item))
            list->addItem(item);
    for (int idx : cOrd)
        list->addItem(containerRows.at(idx));
    for (int idx : eOrd)
        list->addItem(entityRows.at(idx));
    for (QListWidgetItem *item : placeholderRows)
        list->addItem(item); // 防御路径（正常排序无占位行）：保持追加到末尾的现状
}

// 非显示层的数据重排（§2.6 规则变更时全缓存层统一重排；非显示层的枚举收尾）：
// 容器组内排 + 实体组内排，与 sortListItems 的比较器口径逐项一致，不动任何行。
// **元数据只读缓存（cachedXxxMeta），未解析一律按未知**——本函数作用于任意缓存层
// （可能从未预读），绝不触发同步求值（GUI 线程冻结隐患，N26 的懒求值 getter 在
// 此禁用）；未排全的层被显示时经 needsMetaRescan 预读后由 advanceTab 重排补全
void PlaylistWidget::sortLevelData(Level &lvl, SortRule rule) {
    const SortKey key = rule.key; // N57 §2.9：入参值对象化，函数体不动
    const bool ascending = rule.ascending;
    if (key == SortKey::None)
        return;

    // ——容器组：分段序列现算（仅两个数字化键需要，不在比较器里逐次切分；容器侧
    // 两键同读 container.title）——
    std::stable_sort(
        lvl.containers.begin(), lvl.containers.end(),
        [key, ascending](const Level::ContainerEntry &a, const Level::ContainerEntry &b) {
            if (isNaturalNameKey(key))
                return containerLess(key, a, b, ascending, Util::segmentName(a.container.title),
                                     Util::segmentName(b.container.title));
            return containerLess(key, a, b, ascending, s_emptySeg, s_emptySeg);
        });
    // ——实体组：元数据只读缓存（Cached 档），未解析一律按未知——本函数作用于任意
    //    缓存层（可能从未预读），绝不触发同步求值（GUI 线程冻结隐患，N26 的懒求值
    //    getter 在此禁用）；未排全的层被显示时经 needsMetaRescan 预读后由 advanceTab
    //    重排补全
    std::stable_sort(
        lvl.entities.begin(), lvl.entities.end(),
        [key, ascending](const Level::EntityEntry &a, const Level::EntityEntry &b) {
            if (isNaturalNameKey(key)) {
                return entityLess(key, a, b, MetaAccess::Cached, ascending,
                                  Util::segmentName(naturalNameOf(key, a.entity.get())),
                                  Util::segmentName(naturalNameOf(key, b.entity.get())));
            }
            return entityLess(key, a, b, MetaAccess::Cached, ascending, s_emptySeg, s_emptySeg);
        });
}

// ——排序（N4 第五节）——

// N57 §2.9：原 `ruleKeyOf` / `ruleAscendingOf` 两个收敛读取点删除——它们在 N18 把
// 菜单 action 改为显式携带键值后即失去全部调用点（现取规则一律直读提供者 sortRule()），
// 属零调用死代码，删除不改任何行为。

// N32：手动排序链——只组装意图上下文（含判类别与写规则共用的规则，不预写规则），
// 预读与收尾一律由推进函数负责。取消由 finishCanceled 按意图分支处理：Sort 侧无快照，
// 一律不动（顺序不动、规则不落盘）
void PlaylistWidget::sortCurrent(SortRule rule) {
    PlaylistListView *list = viewAt(currentIndex());
    const int tab = currentIndex();
    // N19：忙时（菜单已禁用，此为兜底）与未加载/占位 tab 不排序。
    // N45：判定对象是当前显示层
    if (!list || tab < 0 || rule.key == SortKey::None || m_busy)
        return;
    if (!list->layerLoaded(list->browsePathIds()))
        return;

    JobContext ctx;
    ctx.intent = Intent::Sort;
    ctx.list = list;
    ctx.parent = list->browsePathIds(); // N45：目标层 = 当前显示层
    ctx.rule = rule;
    advanceTab(ctx, false);
}

// N19：排序执行体（无预读）——规则落位 + 非播放直排 / 播放四步式。
// N45 §2.6：四步式判据 = "当前显示层是不是队列根层"——排序非队列根层纯粹是视图
// 重排，mpv 队列不受影响；规则是全树统一的，改规则时队列根层一并按新规则重排，
// 并在需要时按四步式重推 mpv——即使此刻显示的是另一层
void PlaylistWidget::applySort(PlaylistListView *list, SortRule rule, bool writeRule) {
    if (!list)
        return;
    const int tab = indexOf(list);
    if (tab < 0)
        return;

    // N9：排序规则落位于提供者（随其持久化）
    if (writeRule) {
        if (auto *p = providerOfView(viewAt(tab)))
            p->setSortRule(rule);
    }
    const bool hadMemory = list->memory().lastPos >= 0;
    const QString captured = captureMemoryPath(list);

    const bool displayedIsQueueRoot = (list->browsePathIds() == list->queueRootIds());
    if (displayedIsQueueRoot) {
        // 显示层即队列根层：与旧行为同形——有正播行走四步式（重排 = 重排队列），
        // 否则直排
        if (tab == playingTabIndex() && playingRowOf(list) >= 0)
            runFourStep(list, tab, [&] { sortListItems(list, rule); });
        else
            sortListItems(list, rule);
    } else {
        // 显示层非队列根层：视图重排不动 mpv；队列根层（若已加载）数据一并重排，
        // 播放中再走四步式重推（停止 → 数据已排好 → 整表重推 → 续播）
        sortListItems(list, rule);
        if (tab == playingTabIndex()) {
            if (const Level *ql = list->findLevel(list->queueRootIds());
                ql && ql->loadError.isEmpty()) {
                runFourStep(list, tab, [this, list, rule] {
                    if (Level *ql = list->findLevel(list->queueRootIds()))
                        sortLevelData(*ql, rule);
                });
            }
        } else if (Level *ql = list->findLevel(list->queueRootIds())) {
            sortLevelData(*ql, rule);
        }
    }
    // N45 §2.6：规则是全树统一的——其余已缓存层一并按新规则重排。纯数据、
    // 只读缓存元数据（sortLevelData 零求值）；显示层与播放中 tab 的队列根层
    // 已在上方处理，此处跳过，避免双重排序
    {
        // 循环外一次取好：现算的坐标换算与两个 id 序列不该摊到每一层（本循环纯数据，
        // sortLevelData 只重排层内容，不动浏览路径 / 队列根 / 播放目标）
        const int playingTab = playingTabIndex();
        const QStringList shownIds = list->browsePathIds();
        const QStringList queueIds = list->queueRootIds();
        auto &cache = list->levels();
        for (auto lit = cache.begin(); lit != cache.end(); ++lit) {
            if (lit.key() == shownIds)
                continue; // 显示层已排（sortListItems）
            if (tab == playingTab && lit.key() == queueIds)
                continue; // 播放中 tab 的队列根层已排（四步式 mutate）
            sortLevelData(lit.value(), rule);
        }
    }

    restoreMemoryPath(list, captured, hadMemory);
}

// ——清理操作（N4 第六节）——

void PlaylistWidget::executeRemoval(const QList<int> &rowsIn) {
    PlaylistListView *list = viewAt(currentIndex());
    const int tab = currentIndex();
    if (!list || rowsIn.isEmpty() || tab < 0 || m_busy)
        return; // N19：忙时菜单已禁用，此为兜底

    // N45 §2.4.3：容器行 / 上一级行不参与删除——只保留实体行
    QList<int> rows;
    for (int r : rowsIn)
        if (rowToEntityIndex(list, r) >= 0)
            rows << r;
    if (rows.isEmpty())
        return;

    // N29：条目集合变化，两类已读元数据一并失效（N45：落在当前显示层）
    {
        Level &lvl = list->levelOf();
        lvl.mediaMetaLoaded = false;
        lvl.fileMetaLoaded = false;
    }
    const bool playingTab = (tab == playingTabIndex());
    // N45 §2.6：四步式判据 = 显示层即队列根层——删除队列根层的条目才动 mpv 队列；
    // 删除非队列根层的条目纯粹是本地视图变更
    const bool queueLayerPlaying = playingTab && list->browsePathIds() == list->queueRootIds();
    const bool hadMemory = list->memory().lastPos >= 0;
    const QString captured = captureMemoryPath(list);
    // 从大到小删除避免行号漂移
    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());

    // N30：队列根层播放中且存在正播行则走四步式（统一入口），mutate 即删行；
    // 否则本地直删后落盘
    if (queueLayerPlaying && playingRowOf(list) >= 0) {
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
void PlaylistWidget::removeRowsLocal(PlaylistListView *list, const QList<int> &rowsIn) {
    if (!list || rowsIn.isEmpty())
        return;
    // N45 §2.4.3：容器行 / 上一级行不参与删除——过滤掉非实体行
    QList<int> rows;
    for (int r : rowsIn)
        if (rowToEntityIndex(list, r) >= 0)
            rows << r;
    if (rows.isEmpty())
        return;
    // N29：条目集合变化，两类已读元数据一并失效（N45：落在当前显示层）
    {
        Level &lvl = list->levelOf();
        lvl.mediaMetaLoaded = false;
        lvl.fileMetaLoaded = false;
    }
    const bool hadMemory = list->memory().lastPos >= 0;
    const QString captured = captureMemoryPath(list);

    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    eraseEntitiesAt(list, sorted); // 先批量缩实体容器（行还在，按行序定位）
    for (int row : sorted) {
        if (row >= 0 && row < list->count())
            delete list->takeItem(row);
    }
    restoreMemoryPath(list, captured, hadMemory);
    // N60 §2.4：不再登记在途回显。本函数有两个调用点，对 mpv 侧的处置正好相反：
    // 四步式那支（右键"删除选中"在 isPlayingTab && queueLayer 分支内）随后 emit
    // removeRequested，会话由喂入侧 MainWindow::onRemoveRequested 的守卫开闭；
    // 来源失效清扫（applySourceRetired）故意不 emit——那支的 mpv 侧已被 stop 清空，
    // 再喂逐行 playlist-remove 打的是空队列（N55 §2.6 处置 4）
    emit contentsChanged(); // A17：本地删行条目数已变，刷新导航启停
}

void PlaylistWidget::removeInvalidItems() {
    PlaylistListView *list = viewAt(currentIndex());
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
    PlaylistListView *list = viewAt(currentIndex());
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
    PlaylistListView *list = viewAt(currentIndex());
    if (!list || m_busy)
        return; // N19：忙时菜单已禁用，此为兜底
    const int tab = currentIndex();
    // N64：形态现查（不存模式位）——失效面与 ②'' 口径随形态分派，换源后随提供者
    // 自动切换。整树提供者以不可修改示人，① 的 supportsModify 守卫自然跳过回推
    // （本篇不定义整树提供者的回推语义）
    const auto *provider = providerOfView(viewAt(tab));
    const bool wholeTree = provider && provider->enumeratesWholeTree();
    // ① 回推：已加载且可修改的表把当前内容写回提供者。默认 tab 同样回推——其提供者
    // 从不落盘，不回推则枚举返回空表，刷新即清空内容。回推只改内存，不落盘。
    // N45：已加载 = 层在缓存且无失败文案（§2.5）
    if (auto *p = providerOfView(viewAt(tab));
        list->layerLoaded(list->browsePathIds()) && p && p->supportsModify()) {
        // N58 §2.2：回传面消费裸实体表，此处拆包一次
        PlaylistEntity::List items;
        items.reserve(list->entries().size());
        for (const auto &entry : list->entries())
            items.push_back(entry.entity);
        p->setItems(std::move(items));
    }
    // ② 置位：旧代码在此清 entriesLoaded + loadError。N45 层模型下"未加载"由
    // 收尾覆盖表达：当前层的层仓保留（在途期显示行与实体须保活——recordPlayback
    // 与回刷仍读旧内容），仅清失败文案（旧②同样清它，使回刷在途比对不被失败态拦下）；
    // 成功收尾在 onLoadFinished 就地覆盖层内容。
    // N64：整树批次的目标层恒为根层，清根层（按显示层清会摸到深层，与 V5
    // "旧缓存层不被触及"打架；ensureLevel 对已存在层只清文案）
    list->ensureLevel(wholeTree ? QStringList{} : list->browsePathIds()).loadError.clear();
    // ②' N45 §2.5：失效当前层及其全部子孙层（按键的前缀匹配）。保留例外只剩浏览层
    // 一项——在途期显示行上的实体必须存活（Refresh 续作的 oldKeys 采集、回刷比对、
    // recordPlayback 均是硬依赖），由本批收尾覆盖；其余被失效层（含队列根层，
    // N53 §2.2）移出缓存，下次被显示时按需重枚举
    const QStringList browseIds = list->browsePathIds();
    const QStringList queueIds = list->queueRootIds();
    // ②' N45 §2.5（逐层）：失效当前层及其全部子孙层（按键的前缀匹配）。浏览层自身
    // 保留——在途期显示行上的实体必须存活（Refresh 续作的 oldKeys 采集、回刷比对、
    // recordPlayback 均是硬依赖），由本批收尾覆盖；其余被失效层（含队列根层，
    // N53 §2.2）移出缓存，下次被显示时按需重枚举。
    // N64 整树：发起不失效（变更记录 V5）——收尾"先删"本就是全树对账出口，发起再删
    // 一遍对成功冗余、对失败却把整棵旧树删掉（整树钻层不再枚举，恢复只剩再次刷新
    // 一条路）。在途期 m_busy 拦截一切导航，旧缓存层不被触及：成功收尾覆盖全树，
    // 失败旧树原地保活（根层照既有失败路径进占位，深层仍可经跳转取回）
    if (!wholeTree)
        list->invalidateDescendantsOf(browseIds);
    if (wholeTree) {
        // ②'' N64：整树重建必含队列根层，按"队列根层被重建"口径触发，不随发起时的
        // 浏览深度变化。判据取根层视角：队列根锚点非空（队列根即根层时与"从根层
        // 发起刷新"等效——播放连续性由收尾的比对三分支承载，不在此停播）。
        // 续播记忆不在此作废（与逐层同一口径，N53 变更记录 V2）
        if (tab == playingTabIndex() && !queueIds.isEmpty()) {
            emit stopRequested();
            m_playingUuid = kDefaultListUuid; // 同步目标归位默认列表（N57 §2.8：换身份）
            clearPlayingMarks(list);
            list->setQueueRoot({}); // 队列根锚点作废（空序列 ⇒ savePlaylists 不写该两键）
        }
    } else {
        // ②'' N53 §2.2：浏览层是队列根层的严格祖先且该 tab 是播放 tab ⇒ 整棵子树不可信
        // ——发起即一个同帧完成的原子动作组，成败都不回头：停播；同步目标无条件归位
        // （空闲时也归位，不留指向空队列基准的播放 tab）；清旧播放 tab 行上的播放标记
        // （归位不经 markPlayingTab，行上 PlayingRole 没有清除点，成功收尾的行级换入
        // 又刻意复用行——不在此清即永久残留）；队列根锚点作废（savePlaylists 见空序列
        // 不写 queueRootIds / queueRootTitles，重启后该 tab 落根层）。
        // 续播记忆不在此作废（N53 变更记录 V2）：刷新失败可能只是网络波动，恢复后
        // 重钻重举即按 lastPath 重新命中；指向已逐出层的记忆按"序号校验优先、路径
        // 首命中"安全退化，无命中即 -1，无错误续播面。
        // 归位先行使失败收尾的停播合取恒假 ⇒ 不会二次 stop
        const bool ancestorOfQueueRoot =
            queueIds.size() > browseIds.size() &&
            std::equal(browseIds.begin(), browseIds.end(), queueIds.begin());
        if (ancestorOfQueueRoot && tab == playingTabIndex()) {
            emit stopRequested();
            m_playingUuid = kDefaultListUuid; // 同步目标归位默认列表（N57 §2.8：换身份）
            clearPlayingMarks(list);
            list->setQueueRoot({}); // 队列根锚点作废（空序列 ⇒ savePlaylists 不写该两键）
        }
    }
    // ③ 驱逐：N20 条目只驱逐本表；flag 重置所有表（N45：全部层的两类标志一并复位）。
    // N64：驱逐层显式给定——整树批次的目标层是根层，按显示行取会摸到深层旧行
    evictListKeys(list, wholeTree ? QStringList{} : list->browsePathIds());
    // ④ 登记意图 + ⑤ 发起（收尾见 onLoadFinished：换入 → 推进函数）
    m_pendingLoad = PendingLoad{};
    m_pendingLoad.list = list;
    m_pendingLoad.refresh = true;
    startLoadAsync(list);
}

// N20/N26：按表精确驱逐。遍历指定层的实体，按实体 key 逐个删 m_duration；
// flag 重置所有表（N20 语义保留：本表驱逐可能清掉他表共享 key 的 mpv 推送时长，
// 他表切入经预读重读恢复）。N45：标志复位覆盖全部 tab 的全部层。
// N64：驱逐层显式给定——刷新发起时显示行与目标层在整树形态下不同层（行是深层、
// 目标是根层），按层取即与逐层"行 == 浏览层内容"等价（层所有权仓序恒等行序）
void PlaylistWidget::evictListKeys(PlaylistListView *list, const QStringList &ids) {
    if (!list)
        return;
    if (const Level *lvl = list->findLevel(ids))
        for (const auto &entry : lvl->entities)
            if (entry.entity)
                m_duration->remove(entry.entity->key());
    // 标志复位覆盖全部 tab 的全部层（N45）：跨表遍历留在本类，逐表交给自己清
    for (int i = 0; i < count(); ++i) {
        if (PlaylistListView *every = viewAt(i))
            every->resetMetaFlags();
    }
}

// ——N26 元数据能力谓词（一对，同形）：遍历该层实体，任一支持即真；
// 空表 / 未加载层为假——

bool PlaylistWidget::anyEntitySupportsMediaMeta(const PlaylistListView *list,
                                                const QStringList &ids) const {
    if (!list)
        return false;
    const Level *lvl = list->findLevel(ids);
    if (!lvl)
        return false;
    for (const auto &entry : lvl->entities) {
        if (entry.entity->supportsMediaMeta())
            return true;
    }
    return false;
}

bool PlaylistWidget::anyEntitySupportsFileMeta(const PlaylistListView *list,
                                               const QStringList &ids) const {
    if (!list)
        return false;
    const Level *lvl = list->findLevel(ids);
    if (!lvl)
        return false;
    for (const auto &entry : lvl->entities) {
        if (entry.entity->supportsFileMeta())
            return true;
    }
    return false;
}

// ——N29 类别映射与判定收敛点——
// N57 §2.9：排序键 → 类别的唯一映射 `metaCategoryOf` 已随键搬到 `PlaylistEntity.cpp`
// （本类不再持有），菜单灰态判据与本节的判定都直调那个全局函数。

// 对应类别支持者谓词：Media / File 各取其一（None 由调用方上游挡掉）
bool PlaylistWidget::anyEntitySupports(const PlaylistListView *list, const QStringList &ids,
                                       MetaCategory cat) const {
    return cat == MetaCategory::Media ? anyEntitySupportsMediaMeta(list, ids)
                                      : anyEntitySupportsFileMeta(list, ids);
}

// 切入重读判定内核：该层该排序规则仍需预读的类别（无需预读返回 None）。
// 注意只含"类别 / 支持者 / 标志位"三要素，busy / 已加载 / 占位等
// 外围条件由调用方自判
MetaCategory PlaylistWidget::pendingMetaCategory(const PlaylistListView *list,
                                                 const QStringList &ids, SortRule rule) const {
    const auto cat = metaCategoryOf(rule.key);
    if (cat == MetaCategory::None)
        return MetaCategory::None;
    if (!anyEntitySupports(list, ids, cat))
        return MetaCategory::None;
    return metaLoadedOf(list, ids, cat) ? MetaCategory::None : cat;
}

bool PlaylistWidget::metaLoadedOf(const PlaylistListView *list, const QStringList &ids,
                                  MetaCategory cat) const {
    const Level *lvl = list->findLevel(ids);
    if (!lvl)
        return false;
    return cat == MetaCategory::File ? lvl->fileMetaLoaded : lvl->mediaMetaLoaded;
}

void PlaylistWidget::setMetaLoadedOf(PlaylistListView *list, const QStringList &ids,
                                     MetaCategory cat, bool on) {
    Level &lvl = list->ensureLevel(ids);
    if (cat == MetaCategory::File)
        lvl.fileMetaLoaded = on;
    else
        lvl.mediaMetaLoaded = on;
}

// ——N19 忙时与遮罩——

void PlaylistWidget::setBusy(bool on) {
    m_busy = on;
    setListsEnabled(!on);
    if (!on) {
        m_overlayTimer.stop();
        m_overlay->finish();
        flushPendingTab();
        // N49：延后的手动定位补执行（同样在 flushPendingTab 之后：若排队切 tab 起了
        // 新任务，m_busy 复真，locatePlayingItem 会再次置旗延后，由那一轮的 busy 结束
        // 消费）。先于 m_pendingReveal：手动定位含自动归位的全部效果，两者同时待办时
        // 只执行手动那一次
        if (m_pendingLocate && !m_busy) {
            m_pendingLocate = false;
            locatePlayingItem();
        }
        // N45：延后的切曲归位补执行（在 flushPendingTab 之后：若排队切 tab 起了
        // 新任务，m_busy 复真，归位再次延后，由那一轮的 busy 结束消费）。
        // 归位自身不起任务（播放中读队列根层时它恒在缓存——逐出必与归位同步，
        // N53 §2.2，缓存命中即同步换入）
        if (m_pendingReveal && !m_busy) {
            m_pendingReveal = false;
            revealPlayingItem();
        }
        // N54：延后的层级定位补执行（同族纪律：忙时不动列表，走到这里才落浏览路径）。
        // 放在 reveal 之后、updateBottomBarState 之前：定位成功会由 MainWindow 起播并
        // 转移同步目标，按钮启用态要按新状态刷；若 flushPendingTab / reveal 又起了新
        // 任务使 m_busy 复真，advanceLevelJump 自守早退，旗留到那一轮的出口消费
        advanceLevelJump();
        // N49：忙时结束一并刷新定位按钮启用态——setListsEnabled(true) 只是把工具条
        // 整体恢复可用，按钮自身的 enabled 仍停留在进入忙时前的旧值；若在途期间停止
        // 播放，按钮会以"可点"的样子留到下一次切 tab 或回刷才纠正。放在延后消费之后，
        // 免得定位刚改过的状态又被覆盖
        updateBottomBarState();
    }
}

void PlaylistWidget::setListsEnabled(bool on) {
    tabBar()->setEnabled(on); // protected 成员，子类可用
    for (int i = 0; i < count(); ++i)
        if (PlaylistListView *l = viewAt(i))
            l->setEnabled(on);
    // N49：工具条随列表一并禁用（与遮罩覆盖构成双保险，沿用列表的既有做法）
    if (m_bottomBar)
        m_bottomBar->setEnabled(on);
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
    reserveBottomBarSpace(); // N49：本控件尺寸变化，工具条高度与 stack 的 y 都需重算
    if (m_overlay)
        m_overlay->setGeometry(rect()); // 遮罩贴合播放列表全域（含 tab 栏）
}

// ——N49 底部工具条——

// 让位：把内部页面栈压到"让出底部一行"的高度，工具条摆到底部通栏。
// 不装布局——基类 QTabWidget 自己是这一层的几何管理者（tab 栏与页面栈的几何由私有
// setUpLayout 经样式子元素矩形算出后直接 setGeometry），在这一层装布局就得复刻那套
// 算法并与基类争同一个 stack 的几何；改用最大高度约束：setGeometry 尊重 min/max 是
// 文档保证的行为，故基类每一次内部重排都被自动钳住。
// 基类重排恒发生在约束更新之前（resizeEvent 先调基类；insertTab / removeTab 先
// setUpLayout 后回调虚函数），故更新约束之后须主动补一次几何——否则既有几何仍停留在
// 旧约束下：窗口放大时矮一截、tab 栏换行增高时底边压进工具条。
// 入参全取自当前状态，且只改 stack 的高度、不改其 y 偏移，故本函数幂等、可任意重复调用
void PlaylistWidget::reserveBottomBarSpace() {
    if (!m_bottomBar || !m_stack)
        return;                                     // 页面栈取不到时整个让位静默跳过，不引入崩溃面
    const int h = m_bottomBar->sizeHint().height(); // 不设固定高度：随样式 / 字体自适应
    m_bottomBar->setGeometry(0, height() - h, width(), h);
    const int want = qMax(height() - h - m_stack->y(), 0);
    m_stack->setMaximumHeight(want);
    if (m_stack->height() != want)
        m_stack->setGeometry(m_stack->x(), m_stack->y(), m_stack->width(), want);
}

// 三个补算点一律在基类处理之后（与 Qt 内部让位机制的触发点对齐）
void PlaylistWidget::tabInserted(int index) {
    QTabWidget::tabInserted(index);
    reserveBottomBarSpace(); // 增删列表会改 tab 栏高度，进而改 stack 的 y 偏移
}

void PlaylistWidget::tabRemoved(int index) {
    QTabWidget::tabRemoved(index);
    reserveBottomBarSpace();
    // N54：删 tab 即作废挂起的层级定位——作废是静默的（不回报，MainWindow 不弹错）。
    // 忙时 tab 栏与列表整体禁用（setListsEnabled），正常流程走不到"在途删 tab"，此项属防御。
    // N57 §2.8 待办目标已改持身份，"索引漂移打到别的表"在结构上已不可能；但"被删的正是
    // 待办目标"这一情形下，本处静默作废与 §2.8 说的"换算不到坐标 ⇒ 出 ListGone"仍不一致
    // ——现状因忙时拦截不可达，**若日后放开忙时对删除的拦截，须回到此处复核**：应改为
    // 不在此清、交给 advanceLevelJump 换算失败出 ListGone，用户才拿得到一句解释
    m_pendingJump = PendingJump{};
}

bool PlaylistWidget::event(QEvent *event) {
    const bool handled = QTabWidget::event(event);
    // StyleChange 可能改字体与边框，进而改工具条高度与 stack 的 y 偏移
    if (event->type() == QEvent::StyleChange)
        reserveBottomBarSpace();
    return handled;
}

// 正播中判据（§2.4）：按钮启用态与定位入口的守卫共用，判据只此一份
// N61 §2.3：正播与否按正播路径出口判，不再裸读 path（无正播时该出口返空串，形状一致）
bool PlaylistWidget::hasPlayingItem() const {
    return m_mpv && !m_mpv->propertyFlag(QStringLiteral("idle-active"), true) &&
           !m_mpv->currentPlayingPath().isEmpty();
}

// 按钮启用态刷新（§2.4）：漏刷只影响灰态——定位入口幂等无副作用（切 tab 与归位皆幂等）
void PlaylistWidget::updateBottomBarState() {
    if (m_locateButton)
        m_locateButton->setEnabled(hasPlayingItem());
}

// 手动定位唯一入口（§2.4）：切到在播 tab 并把在播项选中滚动可见。
// 与 revealPlayingItem 分开的理由：后者在"显示层已是队列根层"这一分支上**故意**早退
// （每次切曲不打断用户的滚动位置），而手动定位必须补选中与滚动，两条入口各自保留语义
void PlaylistWidget::locatePlayingItem() {
    if (m_busy) {
        m_pendingLocate = true; // last-wins：定位幂等，多次触发一次执行
        return;
    }
    if (!hasPlayingItem())
        return;
    if (currentIndex() != playingTabIndex())
        setCurrentIndex(playingTabIndex()); // 切 tab 可能经 onTabChanged 起异步加载
    if (m_busy) {                           // 切完复查：加载在途则延后，由 busy 结束补执行
        m_pendingLocate = true;
        return;
    }
    PlaylistListView *list = viewAt(playingTabIndex());
    if (!list)
        return;
    if (list->displayedIds() == list->queueRootIds())
        restorePlayingState(list, playingTabIndex()); // 已是队列根层：只补选中与滚动，不重建行
    else
        revealQueueRoot(list); // 下钻后的子层：归位队列根层（内部已含选中与滚动）
}

// 可匹配行谓词（§2.5）：容器行参与——它是列表里实际显示的名字，也是层级浏览下最想跳转
// 的目标；"上一级"行是导航件不是内容、失败占位行本身不可选且文案是错误提示，两者都不参与
bool PlaylistWidget::isSearchableRow(const QListWidgetItem *item) {
    return item && !isPlaceholderRow(item) && !isParentLinkRow(item);
}

// 搜索推进（§2.5）：只按行的显示名做包含匹配、不区分大小写；查询串不变时反复回车继续
// 找后续匹配项；末尾回绕；回绕与全表无命中都"叮"一声；全程不改列表内容
void PlaylistWidget::searchNextInCurrentList() {
    if (m_busy)
        return;
    PlaylistListView *list = viewAt(currentIndex());
    const QString query = m_searchBox ? m_searchBox->text() : QString();
    if (query.isEmpty() || !list) {
        m_search = {}; // 空串回车不动作：不回绕、不发声、不报错
        return;
    }
    // 锚点有效性校验：查询串相同、同一张表、行号在范围内、且该行名字仍包含查询串
    // （末条即"行内容未变"这一层校验）。任一项不成立即视为新查询，从头开始——这是
    // "首行起搜"与"列表被重建 / 切过 tab 后不误用旧锚点"的同一套判据
    const bool resume = m_search.query == query && m_search.anchorList.data() == list &&
                        m_search.anchorRow >= 0 && m_search.anchorRow < list->count() &&
                        list->item(m_search.anchorRow)->text().contains(query, Qt::CaseInsensitive);
    const int last = list->count() - 1;
    const int wrapEnd = resume ? m_search.anchorRow : last;
    int hit = -1;
    for (int row = resume ? m_search.anchorRow + 1 : 0; row <= last; ++row) {
        QListWidgetItem *item = list->item(row);
        if (isSearchableRow(item) && item->text().contains(query, Qt::CaseInsensitive)) {
            hit = row;
            break;
        }
    }
    if (hit < 0) {
        // 只有"顺着往下直接命中的那一段"不发声：一旦进入回绕段（没能在锚点之后找到）
        // 就响一声，故回绕命中与全表无命中都响、且同一次回车至多一声
        QApplication::beep();
        if (resume) { // 新查询的第一段已是整表一趟，无需再扫
            for (int row = 0; row <= wrapEnd; ++row) {
                QListWidgetItem *item = list->item(row);
                if (isSearchableRow(item) && item->text().contains(query, Qt::CaseInsensitive)) {
                    hit = row;
                    break;
                }
            }
        }
    }
    if (hit < 0)
        return; // 全表无命中：选中与滚动一概不动，锚点保持原值（下次回车等价重试）
    QListWidgetItem *item = list->item(hit);
    list->setCurrentItem(item); // 不夺焦——焦点留在搜索框，故可连按回车
    list->scrollToItem(item, QListWidget::PositionAtCenter);
    m_search.query = query;
    m_search.anchorList = list;
    m_search.anchorRow = hit;
}

// ——N19 失败占位——

bool PlaylistWidget::isPlaceholderRow(const QListWidgetItem *item) {
    return item && item->data(PlaceholderRole).toBool();
}

void PlaylistWidget::showPlaceholder(PlaylistListView *list, const QString &error) {
    if (!list)
        return;
    clearPlaceholder(list);
    auto *item = new QListWidgetItem(list);
    item->setData(Qt::DisplayRole, tr("<Load failed>"));
    item->setData(PlaceholderRole, true);
    item->setToolTip(error);
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable); // 不可选中，防选中态残留
}

bool PlaylistWidget::clearPlaceholder(PlaylistListView *list) {
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

// N35 重写：判定看能力位；N39 变更：两类元数据一律追加缓存命中判定，已有值即不
// 重读（理由见函数内注释）。两者皆不求值只显示文件路径与已有值（N39 起以缓存值
// 渲染）。任务线程经求值转发重读（eval 即重读，不读缓存不落缓存），GUI 续作经
// 注入口写回实体缓存后用同一套行构建渲染——任务内不得直接调
// 共享实体的 fileMeta() / mediaMeta()
// （命中前写缓存与 GUI 绘制并发读写即 data race，N32 纪律），求值转发 + GUI 写回
// 是唯一合法形状。pending 态 General 组只放文件路径行，空一行后显示"正在读取元数据"。
// N35 修订 2：同步求值 + 结果静默丢弃——关闭对话框置 closed 旗（事件非轮询），
// 对话框即刻返回，后台任务跑完结果自然蒸发。发起前 GUI 线程显式判
// MediaInfoHelper::available()（supportsMediaMeta 内含，N32 前提）
void PlaylistWidget::showPropertiesDialog(const PlaylistEntity::Ptr &entityPtr) {
    const PlaylistEntity &entity = *entityPtr;
    // N39 变更：两类元数据一律"已有值即不重读"——判定除能力位外还看缓存命中，
    // File 与 Media 同口径（不可改一半留一半，否则同一对话框内语义冲突）。
    // 直接动因在 File 侧：无自主求值途径的实体（WebDavEntity）其 computeFileMeta()
    // 恒空，若照旧强制重读，结果经注入口写回会把枚举时注入的 size / modified
    // 覆盖成未知。两类均以缓存值为初值装进 result（GUI 线程读取，任务线程不得读
    // 实体缓存，N32 纪律）：需重读的那一类由任务覆盖，其余原样渲染——渲染恒取
    // result，调用点无需区分"求值过的"与"走缓存的"
    const bool needFileMeta = entity.supportsFileMeta() && entity.cachedFileMeta() == nullptr;
    const bool needMediaMeta = entity.supportsMediaMeta() && entity.cachedMediaMeta() == nullptr;
    struct PropResult {
        FileMeta fm;
        MediaInfoHelper::Data mm;
    };
    const auto result = std::make_shared<PropResult>();
    if (const FileMeta *c = entity.cachedFileMeta())
        result->fm = *c;
    if (const PlaylistEntity::MediaMeta *c = entity.cachedMediaMeta())
        result->mm = *c;

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
        // N56 §2.4：属性对话框 File Path 走显示出口（剥凭据）
        general.append({tr("File Path"), entity.displayUrl()});
        // N70：紧随路径并列两行——File Name（名称类排序键的取值）与 Display Name（列表行
        // 上显示的那份名字）。分家后两者在远端表上不再相同，此两行是唯一的核对面；
        // 值为空则省略该行，与本对话框 Format / Duration 的既有口径一致
        if (!entity.fileName().isEmpty())
            general.append({tr("File Name"), entity.fileName()});
        if (!entity.title().isEmpty())
            general.append({tr("Display Name"), entity.title()});
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

    // ——分支一：无需求值（不支持，或两类皆已缓存）→ 同步出框——
    if (!needFileMeta && !needMediaMeta) {
        renderDocument(result->fm, &result->mm,
                       /*pending=*/false); // N39：恒取 result（已装缓存值）
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
        // N39：渲染恒取 result——未求值的那一类仍是发起时装填的缓存值
        renderDocument(result->fm, &result->mm, /*pending=*/false);
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

// N47：容器属性对话框——同步即时（容器数据枚举时已齐，无异步求值、无 pending 态、
// 无媒体流分组）。渲染视觉语言与实体属性对话框一致（只读文本框、制表对齐、
// 三色体系），数据逻辑独立实现，不动 showPropertiesDialog
void PlaylistWidget::showContainerPropertiesDialog(const PlaylistProvider::Container &container,
                                                   const QString &fallbackPath) {
    auto timeText = [](const QDateTime &dt) {
        return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString();
    };

    struct Row {
        QString prop;
        QString value;
    };
    QList<Row> rows;
    rows.append({tr("Name"), container.title});
    // 路径行恒有内容：path 缺失回退相对路径（§2.2），缺省两者皆空才省略
    const QString path = !container.path.isEmpty() ? container.path : fallbackPath;
    if (!path.isEmpty())
        rows.append({tr("Path"), path});
    if (container.meta) {
        if (container.meta->created.has_value()) {
            const QString t = timeText(*container.meta->created);
            if (!t.isEmpty())
                rows.append({tr("Created"), t});
        }
        if (container.meta->modified.has_value()) {
            const QString t = timeText(*container.meta->modified);
            if (!t.isEmpty())
                rows.append({tr("Modified"), t});
        }
    }
    // 大小行一律不显示（§2.2）：目录大小无统计意义，WebDAV 恒未知

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Properties"));
    auto *view = new QTextEdit(&dialog);
    view->setReadOnly(true);
    view->setFrameShape(QFrame::NoFrame);
    view->setLineWrapMode(QTextEdit::NoWrap);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->addWidget(view);

    view->clear();
    { // 键列统一停靠宽度（与实体属性对话框同值）
        QTextOption opt = view->document()->defaultTextOption();
        opt.setTabStopDistance(100.0);
        view->document()->setDefaultTextOption(opt);
    }
    QTextCursor cur(view->document());
    // 配色与实体属性对话框一致：属性名蓝、属性值灰
    QTextCharFormat nameFmt;
    nameFmt.setForeground(QColor(0x19, 0x76, 0xD2));
    QTextCharFormat valueFmt;
    valueFmt.setForeground(QColor(0x60, 0x60, 0x60));
    for (const Row &row : rows) {
        cur.setCharFormat(nameFmt);
        cur.insertText(row.prop + QStringLiteral(":"));
        cur.setCharFormat(valueFmt);
        cur.insertText(QStringLiteral("\t"));
        cur.insertText(row.value);
        cur.insertBlock();
    }

    // 光标归位到文档开头，避免打开时选区残留
    view->setTextCursor(QTextCursor(view->document()));

    dialog.resize(520, qMin(int(rows.size()) * 22 + 40, 600));
    dialog.exec();
}

void PlaylistWidget::contextMenuEvent(QContextMenuEvent *event) {
    if (m_busy)
        return; // N19：忙时右键菜单禁用（表头/主体同守卫）
    // N49 §2.7：工具条区域不弹任何菜单——它既不在 tab 栏几何内、也不是列表主体，
    // 不早退就会在面板底部弹出一整套列表主体菜单，落点与语义都不对。
    // 搜索框内右键由 QLineEdit 自身的原生编辑菜单消费，事件不冒泡到本类，无需处理
    if (m_bottomBar && m_bottomBar->geometry().contains(event->pos()))
        return;
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
        // N62 §2.4：按名快速跳转组——一项一列表，序 = tab 序（改序与复制后菜单自动
        // 同步）；文本取列表对象的名（权威在列表对象，tab 文本只是镜像，N57 §2.6），
        // 默认列表的名构造期即 defaultListDisplayName。勾选 = 当前停留的 tab，恒可用
        // （跳转不需要作用对象，与 New Playlist 同档）；执行一律走 requestTab（N19
        // 统一入口），点当前项同索引早退无副作用。序即键：分发处按组内动作序号取
        // 目标下标，不另存映射表
        QActionGroup *jumpGroup = new QActionGroup(&menu);
        for (int i = 0; i < count(); ++i) {
            const Playlist *l = listOf(i);
            QAction *act = menu.addAction(l ? l->name() : tabText(i));
            act->setCheckable(true);
            act->setChecked(i == currentIndex());
            jumpGroup->addAction(act);
        }
        menu.addSeparator();
        QAction *newTabAction = menu.addAction(tr("New Playlist"));
        // N62 §2.3：点一下立即复制同配置同条目的新表（落末尾并激活），无对话框
        QAction *duplicateAction = menu.addAction(tr("Duplicate Playlist"));
        QAction *closeTabAction = menu.addAction(tr("Delete Playlist"));
        duplicateAction->setEnabled(hitTab > 0);
        closeTabAction->setEnabled(hitTab > 0); // 默认 tab 与空白区均无复制/删除
        // N62 §2.2 / §2.5：改序组——前移 = 朝表头起点（索引减小），后移 = 朝表尾；
        // 轴向词不进文案，表头布局改纵向时语义照旧成立。前移在 hitTab = 1 置灰即
        // "不许越过默认 tab"这条不变量在菜单上的表达
        menu.addSeparator();
        QAction *moveForwardAction = menu.addAction(tr("Move Forward"));
        QAction *moveBackwardAction = menu.addAction(tr("Move Backward"));
        moveForwardAction->setEnabled(hitTab > 1);
        moveBackwardAction->setEnabled(hitTab > 0 && hitTab < count() - 1);
        menu.addSeparator();
        QAction *settingsAction = menu.addAction(tr("Playlist Settings…"));
        settingsAction->setEnabled(hitTab > 0); // 默认 tab 与空白区无设置

        QAction *chosen = menu.exec(event->globalPos());
        if (!chosen)
            return;
        if (const int jumpIndex = jumpGroup->actions().indexOf(chosen); jumpIndex >= 0)
            requestTab(jumpIndex); // 忙时排队（N19）；本菜单空闲态弹出，即 setCurrentIndex
        else if (chosen == newTabAction)
            createNewList();
        else if (chosen == duplicateAction)
            duplicateList(hitTab);
        else if (chosen == closeTabAction)
            removeTabAt(hitTab);
        else if (chosen == moveForwardAction)
            reorderList(hitTab, hitTab - 1);
        else if (chosen == moveBackwardAction)
            reorderList(hitTab, hitTab + 1);
        else if (chosen == settingsAction)
            openListSettings(hitTab);
        return;
    }

    // ——列表主体菜单（原全量菜单剔除新建/删除/重命名三项）——
    PlaylistListView *list = viewAt(currentIndex());
    const int tab = currentIndex();
    const bool isPlayingTab = (tab == playingTabIndex());
    const bool modifiable = isModifiableTab(tab);

    // 右键落点命中检测：条目级操作以命中的行为作用对象。
    // 注意 childAt 命中的是 QListWidget 的 viewport（孙级），需回溯一级
    m_menuTargetList = nullptr;
    m_menuTargetRow = -1;
    PlaylistListView *hitList = underMouse ? qobject_cast<PlaylistListView *>(underMouse) : nullptr;
    if (!hitList && underMouse)
        hitList = qobject_cast<PlaylistListView *>(underMouse->parentWidget());
    if (hitList) {
        const int row =
            hitList->indexAt(hitList->viewport()->mapFromGlobal(event->globalPos())).row();
        if (row >= 0) {
            m_menuTargetList = hitList;
            m_menuTargetRow = row;
        }
    }
    // N19/N45：占位行 / 上一级行不是文件——命中即视为空目标
    // （复制/属性/定位全禁用，仅保留刷新与排序）。
    // N47：容器行不再视为空目标（属性 / 复制 / 本地打开按 §2.4 启停）
    if (m_menuTargetList && m_menuTargetRow >= 0 &&
        !isEntityRow(m_menuTargetList->item(m_menuTargetRow)) &&
        !isContainerRow(m_menuTargetList->item(m_menuTargetRow))) {
        m_menuTargetList = nullptr;
        m_menuTargetRow = -1;
    }
    // N26：目标行实体的共享计数副本——menu.exec() 的嵌套事件循环可能处理 mpv 推送，
    // 触发收编重建默认 tab（实体被整体释放），裸指针/引用届时即悬空；
    // 副本保活到分发结束，对话框读取的是存活实体（内容与右键时刻一致）
    PlaylistEntity::Ptr targetEntity =
        m_menuTargetList ? entityPtrAt(m_menuTargetList, m_menuTargetRow) : nullptr;
    const QString targetUrl = targetEntity ? targetEntity->path() : QString();
    // N47：容器目标的按值副本（实体行为空；菜单 exec 期间持有安全，见 containerAt）。
    // 展示路径取 path，缺失回退相对路径（标题拼接，与 tooltip 同口径，§2.2）；
    // 复制 / 打开只用 path 本体，不用回退值
    const std::optional<PlaylistProvider::Container> targetContainer =
        m_menuTargetList ? containerAt(m_menuTargetList, m_menuTargetRow) : std::nullopt;
    const QString containerPath = targetContainer ? targetContainer->path : QString();
    QString containerFallback;
    if (targetContainer) {
        QStringList titles;
        for (const PathSeg &seg : m_menuTargetList->browsePath())
            titles << seg.title;
        containerFallback = relativePathText(titles + QStringList{targetContainer->title});
    }

    // 弹层父指针挂顶层窗口（原因见表头菜单分支注释）
    QMenu menu(window());

    QAction *addAction = menu.addAction(tr("Add Files…"));
    menu.addSeparator();

    QMenu *sortMenu = menu.addMenu(tr("Sort"));
    QAction *sortActions[10];
    // N58：Original Order 置首位——回到枚举返回序（文案迁就读者、标识符
    // SortKey::EnumerationOrder 迁就实现，与 Released ↔ ReleaseDate 同形）
    sortActions[0] = sortMenu->addAction(tr("Original Order"));
    sortActions[1] = sortMenu->addAction(tr("File Name"));
    sortActions[2] = sortMenu->addAction(tr("File Name (Numeric)"));
    sortActions[3] = sortMenu->addAction(tr("Extension"));
    // N71：显示名档——只有一档（实际排序走数字化对比），落在 Extension 之后
    sortActions[4] = sortMenu->addAction(tr("Display Name"));
    sortActions[5] = sortMenu->addAction(tr("Created"));
    sortActions[6] = sortMenu->addAction(tr("Modified"));
    sortActions[7] = sortMenu->addAction(tr("File Size"));
    sortActions[8] = sortMenu->addAction(tr("Duration"));
    sortActions[9] = sortMenu->addAction(tr("Released"));
    // N18：菜单 action 显式携带 SortKey，解除"菜单数组下标 +1 == 枚举值"的脆弱不变式
    sortActions[0]->setData(int(SortKey::EnumerationOrder));
    sortActions[1]->setData(int(SortKey::FileName));
    sortActions[2]->setData(int(SortKey::FileNameNumeric));
    sortActions[3]->setData(int(SortKey::Extension));
    sortActions[4]->setData(int(SortKey::DisplayName));
    sortActions[5]->setData(int(SortKey::Created));
    sortActions[6]->setData(int(SortKey::Modified));
    sortActions[7]->setData(int(SortKey::FileSize));
    sortActions[8]->setData(int(SortKey::Duration));
    sortActions[9]->setData(int(SortKey::ReleaseDate));
    sortMenu->addSeparator();
    // N9：规则为单选勾选组（与持久化配套），方向互斥勾选且切向即时重排
    for (QAction *a : sortActions)
        a->setCheckable(true);
    QActionGroup *ruleGroup = new QActionGroup(sortMenu);
    auto *curProvider = providerOfView(viewAt(tab));
    // N57 §2.9：两读点塌成一个值对象读（无提供者时落 {None, 升序}，与现状同形）
    const SortRule cur = curProvider ? curProvider->sortRule() : SortRule{};
    for (QAction *a : sortActions)
        ruleGroup->addAction(a);
    if (cur.key != SortKey::None) {
        // N18：从 action data 反查 SortKey 对应 action，避免下标耦合
        for (QAction *a : sortActions) {
            if (a->data().toInt() == int(cur.key)) {
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
    ascAction->setChecked(cur.ascending);
    descAction->setChecked(!cur.ascending);

    QMenu *removeMenu = menu.addMenu(tr("Remove"));
    QAction *removeSelAction = removeMenu->addAction(tr("Remove Selected"));
    QAction *clearAction = removeMenu->addAction(tr("Clear Current Playlist"));
    removeMenu->addSeparator();
    QAction *removeInvalidAction = removeMenu->addAction(tr("Remove Invalid"));
    QAction *removeDupAction = removeMenu->addAction(tr("Remove Duplicates"));

    QAction *refreshAction = menu.addAction(tr("Refresh"));
    // N49：原"Locate Playing Item"项已上移为底部工具条的定位按钮，此处不再列该项
    menu.addSeparator();
    // N79：列表维护类（Refresh）→ 本组 → 条目类（Reveal / Copy Path / Properties）
    // 三段；N81 导入落位后本组三项的最终次序固定（复制 → 导入 → 导出）
    QAction *copyContentsAction = menu.addAction(tr("Copy Contents to New Playlist"));
    // N81：导入与导出成对反向；文案不点名格式与来源——格式由模块按后缀认，
    // 来源由上下文承担；省略号表会弹对话框
    QAction *importListAction = menu.addAction(tr("Import Playlist…"));
    // N80：文案不点名格式——格式由另存对话框的过滤器选；省略号表会弹对话框
    QAction *exportListAction = menu.addAction(tr("Export Playlist As…"));
    menu.addSeparator();

    QAction *openLocationAction = menu.addAction(tr("Reveal in File Explorer"));
    QAction *copyPathAction = menu.addAction(tr("Copy Path"));
    QAction *propsAction = menu.addAction(tr("Properties"));

    // ——启用条件——
    const bool hasList = list != nullptr;
    const int itemCount = hasList ? list->count() : 0;
    // N19：枚举失败占位态仅启用刷新与排序（排序只改持久化规则，不执行）。
    // N45：失败口径 = 当前显示层的 loadError 非空
    const bool loadFailed = hasList && !list->levelErrorOf(list->browsePathIds()).isEmpty();
    addAction->setEnabled(modifiable && !loadFailed); // 不可修改的 tab 内容来自枚举，禁手工添加
    auto *menuProvider =
        providerOfView(viewAt(tab)); // 排序能力遵从提供者（单项目录亦可持久化规则）
    sortMenu->setEnabled(hasList && menuProvider && menuProvider->supportsSort());
    // N30：统一后静态表刷新同样走枚举，"静态空表禁刷新"的前提已不成立，保持默认启用
    // （refreshAction 无额外限制，此处不调用 setEnabled）
    // N9：不可修改的 tab 移除子菜单整组禁用（失效清理走刷新）
    removeMenu->setEnabled(hasList && modifiable && !loadFailed);
    removeSelAction->setEnabled(hasList && !list->selectedItems().isEmpty());
    clearAction->setEnabled(itemCount > 0);
    // N79/N80 启用判据（exec() 前一次算好，两动作共用一份表达式）：有当前列表、
    // 当前层已加载且未失败、有实体、本列表非转接形态（N52 §2.8 三态表的唯一有效
    // 决策出口；取不到提供者按禁用）。其中 layerLoaded 已蕴含 !loadFailed（同一层
    // 同一 loadError），按规格两行分列写而不合并。有实体一项只判空、不换算内容：
    // itemCount 不可作判据（容器行与"上一级"行也计行），整表副本的量级与本类既有
    // 本层遍历判据（anyEntitySupports*）同档。其余不设限——默认 tab 同样可复制 /
    // 导出；忙时由菜单入口既有 m_busy 守卫统一挡住
    const bool contentsActionsEnabled =
        hasList && !loadFailed && list->layerLoaded(list->browsePathIds()) &&
        !currentLayerEntitiesOf(tab).empty() && menuProvider && !menuProvider->usesTransit();
    copyContentsAction->setEnabled(contentsActionsEnabled);
    exportListAction->setEnabled(contentsActionsEnabled);
    // N81 导入判据：有当前列表、可修改（= 静态表，恒平铺根层、恒非转接）、当前层
    // 已加载（静态表恒根层，此判据属防御；layerLoaded 已蕴含 !loadFailed）。不设
    // "有实体"——空表亦可导入；不设转接判据——走得到这里的只有静态表。忙时由
    // 菜单入口既有 m_busy 守卫统一挡住
    importListAction->setEnabled(hasList && modifiable && list->layerLoaded(list->browsePathIds()));

    // N26：启用真值统一绑定实体（占位 / 上一级行已在命中检测时挡掉，此处指针即真相）。
    // N47：容器目标绑定容器副本——属性恒启用；复制需 path 非空；打开需本地且存在
    const bool hasTarget = targetEntity != nullptr;
    // 本地形态判定看 path 本体（目录绝对路径天然本地，WebDAV 完整 URL 天然网络），
    // 不引入提供者身份接口；存在性以点击时刻为准
    const bool containerLocalExists = targetContainer && !containerPath.isEmpty() &&
                                      !Util::Url::isNetworkStream(containerPath) &&
                                      QDir(containerPath).exists();
    // 能力位取代字符串前缀猜测——定位仅对本地存在项启用
    openLocationAction->setEnabled(
        (targetEntity && targetEntity->isLocalFile() && targetEntity->exists()) ||
        containerLocalExists);
    copyPathAction->setEnabled(hasTarget || (targetContainer && !containerPath.isEmpty()));
    propsAction->setEnabled(hasTarget || targetContainer.has_value());

    // N26：依赖实体能力的排序项按本层支持者启用；其余排序项不受影响
    if (list) {
        const QStringList layerIds = list->browsePathIds();
        if (!anyEntitySupportsMediaMeta(list, layerIds)) {
            for (QAction *a : sortActions) {
                const int k = a->data().toInt();
                if (k == int(SortKey::Duration) || k == int(SortKey::ReleaseDate))
                    a->setEnabled(false);
            }
        }
        if (!anyEntitySupportsFileMeta(list, layerIds)) {
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
        QStringList files = QFileDialog::getOpenFileNames(
            this, tr("Add Files"), QString(),
            tr("Media files (%1);;All files (*)").arg(patterns.join(QLatin1Char(' '))));
        // 入口定形（N56 §2.6）：对话框返回正斜杠绝对路径，转原生分隔符——与
        // MainWindow::onOpenFile / 拖放同款；工厂不再代做定形
        for (QString &f : files)
            f = Util::Url::toNative(f);
        if (!files.isEmpty())
            addFiles(files);
    } else if (chosen == refreshAction) {
        refreshCurrentTab();
    } else if (chosen == copyContentsAction) {
        copyContentsToNewList(tab);
    } else if (chosen == importListAction) {
        importFromFile(); // N81：对话框、定形与失败处置收口在函数内，追加走 addFiles
    } else if (chosen == exportListAction) {
        exportListAs(tab);
    } else if (chosen == openLocationAction) {
        // N47：容器目标打开其 path（本地目录本身）；实体走原身份串
        Util::openFileLocation(targetContainer ? containerPath : targetUrl, this);
    } else if (chosen == copyPathAction) {
        // N47：容器目标复制其 path（无凭据形态）；实体走原身份串
        copyPathToClipboard(targetContainer ? containerPath : targetUrl);
    } else if (chosen == propsAction) {
        if (targetContainer)
            showContainerPropertiesDialog(*targetContainer, containerFallback);
        else if (targetEntity)
            showPropertiesDialog(targetEntity); // N35：传共享计数副本（异步对话框持有实体）
    } else if (chosen == removeSelAction && list && modifiable) {
        QList<int> rows;
        for (QListWidgetItem *sel : list->selectedItems())
            rows.append(list->row(sel));
        // N45 §2.6：只有显示层即队列根层时，删除才同步进 mpv（逐行 playlist-remove）
        const bool queueLayer = list->browsePathIds() == list->queueRootIds();
        if (isPlayingTab && queueLayer) {
            // N25：本地先行删行，再走既有同步通道——mpv 侧按实体序（队列序）移除，
            // 行序先行换算；逐个移除推送的中间态落在喂入侧会话的吞域内（N60），
            // 最终序列与本地一致由屏障终值对账兜住
            QList<int> entityIdxs;
            for (int r : rows) {
                const int idx = rowToEntityIndex(list, r);
                if (idx >= 0)
                    entityIdxs << idx;
            }
            removeRowsLocal(list, rows);
            emit removeRequested(entityIdxs);
        } else {
            executeRemoval(rows);
        }
    } else if (chosen == clearAction && list && modifiable) {
        QList<int> all;
        for (int i = 0; i < list->count(); ++i)
            all.append(i);
        // N45：清空的若不是队列根层，mpv 队列不受影响，无需停止
        if (isPlayingTab && list->browsePathIds() == list->queueRootIds())
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
        if (auto *p = providerOfView(viewAt(tab))) {
            p->setSortRule({p->sortRule().key, ascending});
            if (!loadFailed && p->sortRule().key != SortKey::None)
                sortCurrent(p->sortRule());
        }
    } else {
        // 排序规则命中：按当前方向执行一次并迁移勾选（sortCurrent 落位状态）
        // N18：从 action data 取 SortKey 而非数组下标 +1（避免菜单顺序与枚举序耦合）
        const int keyInt = chosen->data().toInt();
        if (keyInt > int(SortKey::None)) {
            const SortRule picked{static_cast<SortKey>(keyInt), cur.ascending};
            if (loadFailed) {
                if (auto *p = providerOfView(viewAt(tab)))
                    p->setSortRule(picked); // 仅持久化记忆，随下次成功加载生效
            } else {
                sortCurrent(picked);
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
    // N62 §2.6：删除的最后一道反悔，落点在守卫之后、三样现取之前——任何新入口
    // （工具条、快捷键、外部命令）接删除都必经这道确认；确认与动作之间的嵌套事件
    // 循环期间发生的任何变化都会被下方既有纪律现取到，所以本块的视图指针不带出块外
    // （exec 本身就是嵌套循环，被删页面的 deleteLater 会在其中执行掉）。parent 挂
    // window()（AGENTS §八），沿用 BookmarkEditorDialog::onClearAll 的标准 Yes / No
    // 形态，默认键 No（手滑回车不丢表）；文本格式钉死 PlainText，列表名不经富文本解析
    if (const PlaylistListView *confirmView = viewAt(idx); confirmView && confirmView->list()) {
        QMessageBox box(window());
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Confirm"));
        box.setText(tr("Delete playlist \"%1\"?").arg(confirmView->list()->name()));
        box.setTextFormat(Qt::PlainText);
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box.setDefaultButton(QMessageBox::No);
        if (box.exec() != QMessageBox::Yes)
            return;
    }
    PlaylistListView *closing = viewAt(idx);
    QUuid gone; // 待摘除列表的身份；无页面即无列表可摘
    if (closing)
        gone = closing->list()->uuid();
    // N55 §2.6：列表名与停播事实先采——摘除（m_lists->remove）后 nameOf 即空返回，
    // 下方停止块又会归位 m_playingUuid 改写判据状态，两样都必须在动作前现取。
    // 停播事实按身份判（m_playingUuid == gone）并就地驱动那个停止块：原写法
    // idx == playingTabIndex() 与它是同一判据的两种拼写，等价性靠 N57 §2.8 的身份↔
    // 坐标不变式——同一件事处处各写一遍，将来 tabIndexOfList 一旦添个回退分支即静默分叉
    const QString goneName = gone.isNull() ? QString() : m_lists->nameOf(gone);
    const bool playingRetired = (m_playingUuid == gone);
    if (playingRetired) {
        emit stopRequested();
        emit clearRequested();
        m_playingUuid = kDefaultListUuid; // 同步目标归位默认列表（身份判据，不是位置）
    }
    if (closing) {
        // ①（§2.7 关闭定序）在途批次若目标即本表，先作废
        if (m_job.target == closing)
            abandonJob();
        if (m_pendingLoad.list == closing)
            m_pendingLoad = PendingLoad{};
    }
    // N55 §2.6：清扫与触发点同点同步（正播是 U 时停播已由上方既有块完成，正播在
    // 默认 tab 时由清扫补停），须在 removeTab 之前做——tab 序号此后漂移，而清扫的
    // 停播判据与 removeRowsLocal 都依赖现行索引；动作先行的每一瞬间世界都处于一致态，
    // 弹窗不守护任何中间状态、只转述已发生的事实
    if (!gone.isNull())
        applySourceRetired(gone, goneName, playingRetired);
    // N57 §2.8：旧代码此处有一段"播放目标索引 > 被删索引则 --"的漂移修正——目标改持
    // 身份后 tab 序号与本对象无关，整段消失（现状下 m_busy 本就拦住在途删除，修正从不
    // 可达，属架构纯化而非修缺陷）
    removeTab(idx); // ② 此后 viewAt 的 index 域不再含它 ⇒ 注册表与视图之间无读者
    // ③ 注册表摘除即列表对象的唯一析构点（N28：若旧提供者仍在途，任务 lambda 持有的
    // shared_ptr 副本令其销毁自动延后到枚举任务结束）。状态随视图对象一并回收——
    // 原五张平行表的 remove 在此塌成"对象销毁"一件事
    if (!gone.isNull())
        m_lists->remove(gone);
    // ④ 页面 widget 必须显式销毁：QTabWidget::removeTab 只把它移出页面栈、不销毁
    // 对象（Qt 契约），状态进视图后留着即是把一切视图态挂在泄漏页面上。
    // deleteLater 而非立即 delete：本轮栈里仍可能持该 view 裸指针的调用点
    // （右键菜单收尾、信号回传）要活过本次事件处理
    if (closing)
        closing->deleteLater();
    savePlaylists();
}

// N19：忙时表头菜单已禁用，此为兜底
// N9：新建播放列表——弹列表设置对话框，确定后建 tab；内容一律经首次激活的异步枚举
// 填充（N28：所有源同路，静态源不再同步填充）
void PlaylistWidget::createNewList() {
    if (m_busy)
        return;
    // 默认名 = 既有列表名里 "Playlist <n>" 的最大序号 +1（Util::nextSequenceNumber，
    // 与书签自动命名同一份算法）。同一份名清单兼作对话框的重名占位表：采集一次、两处
    // 同源。N57 §2.7：唯一判据源是注册表清单（含默认列表名 ⇒"改成与默认同名"同处拒判），
    // 视图与对话框不再各扫一遍 tab
    const QStringList occupied = m_lists->names();
    const int nextN = Util::nextSequenceNumber(tr("Playlist "), occupied);
    ListSettingsDialog dlg(this, tr("Playlist %1").arg(nextN), nullptr, occupied, m_registry);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.listName();

    // 新建必产出提供者（对话框入口 m_current 为 null，不会走"未变更"分支）；
    // 断言守候不变量——空值会让注册表登记崩在 Debug 期，宁可炸出
    auto result = dlg.takeProvider();
    Q_ASSERT(result.has_value() && *result != nullptr);
    // N57 §2.7 新建序：登记（一次成型，身份随附）→ 建视图与 tab → 触发首载 → 保存。
    // 排序默认由提供者自述（不可修改的源默认按修改时间）
    auto *pl = m_lists->append(name, std::move(*result), QUuid());
    pl->provider()->setSortRule(pl->provider()->defaultSortRule());
    auto *list = createView(pl);
    addTab(list, name);
    // N28：静态表不再同步填充，切入（setCurrentIndex）触发首次异步加载
    setCurrentIndex(count() - 1);
    savePlaylists();
}

// N62 §2.2：改序内核——前移（toTab = fromTab - 1）与后移（+1）共用。断言的是调用方
// 菜单判据（hitTab > 0、方向步进一档）已排除的违约情形
void PlaylistWidget::reorderList(int fromTab, int toTab) {
    Q_ASSERT(fromTab > 0 && toTab > 0 && fromTab != toTab && fromTab < count() && toTab < count());
    if (fromTab <= 0 || toTab <= 0 || fromTab == toTab || fromTab >= count() || toTab >= count())
        return;
    PlaylistListView *view = viewAt(fromTab);
    if (!view || !view->list())
        return;
    // 权威先行：注册表序即落盘节序，先回写再动镜像，两序才不会分叉
    if (!m_lists->moveList(view->list()->uuid(), toTab - 1))
        return;
    // 镜像跟随：页面栈由 QTabWidget 内部的 tabMoved 同步槽自动搬移（Qt 契约），
    // viewAt / listOf 等按索引取页面的出口自动成立；移动的是当前表时 currentChanged
    // 照发，由 onTabChanged 的停留身份守卫收编为无操作
    tabBar()->moveTab(fromTab, toTab);
    savePlaylists();
}

// N62 §2.3：显示中条目回传提供者（写侧唯一入口）——落盘与复制共用同一判据。
// 未加载（含首载在途）的表不回传（N28）：此时副本条目 = 提供者自持串 = 源重启后的
// 条目，与"复制配置"自洽
void PlaylistWidget::backfillItems(Playlist *list) {
    PlaylistListView *view = viewAt(tabIndexOfList(list->uuid()));
    if (view && list->provider()->supportsModify() && view->layerLoaded(view->browsePathIds())) {
        // N58 §2.2：回传面消费裸实体表，此处拆包一次
        PlaylistEntity::List items;
        items.reserve(view->entries().size());
        for (const auto &entry : view->entries())
            items.push_back(entry.entity);
        list->provider()->setItems(std::move(items));
    }
}

// N62 §2.3：复制——点一下立即产出一张同配置同条目的新表；不弹任何对话框，
// 名字自动生成、落末尾并激活。复制是只读动作：不停播、不动播放目标与会话键
// （不经 markPlayingTab，播放语义什么都没发生）
void PlaylistWidget::duplicateList(int tabIndex) {
    if (tabIndex <= 0 || m_busy || m_configFile.isEmpty())
        return; // 与 openListSettings 同档兜底（N19：忙时表头菜单已禁用）
    PlaylistListView *srcView = viewAt(tabIndex);
    if (!srcView || !srcView->list())
        return;
    Playlist *src = srcView->list();
    // 条目回传先行：播放中 tab 经四步式与 mpv 对齐不落盘，提供者串可能滞后于显示，
    // 复制要的是屏幕上的那份（对源提供者是一次幂等写入）
    backfillItems(src);

    // 配置往返走现有配置文件上的暂存组（变更记录 1）：写、读、删在同一会话内一气
    // 呵成，暂存组从不 sync 落盘；组名带 Playlist_ 前缀，即便极端残留也会被
    // savePlaylists 的旧节清理收走
    QSettings settings(m_configFile, QSettings::IniFormat);
    const QString staging = QStringLiteral("Playlist_duplicate_staging");
    settings.beginGroup(staging);
    auto dup = src->provider()->duplicated(settings);
    settings.endGroup();
    settings.remove(staging);
    Q_ASSERT(dup != nullptr); // 与 createNewList 同口径：无提供者会使不变式在 Debug 撞断

    // 登记与落位与 createNewList 同形：登记（现铸身份，源身份已在册不另设防）→
    // 建视图 → 挂 tab → 激活 → 保存
    const int nextN = Util::nextSequenceNumber(tr("Playlist "), m_lists->names());
    auto *pl = m_lists->append(tr("Playlist %1").arg(nextN), std::move(dup), QUuid());
    auto *newView = createView(pl);
    addTab(newView, pl->name());
    requestTab(count() - 1); // 非忙时即 setCurrentIndex，激活经 onTabChanged 触发首载（N28）
    savePlaylists();
}

// N80 §2：条目采集共用化——N79 取数段（现取实体表 → playTargets → 逐项跳空串、
// 跳转接串）的原样收拢，复制与导出两个动作逐字共用同一段，不再有第二处取数真相；
// playTargets 仍是唯一的形态来源，本函数只是"既有出口的产物过一遍滤"，不是另造
// 通路。导出元数据只有标题（变更记录 2）：时长不进条目、不查任何时长源
QList<PlaylistFile::Entry> PlaylistWidget::collectTrackEntries(int tabIndex,
                                                               bool *skippedSome) const {
    if (skippedSome)
        *skippedSome = false;
    QList<PlaylistFile::Entry> entries;
    const PlaylistEntity::List src = currentLayerEntitiesOf(tabIndex);
    if (src.empty())
        return entries;
    const QVector<PlayTarget> targets = playTargets(tabIndex, src);
    entries.reserve(targets.size());
    for (int i = 0; i < targets.size(); ++i) {
        const PlayTarget &target = targets.at(i);
        if (target.url.isEmpty())
            continue; // 空槽位占位（N55 §2.3），不进产物
        // 转接串脱离本进程无意义（端点与列表身份都是会话内的），不进产物——兜住
        // "收编实体带原归属、原提供者是 Required 转接型仍铸转接串"的缝隙
        if (StreamTransit::isTransitUri(target.url)) {
            if (skippedSome)
                *skippedSome = true;
            continue;
        }
        // playTargets 与实体表一一对应（空实体也占位回空串），按下标取回标题
        entries.push_back(PlaylistFile::Entry{target.url, src[i]->title()});
    }
    return entries;
}

// N79：复制当前层内容到新建静态列表——内容的拍平快照，与 duplicateList 的整表
// 配置复制不是一回事：脱钩源类型，落成一串可独立播放的地址。只读动作：不停播、
// 不经 markPlayingTab、不动会话键（m_lastPlayingTargetUuid），播放语义什么都没发生。
// N80 §2：取数段收拢进 collectTrackEntries（两动作共用一个采集点），本动作只取
// url 字段——新静态表仍只含播放 url，标题与时长对它无意义（N79 §4 口径不动）
void PlaylistWidget::copyContentsToNewList(int tabIndex) {
    if (tabIndex < 0 || m_busy)
        return; // 与 openListSettings 同档兜底（N19：忙时菜单已禁用），但本项默认 tab 亦可，
                // 故只挡 <0（openListSettings 挡 <=0）
    PlaylistListView *srcView = viewAt(tabIndex);
    if (!srcView || !srcView->list())
        return;

    bool skippedSome = false;
    const QList<PlaylistFile::Entry> collected = collectTrackEntries(tabIndex, &skippedSome);
    // 跳过告知只记"有没有"不记条数（免复数译文负担）；延后出口，不在本栈 exec()
    if (skippedSome)
        queueDeferredNotice(tr("Some items were skipped because they use transit URLs"));
    if (collected.isEmpty())
        return;
    QStringList urls;
    urls.reserve(collected.size());
    for (const PlaylistFile::Entry &entry : collected)
        urls.append(entry.url); // 只取 url：标题与时长一律无视

    // 造表与 createNewList 同形：铸提供者 → 灌内容（新表的身份串就是播放串本身，
    // 元数据一切由 url 当场派生，fromPath 的已定形契约由播放串形态天然满足）→
    // 排序取提供者自述默认（静态即 {}）
    auto provider = StaticProvider::createDefault(m_registry);
    PlaylistProvider::EntityList items;
    items.reserve(urls.size());
    for (const QString &url : urls)
        items.push_back(PlaylistEntity::fromPath(url, m_registry));
    provider->setItems(items);
    provider->setSortRule(provider->defaultSortRule());

    // 登记与落位（N57 §2.7 同形）：登记（现铸身份）→ 建视图 → 挂 tab → 激活 → 保存
    const int nextN = Util::nextSequenceNumber(tr("Playlist "), m_lists->names());
    auto *pl = m_lists->append(tr("Playlist %1").arg(nextN), std::move(provider), QUuid());
    auto *newView = createView(pl);
    addTab(newView, pl->name());
    requestTab(count() - 1); // 非忙时即 setCurrentIndex，激活经 onTabChanged 触发首载（N28）
    savePlaylists();
}

// N80 §4：导出当前层内容为清单文件——采集 → 另存对话框（后缀即格式）→ 写盘 →
// 结果处置。只读动作：不停播、不经 markPlayingTab、不动会话键（N79 同款）；
// 成功静默（目标文件出现在用户选的位置即反馈），失败经延后出口弹一次窗
void PlaylistWidget::exportListAs(int tabIndex) {
    if (tabIndex < 0 || m_busy)
        return; // 与 copyContentsToNewList 同档兜底（忙时菜单已禁用），默认 tab 亦可
    if (!listOf(tabIndex))
        return; // listOf 内含 viewAt（view → list 一致性由构造断言守候），无需另判 view
    const QString listName = listOf(tabIndex)->name();

    bool skippedSome = false;
    const QList<PlaylistFile::Entry> tracks = collectTrackEntries(tabIndex, &skippedSome);
    // 转接跳过告知（文案与 N79 那处逐字相同——queueDeferredNotice 按串去重，同串
    // 天然合并，两处字面量不可各自改写）：本动作随后要开自己的嵌套循环（另存对话
    // 框），若走 singleShot 延后投递会落在对话框循环里、告知窗压住刚弹出的对话框
    // （N79 的调用序列无嵌套循环故无此患）——变更记录 2：入队后本栈直接同步投递，
    // 告知先于对话框出现（其描述的是采集结果，取消与否不影响）
    if (skippedSome) {
        queueDeferredNotice(tr("Some items were skipped because they use transit URLs"));
        pumpDeferredNotices();
    }
    // 采空早退：条目全部被滤掉（启用判据只保证有实体、不保证滤后非空）不开对话框，
    // 告知（上一条已投递）后静默返回——与 N79 空集早退对称
    if (tracks.isEmpty())
        return;

    // 两档（过滤器串，首选后缀）常量对摆在本侧：filterSelected 回串与两者逐字节
    // 相等比对即用对应后缀，不解析过滤器括号（N78 那处就地解析的写法不沿用）。
    // PLS 首位即默认选中；M3U 档括号内双 glob 以空格分隔是 Qt 语法，首选恒 .m3u。
    // ASX 档曾在冻结稿内，实测兼容性极差删除（N80 变更记录 1）
    const QList<QPair<QString, QString>> formats = {
        {tr("Playlist (*.pls)"), QStringLiteral("pls")},
        {tr("Winamp Playlist (*.m3u *.m3u8)"), QStringLiteral("m3u")},
    };
    QStringList filterTexts;
    for (const auto &f : formats)
        filterTexts.append(f.first);

    // 栈上实例（N78 §5 同款）：起始目录不设置、用系统默认；预填名 = 当前列表名
    // 原样拼默认格式后缀 .pls（名内 Windows 非法字符不处理，与 N78 变更记录 3
    // 同源、登记不做）；后缀联动——非原生对话框由 Qt 补，原生由系统补，两路不冲突
    QFileDialog dlg(this, tr("Export Playlist As"), listName + QStringLiteral(".pls"),
                    filterTexts.join(QStringLiteral(";;")));
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    const auto applyDefaultSuffix = [&dlg, &formats](const QString &filter) {
        for (const auto &f : formats) {
            if (f.first == filter) {
                dlg.setDefaultSuffix(f.second);
                return;
            }
        }
    };
    applyDefaultSuffix(dlg.selectedNameFilter());
    connect(&dlg, &QFileDialog::filterSelected, &dlg, applyDefaultSuffix);
    if (dlg.exec() != QDialog::Accepted)
        return; // 取消静默返回
    QString path = dlg.selectedFiles().value(0);
    if (path.isEmpty())
        return;

    // 后缀即格式：不在两档后缀常量内（不区分大小写）→ 按当前选中档首选后缀补齐，
    // 兜住手删后缀 / 手输怪后缀；selectedNameFilter 与两档常量都比对不上（理论
    // 不可达）→ 回退默认档首选后缀 .pls 继续写出，不静默返回——点了导出至少得到
    // 一个文件，writeFile 内部仍按后缀显影（N80 变更记录 2）。此后 writeFile 自己
    // 按后缀分派，此处不再传格式
    const QStringList knownSuffixes = {QStringLiteral("pls"), QStringLiteral("m3u"),
                                       QStringLiteral("m3u8")};
    if (!knownSuffixes.contains(QFileInfo(path).suffix().toLower())) {
        QString preferred = QStringLiteral("pls");
        for (const auto &f : formats) {
            if (f.first == dlg.selectedNameFilter()) {
                preferred = f.second;
                break;
            }
        }
        path += QLatin1Char('.') + preferred;
    }

    const auto result = PlaylistFile::writeFile(path, tracks);
    if (!result) {
        // 失败弹一次窗：首行固定文案，第二行模块给出的具体原因（诊断串不进翻译
        // 清单）——让用户看得出是权限不足、路径不可写还是格式不对；延后出口不在
        // 本栈 exec()（与 N79 跳过告知同款），日志留路径与错误
        qWarning() << "Failed to export playlist file:" << path << result.error();
        queueDeferredNotice(tr("Failed to export playlist file") + QLatin1Char('\n') +
                            result.error());
    }
    // 成功静默；写出的文件不登记进任何列表
}

// N81 §3-6：从 PLS / M3U 清单文件导入条目，末尾追加进当前列表。只取 url——
// 注入显示名持久化不了（静态表落盘只有 url 串），与 N79"只含播放 url"同一条
// 理由；条目定形与 ExternalProvider::normalizeTarget 的本地清单分支同一条规则
// （N81 变更记录 2：网络收口 → file: 折本地 → 绝对收口 → 余者按清单目录拼接，
// 四步次序不可乱）；定形后的串表交既有 addFiles——守卫、排序复位、元数据失效、
// 入列盖章、重编号、落盘、filesAdded 全由既有实现承担，本函数不另做追加、
// addFiles 不改
void PlaylistWidget::importFromFile() {
    // 栈上实例（N78 §5 同款）：打开模式，起始目录用系统默认；取消即静默返回。
    // 不设格式判定——格式由 PlaylistFile 按后缀认（只认后缀，不内容嗅探）
    QFileDialog dlg(this, tr("Import Playlist"), QString(),
                    tr("Playlist Files (*.pls *.m3u *.m3u8);;All Files (*)"));
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString path = dlg.selectedFiles().value(0);
    if (path.isEmpty())
        return;

    const auto read = PlaylistFile::readFile(path);
    if (!read) {
        // 失败弹一次窗（与导出同款：首行固定文案，第二行模块给出的具体原因，
        // 诊断串不进翻译清单；延后出口不在本栈 exec()），日志留路径与错误
        qWarning() << "Failed to read playlist file:" << path << read.error();
        queueDeferredNotice(tr("Failed to read playlist file") + QLatin1Char('\n') + read.error());
        return;
    }
    if (read->isEmpty()) {
        // 零有效条目：解析层空表非 error，处置归调用方（§7）
        queueDeferredNotice(tr("No entries found in the selected playlist file"));
        return;
    }

    // 条目定形：四步次序与 ExternalProvider::normalizeTarget 的本地清单分支同源
    // （N63：本地清单基址 = 清单文件所在目录，此处基址即它）。次序即正确性——file:
    // 前缀若不先折本地，它会被当相对串拼进基址；UNC 等无盘符绝对形态若不直接收口，
    // 有被补上基址盘符之虞（N81 变更记录 2）。唯一不跟 N63 的一处：畸形 file: 串折
    // 不出本地路径时本侧整条跳过，N63 侧会落基址目录——读侧既有"空串条目跳过"的
    // 口径优先
    const QDir baseDir(QFileInfo(path).absolutePath());
    QStringList urls;
    urls.reserve(read->size());
    for (const QString &entry : *read) {
        if (Util::Url::isNetworkStream(entry)) {
            urls.push_back(Util::Url::toNative(entry)); // 网络/流收口（http(s) 收规范形态）
            continue;
        }
        QString local = entry;
        if (local.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
            local = Util::Url::toIdentity(local); // file:// 折本地盘符串
            if (local.isEmpty())
                continue; // 畸形 file: 串折不出本地路径——与解析层"空串条目跳过"同口径
        }
        urls.push_back(QDir::isAbsolutePath(local)
                           ? Util::Url::toNative(local) // 绝对形态（含 UNC）直接收口
                           : Util::Url::toNative(baseDir.filePath(local)));
    }
    // 播放中 tab 的追加经 filesAdded 由 MainWindow 在喂入侧开推送会话同步进 mpv
    addFiles(urls);
}

// N9：存量列表设置——改名/换提供者。非法参数由对话框校验报错留在窗口；
// 合法后：仅改名直接生效；提供者有变则静默清空重建，作用对象为播放中 tab 时
// 停止（N25：同步目标归位默认 tab，空列表推送由回刷忽略）
void PlaylistWidget::openListSettings(int tabIndex) {
    if (tabIndex <= 0 || m_busy)
        return; // N19：忙时表头菜单已禁用，此为兜底
    PlaylistListView *list = viewAt(tabIndex);
    if (!list)
        return;

    // N57 §2.7：重名判据单点——注册表清单剔除自身身份（含默认列表名）。对话框不再
    // 自带第二份"默认"源文案，改判成与默认列表同名由这份清单拒掉
    const QStringList occupied = m_lists->names(list->list()->uuid());
    // 种子取列表对象的名字（tab 文本只是它的镜像）：只用于回显与变更判定
    ListSettingsDialog dlg(this, list->list()->name(), providerOfView(list), occupied, m_registry);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.listName();
    auto result = dlg.takeProvider();

    Playlist *pl = list->list();
    // N57 §2.6 不变式：名字住列表对象，tab 文本只是它的镜像，两处同点改
    pl->setName(name);
    setTabText(tabIndex, name);
    if (!result.has_value()) {
        // 仅改名（对话框判定提供者未变）：内容与提供者不动
        savePlaylists();
        return;
    }

    const bool wasPlaying = (tabIndex == playingTabIndex());
    if (wasPlaying) {
        emit stopRequested();             // 纯 stop 清空 mpv 侧列表（AGENTS 陷阱条目）
        m_playingUuid = kDefaultListUuid; // 同步目标归位默认列表（空列表推送由回刷忽略）
    }

    // N57 §2.7 换身：替换提供者即换身，身份与名字不动（视图不再持有提供者）
    pl->setProvider(std::move(*result));
    // 排序规则重置为新提供者类型默认（N57 §2.9：自述默认）
    pl->provider()->setSortRule(pl->provider()->defaultSortRule());

    // N55 §2.6 实施约束 1：换身恰好等于"来源参数变了"（hasChanges 把转接偏好与凭据
    // 都算进变更——改密码、改根 URL、翻转转接偏好都必经换身；仅改名不动实例不触发）。
    // 清扫必须与 setProvider 同点同步做完、先于新列表首载（新提供者首次枚举 / 播放即
    // publish 抢走 U 的表项），只有告知弹窗延后——经合用出口，天然落在设置对话框
    // exec() 退出之后，不在 accept() 栈内嵌套 exec()。列表名在 setName 之后现取
    applySourceRetired(pl->uuid(), pl->name(), wasPlaying);

    // N45 §2.5 + N57 §2.8：换源即整个 tab 的视图态作废——层缓存、浏览路径、队列根、
    // 显示层记录、续播记忆全清 + 行与实体一并回收，随后以新提供者异步重建。
    // 原"五张表各 remove 一处 + list->clear()"塌成视图的一个方法
    list->resetForNewSource();
    // N28：换源不再区分能力位，一律异步填充；当前 tab 索引未变（onTabChanged
    // 不会触发），显式发起以保留"换源即出内容"的体验，完成后选中首项（不播放）
    m_pendingLoad = PendingLoad{};
    m_pendingLoad.list = list;
    m_pendingLoad.selectFirst = true;
    startLoadAsync(list);
    savePlaylists();
}

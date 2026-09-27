#include "pch.h"

#include "PlaylistListView.h"

PlaylistListView::PlaylistListView(Playlist *list, QWidget *parent)
    : QListWidget(parent), m_list(list) {
    Q_ASSERT(m_list != nullptr); // 构造期注入，不存在无列表的视图
    // 序号位数随总数自适应（绘制时实时计算）；增删行后强制整表重绘，避免未受影响行
    // 保留旧的位数宽度造成混排。接线句柄留存：析构开头要抢先断开
    if (const auto *m = model()) {
        auto touch = [this] { viewport()->update(); };
        m_rowRepaints << connect(m, &QAbstractItemModel::rowsInserted, this, touch)
                      << connect(m, &QAbstractItemModel::rowsRemoved, this, touch)
                      << connect(m, &QAbstractItemModel::modelReset, this, touch);
    }
}

// 关闭 tab 时页面被显式销毁（§2.7 第④步）。QObject 要到自身析构的末尾才断开连接，
// 而子对象（viewport 与本表的 child model）在基类析构链里已先被删；销毁过程中 model
// 仍会发出 modelReset / rowsRemoved，捕了 this 的重绘回调届时取 viewport() 即悬空读
// （实测右键删除 tab 崩在 0xC0000005）。故在**最派生类的析构开头**断开这三条连接——
// 此刻基类尚完好，是唯一可靠的位置
PlaylistListView::~PlaylistListView() {
    for (QMetaObject::Connection &c : m_rowRepaints)
        QObject::disconnect(std::move(c));
    m_rowRepaints.clear();
}

// ——层缓存——

// 缺省插入空层；调用点均伴随建行 / 回传 / 直写 / 失败登记（插入即"已加载"）
PlaylistListView::Level &PlaylistListView::ensureLevel(const QStringList &ids) {
    return m_levels[ids];
}

const PlaylistListView::Level *PlaylistListView::findLevel(const QStringList &ids) const {
    const auto it = m_levels.constFind(ids);
    return it == m_levels.constEnd() ? nullptr : &it.value();
}

PlaylistListView::Level *PlaylistListView::findLevel(const QStringList &ids) {
    const auto it = m_levels.find(ids);
    return it == m_levels.end() ? nullptr : &it.value();
}

PlaylistListView::Level &PlaylistListView::levelOf() { return ensureLevel(browsePathIds()); }

bool PlaylistListView::layerLoaded(const QStringList &ids) const {
    const Level *lvl = findLevel(ids);
    return lvl && lvl->loadError.isEmpty();
}

QString PlaylistListView::levelErrorOf(const QStringList &ids) const {
    const Level *lvl = findLevel(ids);
    return lvl ? lvl->loadError : QString();
}

// 无条件删单层、不连带子孙（§2.2 列出的出口，也是下面"仅失败层"档的底座）
void PlaylistListView::eraseLevel(const QStringList &ids) { m_levels.remove(ids); }

// 只删"已失败"的那一层（未加载与成功层都不动）：navigateUp 的失败层不长期驻留，
// N54 的跳转是用户点名的显式重试入口，两处同形态。删除点不得铺开——切入失败层
// 按缓存渲染零请求，删层若挂在切 tab 等高频路径上才构成反复打服务端。不连带删
// 子孙——刷新发起时已按前缀整枝逐出（invalidateDescendantsOf）
void PlaylistListView::eraseFailedLevel(const QStringList &ids) {
    const auto lit = m_levels.constFind(ids);
    if (lit != m_levels.constEnd() && !lit->loadError.isEmpty())
        eraseLevel(ids);
}

// 刷新整枝（N45 §2.5）：失效当前层及其全部子孙层（按键的前缀匹配）。浏览层自身
// 保留——在途期显示行上的实体必须存活（Refresh 续作的 oldKeys 采集、回刷比对、
// recordPlayback 均是硬依赖），由本批收尾覆盖；其余（含队列根层）移出缓存
void PlaylistListView::invalidateDescendantsOf(const QStringList &browseIds) {
    for (auto it = m_levels.begin(); it != m_levels.end();) {
        const QStringList &k = it.key();
        const bool inSubtree = k.size() >= browseIds.size() &&
                               std::equal(browseIds.begin(), browseIds.end(), k.begin());
        if (!inSubtree || k == browseIds) {
            ++it; // 浏览层保留：本批收尾覆盖（行上实体在途期必须存活）
            continue;
        }
        it = m_levels.erase(it);
    }
}

void PlaylistListView::resetMetaFlags() {
    for (auto lit = m_levels.begin(); lit != m_levels.end(); ++lit) {
        lit->mediaMetaLoaded = false; // N29：两类标志位对称复位
        lit->fileMetaLoaded = false;
    }
}

QMap<QStringList, PlaylistListView::Level> &PlaylistListView::levels() { return m_levels; }

QList<PlaylistListView::Level::EntityEntry> &PlaylistListView::entries() {
    return levelOf().entities;
}

// N58 §2.3：重编号是 O(n)，只发生在内容集合变化的手工 / 换入路径上——天然无空洞
// 无冲突，删除留下的跳号也被顺手抹平（比较只看相对大小，重编号非必须但统一走一条纪律）
void PlaylistListView::renumberEnumerationOrder(Level &lvl) {
    for (int i = 0; i < lvl.containers.size(); ++i)
        lvl.containers[i].enumerationIndex = i;
    for (int i = 0; i < lvl.entities.size(); ++i)
        lvl.entities[i].enumerationIndex = i;
}

// ——路径与显示层——

QStringList PlaylistListView::browsePathIds() const {
    QStringList ids;
    for (const PathSeg &seg : m_browsePath)
        ids << seg.id;
    return ids;
}

QStringList PlaylistListView::queueRootIds() const {
    QStringList ids;
    for (const PathSeg &seg : m_queueRoot)
        ids << seg.id;
    return ids;
}

QList<PlaylistListView::PathSeg> PlaylistListView::browsePath() const { return m_browsePath; }

QList<PlaylistListView::PathSeg> PlaylistListView::queueRoot() const { return m_queueRoot; }

void PlaylistListView::setBrowsePath(const QList<PathSeg> &path) { m_browsePath = path; }

void PlaylistListView::setQueueRoot(const QList<PathSeg> &root) { m_queueRoot = root; }

QStringList PlaylistListView::displayedIds() const { return m_displayedIds; }

void PlaylistListView::setDisplayedIds(const QStringList &ids) { m_displayedIds = ids; }

// ——续播记忆——

PlaylistListView::Memory &PlaylistListView::memory() { return m_memory; }

const PlaylistListView::Memory &PlaylistListView::memory() const { return m_memory; }

void PlaylistListView::setMemory(const Memory &m) { m_memory = m; }

void PlaylistListView::clearMemory() { m_memory = Memory{}; }

void PlaylistListView::clearResumeProgress() {
    m_memory.lastTimePos = -1.0; // 保留最后播放项（B9）
}

// ——换源清理——

void PlaylistListView::resetForNewSource() {
    m_levels.clear();
    m_browsePath.clear();
    m_queueRoot.clear();
    m_displayedIds.clear();
    m_memory = Memory{};
    clear(); // 行与实体一并回收（实体所有权在 Level，随缓存同清）
}

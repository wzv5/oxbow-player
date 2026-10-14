#include "BookmarkManager.h"
#include "Util.h"
#include "pch.h"
#include "playlist/PlaylistRegistry.h" // N77 §2.5：过户目标组的保留身份 kDefaultListUuid（纯身份常量，无行为耦合）

BookmarkManager::BookmarkManager(const QString &configFile, QObject *parent)
    : QObject(parent), m_configFile(configFile) {}

void BookmarkManager::load() {
    m_data.clear();
    m_order.clear();
    m_displayOf.clear();
    m_originOf.clear();
    if (m_configFile.isEmpty())
        return;
    QSettings s(m_configFile, QSettings::IniFormat);
    const int size = s.value(QStringLiteral("Bookmarks/size"), 0).toInt();
    for (int i = 0; i < size; ++i) {
        const QString prefix = QStringLiteral("Bookmarks/%1").arg(i);
        const QString path = s.value(prefix + QStringLiteral("/path")).toString();
        if (path.isEmpty())
            continue;
        // 键归一化（Windows 统一小写）；原始形态保留给 UI 展示。
        // N77 §2.2：逐条归组 = (条目自带 listUuid, key(path))；缺键 / 无效 ⇒ nil
        // 独立成组（不并默认组）。旧 key 组里混着别列表追加的书签仍读作一组——混账不拆
        const Group group{
            QUuid::fromString(s.value(prefix + QStringLiteral("/listUuid")).toString()),
            Util::Url::key(path)};
        const int count = s.value(prefix + QStringLiteral("/count"), 0).toInt();
        QList<Bookmark> list;
        list.reserve(count);
        for (int j = 0; j < count; ++j) {
            const QString bp = prefix + QStringLiteral("/%1").arg(j);
            const double t = s.value(bp + QStringLiteral("/time"), 0.0).toDouble();
            const QString title = s.value(bp + QStringLiteral("/title")).toString();
            list.append({t, title});
        }
        if (!m_data.contains(group)) {
            std::stable_sort(list.begin(), list.end(),
                             [](const Bookmark &a, const Bookmark &b) { return a.time < b.time; });
            m_order.append(group);
            m_displayOf[group] = path;
            // N54：来源两键。缺键（旧 INI / 无身份书签）即空 Origin，不设迁移；
            // listUuid 为 nil 时 levelIds 不参与解读（整条来源无效）。
            // N57 §2.4：字符串→身份的转换只落这一处（fromString 对空串与非法串返 nil）
            const Origin origin{group.listUuid,
                                s.value(prefix + QStringLiteral("/levelIds")).toStringList()};
            if (origin.isValid())
                m_originOf[group] = origin;
            m_data[group] = list;
        } else {
            // 同组重复条目（理论不该出现，出现即历史脏数据）：并入既有组，首见展示与
            // 来源即定，条目合并不丢数据
            auto &existing = m_data[group];
            existing += list;
            std::stable_sort(existing.begin(), existing.end(),
                             [](const Bookmark &a, const Bookmark &b) { return a.time < b.time; });
        }
    }
}

void BookmarkManager::save() const {
    if (m_configFile.isEmpty())
        return;
    QSettings s(m_configFile, QSettings::IniFormat);
    s.remove(QStringLiteral("Bookmarks"));
    s.setValue(QStringLiteral("Bookmarks/size"), m_order.size());
    for (int i = 0; i < m_order.size(); ++i) {
        const Group group = m_order[i];
        const auto &list = m_data.value(group);
        const QString prefix = QStringLiteral("Bookmarks/%1").arg(i);
        // 落盘原始形态（展示保原样），载入时再归一为键
        s.setValue(prefix + QStringLiteral("/path"), m_displayOf.value(group, group.key));
        // N54：来源两键。无效来源（旧数据 / 损坏）两个键都不写，不留空串占位——
        // "缺键即无来源"是 load 的唯一判据。N77：uuid 分量即组标识自身，格式零改动。
        // N57 §2.4：身份→串只落这一处，字面形态（无花括号小写）与改前逐字相同
        const Origin origin = m_originOf.value(group);
        if (origin.isValid()) {
            s.setValue(prefix + QStringLiteral("/listUuid"),
                       origin.listUuid.toString(QUuid::StringFormat::WithoutBraces));
            s.setValue(prefix + QStringLiteral("/levelIds"), origin.levelIds);
        }
        s.setValue(prefix + QStringLiteral("/count"), list.size());
        for (int j = 0; j < list.size(); ++j) {
            const QString bp = prefix + QStringLiteral("/%1").arg(j);
            s.setValue(bp + QStringLiteral("/time"), list[j].time);
            s.setValue(bp + QStringLiteral("/title"), list[j].title);
        }
    }
    s.sync();
}

bool BookmarkManager::allGroupsLocal(const QString &key) const {
    bool any = false;
    for (const Group &group : std::as_const(m_order)) {
        if (group.key != key)
            continue;
        any = true;
        if (!Util::Url::isLocal(m_displayOf.value(group, group.key)))
            return false;
    }
    return any;
}

QList<BookmarkManager::GroupEntries> BookmarkManager::groupsByKey(const QString &key) const {
    QList<GroupEntries> out;
    for (const Group &group : std::as_const(m_order)) {
        if (group.key != key)
            continue;
        out.append({group, m_originOf.value(group), m_data.value(group)});
    }
    return out;
}

void BookmarkManager::addBookmark(const QString &rawPath, double time, const QString &title,
                                  const Origin &origin) {
    if (rawPath.isEmpty())
        return;
    // 键归一化（与仓/跳转匹配口径一致）；原始路径供展示。
    // N77 §2.1：组 = (origin.listUuid, key)。组即来源——在哪个列表加书签就落哪个
    // 列表的组，不存在"首见霸占"；存在即追加（不碰来源与展示）
    const Group group{origin.listUuid, Util::Url::key(rawPath)};
    // N56 §2.3/§2.4：书签层完全豁免显示纪律——存串取自正播实体的 path()，不含
    // 提供者注入的凭据（N48 的写侧剥除取消），用户显式提供的凭据原样保留并展示。
    // 展示路径 = 身份串原样（首见即定）
    const QString display = rawPath;
    auto it = m_data.find(group);
    if (it == m_data.end()) {
        m_data[group] = QList<Bookmark>{{time, title}};
        m_order.append(group);
        m_displayOf[group] = display;
        // 来源与展示路径同纪律——首见即定，之后不改。N77：组的 uuid 分量即来源身份，
        // 无效来源（旧数据形态）不记，组以 nil 身份成立
        if (origin.isValid())
            m_originOf[group] = origin;
    } else {
        it->append({time, title});
        std::stable_sort(it->begin(), it->end(),
                         [](const Bookmark &a, const Bookmark &b) { return a.time < b.time; });
    }
    save();
    emit changed();
}

void BookmarkManager::updateBookmark(const Group &group, int index, double time,
                                     const QString &title) {
    auto it = m_data.find(group);
    if (it == m_data.end() || index < 0 || index >= it->size())
        return;
    (*it)[index].time = time;
    (*it)[index].title = title;
    std::stable_sort(it->begin(), it->end(),
                     [](const Bookmark &a, const Bookmark &b) { return a.time < b.time; });
    save();
    emit changed();
}

void BookmarkManager::removeBookmark(const Group &group, int index) {
    auto it = m_data.find(group);
    if (it == m_data.end() || index < 0 || index >= it->size())
        return;
    it->removeAt(index);
    if (it->isEmpty()) {
        m_data.erase(it);
        m_order.removeAll(group);
        m_displayOf.remove(group);
        m_originOf.remove(group);
    }
    save();
    emit changed();
}

void BookmarkManager::removeFile(const Group &group) {
    if (!m_data.contains(group))
        return;
    m_data.remove(group);
    m_order.removeAll(group);
    m_displayOf.remove(group);
    m_originOf.remove(group);
    save();
    emit changed();
}

void BookmarkManager::clearAll() {
    if (m_data.isEmpty())
        return;
    m_data.clear();
    m_order.clear();
    m_displayOf.clear();
    m_originOf.clear();
    save();
    emit changed();
}

int BookmarkManager::removeInvalid(const ListAlivePredicate &listAlive) {
    int removed = 0;
    QList<Group> toRemove;
    for (const Group &group : std::as_const(m_order)) {
        // 校验以原始显示路径为准（而非小写归一键）。本地项判据收在 Util::Url::isMissingLocal
        // 单点（与书签跳转的本地闸门同一口径，N54 §2.7）：file:// 与 oxbow:// 先还原成
        // 来源路径，目录不算缺失（mpv 能播目录）
        const QString disp = m_displayOf.value(group, group.key);
        if (Util::Url::isMissingLocal(disp)) {
            toRemove.append(group);
            continue;
        }
        // 本地且存在 → 有效：磁盘就是它的归属，列表在不在都能就地播（§2.2 第 4 步的
        // 本地回退档）
        if (Util::Url::isLocal(disp))
            continue;
        // 远端项看归属（N54）：无来源身份属数据异常（本篇不做 INI 兼容与迁移），来源
        // 身份在现存列表里查不到则跳转必被拒（远端不回退，凭据与转接端点都随列表没了）
        // ——两类都是死条目。N77：谓参逐组以其自身 uuid 调用（组标识的 uuid 分量与
        // 来源身份恒等，语义与改前一致）
        const Origin origin = m_originOf.value(group);
        if (!origin.isValid() || !listAlive(origin.listUuid))
            toRemove.append(group);
    }
    for (const Group &group : std::as_const(toRemove)) {
        m_data.remove(group);
        m_order.removeAll(group);
        m_displayOf.remove(group);
        m_originOf.remove(group);
        ++removed;
    }
    if (removed > 0) {
        save();
        emit changed();
    }
    return removed;
}

void BookmarkManager::transferToDefault(const QUuid &fromUuid, const QString &key) {
    // 入参须为非默认身份：from == to 时整体迁移支会把默认组的 levelIds 静默清空。
    // 现下不可达（默认列表恒在注册表 ⇒ 跳转的 targetAlive 恒真，本地分流到不了这里），
    // 但本函数是公开 API，在入口钉死
    if (fromUuid == kDefaultListUuid)
        return;
    // N77 §2.5：源组缺席即无事可做（跳转前组已不存在的防御早退）
    const Group from{fromUuid, key};
    if (!m_data.contains(from))
        return;
    // m_data 与 m_order 恒同步（所有变更路径同支维护），indexOf 失配即不变式已破，
    // 就地早退兜住 QList 负下标
    const int pos = m_order.indexOf(from);
    if (pos < 0)
        return;
    const Group to{kDefaultListUuid, key};
    QList<Bookmark> list = m_data.take(from);
    if (!m_data.contains(to)) {
        // 整体迁移：组 uuid 改默认，来源 uuid 同步改写、levelIds 清空（空配有效身份
        // 即根层，默认 tab 恒平铺），组序原位保留
        m_order[pos] = to;
        m_displayOf[to] = m_displayOf.take(from);
        Origin origin = m_originOf.take(from);
        origin.listUuid = kDefaultListUuid;
        origin.levelIds.clear();
        m_originOf[to] = origin;
        m_data[to] = list;
    } else {
        // 合并：书签并入后按时间稳定排序，不去重（永不查重一贯）；展示串保留目标
        // 既有（首见即定一贯）；源组层引用丢弃（层跟目标组）
        m_data[to] += list;
        std::stable_sort(m_data[to].begin(), m_data[to].end(),
                         [](const Bookmark &a, const Bookmark &b) { return a.time < b.time; });
        m_order.removeAll(from);
        m_displayOf.remove(from);
        m_originOf.remove(from);
    }
    save();
    emit changed();
}

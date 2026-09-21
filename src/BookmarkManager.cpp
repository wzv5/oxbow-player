#include "BookmarkManager.h"
#include "Util.h"
#include "pch.h"

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
        // 键归一化（Windows 统一小写）；原始形态保留给 UI 展示
        const QString key = Util::normalizePathForMatch(path);
        const int count = s.value(prefix + QStringLiteral("/count"), 0).toInt();
        QList<Bookmark> list;
        list.reserve(count);
        for (int j = 0; j < count; ++j) {
            const QString bp = prefix + QStringLiteral("/%1").arg(j);
            const double t = s.value(bp + QStringLiteral("/time"), 0.0).toDouble();
            const QString title = s.value(bp + QStringLiteral("/title")).toString();
            list.append({t, title});
        }
        // 按 time 升序，稳定
        std::stable_sort(list.begin(), list.end(),
                         [](const Bookmark &a, const Bookmark &b) { return a.time < b.time; });
        if (!m_data.contains(key))
            m_order.append(key);
        m_displayOf[key] = path;
        // N54：来源两键。缺键（旧 INI / 无身份书签）即空 Origin，不设迁移；
        // listUuid 为空时 levelIds 不参与解读（整条来源无效）。同键多组（理论不该
        // 出现，出现即历史脏数据）按"首见即定"取前一组，与 addBookmark 同纪律
        const Origin origin{s.value(prefix + QStringLiteral("/listUuid")).toString(),
                            s.value(prefix + QStringLiteral("/levelIds")).toStringList()};
        if (origin.isValid() && !m_originOf.contains(key))
            m_originOf[key] = origin;
        m_data[key] = list;
    }
}

void BookmarkManager::save() const {
    if (m_configFile.isEmpty())
        return;
    QSettings s(m_configFile, QSettings::IniFormat);
    s.remove(QStringLiteral("Bookmarks"));
    s.setValue(QStringLiteral("Bookmarks/size"), m_order.size());
    for (int i = 0; i < m_order.size(); ++i) {
        const QString key = m_order[i];
        const auto &list = m_data.value(key);
        const QString prefix = QStringLiteral("Bookmarks/%1").arg(i);
        // 落盘原始形态（展示保原样），载入时再归一为键
        s.setValue(prefix + QStringLiteral("/path"), m_displayOf.value(key, key));
        // N54：来源两键。无效来源（默认 tab 出生 / 旧数据）两个键都不写，不留空串
        // 占位——"缺键即无来源"是 load 的唯一判据
        const Origin origin = m_originOf.value(key);
        if (origin.isValid()) {
            s.setValue(prefix + QStringLiteral("/listUuid"), origin.listUuid);
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

QList<BookmarkManager::Bookmark> BookmarkManager::bookmarks(const QString &path) const {
    return m_data.value(path);
}

void BookmarkManager::addBookmark(const QString &rawPath, double time, const QString &title,
                                  const Origin &origin) {
    if (rawPath.isEmpty())
        return;
    // 键归一化（与仓/跳转匹配口径一致）；原始路径供展示
    const QString key = Util::normalizePathForMatch(rawPath);
    // N48：展示路径不留凭据——rawPath 取自 mpv 的 path，播放 WebDAV 时是带凭据的
    // 播放形态，原样落盘会让密码与 davPassword 并列存进同一个 INI、"展示保原样"
    // 的例外仅此一项。只约束写侧：load 不为已有值做任何处理（旧数据的兼容问题
    // 已知且接受，不做清理）
    const QString display = Util::stripUrlCredentials(rawPath);
    auto it = m_data.find(key);
    if (it == m_data.end()) {
        m_data[key] = QList<Bookmark>{{time, title}};
        m_order.append(key);
        m_displayOf[key] = display;
        // N54：来源与展示路径同纪律——首见即定，之后不改（写在新增分支内即为该纪律
        // 的全部实现：同键再次加书签不进本分支，故后来者不覆盖既有来源）
        if (origin.isValid())
            m_originOf[key] = origin;
    } else {
        it->append({time, title});
        std::stable_sort(it->begin(), it->end(),
                         [](const Bookmark &a, const Bookmark &b) { return a.time < b.time; });
    }
    save();
    emit changed();
}

void BookmarkManager::updateBookmark(const QString &path, int index, double time,
                                     const QString &title) {
    auto it = m_data.find(path);
    if (it == m_data.end() || index < 0 || index >= it->size())
        return;
    (*it)[index].time = time;
    (*it)[index].title = title;
    std::stable_sort(it->begin(), it->end(),
                     [](const Bookmark &a, const Bookmark &b) { return a.time < b.time; });
    save();
    emit changed();
}

void BookmarkManager::removeBookmark(const QString &path, int index) {
    auto it = m_data.find(path);
    if (it == m_data.end() || index < 0 || index >= it->size())
        return;
    it->removeAt(index);
    if (it->isEmpty()) {
        m_data.erase(it);
        m_order.removeAll(path);
        m_displayOf.remove(path);
        m_originOf.remove(path);
    }
    save();
    emit changed();
}

void BookmarkManager::removeFile(const QString &path) {
    if (!m_data.contains(path))
        return;
    m_data.remove(path);
    m_order.removeAll(path);
    m_displayOf.remove(path);
    m_originOf.remove(path);
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
    QStringList toRemove;
    for (const QString &key : std::as_const(m_order)) {
        // 校验以原始显示路径为准（而非小写归一键）。本地项判据收在 Util::isMissingLocalSource
        // 单点（与书签跳转的本地闸门同一口径，N54 §2.7）：file:// 与 mpvqt:// 先还原成
        // 来源路径，目录不算缺失（mpv 能播目录）
        const QString disp = m_displayOf.value(key, key);
        if (Util::isMissingLocalSource(disp)) {
            toRemove.append(key);
            continue;
        }
        // 本地且存在 → 有效：磁盘就是它的归属，列表在不在都能就地播（§2.2 第 4 步的
        // 本地回退档）
        if (Util::isLocalSourcePath(disp))
            continue;
        // 远端项看归属（N54）：无来源 uuid 属数据异常（本篇不做 INI 兼容与迁移），来源
        // uuid 在现存列表里查不到则跳转必被拒（远端不回退，凭据与转接端点都随列表没了）
        // ——两类都是死条目。listAlive 未接时不据"列表是否还在"删有来源的项（无从判定）
        const Origin origin = m_originOf.value(key);
        if (!origin.isValid() || (listAlive && !listAlive(origin.listUuid)))
            toRemove.append(key);
    }
    for (const QString &p : toRemove) {
        m_data.remove(p);
        m_order.removeAll(p);
        m_displayOf.remove(p);
        m_originOf.remove(p);
        ++removed;
    }
    if (removed > 0) {
        save();
        emit changed();
    }
    return removed;
}

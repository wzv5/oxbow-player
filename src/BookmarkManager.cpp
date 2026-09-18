#include "BookmarkManager.h"
#include "PlaylistWidget.h"
#include "Util.h"
#include "pch.h"

BookmarkManager::BookmarkManager(const QString &configFile, QObject *parent)
    : QObject(parent), m_configFile(configFile) {}

void BookmarkManager::load() {
    m_data.clear();
    m_order.clear();
    m_displayOf.clear();
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

void BookmarkManager::addBookmark(const QString &rawPath, double time, const QString &title) {
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
    save();
    emit changed();
}

void BookmarkManager::clearAll() {
    if (m_data.isEmpty())
        return;
    m_data.clear();
    m_order.clear();
    m_displayOf.clear();
    save();
    emit changed();
}

int BookmarkManager::removeInvalid() {
    int removed = 0;
    QStringList toRemove;
    for (const QString &key : std::as_const(m_order)) {
        // 校验以原始显示路径为准（而非小写归一键）；先经 resolveLocalPath 把 file:// 转本地路径，
        // 使其能按本地文件做存在性判定（本地文件已删除则为失效项，应移除）。
        // 剩余的网络/流路径（http/ftp/stream 等，含 :// 且非
        // file:）保持原样、无法以文件存在性判定，须保留。
        const QString disp = m_displayOf.value(key, key);
        const QString local = Util::resolveLocalPath(disp);
        if (Util::isNetworkStreamPath(local))
            continue;
        if (!QFile::exists(local))
            toRemove.append(key);
    }
    for (const QString &p : toRemove) {
        m_data.remove(p);
        m_order.removeAll(p);
        m_displayOf.remove(p);
        ++removed;
    }
    if (removed > 0) {
        save();
        emit changed();
    }
    return removed;
}

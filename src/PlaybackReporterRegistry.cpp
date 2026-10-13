#include "pch.h"

#include "PlaybackReporterRegistry.h"

std::vector<PlaybackReporterRegistry::Entry> PlaybackReporterRegistry::snapshot() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Entry> out;
    out.reserve(m_entries.size());
    // lock 失败者即过期并顺手清理（erase 返回下一迭代子，其余项的注册顺序不变）
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (auto reporter = it->reporter.lock()) {
            out.push_back(Entry{it->key, it->listUuid, std::move(reporter)});
            ++it;
        } else {
            it = m_entries.erase(it);
        }
    }
    return out;
}

void PlaybackReporterRegistry::add(const QString &key, const QUuid &listUuid,
                                   const std::shared_ptr<PlaybackReporter> &reporter) {
    Q_ASSERT(reporter != nullptr);
    const std::lock_guard<std::mutex> lock(m_mutex);
    // 同 (种类键, 归属列表) 只留最新一册：换源 / 重新授权后旧提供者实例可能仍被
    // 在途任务的 shared_ptr 吊着（N55 §2.6），不顶掉就会有两个归属同一条列表的
    // 报告者并存——重复投递，甚至把进度写进旧凭据那个账户
    std::erase_if(m_entries,
                  [&](const Row &row) { return row.key == key && row.listUuid == listUuid; });
    m_entries.push_back(Row{key, listUuid, reporter});
    m_hasEntry = true;
}

bool PlaybackReporterRegistry::hasEntry() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_hasEntry;
}

bool PlaybackReporterRegistry::claimsSource(const QUuid &ownerUuid, const QUuid &sourceUuid) {
    if (ownerUuid.isNull())
        return true; // 无归属的注册（测试自立的表 / 不经列表绑定点的形态）：一律投
    if (sourceUuid.isNull())
        return false; // 来源不明（正播实体取不到或错配）——宁漏不串
    return ownerUuid == sourceUuid;
}

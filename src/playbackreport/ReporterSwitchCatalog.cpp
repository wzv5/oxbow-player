#include "pch.h"

#include "ReporterSwitchCatalog.h"

#include "JellyfinProvider.h" // kJellyfinProviderKey：本文件是取具体种类键常量的唯一聚合点

namespace {
// 账目行的定义处：行以命名常量出生、由 buildSwitches 组装成表，具名单行出口认常量不认
// 行序（"第 0 行是 Jellyfin"这种位置假设一旦有人插队就静默错位——设置页那一项会去读写
// 别家的键，正是本表要消灭的那类漏配）
constexpr ReporterSwitch kJellyfinRow{"reportJellyfin", kJellyfinProviderKey, true};

// 登记表自检（ProviderCatalog::buildCatalog 同层）：行非空、iniKey 与 reporterKey 各自
// 互不相同。空键与重复键一律 qWarning 显影、不阻断启动（同 ProviderCatalog 与 R4 数据
// 目录自检的口径）；Debug 期另断言表非空——表为空即聚合点漏配，消费方处处拿到空表
std::vector<ReporterSwitch> buildSwitches() {
    std::vector<ReporterSwitch> table = {kJellyfinRow};
    QSet<QString> iniKeys;
    QSet<QString> reporterKeys;
    for (size_t i = 0; i < table.size(); ++i) {
        const QString ini = QString::fromLatin1(table[i].iniKey);
        const QString reporter = QString::fromLatin1(table[i].reporterKey);
        if (ini.isEmpty())
            qWarning("ReporterSwitchCatalog: row %zu carries an empty ini key", i);
        else if (iniKeys.contains(ini))
            qWarning("ReporterSwitchCatalog: duplicate ini key \"%s\"", qPrintable(ini));
        if (reporter.isEmpty())
            qWarning("ReporterSwitchCatalog: row %zu carries an empty reporter key", i);
        else if (reporterKeys.contains(reporter))
            qWarning("ReporterSwitchCatalog: duplicate reporter key \"%s\"", qPrintable(reporter));
        iniKeys.insert(ini);
        reporterKeys.insert(reporter);
    }
    Q_ASSERT(!table.empty());
    return table;
}
} // namespace

const std::vector<ReporterSwitch> &reporterSwitches() {
    static const std::vector<ReporterSwitch> table = buildSwitches();
    return table;
}

const ReporterSwitch &jellyfinSwitch() { return kJellyfinRow; }

QHash<QString, bool> readReporterSwitchValues(const QString &configFile) {
    std::optional<QSettings> ini;
    if (!configFile.isEmpty())
        ini.emplace(configFile, QSettings::IniFormat);
    QHash<QString, bool> values;
    for (const ReporterSwitch &row : reporterSwitches()) {
        const bool enabled = ini ? ini->value(QLatin1String(row.iniKey), row.defaultValue).toBool()
                                 : row.defaultValue; // ini 不可用即整行落兜底
        values.insert(QLatin1String(row.reporterKey), enabled);
    }
    return values;
}

bool anyEnabled(const QHash<QString, bool> &values, const std::vector<QString> &registeredKeys) {
    for (const bool v : values)
        if (v)
            return true;
    // 未登记的键按启用处理：表外还有在册键即算开（第二家报告者不进设置目录、
    // 不在 values 里的那种，不得被别家键的关闭一起判成"全关"）
    for (const QString &key : registeredKeys)
        if (!values.contains(key))
            return true;
    return false;
}

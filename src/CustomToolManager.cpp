#include "pch.h"

#include "CustomToolManager.h"

#include "Logging.h"

CustomToolManager::CustomToolManager(const QString &configFile, QObject *parent)
    : QObject(parent), m_configFile(configFile) {}

void CustomToolManager::load() {
    m_tools.clear();
    if (m_configFile.isEmpty())
        return;
    QSettings s(m_configFile, QSettings::IniFormat);
    const int size = s.value(QStringLiteral("Tools/size"), 0).toInt();
    for (int i = 0; i < size; ++i) {
        const QString bp = QStringLiteral("Tools/%1").arg(i);
        CustomTool tool;
        tool.name = s.value(bp + QStringLiteral("/name")).toString().trimmed();
        tool.command = s.value(bp + QStringLiteral("/command")).toString().trimmed();
        tool.note = s.value(bp + QStringLiteral("/note")).toString(); // 原样保留，不 trim
        // 手改坏的数据不参与菜单
        if (tool.command.isEmpty() || pathSegments(tool.name).isEmpty()) {
            OXBOW_LOG_WARNING() << "custom tool entry" << i
                                << "skipped: empty command or invalid name path";
            continue;
        }
        m_tools.append(tool);
    }
}

bool CustomToolManager::replaceTools(QList<CustomTool> tools) {
    m_tools = std::move(tools);
    return save();
}

QStringList CustomToolManager::pathSegments(const QString &name) {
    QStringList out;
    for (const QString &seg : name.split(u'>'))
        out.append(seg.trimmed());
    out.removeAll(QString());
    return out;
}

bool CustomToolManager::save() const {
    if (m_configFile.isEmpty())
        return true;
    QSettings s(m_configFile, QSettings::IniFormat);
    s.remove(QStringLiteral("Tools"));
    s.setValue(QStringLiteral("Tools/size"), m_tools.size());
    for (int i = 0; i < m_tools.size(); ++i) {
        const QString bp = QStringLiteral("Tools/%1").arg(i);
        s.setValue(bp + QStringLiteral("/name"), m_tools[i].name);
        s.setValue(bp + QStringLiteral("/command"), m_tools[i].command);
        s.setValue(bp + QStringLiteral("/note"), m_tools[i].note);
    }
    s.sync();
    return s.status() == QSettings::NoError;
}

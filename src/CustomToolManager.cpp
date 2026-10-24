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

QString CustomToolManager::joinPath(const QStringList &segments) {
    return segments.join(QStringLiteral(" > "));
}

QString CustomToolManager::normalizedPath(const QString &name) {
    return joinPath(pathSegments(name));
}

// N134：确定语义的唯一收口。占位条目（名称与命令皆空）是等待输入的形态、根本不进
// 菜单，故既不判违规也不参与前缀比较——否则它的空路径是任何路径的严格前缀，会把
// 一整表有效工具都拦成"既是工具又是子菜单"
CustomToolManager::Sanitized CustomToolManager::sanitize(const QList<CustomTool> &tools) {
    struct Kept {
        int row;
        CustomTool tool;
        QStringList segments;
    };
    QList<Kept> kept;
    kept.reserve(tools.size());
    for (int i = 0; i < tools.size(); ++i) {
        const QStringList segments = pathSegments(tools[i].name);
        const CustomTool tool{joinPath(segments), tools[i].command.trimmed(), tools[i].note};
        const bool nameEmpty = tool.name.isEmpty();
        const bool commandEmpty = tool.command.isEmpty();
        if (nameEmpty && commandEmpty)
            continue;
        if (nameEmpty || commandEmpty) {
            return {.problem = nameEmpty ? Problem::MissingName : Problem::MissingCommand,
                    .row = i};
        }
        kept.append({i, tool, segments});
    }

    // 前缀冲突按路径段逐段比（不是字符串前缀），报短路径那一行
    for (const Kept &prefix : kept) {
        for (const Kept &other : kept) {
            if (other.segments.size() > prefix.segments.size() &&
                other.segments.mid(0, prefix.segments.size()) == prefix.segments)
                return {.problem = Problem::PrefixConflict,
                        .row = prefix.row,
                        .path = prefix.tool.name};
        }
    }

    Sanitized ok;
    ok.tools.reserve(kept.size());
    for (const Kept &item : kept)
        ok.tools.append(item.tool);
    return ok;
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

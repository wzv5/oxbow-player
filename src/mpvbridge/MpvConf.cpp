#include "pch.h"

#include "MpvConf.h"

std::optional<MpvConf> MpvConf::open(const QString &path) {
    MpvConf c;
    c.m_path = path;

    // 文件存在 → parse；不存在 → 保持空 m_lines（commit 时创建）
    if (QFileInfo::exists(path)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return std::nullopt;
        c.parse();
    }
    return c;
}

void MpvConf::parse() {
    m_lines.clear();
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QString content = QString::fromUtf8(f.readAll());
    // 去 BOM（UTF-8 EF BB BF）
    if (content.startsWith(QChar(0xFEFF)))
        content.remove(0, 1);

    QString currentSection;
    const QStringList rawLines = content.split('\n');
    // 预编译 key=value 正则：^(\s*)([\w][\w\-.]*)(\s*)=(\s*)(.*)$
    static const QRegularExpression kvRe(QStringLiteral(R"(^(\s*)([\w][\w\-.]*)(\s*)=(\s*)(.*)$)"));
    for (const QString &lineUnstripped : rawLines) {
        QString line = lineUnstripped;
        // 去行尾 \r
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);

        const QString t = line.trimmed();
        MpvConfLine ml;
        ml.raw = line;

        if (t.isEmpty()) {
            ml.kind = MpvConfLine::Kind::Empty;
        } else if (t.startsWith(QLatin1Char('#')) || t.startsWith(QLatin1Char(';'))) {
            ml.kind = MpvConfLine::Kind::Comment;
        } else if (t.startsWith(QLatin1Char('[')) && t.endsWith(QLatin1Char(']'))) {
            ml.kind = MpvConfLine::Kind::Section;
            ml.name = t.mid(1, t.size() - 2);
            currentSection = ml.name;
        } else {
            QRegularExpressionMatch m = kvRe.match(line);
            if (m.hasMatch()) {
                ml.kind = MpvConfLine::Kind::KeyValue;
                ml.section = currentSection;
                ml.key = m.captured(2);
                ml.value = m.captured(5);
            } else {
                // 非法行：按 Comment 兜底保留（不丢）
                ml.kind = MpvConfLine::Kind::Comment;
            }
        }
        m_lines.append(ml);
    }
    // trim 末尾 Empty 行：原文件末尾换行与否决定了是否产生空尾项，
    // 全部 trim 掉后 serialize 不会额外输出空行（round-trip 行数一致）
    while (!m_lines.isEmpty() && m_lines.last().kind == MpvConfLine::Kind::Empty)
        m_lines.removeLast();
}

QString MpvConf::serialize() const {
    QString out;
    for (const auto &line : m_lines) {
        if (line.kind == MpvConfLine::Kind::KeyValue)
            out += line.key + QLatin1Char('=') + line.value;
        else
            out += line.raw;
        out += QLatin1Char('\n');
    }
    return out;
}

QString MpvConf::getGlobal(const QString &key, const QString &defaultValue) const {
    for (const auto &line : m_lines) {
        if (line.kind == MpvConfLine::Kind::KeyValue && line.section.isEmpty() && line.key == key)
            return line.value;
    }
    return defaultValue;
}

QStringList MpvConf::globalKeys() const {
    QStringList result;
    for (const auto &line : m_lines) {
        if (line.kind == MpvConfLine::Kind::KeyValue && line.section.isEmpty() &&
            !result.contains(line.key))
            result.append(line.key);
    }
    return result;
}

QString MpvConf::get(const QString &section, const QString &key,
                     const QString &defaultValue) const {
    for (const auto &line : m_lines) {
        if (line.kind == MpvConfLine::Kind::KeyValue && line.section == section && line.key == key)
            return line.value;
    }
    return defaultValue;
}

QStringList MpvConf::keys(const QString &section) const {
    QStringList result;
    for (const auto &line : m_lines) {
        if (line.kind == MpvConfLine::Kind::KeyValue && line.section == section &&
            !result.contains(line.key))
            result.append(line.key);
    }
    return result;
}

bool MpvConf::setGlobal(const QString &key, const QString &value) {
    return applyChange(QString(), key, value, false);
}

void MpvConf::set(const QString &section, const QString &key, const QString &value) {
    applyChange(section, key, value, false);
}

bool MpvConf::removeGlobal(const QString &key) {
    return applyChange(QString(), key, QString(), true);
}

void MpvConf::remove(const QString &section, const QString &key) {
    applyChange(section, key, QString(), true);
}

bool MpvConf::applyChange(const QString &section, const QString &key, const QString &value,
                          bool isRemove) {
    bool changed = false;
    bool found = false; // set 语义下区分"键不存在（须追加）"与"键存在但同值（不动）"
    for (int i = 0; i < m_lines.size(); ++i) {
        auto &line = m_lines[i];
        if (line.kind != MpvConfLine::Kind::KeyValue)
            continue;
        if (line.key != key || line.section != section)
            continue;
        if (isRemove) {
            // 重复顶层 key 全部删除（不 break）
            m_lines.removeAt(i);
            --i;
            changed = true;
        } else {
            found = true;
            // 同值短路：现值与待写值相同则不记变更（调用方据此免空 commit）
            if (line.value != value) {
                line.value = value;
                changed = true;
            }
            break; // set 仅落首条（既有语义）
        }
    }
    if (!found && !isRemove) {
        MpvConfLine kv;
        kv.kind = MpvConfLine::Kind::KeyValue;
        kv.section = section;
        kv.key = key;
        kv.value = value;
        kv.raw = key + QLatin1Char('=') + value;
        if (section.isEmpty()) {
            // 顶层裸 key：append 末尾
            m_lines.append(kv);
        } else {
            // 找 section 末尾插入
            int sectionHeaderIdx = -1;
            int lastKvInSection = -1;
            for (int i = 0; i < m_lines.size(); ++i) {
                if (m_lines[i].kind == MpvConfLine::Kind::Section && m_lines[i].name == section)
                    sectionHeaderIdx = i;
                if (m_lines[i].section == section && m_lines[i].kind == MpvConfLine::Kind::KeyValue)
                    lastKvInSection = i;
            }
            if (lastKvInSection >= 0) {
                m_lines.insert(lastKvInSection + 1, kv);
            } else if (sectionHeaderIdx >= 0) {
                m_lines.insert(sectionHeaderIdx + 1, kv);
            } else {
                // section 不存在 → 追加 section 头 + kv
                MpvConfLine sec;
                sec.kind = MpvConfLine::Kind::Section;
                sec.name = section;
                sec.raw = QLatin1Char('[') + section + QLatin1Char(']');
                m_lines.append(sec);
                m_lines.append(kv);
            }
        }
        changed = true;
    }
    return changed;
}

bool MpvConf::commit() {
    if (m_path.isEmpty())
        return false;
    QSaveFile sf(m_path);
    // 不加 QIODevice::Text：serialize() 已经处理过 \r 剥离，强制 \n 输出；
    // 加 Text 会让 Qt 自动翻译 \n → \r\n，破坏 mpv.conf 的 LF 格式
    if (!sf.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = serialize().toUtf8();
    if (sf.write(data) != data.size()) {
        sf.cancelWriting();
        return false;
    }
    return sf.commit();
}
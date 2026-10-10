#include "pch.h"

#include "InputConf.h"

namespace {

// 引号（双引号串参数，\ 转义）之外首个 '#' 的下标——手册「input.conf syntax」：
// "# starts a comment (outside of quoted string arguments)"。无则 -1。
// 引号内的 '#' 按原文保留在命令里（mpv 解析时自行剥走尾部注释）。
qsizetype findCommentStart(const QString &s) {
    bool inQuote = false;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (inQuote && c == QLatin1Char('\\')) {
            ++i; // 跳过转义字符
            continue;
        }
        if (c == QLatin1Char('"'))
            inQuote = !inQuote;
        else if (c == QLatin1Char('#') && !inQuote)
            return i;
    }
    return -1;
}

// default-bindings start 指令（变更记录 3：仅认 start；stop 等不存在该语法，
// 落 KeyValue 按解不出键名的行显示）。要求后随边界（串尾 / 空白），防 "startX" 误中。
bool isDefaultBindingsStart(const QString &body) {
    const QLatin1String prefix("default-bindings start");
    if (!body.startsWith(prefix))
        return false;
    return body.size() == prefix.size() || body.at(prefix.size()).isSpace();
}

// 正文切分：引号外首个 '#' 起为行末注释；其左首 token 为键、其余整体为命令
// （value 是命令串整体，不得按首个 token 截断）。
struct BodySplit {
    QString key;
    QString cmdRest; // 命令部分（未整行 trim；字段赋值时再收边）
    QString tail;    // '#' 之后的原文（无 '#' 则空）
};
BodySplit splitBody(const QString &body) {
    BodySplit r;
    const qsizetype hash = findCommentStart(body);
    const QString main = hash >= 0 ? body.left(hash) : body;
    if (hash >= 0)
        r.tail = body.mid(hash + 1);
    qsizetype i = 0;
    while (i < main.size() && !main.at(i).isSpace())
        ++i;
    r.key = main.left(i);
    while (i < main.size() && main.at(i).isSpace())
        ++i;
    r.cmdRest = main.mid(i);
    return r;
}

QString cleanComment(const QString &tail) {
    QString c = tail;
    while (!c.isEmpty() && c.front().isSpace())
        c.removeFirst();
    while (!c.isEmpty() && (c.back().isSpace() || c.back() == QLatin1Char('\r')))
        c.chop(1);
    return c;
}

// 按字段重组该行（唯一的重组路径：setLineFields 与 appendLine 都经它，与 disableLine
// 的插桩同形，保证写出与认领判据互逆）。cr 带上原行的行尾 \r——重组只动字段，
// 不把 CRLF 的行换成 LF。
QString assembleFields(const InputConfLine &l, bool cr) {
    QString body = l.key;
    if (!l.command.isEmpty())
        body += QLatin1Char(' ') + l.command; // 命令为空时不留孤零零的空格
    if (!l.comment.isEmpty())
        body += QStringLiteral(" # ") + l.comment;
    return l.indent + (l.disabled ? QStringLiteral("# ") : QString()) + body +
           (cr ? QStringLiteral("\r") : QString());
}

// 原行是否 CRLF 收尾（\r 留在 raw 里，随重组带走）
bool hasCr(const QString &raw) { return raw.endsWith(QLatin1Char('\r')); }

} // namespace

std::optional<InputConf> InputConf::open(const QString &path, const KeyValidator &validator) {
    InputConf c;
    c.m_path = path;
    c.m_validator = validator;
    if (!QFileInfo::exists(path))
        return c; // 不存在 → 空 lines，commit 时创建
    if (!c.parse())
        return std::nullopt; // 存在但读不到：绝不当空文件用，否则 commit 会盖掉原内容
    return c;
}

bool InputConf::parse() {
    m_lines.clear();
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QString content = QString::fromUtf8(f.readAll());
    // 去 BOM（UTF-8 EF BB BF），与 MpvConf 同口径：不回写
    if (content.startsWith(QChar(0xFEFF)))
        content.remove(0, 1);

    m_trailingNewline = m_originalTrailingNewline = content.endsWith(QLatin1Char('\n'));
    const QStringList rawLines = content.split(QLatin1Char('\n'));
    const qsizetype count = m_trailingNewline ? rawLines.size() - 1 : rawLines.size();
    for (qsizetype i = 0; i < count; ++i) {
        InputConfLine l;
        l.raw = rawLines.at(i);
        if (hasCr(l.raw))
            m_crlf = true; // 新增行随文件既有形态
        classify(l);
        m_lines.append(l);
    }
    return true;
}

void InputConf::classify(InputConfLine &l) const {
    const QString t = l.raw.trimmed();
    if (t.isEmpty()) {
        l.kind = InputConfLine::Kind::Empty;
        return;
    }

    // 注释行：先试认领（被禁用的绑定行，mpv 对行首 # 整行不解析——附录 C 第 1 条）
    if (t.startsWith(QLatin1Char('#'))) {
        if (claimDisabled(l.raw, &l)) {
            l.kind = InputConfLine::Kind::KeyValue;
            return;
        }
        l.kind = InputConfLine::Kind::Comment;
        return;
    }

    // 行首缩进（活动行同样保留：禁用插桩要求 "#" 落在缩进之后，启用时正文左移回原位）
    qsizetype i = 0;
    while (i < l.raw.size() &&
           (l.raw.at(i) == QLatin1Char(' ') || l.raw.at(i) == QLatin1Char('\t')))
        ++i;
    l.indent = l.raw.left(i);
    const QString body = l.raw.mid(i);

    // Directive 两类：default-bindings start / <键> {区名} <命令>。
    // 判据逐字照抄 mpv parse_config（附录 C）：{ 区名只在键之后的命令段被解释。
    if (isDefaultBindingsStart(body)) {
        l.kind = InputConfLine::Kind::Directive;
        return;
    }

    const BodySplit split = splitBody(body);
    if (split.cmdRest.startsWith(QLatin1Char('{'))) {
        l.kind = InputConfLine::Kind::Directive;
        return; // 不显示、不填充字段，仅原样回写
    }

    // 键名解析不出来的行（foo bar、default-bindings stop）照常显示、不判定：
    // 键列由对话框反解后留空，原文留在数据层
    l.kind = InputConfLine::Kind::KeyValue;
    l.disabled = false;
    l.key = split.key;
    l.command = split.cmdRest.trimmed();
    l.comment = cleanComment(split.tail);
}

bool InputConf::claimDisabled(const QString &raw, InputConfLine *line) const {
    // 缩进
    qsizetype i = 0;
    while (i < raw.size() && (raw.at(i) == QLatin1Char(' ') || raw.at(i) == QLatin1Char('\t')))
        ++i;
    if (i >= raw.size() || raw.at(i) != QLatin1Char('#'))
        return false;
    const QString indent = raw.left(i);
    // 剥 "#" 与其后紧随的一段连续空白（"#f ignore"、"#  f ignore" 等不规整形态同样命中）
    ++i;
    while (i < raw.size() && raw.at(i).isSpace())
        ++i;
    const QString rest = raw.mid(i);

    // 不认领：剥完仍是 "#" 开头（## 形态）、空串
    if (rest.isEmpty() || rest.startsWith(QLatin1Char('#')))
        return false;
    // 不认领：两类 Directive 的注释形态（#default-bindings start、#{encode} quit）
    if (isDefaultBindingsStart(rest))
        return false;
    const BodySplit split = splitBody(rest);
    if (split.cmdRest.startsWith(QLatin1Char('{')))
        return false;
    // 不认领：解不出 <键> <命令>（命令非空）或键名不合法（变更记录 2：
    // 合法性经注入的判定器，即 MpvKeyName 反解）
    if (split.key.isEmpty() || split.cmdRest.trimmed().isEmpty())
        return false;
    if (!m_validator || !m_validator(split.key))
        return false;

    line->kind = InputConfLine::Kind::KeyValue;
    line->disabled = true;
    line->indent = indent;
    line->key = split.key;
    line->command = split.cmdRest.trimmed();
    line->comment = cleanComment(split.tail);
    return true;
}

void InputConf::disableLine(int index) {
    InputConfLine &l = m_lines[index];
    Q_ASSERT(l.kind == InputConfLine::Kind::KeyValue && !l.disabled);
    // 写出：缩进之后插入固定的 "# "（"#" + 恰好一个空格），其后正文一字节不改
    l.raw = l.indent + QStringLiteral("# ") + l.raw.mid(l.indent.size());
    l.disabled = true;
    // 互逆断言：键名合法的行，认领必须原样还原这组字段。键名反解不出的行本就不被
    // 认领（变更记录 2 的既定代价：禁用后重开不显示、文件原样留着），不在此断言范围
    if (m_validator && m_validator(l.key)) {
        InputConfLine check;
        Q_ASSERT(claimDisabled(l.raw, &check) && check.indent == l.indent && check.key == l.key &&
                 check.command == l.command && check.comment == l.comment);
    }
}

void InputConf::enableLine(int index) {
    InputConfLine &l = m_lines[index];
    Q_ASSERT(l.kind == InputConfLine::Kind::KeyValue && l.disabled);
    // 启用：删掉认领时实际剥掉的那一段（缩进后的 "#" 与其后一段连续空白），
    // 正文左移到原缩进之后——与认领严格互逆
    qsizetype i = l.indent.size();
    Q_ASSERT(i < l.raw.size() && l.raw.at(i) == QLatin1Char('#'));
    ++i;
    while (i < l.raw.size() && l.raw.at(i).isSpace())
        ++i;
    l.raw = l.indent + l.raw.mid(i);
    l.disabled = false;
}

void InputConf::removeLine(int index) {
    m_lines.removeAt(index);
    // 删掉的正是末行时，末行终止符回到原文件形态——增删往返是净零操作，不得留下换行
    if (index == m_lines.size())
        m_trailingNewline = m_originalTrailingNewline;
}

void InputConf::setLineFields(int index, const QString &key, const QString &command,
                              const QString &comment) {
    InputConfLine &l = m_lines[index];
    Q_ASSERT(l.kind == InputConfLine::Kind::KeyValue);
    const bool cr = hasCr(l.raw); // 行尾 \r 属该行自身，重组后原样带走
    l.key = key;
    l.command = command;
    l.comment = comment;
    l.raw = assembleFields(l, cr);
}

int InputConf::appendLine(const QString &key, const QString &command, const QString &comment) {
    InputConfLine l;
    l.kind = InputConfLine::Kind::KeyValue;
    l.disabled = false;
    l.indent = QString();
    l.key = key;
    l.command = command;
    l.comment = comment;
    l.raw = assembleFields(l, m_crlf); // 新行随文件既有换行形；空文件按 LF
    m_lines.append(l);
    // 有了新末行，文件末尾就该正常终止；他行的行尾符由 serialize 按位置现算，不受影响
    m_trailingNewline = true;
    return m_lines.size() - 1;
}

QString InputConf::serialize() const {
    QString out;
    for (qsizetype i = 0; i < m_lines.size(); ++i) {
        out += m_lines.at(i).raw;
        // 只有末行的换行符可缺（无末尾换行的文件原样再现），其余一律带行间符
        if (i + 1 < m_lines.size() || m_trailingNewline)
            out += QLatin1Char('\n');
    }
    return out;
}

bool InputConf::commit() {
    if (m_path.isEmpty())
        return false;
    QSaveFile sf(m_path);
    // 不加 QIODevice::Text：CRLF 保真靠 raw 原样携带 \r（与 MpvConf 的强制 LF 不同——
    // 本类按规格要求逐行保真，不统一换行符）
    if (!sf.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = serialize().toUtf8();
    if (sf.write(data) != data.size()) {
        sf.cancelWriting();
        return false;
    }
    return sf.commit();
}

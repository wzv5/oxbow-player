#include "pch.h"

#include "ui/MaterialIconCatalog.h"

#include "Logging.h"

namespace {

// 工作量与集合规模上限：损坏字节会让嵌套计数相乘爆量，超限即判失败
constexpr qsizetype kMaxWork = 20 * 1000 * 1000;
constexpr qsizetype kMaxGlyphMap = 1 << 20;
constexpr qsizetype kMaxCoverage = 1 << 20;

// 大端读取，越界即置失败标记（后续读数恒 0），由 ok() 在循环与收口处把关
class BigEndianReader {
  public:
    explicit BigEndianReader(const QByteArray &data) : m_data(data) {}

    bool ok() const { return m_ok; }
    void spend(qsizetype steps = 1) {
        m_spent += steps;
        if (m_spent > kMaxWork)
            m_ok = false;
    }

    quint16 u16(qsizetype offset) {
        if (!m_ok || offset < 0 || offset + 2 > m_data.size()) {
            m_ok = false;
            return 0;
        }
        const auto *p = reinterpret_cast<const uchar *>(m_data.constData()) + offset;
        return quint16(p[0] << 8) | p[1];
    }

    quint32 u32(qsizetype offset) {
        if (!m_ok || offset < 0 || offset + 4 > m_data.size()) {
            m_ok = false;
            return 0;
        }
        const auto *p = reinterpret_cast<const uchar *>(m_data.constData()) + offset;
        return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3];
    }

    // 表内相对跳转：目标偏移必须落在数据内
    qsizetype follow(qsizetype base, quint32 relative) {
        const qsizetype target = base + qsizetype(relative);
        if (!m_ok || target < 0 || target >= m_data.size()) {
            m_ok = false;
            return 0;
        }
        return target;
    }

    // 4 字节表标签（"cmap" / "GSUB" / "rlig"）
    QByteArray tag(qsizetype offset) {
        if (!m_ok || offset < 0 || offset + 4 > m_data.size()) {
            m_ok = false;
            return QByteArray();
        }
        return QByteArray::fromRawData(m_data.constData() + offset, 4);
    }

  private:
    const QByteArray &m_data;
    bool m_ok = true;
    qsizetype m_spent = 0;
};

// Coverage（format 1 / 2）：首字形集合
QList<quint32> coverageGlyphs(BigEndianReader &r, qsizetype coverage) {
    QList<quint32> firsts;
    if (!r.ok())
        return firsts;
    if (r.u16(coverage) == 1) {
        const quint16 count = r.u16(coverage + 2);
        for (quint16 i = 0; r.ok() && i < count; ++i) {
            r.spend();
            firsts.append(r.u16(coverage + 4 + 2 * qsizetype(i)));
        }
    } else {
        const quint16 rangeCount = r.u16(coverage + 2);
        for (quint16 i = 0; r.ok() && i < rangeCount && firsts.size() <= kMaxCoverage; ++i) {
            r.spend();
            const qsizetype rec = coverage + 4 + 6 * qsizetype(i);
            const quint32 first = r.u16(rec);
            const quint32 last = r.u16(rec + 2);
            for (quint32 g = first; r.ok() && g <= last && firsts.size() <= kMaxCoverage; ++g) {
                r.spend();
                firsts.append(g);
            }
        }
    }
    return firsts;
}

// LigatureSubstitution（type 4）子表：Coverage 首字形 + Ligature 其余 component glyph
// 反查拼成连字名；component 查不到字符的整条丢弃
void collectLigatures(BigEndianReader &r, qsizetype subtable,
                      const QHash<quint32, QString> &glyphToChar, QSet<QString> &out) {
    const QList<quint32> firsts = coverageGlyphs(r, r.follow(subtable, r.u16(subtable + 2)));
    if (!r.ok() || firsts.isEmpty() || firsts.size() > kMaxCoverage)
        return;
    const quint16 ligSetCount = r.u16(subtable + 4);
    // ligSet 与 Coverage 逐位对应：第 s 组首字形即 firsts[s]
    for (quint16 s = 0; r.ok() && s < ligSetCount && s < firsts.size(); ++s) {
        r.spend();
        const qsizetype ligSet = r.follow(subtable, r.u16(subtable + 6 + 2 * qsizetype(s)));
        const quint16 ligCount = r.u16(ligSet);
        for (quint16 q = 0; r.ok() && q < ligCount; ++q) {
            r.spend(4);
            const qsizetype lig = r.follow(ligSet, r.u16(ligSet + 2 + 2 * qsizetype(q)));
            const quint16 compCount = r.u16(lig + 2);
            QList<QString> parts;
            parts.append(glyphToChar.value(firsts[s]));
            for (quint16 x = 0; r.ok() && x + 1 < compCount; ++x) {
                r.spend();
                parts.append(glyphToChar.value(r.u16(lig + 4 + 2 * qsizetype(x))));
            }
            bool complete = r.ok();
            for (const QString &part : parts)
                complete = complete && !part.isEmpty();
            if (complete)
                out.insert(parts.join(QString()));
        }
    }
}

} // namespace

namespace MaterialIconCatalog {

QString family() {
    // 字体加载单点：进程内加一次，family 从 QFontDatabase 取实际值；资源缺失返回空串
    static const QString cached =
        [] {
            const int id = QFontDatabase::addApplicationFont(
                QStringLiteral(":/fonts/MaterialSymbolsRounded_Filled-Regular.ttf"));
            const QStringList families =
                id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
            if (families.isEmpty()) {
                OXBOW_LOG_WARNING()
                    << "icon font resource missing or unusable, icons render as error glyph";
                return QString();
            }
            return families.first();
        }();
    return cached;
}

const QStringList &names() {
    // 失败结果同样缓存，进程内不重试
    static const QStringList cached = [] {
        QFile font(QStringLiteral(":/fonts/MaterialSymbolsRounded_Filled-Regular.ttf"));
        if (!font.open(QIODevice::ReadOnly))
            return QStringList();
        return parseFontLigatures(font.readAll());
    }();
    return cached;
}

bool contains(const QString &name) {
    // 查表集与清单同源，不出现两份漂移
    static const QSet<QString> cached = [] {
        QSet<QString> set;
        set.reserve(names().size());
        for (const QString &name : names())
            set.insert(name);
        return set;
    }();
    return cached.contains(name);
}

QStringList parseFontLigatures(const QByteArray &data) {
    BigEndianReader r(data);

    const quint16 numTables = r.u16(4);
    qsizetype cmapOff = -1;
    qsizetype gsubOff = -1;
    for (quint16 i = 0; r.ok() && i < numTables; ++i) {
        r.spend();
        const qsizetype rec = 12 + 16 * qsizetype(i);
        if (r.tag(rec) == "cmap")
            cmapOff = r.follow(0, r.u32(rec + 8));
        else if (r.tag(rec) == "GSUB")
            gsubOff = r.follow(0, r.u32(rec + 8));
    }
    if (!r.ok() || cmapOff < 0 || gsubOff < 0)
        return {};

    // cmap format 12（platform 3, encoding 10）glyph → 字符反查；ASCII 大写归一为小写，
    // 因 cmap 把 A–Z 与 a–z 映射到同一字形且按码点升序，反查先撞大写
    const quint16 cmapCount = r.u16(cmapOff + 2);
    qsizetype sub12 = -1;
    for (quint16 i = 0; r.ok() && i < cmapCount; ++i) {
        const qsizetype rec = cmapOff + 4 + 8 * qsizetype(i);
        if (r.u16(rec) == 3 && r.u16(rec + 2) == 10)
            sub12 = r.follow(cmapOff, r.u32(rec + 4));
    }
    if (!r.ok() || sub12 < 0)
        return {};
    QHash<quint32, QString> glyphToChar;
    const quint32 numGroups = r.u32(sub12 + 12);
    for (quint32 g = 0; r.ok() && g < numGroups && glyphToChar.size() <= kMaxGlyphMap; ++g) {
        r.spend();
        const qsizetype rec = sub12 + 16 + 12 * qsizetype(g);
        const quint32 start = r.u32(rec);
        const quint32 end = r.u32(rec + 4);
        const quint32 firstGlyph = r.u32(rec + 8);
        if (end < start || end > 0x10FFFF)
            continue;
        for (quint32 cp = start; r.ok() && cp <= end; ++cp) {
            r.spend();
            const char32_t codepoint = char32_t(cp);
            const QString ch =
                (cp >= 65 && cp <= 90) ? QString(QChar(cp + 32)) : QString::fromUcs4(&codepoint, 1);
            // 同字形多码点时保留码点靠前者
            const quint32 glyph = firstGlyph + cp - start;
            if (!glyphToChar.contains(glyph))
                glyphToChar.insert(glyph, ch);
        }
    }
    if (!r.ok() || glyphToChar.isEmpty())
        return {};

    // rlig → LookupList → Extension(type 7) 解包为 LigatureSubstitution(type 4)；
    // rclt 单替换不合并字形、不参与
    const qsizetype featureList = r.follow(gsubOff, r.u16(gsubOff + 6));
    const qsizetype lookupList = r.follow(gsubOff, r.u16(gsubOff + 8));
    if (!r.ok())
        return {};
    QSet<QString> names;
    const quint16 featureCount = r.u16(featureList);
    for (quint16 i = 0; r.ok() && i < featureCount; ++i) {
        r.spend();
        const qsizetype rec = featureList + 2 + 6 * qsizetype(i);
        if (r.tag(rec) != "rlig")
            continue;
        const qsizetype feature = r.follow(featureList, r.u16(rec + 4));
        const quint16 lookupCount = r.u16(feature + 2);
        for (quint16 j = 0; r.ok() && j < lookupCount; ++j) {
            r.spend();
            // rlig 经 Feature 的 LookupListIndices 引用 LookupList，序号不保证从 0 连续
            const quint16 lookupIndex = r.u16(feature + 4 + 2 * qsizetype(j));
            const qsizetype lookup =
                r.follow(lookupList, r.u16(lookupList + 2 + 2 * qsizetype(lookupIndex)));
            const quint16 lookupType = r.u16(lookup);
            const quint16 subTableCount = r.u16(lookup + 4);
            for (quint16 k = 0; r.ok() && k < subTableCount; ++k) {
                r.spend();
                const qsizetype subtable = r.follow(lookup, r.u16(lookup + 6 + 2 * qsizetype(k)));
                quint16 type = lookupType;
                qsizetype actual = subtable;
                if (lookupType == 7) { // ExtensionSubstitution：真实类型与子表在内层
                    type = r.u16(subtable + 2);
                    actual = r.follow(subtable, r.u32(subtable + 4));
                }
                if (r.ok() && type == 4)
                    collectLigatures(r, actual, glyphToChar, names);
            }
        }
    }
    if (!r.ok())
        return {};
    QStringList list(names.cbegin(), names.cend());
    std::sort(list.begin(), list.end());
    return list;
}

} // namespace MaterialIconCatalog

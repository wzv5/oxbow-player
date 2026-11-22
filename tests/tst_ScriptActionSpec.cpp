// ScriptActionSpec 单元测试（N173）；一类一可执行，<槽名> 跑单个用例

#include "pch.h"

#include <QtTest/QtTest>

#include "ui/ScriptActionSpec.h"

using namespace ScriptActionSpec;

namespace {

// 七字段表序：command / callback / icon / tooltip / checked / disabled / hidden
constexpr int kFieldCount = 7;

QString fieldName(int i) {
    static constexpr QLatin1String kNames[] = {QLatin1String("command"), QLatin1String("callback"),
                                               QLatin1String("icon"),    QLatin1String("tooltip"),
                                               QLatin1String("checked"), QLatin1String("disabled"),
                                               QLatin1String("hidden")};
    return QString(kNames[i]);
}

// 补丁里的新值：字符串族写 "new-<字段名>"，布尔族一律 false（base 全为 true，可分辨）
QJsonValue newValue(int i) {
    return i < 4 ? QJsonValue(QStringLiteral("new-") + QString(fieldName(i))) : QJsonValue(false);
}

QString newValueText(int i) { return i < 4 ? newValue(i).toString() : QStringLiteral("0"); }

// 字段值统一成串比较，bool 与 QString 两族走同一条断言路径
QString fieldValue(const ScriptAction &a, int i) {
    switch (i) {
    case 0:
        return a.command;
    case 1:
        return a.callback;
    case 2:
        return a.icon;
    case 3:
        return a.tooltip;
    case 4:
        return a.checked ? QStringLiteral("1") : QStringLiteral("0");
    case 5:
        return a.disabled ? QStringLiteral("1") : QStringLiteral("0");
    default:
        return a.hidden ? QStringLiteral("1") : QStringLiteral("0");
    }
}

} // namespace

class TestScriptActionSpec : public QObject {
    Q_OBJECT

  private slots:
    void parsesAllFields();
    void tooltipEmptyStringIsEngaged();
    void badFieldTypesDoNotDropAction();
    void applyPatchKeepsUnsetFields();
    void applyPatchOnDefaultFillsDefaults();
    void rejectsBadJson();
    void boundIdRecognition();
    void idValidation();
    void commandTextRoundTrip();

  private:
    static QByteArray fullJson();
    static ScriptAction nonDefaultBase();
};

QByteArray TestScriptActionSpec::fullJson() {
    return QByteArray(R"({"command":"screenshot","callback":"tool-picked",)"
                      R"("icon":"play_arrow","tooltip":"Take it","checked":true,)"
                      R"("disabled":true,"hidden":true,"extra":"ignored"})");
}

ScriptAction TestScriptActionSpec::nonDefaultBase() {
    ScriptAction base;
    base.id = QStringLiteral("id-a");
    base.owner = QStringLiteral("owner-a");
    base.command = QStringLiteral("base-cmd");
    base.callback = QStringLiteral("base-cb");
    base.icon = QStringLiteral("base-icon");
    base.tooltip = QStringLiteral("base-tip");
    base.checkable = true;
    base.checked = true;
    base.disabled = true;
    base.hidden = true;
    return base;
}

// 七字段各类型正确时逐项入补丁；未知字段忽略
void TestScriptActionSpec::parsesAllFields() {
    const ParseResult result = parse(fullJson());
    QCOMPARE(result.error, ParseError::None);
    QVERIFY(result.patch.command.has_value());
    QCOMPARE(*result.patch.command, QStringLiteral("screenshot"));
    QVERIFY(result.patch.callback.has_value());
    QCOMPARE(*result.patch.callback, QStringLiteral("tool-picked"));
    QVERIFY(result.patch.icon.has_value());
    QCOMPARE(*result.patch.icon, QStringLiteral("play_arrow"));
    QVERIFY(result.patch.tooltip.has_value());
    QCOMPARE(*result.patch.tooltip, QStringLiteral("Take it"));
    QVERIFY(result.patch.checked.has_value());
    QVERIFY(*result.patch.checked);
    QVERIFY(result.patch.disabled.has_value());
    QVERIFY(*result.patch.disabled);
    QVERIFY(result.patch.hidden.has_value());
    QVERIFY(*result.patch.hidden);

    // 空对象合法且补丁全 nullopt
    const ParseResult empty = parse(QByteArray("{}"));
    QCOMPARE(empty.error, ParseError::None);
    QVERIFY(!empty.patch.command.has_value());
    QVERIFY(!empty.patch.callback.has_value());
    QVERIFY(!empty.patch.icon.has_value());
    QVERIFY(!empty.patch.tooltip.has_value());
    QVERIFY(!empty.patch.checked.has_value());
    QVERIFY(!empty.patch.disabled.has_value());
    QVERIFY(!empty.patch.hidden.has_value());
}

void TestScriptActionSpec::tooltipEmptyStringIsEngaged() {
    const ParseResult result = parse(QByteArray(R"({"tooltip":""})"));
    QCOMPARE(result.error, ParseError::None);
    QVERIFY(result.patch.tooltip.has_value());
    QVERIFY(result.patch.tooltip->isEmpty());
}

void TestScriptActionSpec::badFieldTypesDoNotDropAction() {
    const ParseResult result = parse(QByteArray(R"({"command":5,"callback":true,)"
                                                R"("icon":[],"tooltip":null,)"
                                                R"("checked":"x","disabled":0,"hidden":{},)"
                                                R"("command2":"keep-out"})"));
    QCOMPARE(result.error, ParseError::None);
    QVERIFY(!result.patch.command.has_value());
    QVERIFY(!result.patch.callback.has_value());
    QVERIFY(!result.patch.icon.has_value());
    QVERIFY(!result.patch.tooltip.has_value());
    QVERIFY(!result.patch.checked.has_value());
    QVERIFY(!result.patch.disabled.has_value());
    QVERIFY(!result.patch.hidden.has_value());

    const ParseResult mixed = parse(QByteArray(R"({"command":"ok","checked":"x"})"));
    QCOMPARE(mixed.error, ParseError::None);
    QVERIFY(mixed.patch.command.has_value());
    QCOMPARE(*mixed.patch.command, QStringLiteral("ok"));
    QVERIFY(!mixed.patch.checked.has_value());
}

// 局部更新的两条判据逐字段各测一次：缺席字段留 base、在场字段取补丁值
void TestScriptActionSpec::applyPatchKeepsUnsetFields() {
    const ScriptAction base = nonDefaultBase();
    QJsonObject full;
    for (int i = 0; i < kFieldCount; ++i)
        full.insert(fieldName(i), newValue(i));

    for (int missing = 0; missing < kFieldCount; ++missing) {
        // 整段补丁只缺第 missing 个键
        QJsonObject minus = full;
        minus.remove(fieldName(missing));
        const ScriptAction merged = apply(base, parse(QJsonDocument(minus).toJson()).patch);
        for (int i = 0; i < kFieldCount; ++i)
            QCOMPARE(fieldValue(merged, i), i == missing ? fieldValue(base, i) : newValueText(i));

        // 单字段补丁：只动第 missing 个键，其余逐项等于 base
        QJsonObject alone;
        alone.insert(fieldName(missing), newValue(missing));
        const ScriptAction single = apply(base, parse(QJsonDocument(alone).toJson()).patch);
        for (int i = 0; i < kFieldCount; ++i)
            QCOMPARE(fieldValue(single, i), i == missing ? newValueText(i) : fieldValue(base, i));
    }

    // checked 的连带判据在 checkable 为假的 base 上才分得出来
    ScriptAction uncheckable = nonDefaultBase();
    uncheckable.checkable = false;
    QVERIFY(apply(uncheckable, parse(fullJson()).patch).checkable);
    QVERIFY(!apply(uncheckable, parse(QByteArray(R"({"disabled":false})")).patch).checkable);
    // "存在即可勾选"：checked 给 false 同样开 checkable
    const ScriptAction off = apply(uncheckable, parse(QByteArray(R"({"checked":false})")).patch);
    QVERIFY(off.checkable);
    QVERIFY(!off.checked);
}

void TestScriptActionSpec::applyPatchOnDefaultFillsDefaults() {
    const ScriptAction merged = apply(ScriptAction{}, {});
    QVERIFY(merged.id.isEmpty());
    QVERIFY(merged.owner.isEmpty());
    QVERIFY(merged.command.isEmpty());
    QVERIFY(merged.callback.isEmpty());
    QVERIFY(merged.icon.isEmpty());
    QVERIFY(merged.tooltip.isEmpty());
    QVERIFY(!merged.checkable);
    QVERIFY(!merged.checked);
    QVERIFY(!merged.disabled);
    QVERIFY(!merged.hidden);

    const ParseResult parsed = parse(fullJson());
    QCOMPARE(parsed.error, ParseError::None);
    const ScriptAction full = apply(ScriptAction{}, parsed.patch);
    QCOMPARE(full.command, QStringLiteral("screenshot"));
    QCOMPARE(full.callback, QStringLiteral("tool-picked"));
    QCOMPARE(full.icon, QStringLiteral("play_arrow"));
    QCOMPARE(full.tooltip, QStringLiteral("Take it"));
    QVERIFY(full.checked);
    QVERIFY(full.disabled);
    QVERIFY(full.hidden);
    QVERIFY(full.checkable);
}

void TestScriptActionSpec::rejectsBadJson() {
    QCOMPARE(parse(QByteArray("not json")).error, ParseError::BadJson);
    QCOMPARE(parse(QByteArray("[1,2,3]")).error, ParseError::BadJson);
    QCOMPARE(parse(QByteArray("\"str\"")).error, ParseError::BadJson);
    QCOMPARE(parse(QByteArray("")).error, ParseError::BadJson);
    // 语法过而根非对象时解析器报 NoError，detail 由本模块补因
    QCOMPARE(parse(QByteArray("[1,2,3]")).detail, QStringLiteral("root is not an object"));
}

void TestScriptActionSpec::boundIdRecognition() {
    QCOMPARE(boundId(QStringLiteral("@abc")), QStringLiteral("abc"));
    QCOMPARE(boundId(QStringLiteral("@a.b-c_d")), QStringLiteral("a.b-c_d"));
    QCOMPARE(boundId(QStringLiteral("@A9_")), QStringLiteral("A9_"));
    // 入表命令已 trim，@ 后空白判非绑定
    QCOMPARE(boundId(QStringLiteral("@a/b")), QString());
    QCOMPARE(boundId(QStringLiteral(u"@动作")), QString());
    QCOMPARE(boundId(QStringLiteral("@")), QString());
    QCOMPARE(boundId(QStringLiteral("@a b")), QString());
    QCOMPARE(boundId(QStringLiteral("@abc ")), QString());
    QCOMPARE(boundId(QStringLiteral("abc")), QString());
    QCOMPARE(boundId(QStringLiteral("mpv@screenshot")), QString());
    QCOMPARE(boundId(QStringLiteral("@") + QString(65, u'x')), QString());
    QCOMPARE(boundId(QStringLiteral("@") + QString(64, u'x')), QString(64, u'x'));
}

void TestScriptActionSpec::idValidation() {
    QVERIFY(isValidId(QStringLiteral("a")));
    QVERIFY(isValidId(QStringLiteral("a.b-c_d")));
    QVERIFY(isValidId(QStringLiteral("A9")));
    QVERIFY(isValidId(QString(64, u'x')));
    QVERIFY(!isValidId(QString()));
    QVERIFY(!isValidId(QString(65, u'x')));
    QVERIFY(!isValidId(QStringLiteral("a b")));
    QVERIFY(!isValidId(QStringLiteral("a/b")));
    QVERIFY(!isValidId(QStringLiteral(u"动作")));
    QVERIFY(!isValidId(QStringLiteral("a:b")));
}

// N182：生成侧与识别侧互逆；非法 id 拼出的串不被认回
void TestScriptActionSpec::commandTextRoundTrip() {
    const QStringList validIds{QStringLiteral("a"), QStringLiteral("a.b-c_d"),
                               QStringLiteral("A9_"), QString(64, u'x')};
    for (const QString &id : validIds) {
        QCOMPARE(commandText(id), QStringLiteral("@") + id);
        QCOMPARE(boundId(commandText(id)), id);
    }
    QVERIFY(boundId(commandText(QString())).isEmpty());
    QVERIFY(boundId(commandText(QStringLiteral("a/b"))).isEmpty());
    QVERIFY(boundId(commandText(QStringLiteral(u"动作"))).isEmpty());
    QVERIFY(boundId(commandText(QString(65, u'x'))).isEmpty());
}

QTEST_GUILESS_MAIN(TestScriptActionSpec)

#include "tst_ScriptActionSpec.moc"

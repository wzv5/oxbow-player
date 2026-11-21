// ScriptMenuSpec 单元测试（N172）；一类一可执行，<槽名> 跑单个用例

#include "pch.h"

#include <QtTest/QtTest>

#include "ui/ScriptMenuSpec.h"

using namespace ScriptMenuSpec;

namespace {

// 节点身份速查
bool isItem(const Node &node) { return node.kind == NodeKind::Item; }
bool isSeparator(const Node &node) { return node.kind == NodeKind::Separator; }
bool isSubmenu(const Node &node) { return node.kind == NodeKind::Submenu; }

} // namespace

class TestScriptMenuSpec : public QObject {
    Q_OBJECT

  private slots:
    void parsesFlatItems();
    void buildsNestedSubmenus();
    void separatorsByTitle();
    void rejectsInvalidTitles();
    void keepsTitleWhitespace();
    void assignsItemIds();
    void rejectsBadJson();
    void checkableFromPresence();
    void ignoresUnknownFields();
    void menuIdValidation();
};

void TestScriptMenuSpec::parsesFlatItems() {
    const ParseResult result = parse(R"json({
        "items": [
            {"title": "Repeat", "checked": true, "callback": "menu-picked", "id": "repeat"},
            {"title": "Shot", "command": "screenshot"},
            {"title": "Off", "checked": false},
            {"title": "Dim", "disabled": true},
            {"title": "Label", "unknown": {"x": 1}}
        ]
    })json");
    QCOMPARE(result.error, ParseError::None);
    const QList<Node> &items = result.menu.items;
    QCOMPARE(items.size(), 5);
    QVERIFY(isItem(items[0]));
    QCOMPARE(items[0].title, QStringLiteral("Repeat"));
    QVERIFY(items[0].checkable);
    QVERIFY(items[0].checked);
    QCOMPARE(items[0].callback, QStringLiteral("menu-picked"));
    QCOMPARE(items[0].id, QStringLiteral("repeat"));
    QVERIFY(items[0].command.isEmpty());
    QCOMPARE(items[1].command, QStringLiteral("screenshot"));
    QVERIFY(items[1].callback.isEmpty());
    QVERIFY(items[2].checkable);
    QVERIFY(!items[2].checked);
    QVERIFY(items[3].disabled);
    QVERIFY(!items[3].checkable);
    QVERIFY(isItem(items[4]));
    QVERIFY(items[4].command.isEmpty());
    QVERIFY(items[4].callback.isEmpty());
    QVERIFY(!items[4].checkable);
}

void TestScriptMenuSpec::buildsNestedSubmenus() {
    const ParseResult result = parse(R"json({
        "items": [
            {"title": "A", "items": [{"title": "A1"}]},
            {"title": "", "items": [{"title": "x"}]},
            {"items": [{"title": "x"}]},
            {"title": "Bad", "items": "nope"},
            {"title": "Empty", "items": []},
            {"title": "Keep", "items": [{"title": "Empty2", "items": []}, {"title": "Deep"}]}
        ]
    })json");
    QCOMPARE(result.error, ParseError::None);
    const QList<Node> &items = result.menu.items;
    // 空串 / 缺失标题的子菜单与 items 非数组的子菜单整树跳过；空子菜单递归移除
    QCOMPARE(items.size(), 2);
    QVERIFY(isSubmenu(items[0]));
    QCOMPARE(items[0].title, QStringLiteral("A"));
    QCOMPARE(items[0].children.size(), 1);
    QCOMPARE(items[0].children[0].title, QStringLiteral("A1"));
    QVERIFY(isSubmenu(items[1]));
    QCOMPARE(items[1].title, QStringLiteral("Keep"));
    QCOMPARE(items[1].children.size(), 1);
    QCOMPARE(items[1].children[0].title, QStringLiteral("Deep"));
}

void TestScriptMenuSpec::separatorsByTitle() {
    const ParseResult result = parse(R"json({
        "items": [
            {"title": "-"},
            {"title": "A"},
            {"title": "-"},
            {"title": "-"},
            {"title": "B"},
            {"title": "--"},
            {"title": "- "},
            {"title": " -"},
            {"title": "-", "items": [{"title": "X"}], "command": "screenshot"},
            {"title": "C"},
            {"title": "Sep", "items": [{"title": "-"}, {"title": "-"}]},
            {"title": "-"}
        ]
    })json");
    QCOMPARE(result.error, ParseError::None);
    const QList<Node> &items = result.menu.items;
    // 前导 / 连续 / 末尾剪除；--、- 、 - 是普通条目；带 items 的 "-" 仍是分割线；
    // 只剩分割线的子菜单整树移除
    QCOMPARE(items.size(), 8);
    QVERIFY(isItem(items[0]));
    QCOMPARE(items[0].title, QStringLiteral("A"));
    QVERIFY(isSeparator(items[1]));
    QCOMPARE(items[2].title, QStringLiteral("B"));
    QCOMPARE(items[3].title, QStringLiteral("--"));
    QCOMPARE(items[4].title, QStringLiteral("- "));
    QCOMPARE(items[5].title, QStringLiteral(" -"));
    QVERIFY(isSeparator(items[6]));
    QCOMPARE(items[7].title, QStringLiteral("C"));
    QVERIFY(isItem(items[3]));
    QVERIFY(isItem(items[7]));
}

void TestScriptMenuSpec::rejectsInvalidTitles() {
    const ParseResult result = parse(R"json({
        "items": [
            {},
            {"title": 5},
            {"title": ""},
            {"title": " ", "command": "x"},
            {"title": "Bad", "command": 5},
            {"title": "Bad2", "checked": "yes"},
            {"title": "Bad3", "callback": []},
            {"title": "Good", "disabled": false}
        ]
    })json");
    QCOMPARE(result.error, ParseError::None);
    const QList<Node> &items = result.menu.items;
    // 缺失 / 非字符串 / 空串标题跳过，坏类型动作字段跳条不废整单；纯空白标题保留
    QCOMPARE(items.size(), 2);
    QCOMPARE(items[0].title, QStringLiteral(" "));
    QCOMPARE(items[0].command, QStringLiteral("x"));
    QCOMPARE(items[1].title, QStringLiteral("Good"));
}

void TestScriptMenuSpec::keepsTitleWhitespace() {
    const ParseResult result = parse(R"json({
        "items": [
            {"title": "  Leading"},
            {"title": "Trailing  "},
            {"title": " Mid "}
        ]
    })json");
    QCOMPARE(result.error, ParseError::None);
    const QList<Node> &items = result.menu.items;
    QCOMPARE(items.size(), 3);
    QCOMPARE(items[0].title, QStringLiteral("  Leading"));
    QCOMPARE(items[1].title, QStringLiteral("Trailing  "));
    QCOMPARE(items[2].title, QStringLiteral(" Mid "));
}

void TestScriptMenuSpec::assignsItemIds() {
    const ParseResult result = parse(R"json({
        "items": [
            {"title": "Repeat", "checked": true, "callback": "menu-picked", "id": "repeat"},
            {"title": "Speed", "items": [
                {"title": "1.0x", "callback": "menu-picked", "id": "speed-1.0"},
                {"title": "1.5x", "checked": false, "callback": "menu-picked", "id": "speed-1.5"},
                {"title": "-"},
                {"title": "Custom…", "disabled": true}
            ]},
            {"title": "-"},
            {"title": "Take Screenshot", "command": "screenshot"}
        ]
    })json");
    QCOMPARE(result.error, ParseError::None);
    const QList<Node> &items = result.menu.items;
    QCOMPARE(items.size(), 4);
    // 显式 id 原样保留；缺省身份为跳过分割线后的序号路径，子菜单容器占号
    QCOMPARE(items[0].id, QStringLiteral("repeat"));
    QVERIFY(isSubmenu(items[1]));
    QCOMPARE(items[1].id, QString());
    QCOMPARE(items[1].children[0].id, QStringLiteral("speed-1.0"));
    QCOMPARE(items[1].children[1].id, QStringLiteral("speed-1.5"));
    QVERIFY(isSeparator(items[1].children[2]));
    QCOMPARE(items[1].children[3].id, QStringLiteral("1/2"));
    QVERIFY(isSeparator(items[2]));
    QCOMPARE(items[3].id, QStringLiteral("2"));
    QCOMPARE(items[3].title, QStringLiteral("Take Screenshot"));

    const ParseResult allDefault = parse(R"json({"items": [{"title": "A"}, {"title": "B"}]})json");
    QCOMPARE(allDefault.error, ParseError::None);
    QCOMPARE(allDefault.menu.items[0].id, QStringLiteral("0"));
    QCOMPARE(allDefault.menu.items[1].id, QStringLiteral("1"));
}

void TestScriptMenuSpec::rejectsBadJson() {
    const ParseResult broken = parse(QStringLiteral("{").toUtf8());
    QCOMPARE(broken.error, ParseError::BadJson);
    QVERIFY(!broken.detail.isEmpty());

    const ParseResult arrayRoot = parse(QStringLiteral("[1, 2]").toUtf8());
    QCOMPARE(arrayRoot.error, ParseError::BadJson);

    const ParseResult missingItems = parse(QStringLiteral("{\"title\": \"x\"}").toUtf8());
    QCOMPARE(missingItems.error, ParseError::BadJson);

    const ParseResult objectItems = parse(QStringLiteral("{\"items\": {}}").toUtf8());
    QCOMPARE(objectItems.error, ParseError::BadJson);

    const ParseResult emptyArray = parse(QStringLiteral("{\"items\": []}").toUtf8());
    QCOMPARE(emptyArray.error, ParseError::Empty);

    const ParseResult onlySeparators =
        parse(QStringLiteral("{\"items\": [{\"title\": \"-\"}]}").toUtf8());
    QCOMPARE(onlySeparators.error, ParseError::Empty);

    const ParseResult prunedToEmpty =
        parse(QStringLiteral("{\"items\": [{\"title\": \"A\", \"items\": []}]}").toUtf8());
    QCOMPARE(prunedToEmpty.error, ParseError::Empty);
}

void TestScriptMenuSpec::checkableFromPresence() {
    const ParseResult result = parse(R"json({
        "items": [
            {"title": "A", "checked": true},
            {"title": "B", "checked": false},
            {"title": "C"}
        ]
    })json");
    QCOMPARE(result.error, ParseError::None);
    // 字段存在即条目可勾选（含 false）；缺席即不可勾选
    QVERIFY(result.menu.items[0].checkable);
    QVERIFY(result.menu.items[0].checked);
    QVERIFY(result.menu.items[1].checkable);
    QVERIFY(!result.menu.items[1].checked);
    QVERIFY(!result.menu.items[2].checkable);
    QVERIFY(!result.menu.items[2].checked);
}

void TestScriptMenuSpec::ignoresUnknownFields() {
    const ParseResult result = parse(R"json({
        "items": [
            {"title": "A", "on-dismiss": "cb", "items2": 123, "icon": {"a": 1}},
            {"title": "B", "on-dismiss": true}
        ]
    })json");
    QCOMPARE(result.error, ParseError::None);
    const QList<Node> &items = result.menu.items;
    // 未知字段（含旧草案的 on-dismiss 形态）一律忽略，不影响 items 解析
    QCOMPARE(items.size(), 2);
    QCOMPARE(items[0].title, QStringLiteral("A"));
    QCOMPARE(items[1].title, QStringLiteral("B"));
    QVERIFY(items[0].callback.isEmpty());
    QVERIFY(items[1].command.isEmpty());
    QVERIFY(!items[1].checkable);
}

void TestScriptMenuSpec::menuIdValidation() {
    QVERIFY(isValidMenuId(QStringLiteral("a")));
    QVERIFY(isValidMenuId(QStringLiteral("A9_-.x")));
    QVERIFY(isValidMenuId(QStringLiteral("main")));
    QVERIFY(isValidMenuId(QString(64, u'x')));
    QVERIFY(!isValidMenuId(QString()));
    QVERIFY(!isValidMenuId(QString(65, u'x')));
    QVERIFY(!isValidMenuId(QStringLiteral("a/b")));
    QVERIFY(!isValidMenuId(QStringLiteral("a:b")));
    QVERIFY(!isValidMenuId(QStringLiteral("a b")));
    QVERIFY(!isValidMenuId(QStringLiteral("菜单")));
}

QTEST_GUILESS_MAIN(TestScriptMenuSpec)

#include "tst_ScriptMenuSpec.moc"

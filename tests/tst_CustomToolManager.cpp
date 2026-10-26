// CustomToolManager 单元测试（N134）：INI 往返、pathSegments 切段规则、写失败
// 不回滚、坏行跳过、note 不 trim、名称命令两端 trim、size 缺失 / 非数字按 0，
// 以及确定语义收口 sanitize 的规范化 / 占位条目 / 缺失字段 / 前缀冲突判定。
// N138：键的 PortableText 往返、坏键只丢键、sanitize 重键判定与 planKeys 优先级。
// 一类一可执行（R5）：oxbow_tests_CustomToolManager <槽名> 跑单个用例。

#include "pch.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "CustomToolManager.h"

class TestCustomToolManager : public QObject {
    Q_OBJECT

  private slots:
    void init();

    void roundTrip();
    void keyRoundTrip();
    void invalidStoredKey();
    void namePathSegments();
    void emptyList();
    void saveFailure();
    void skipsEmptyNameOrCommand();
    void missingNoteIsEmpty();
    void trimsOnLoad();
    void invalidSize();
    void normalizedPath();
    void sanitizeNormalizesAndKeeps();
    void sanitizeIgnoresPlaceholder();
    void sanitizeReportsMissingField();
    void sanitizeInvalidPathCountsAsMissingName();
    void sanitizeReportsPrefixConflict();
    void sanitizeReportsDuplicateKey();
    void planKeysPriority();

  private:
    QString m_ini;
    int m_seq = 0;
    QTemporaryDir m_dir;
};

void TestCustomToolManager::init() {
    QVERIFY(m_dir.isValid());
    m_ini = m_dir.filePath(QStringLiteral("tools%1.ini").arg(++m_seq));
}

void TestCustomToolManager::roundTrip() {
    CustomToolManager mgr(m_ini);
    const QList<CustomTool> tools = {
        {QStringLiteral("Show text"), QStringLiteral("show-text \"hello\""),
         QStringLiteral("demo 引号\"与中文")},
        {QStringLiteral("Playback > Speed"), QStringLiteral("set speed 1.5"), QString()},
        {QStringLiteral("A > B > C"), QStringLiteral("cycle pause"),
         QStringLiteral("note with spaces")},
    };
    QVERIFY(mgr.replaceTools(tools));
    CustomToolManager reader(m_ini);
    reader.load();
    QCOMPARE(reader.tools().size(), tools.size());
    for (int i = 0; i < tools.size(); ++i) {
        QCOMPARE(reader.tools()[i].name, tools[i].name);
        QCOMPARE(reader.tools()[i].command, tools[i].command);
        QCOMPARE(reader.tools()[i].note, tools[i].note);
    }
}

void TestCustomToolManager::namePathSegments() {
    using M = CustomToolManager;
    QCOMPARE(M::pathSegments(QStringLiteral("A > B")), QStringList({"A", "B"}));
    QCOMPARE(M::pathSegments(QStringLiteral("A >B")), QStringList({"A", "B"}));
    QCOMPARE(M::pathSegments(QStringLiteral("  A  >  B  ")), QStringList({"A", "B"}));
    QCOMPARE(M::pathSegments(QStringLiteral("A >> B")), QStringList({"A", "B"}));
    QCOMPARE(M::pathSegments(QStringLiteral("A > B > C")), QStringList({"A", "B", "C"}));
    QVERIFY(M::pathSegments(QStringLiteral(">")).isEmpty());
    QVERIFY(M::pathSegments(QStringLiteral(" >  > ")).isEmpty());
    QVERIFY(M::pathSegments(QString()).isEmpty());
}

void TestCustomToolManager::emptyList() {
    CustomToolManager mgr(m_ini);
    QVERIFY(mgr.replaceTools({}));
    QCOMPARE(mgr.tools().size(), 0);
    CustomToolManager reader(m_ini);
    reader.load();
    QCOMPARE(reader.tools().size(), 0);
    QSettings s(m_ini, QSettings::IniFormat);
    QCOMPARE(s.value(QStringLiteral("Tools/size")).toInt(), 0);
}

void TestCustomToolManager::saveFailure() {
    // 配置路径的父级是普通文件，落盘必败
    const QString blocker = m_dir.filePath(QStringLiteral("blocker"));
    QFile file(blocker);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    CustomToolManager mgr(blocker + QStringLiteral("/tools.ini"));
    const QList<CustomTool> tools = {
        {QStringLiteral("A"), QStringLiteral("cycle pause"), QString()}};
    QVERIFY(!mgr.replaceTools(tools));
    QCOMPARE(mgr.tools().size(), 1);
    QCOMPARE(mgr.tools()[0].name, QStringLiteral("A"));
}

void TestCustomToolManager::skipsEmptyNameOrCommand() {
    {
        QSettings s(m_ini, QSettings::IniFormat);
        s.setValue(QStringLiteral("Tools/size"), 4);
        s.setValue(QStringLiteral("Tools/0/name"), QStringLiteral("A"));
        s.setValue(QStringLiteral("Tools/0/command"), QStringLiteral("cycle pause"));
        s.setValue(QStringLiteral("Tools/1/name"), QString());
        s.setValue(QStringLiteral("Tools/1/command"), QStringLiteral("cycle pause"));
        s.setValue(QStringLiteral("Tools/2/name"), QStringLiteral("B"));
        s.setValue(QStringLiteral("Tools/2/command"), QString());
        s.setValue(QStringLiteral("Tools/3/name"), QStringLiteral(" >  > "));
        s.setValue(QStringLiteral("Tools/3/command"), QStringLiteral("cycle pause"));
        s.sync();
    }
    CustomToolManager mgr(m_ini);
    mgr.load();
    QCOMPARE(mgr.tools().size(), 1);
    QCOMPARE(mgr.tools()[0].name, QStringLiteral("A"));
}

void TestCustomToolManager::missingNoteIsEmpty() {
    QSettings s(m_ini, QSettings::IniFormat);
    s.setValue(QStringLiteral("Tools/size"), 1);
    s.setValue(QStringLiteral("Tools/0/name"), QStringLiteral("A"));
    s.setValue(QStringLiteral("Tools/0/command"), QStringLiteral("cycle pause"));
    s.sync();
    CustomToolManager mgr(m_ini);
    mgr.load();
    QCOMPARE(mgr.tools().size(), 1);
    QVERIFY(mgr.tools()[0].note.isEmpty());
}

void TestCustomToolManager::trimsOnLoad() {
    QSettings s(m_ini, QSettings::IniFormat);
    s.setValue(QStringLiteral("Tools/size"), 1);
    s.setValue(QStringLiteral("Tools/0/name"), QStringLiteral("  Spaced >  Name  "));
    s.setValue(QStringLiteral("Tools/0/command"), QStringLiteral("\t cycle pause "));
    s.setValue(QStringLiteral("Tools/0/note"), QStringLiteral("  raw note  "));
    s.sync();
    CustomToolManager mgr(m_ini);
    mgr.load();
    QCOMPARE(mgr.tools()[0].name, QStringLiteral("Spaced >  Name"));
    QCOMPARE(mgr.tools()[0].command, QStringLiteral("cycle pause"));
    QCOMPARE(mgr.tools()[0].note, QStringLiteral("  raw note  "));
}

void TestCustomToolManager::invalidSize() {
    {
        QSettings s(m_ini, QSettings::IniFormat);
        s.setValue(QStringLiteral("Tools/size"), QStringLiteral("abc"));
        s.setValue(QStringLiteral("Tools/0/name"), QStringLiteral("A"));
        s.setValue(QStringLiteral("Tools/0/command"), QStringLiteral("cycle pause"));
        s.sync();
    }
    CustomToolManager mgr(m_ini);
    mgr.load();
    QCOMPARE(mgr.tools().size(), 0);

    const QString missing = m_dir.filePath(QStringLiteral("missing.ini"));
    CustomToolManager reader(missing);
    reader.load();
    QCOMPARE(reader.tools().size(), 0);
}

void TestCustomToolManager::normalizedPath() {
    using M = CustomToolManager;
    QCOMPARE(M::normalizedPath(QStringLiteral("A >B")), QStringLiteral("A > B"));
    QCOMPARE(M::normalizedPath(QStringLiteral("  A  >  B  ")), QStringLiteral("A > B"));
    QCOMPARE(M::normalizedPath(QStringLiteral("A >> B")), QStringLiteral("A > B"));
    QCOMPARE(M::normalizedPath(QStringLiteral("Solo")), QStringLiteral("Solo"));
    QCOMPARE(M::joinPath({QStringLiteral("A"), QStringLiteral("B")}), QStringLiteral("A > B"));
    QVERIFY(M::normalizedPath(QStringLiteral(" >  > ")).isEmpty());
    QVERIFY(M::normalizedPath(QString()).isEmpty());
}

void TestCustomToolManager::sanitizeNormalizesAndKeeps() {
    using M = CustomToolManager;
    const QList<CustomTool> drafts = {
        CustomTool{QStringLiteral("  Playback >Speed  "), QStringLiteral("\tcycle pause\t"),
                   QStringLiteral("  keep  ")},
        CustomTool{QStringLiteral("Show text"), QStringLiteral("show-text \"hi\""), QString()},
    };
    const M::Sanitized out = M::sanitize(drafts);
    QVERIFY(out.problem == M::Problem::None);
    QCOMPARE(out.tools.size(), 2);
    QCOMPARE(out.tools[0].name, QStringLiteral("Playback > Speed"));
    QCOMPARE(out.tools[0].command, QStringLiteral("cycle pause"));
    QCOMPARE(out.tools[0].note, QStringLiteral("  keep  ")); // 备注原样保留，不 trim
    QCOMPARE(out.tools[1].name, QStringLiteral("Show text"));
}

void TestCustomToolManager::sanitizeIgnoresPlaceholder() {
    using M = CustomToolManager;
    // 占位条目混在有效行之间：不判违规，也不作为空前缀把整表拦下
    const QList<CustomTool> drafts = {
        CustomTool{QStringLiteral("A"), QStringLiteral("cycle pause"), QString()},
        CustomTool{QString(), QString(), QStringLiteral("只有备注的占位行")},
        CustomTool{QStringLiteral("B > C"), QStringLiteral("set speed 1.5"), QString()},
    };
    const M::Sanitized out = M::sanitize(drafts);
    QVERIFY(out.problem == M::Problem::None);
    QCOMPARE(out.tools.size(), 2);
    QCOMPARE(out.tools[0].name, QStringLiteral("A"));
    QCOMPARE(out.tools[1].name, QStringLiteral("B > C"));

    const QList<CustomTool> allPlaceholders = {
        CustomTool{},
        CustomTool{QString(), QString(), QStringLiteral("note")},
    };
    const M::Sanitized emptied = M::sanitize(allPlaceholders);
    QVERIFY(emptied.problem == M::Problem::None);
    QVERIFY(emptied.tools.isEmpty()); // 全表皆占位即空表，保存空列表
}

void TestCustomToolManager::sanitizeReportsMissingField() {
    using M = CustomToolManager;
    const QList<CustomTool> missingName = {
        CustomTool{QStringLiteral("A"), QStringLiteral("cycle pause"), QString()},
        CustomTool{QString(), QStringLiteral("cycle pause"), QString()},
    };
    const M::Sanitized nameOut = M::sanitize(missingName);
    QVERIFY(nameOut.problem == M::Problem::MissingName);
    QCOMPARE(nameOut.row, 1);
    QVERIFY(nameOut.tools.isEmpty());

    const QList<CustomTool> missingCommand = {
        CustomTool{QStringLiteral("A"), QString(), QString()},
        CustomTool{QStringLiteral("B"), QStringLiteral("cycle pause"), QString()},
    };
    const M::Sanitized commandOut = M::sanitize(missingCommand);
    QVERIFY(commandOut.problem == M::Problem::MissingCommand);
    QCOMPARE(commandOut.row, 0);
    QVERIFY(commandOut.tools.isEmpty());
}

void TestCustomToolManager::sanitizeInvalidPathCountsAsMissingName() {
    using M = CustomToolManager;
    // 纯 '>' / 全空段：规范化后名称为空，视同缺名称（聚焦名称框那一侧）
    const QList<CustomTool> drafts = {
        CustomTool{QStringLiteral(" > "), QStringLiteral("cycle pause"), QString()},
    };
    const M::Sanitized out = M::sanitize(drafts);
    QVERIFY(out.problem == M::Problem::MissingName);
    QCOMPARE(out.row, 0);
    QVERIFY(out.tools.isEmpty());
}

void TestCustomToolManager::sanitizeReportsPrefixConflict() {
    using M = CustomToolManager;
    const QList<CustomTool> drafts = {
        CustomTool{QStringLiteral("X"), QStringLiteral("x"), QString()},
        CustomTool{QStringLiteral("A > B"), QStringLiteral("b"), QString()},
        CustomTool{QStringLiteral("A"), QStringLiteral("a"), QString()},
    };
    const M::Sanitized out = M::sanitize(drafts);
    QVERIFY(out.problem == M::Problem::PrefixConflict);
    QCOMPARE(out.row, 2); // 报短路径（既是工具又是子菜单）那一行，与表序无关
    QCOMPARE(out.path, QStringLiteral("A"));
    QVERIFY(out.tools.isEmpty());

    // 逐段比而非字符串前缀：Ab 不是 A > B 的前缀
    const QList<CustomTool> notConflict = {
        CustomTool{QStringLiteral("Ab"), QStringLiteral("a"), QString()},
        CustomTool{QStringLiteral("A > B"), QStringLiteral("b"), QString()},
    };
    const M::Sanitized ok = M::sanitize(notConflict);
    QVERIFY(ok.problem == M::Problem::None);
    QCOMPARE(ok.tools.size(), 2);

    // 同路径两条（重名）不判冲突，照常保留
    const QList<CustomTool> duplicates = {
        CustomTool{QStringLiteral("A > B"), QStringLiteral("b1"), QString()},
        CustomTool{QStringLiteral("A >B"), QStringLiteral("b2"), QString()},
    };
    const M::Sanitized twice = M::sanitize(duplicates);
    QVERIFY(twice.problem == M::Problem::None);
    QCOMPARE(twice.tools.size(), 2);
}

void TestCustomToolManager::keyRoundTrip() {
    using M = CustomToolManager;
    CustomToolManager mgr(m_ini);
    const QList<CustomTool> tools = {
        {QStringLiteral("A"), QStringLiteral("a"), QString(),
         QKeySequence(QStringLiteral("Ctrl+Alt+H"))},
        {QStringLiteral("B"), QStringLiteral("b"), QString(), QKeySequence()},
    };
    QVERIFY(mgr.replaceTools(tools));
    CustomToolManager reader(m_ini);
    reader.load();
    QCOMPARE(reader.tools().size(), tools.size());
    QCOMPARE(reader.tools()[0].name, QStringLiteral("A"));
    QCOMPARE(reader.tools()[0].key, QKeySequence(QStringLiteral("Ctrl+Alt+H")));
    QVERIFY(reader.tools()[1].key.isEmpty());
    QSettings s(m_ini, QSettings::IniFormat);
    QCOMPARE(s.value(QStringLiteral("Tools/0/key")).toString(), QStringLiteral("Ctrl+Alt+H"));
    QCOMPARE(s.value(QStringLiteral("Tools/1/key")).toString(), QString());

    // 键不参与规范化，随保留行原样带走
    const M::Sanitized out = M::sanitize(tools);
    QVERIFY(out.problem == M::Problem::None);
    QCOMPARE(out.tools.size(), 2);
    QCOMPARE(out.tools[0].key, tools[0].key);
    QVERIFY(out.tools[1].key.isEmpty());
}

void TestCustomToolManager::invalidStoredKey() {
    {
        QSettings s(m_ini, QSettings::IniFormat);
        s.setValue(QStringLiteral("Tools/size"), 2);
        s.setValue(QStringLiteral("Tools/0/name"), QStringLiteral("A"));
        s.setValue(QStringLiteral("Tools/0/command"), QStringLiteral("a"));
        s.setValue(QStringLiteral("Tools/0/key"), QStringLiteral("Ctrl+Foo"));
        s.setValue(QStringLiteral("Tools/1/name"), QStringLiteral("B"));
        s.setValue(QStringLiteral("Tools/1/command"), QStringLiteral("b"));
        s.sync();
    }
    CustomToolManager mgr(m_ini);
    mgr.load();
    QCOMPARE(mgr.tools().size(), 2);
    QVERIFY(mgr.tools()[0].key.isEmpty());
    QCOMPARE(mgr.tools()[0].command, QStringLiteral("a"));
    QVERIFY(mgr.tools()[1].key.isEmpty());
}

void TestCustomToolManager::sanitizeReportsDuplicateKey() {
    using M = CustomToolManager;
    const QList<CustomTool> dup = {
        {QStringLiteral("A"), QStringLiteral("a"), QString(),
         QKeySequence(QStringLiteral("Ctrl+J"))},
        {QStringLiteral("B"), QStringLiteral("b"), QString(),
         QKeySequence(QStringLiteral("Ctrl+J"))},
    };
    const M::Sanitized out = M::sanitize(dup);
    QVERIFY(out.problem == M::Problem::DuplicateKey);
    QCOMPARE(out.row, 1); // 报后出现的那一行
    QCOMPARE(out.key, QStringLiteral("Ctrl+J"));
    QVERIFY(out.tools.isEmpty());

    // 空键不参与查重
    const QList<CustomTool> emptyKeys = {
        {QStringLiteral("A"), QStringLiteral("a"), QString(), QKeySequence()},
        {QStringLiteral("B"), QStringLiteral("b"), QString(), QKeySequence()},
    };
    QVERIFY(M::sanitize(emptyKeys).problem == M::Problem::None);

    // 占位空条目先被丢弃、不参与查重；另一条照常保留
    const QList<CustomTool> placeholder = {
        {QStringLiteral("A"), QStringLiteral("a"), QString(),
         QKeySequence(QStringLiteral("Ctrl+J"))},
        {QString(), QString(), QString(), QKeySequence(QStringLiteral("Ctrl+J"))},
    };
    const M::Sanitized ok = M::sanitize(placeholder);
    QVERIFY(ok.problem == M::Problem::None);
    QCOMPARE(ok.tools.size(), 1);
    QCOMPARE(ok.tools[0].key, QKeySequence(QStringLiteral("Ctrl+J")));
}

void TestCustomToolManager::planKeysPriority() {
    using M = CustomToolManager;
    const QList<QPair<QKeySequence, QString>> occupied = {
        {QKeySequence(QStringLiteral("Ctrl+C")), QStringLiteral("Copy Screenshot")},
    };
    const QList<CustomTool> tools = {
        {QStringLiteral("AM 冲突"), QStringLiteral("a"), QString(),
         QKeySequence(QStringLiteral("Ctrl+C"))},
        {QStringLiteral("先到"), QStringLiteral("b"), QString(),
         QKeySequence(QStringLiteral("Ctrl+J"))},
        {QStringLiteral("后到"), QStringLiteral("c"), QString(),
         QKeySequence(QStringLiteral("Ctrl+J"))},
        {QStringLiteral("空键"), QStringLiteral("d"), QString(), QKeySequence()},
    };
    const M::KeyPlan plan = M::planKeys(tools, occupied);
    QCOMPARE(plan.installed.size(), tools.size());
    QVERIFY(plan.installed[0].isEmpty());
    QCOMPARE(plan.installed[1], QKeySequence(QStringLiteral("Ctrl+J")));
    QVERIFY(plan.installed[2].isEmpty());
    QVERIFY(plan.installed[3].isEmpty());
    QCOMPARE(plan.conflicts.size(), 2); // 空键不入 conflicts
    QCOMPARE(plan.conflicts[0].row, 0);
    QCOMPARE(plan.conflicts[0].key, QKeySequence(QStringLiteral("Ctrl+C")));
    QCOMPARE(plan.conflicts[0].owner, QStringLiteral("Copy Screenshot"));
    QCOMPARE(plan.conflicts[1].row, 2);
    QCOMPARE(plan.conflicts[1].owner, QStringLiteral("先到"));
}

QTEST_GUILESS_MAIN(TestCustomToolManager)

#include "tst_CustomToolManager.moc"

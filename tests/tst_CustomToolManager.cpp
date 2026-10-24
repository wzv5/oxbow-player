// CustomToolManager 单元测试（N134）：INI 往返、pathSegments 切段规则、写失败
// 不回滚、坏行跳过、note 不 trim、名称命令两端 trim、size 缺失 / 非数字按 0。
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
    void namePathSegments();
    void emptyList();
    void saveFailure();
    void skipsEmptyNameOrCommand();
    void missingNoteIsEmpty();
    void trimsOnLoad();
    void invalidSize();

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

QTEST_GUILESS_MAIN(TestCustomToolManager)

#include "tst_CustomToolManager.moc"

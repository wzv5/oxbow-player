#include "pch.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "playbackreport/ReporterSwitchCatalog.h"

#include "providers/JellyfinProvider.h" // kJellyfinProviderKey：账目行与注册表键同源的旁证

// N99：上报开关账目——静态聚合表 + 读值 + 启用聚合规则。登记自检（键空 / 键重复）在表
// 首次构建时执行（qWarning 显影 + Debug 期断言表非空），本套件以"行内容断言"从旁守住
// 形状；readReporterSwitchValues 是变更通路 / 启动期同步 / 启动判据共用的唯一读法，
// 三档都要守住（在册值、ini 缺项落兜底、ini 整体不可用落兜底）；anyEnabled 的真值表是
// 主窗启动判据与驱动器开关切换的共同判据，纯函数直接可测
class TestReporterSwitchCatalog : public QObject {
    Q_OBJECT

  private slots:
    void catalogCarriesJellyfinRow();
    void valuesFallBackWithoutIniFile();
    void valuesFallBackOnMissingKey();
    void valuesReadFromIni();
    void anyEnabledTruthTable();

  private:
    QTemporaryDir m_dir;
};

void TestReporterSwitchCatalog::catalogCarriesJellyfinRow() {
    const std::vector<ReporterSwitch> &table = reporterSwitches();
    QVERIFY(!table.empty());
    const ReporterSwitch &jf = jellyfinSwitch();
    QCOMPARE(QLatin1String(jf.iniKey), QLatin1String("reportJellyfin"));
    QCOMPARE(QLatin1String(jf.reporterKey), QLatin1String(kJellyfinProviderKey));
    QVERIFY(jf.defaultValue); // 缺省开：只有主动配了 Jellyfin 的用户受影响
    // 行间键唯一性（自检的旁证：qWarning 在首次构建时已发，此处断言防回归），并顺带
    // 守住"具名单行确在表内"——具名出口与全表由同一批命名常量组装，不得各存一份
    QSet<QString> iniKeys;
    QSet<QString> reporterKeys;
    bool jellyfinRowCarried = false;
    for (const ReporterSwitch &row : table) {
        const QString ini = QString::fromLatin1(row.iniKey);
        const QString reporter = QString::fromLatin1(row.reporterKey);
        QVERIFY(!ini.isEmpty());
        QVERIFY(!reporter.isEmpty());
        QVERIFY(!iniKeys.contains(ini));
        QVERIFY(!reporterKeys.contains(reporter));
        iniKeys.insert(ini);
        reporterKeys.insert(reporter);
        if (ini == QString::fromLatin1(jf.iniKey))
            jellyfinRowCarried = true;
    }
    QVERIFY(jellyfinRowCarried);
}

// configFile 为空（无 ini 文件可用）⇒ 整表落各行 defaultValue：主窗那条"ini 不可用即
// 与变更通路同口径"的分支就住在这几行里
void TestReporterSwitchCatalog::valuesFallBackWithoutIniFile() {
    const QHash<QString, bool> values = readReporterSwitchValues(QString());
    QVERIFY(values.size() == static_cast<int>(reporterSwitches().size()));
    for (const ReporterSwitch &row : reporterSwitches()) {
        QVERIFY(values.contains(QLatin1String(row.reporterKey)));
        QCOMPARE(values.value(QLatin1String(row.reporterKey)), row.defaultValue);
    }
}

// ini 存在但该项从未写过（用户没开过设置对话框）⇒ 同样落行兜底，不得判成 false
void TestReporterSwitchCatalog::valuesFallBackOnMissingKey() {
    const QString iniPath = m_dir.filePath(QStringLiteral("switches-missing.ini"));
    {
        QSettings s(iniPath, QSettings::IniFormat);
        s.setValue(QStringLiteral("unrelatedKey"), QStringLiteral("x"));
        s.sync();
    }
    const QHash<QString, bool> values = readReporterSwitchValues(iniPath);
    QCOMPARE(values.value(QLatin1String(jellyfinSwitch().reporterKey)),
             jellyfinSwitch().defaultValue);
}

// ini 显式写了 false ⇒ 关（驱动器据此把该种类键的投递停掉、主窗判据据此不起线程）
void TestReporterSwitchCatalog::valuesReadFromIni() {
    const QString iniPath = m_dir.filePath(QStringLiteral("switches-off.ini"));
    {
        QSettings s(iniPath, QSettings::IniFormat);
        s.setValue(QLatin1String(jellyfinSwitch().iniKey), false);
        s.sync();
    }
    const QHash<QString, bool> values = readReporterSwitchValues(iniPath);
    QVERIFY(!values.value(QLatin1String(jellyfinSwitch().reporterKey)));
}

// 聚合规则的五档真值表："映射内任一为真 ∪ 注册表在册键存在映射外的键"。registeredKeys
// 必须是未经任何过滤的全部在册键（用过滤子集反推会自指），调用方契约由真值表侧面固定
void TestReporterSwitchCatalog::anyEnabledTruthTable() {
    const std::vector<QString> noKeys;
    const std::vector<QString> jfOnly{QLatin1String(kJellyfinProviderKey)};
    const std::vector<QString> withTrack2{QLatin1String(kJellyfinProviderKey),
                                          QStringLiteral("lastfm")};

    QVERIFY(!anyEnabled({}, noKeys)); // 全空：判全关
    QHash<QString, bool> jfOn{{QString::fromLatin1(kJellyfinProviderKey), true}};
    QVERIFY(anyEnabled(jfOn, noKeys)); // 映射内任一为真
    QHash<QString, bool> jfOff{{QString::fromLatin1(kJellyfinProviderKey), false}};
    QVERIFY(!anyEnabled(jfOff, jfOnly)); // 全关且无表外在册键：判全关
    // 未登记的键按启用处理：轨 2 那类不进设置目录的报告者，不被别家键的关闭误伤
    QVERIFY(anyEnabled(jfOff, withTrack2));
    QVERIFY(anyEnabled({}, withTrack2));
    QHash<QString, bool> mixed{{QStringLiteral("lastfm"), false},
                               {QString::fromLatin1(kJellyfinProviderKey), true}};
    QVERIFY(anyEnabled(mixed, withTrack2)); // 混合表内任一为真
}

QTEST_GUILESS_MAIN(TestReporterSwitchCatalog)
#include "tst_ReporterSwitchCatalog.moc"

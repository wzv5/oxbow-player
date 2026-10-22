#include "pch.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "Logging.h"

// N126：只覆盖纯函数——模式合成、行格式、路径。不安装出口、不碰真实文件 sink：
// 落盘与标准面保留由维护者手动核。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R Logging 跑本套件；
//   oxbow_tests_Logging <槽名> 跑单个用例。
class TestLogging : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void init();
    void cleanup();

    void modeDefaultsToOff();
    void modeReadsThreeLevels();
    void modeInvalidValueIsOff();
    void envModeIsRead();
    void largerOfIniAndEnvWins();
    void formatLineCarriesTimestampAndLevel();
    void filePathUsesTempDirAndPid();

  private:
    QString writeIni(const QByteArray &content);

    QTemporaryDir m_dir;
};

void TestLogging::initTestCase() {
    // 建不出临时目录时各 writeIni 返回空串，而 resolveMode 对空路径同样读不到键——
    // "期望 0"的断言会全绿却什么都没测，故这里先把地基钉死
    QVERIFY(m_dir.isValid());
}

void TestLogging::init() {
    // 环境变量名是日志层与进程环境之间的外部契约（常量收在 Logging.cpp 文件内、不外露），
    // 测试按字面串覆盖同一契约
    qunsetenv("OXBOW_LOG_MODE");
}

void TestLogging::cleanup() { qunsetenv("OXBOW_LOG_MODE"); }

QString TestLogging::writeIni(const QByteArray &content) {
    static int seq = 0;
    const QString path = m_dir.filePath(QStringLiteral("oxbow-%1.ini").arg(++seq));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    file.write(content);
    file.close();
    return path;
}

void TestLogging::modeDefaultsToOff() {
    const QString path = writeIni("language=en\n");
    QVERIFY(!path.isEmpty());
    QCOMPARE(Logging::resolveMode(path), 0);
}

void TestLogging::modeReadsThreeLevels() {
    const QString zero = writeIni("logMode=0\n");
    const QString one = writeIni("logMode=1\n");
    const QString two = writeIni("logMode=2\n");
    QVERIFY(!zero.isEmpty() && !one.isEmpty() && !two.isEmpty());
    QCOMPARE(Logging::resolveMode(zero), 0);
    QCOMPARE(Logging::resolveMode(one), 1);
    QCOMPARE(Logging::resolveMode(two), 2);
}

void TestLogging::modeInvalidValueIsOff() {
    // 空 / 非数字 / 负数 / 越界一律按 0（ini 侧的 true、false 也落进"非数字"）
    const QStringList raws{QStringLiteral("logMode=\n"), QStringLiteral("logMode=abc\n"),
                           QStringLiteral("logMode=true\n"), QStringLiteral("logMode=-1\n"),
                           QStringLiteral("logMode=3\n")};
    for (const QString &raw : raws) {
        const QString path = writeIni(raw.toLatin1());
        QVERIFY2(!path.isEmpty(), qPrintable(QStringLiteral("write failed for %1").arg(raw)));
        QVERIFY2(Logging::resolveMode(path) == 0,
                 qPrintable(QStringLiteral("not off for %1").arg(raw)));
    }
}

void TestLogging::envModeIsRead() {
    const QString path = writeIni("language=en\n");
    QVERIFY(!path.isEmpty());

    qputenv("OXBOW_LOG_MODE", "1");
    QCOMPARE(Logging::resolveMode(path), 1);
    qputenv("OXBOW_LOG_MODE", "2");
    QCOMPARE(Logging::resolveMode(path), 2);
    qputenv("OXBOW_LOG_MODE", "abc");
    QCOMPARE(Logging::resolveMode(path), 0);
    qputenv("OXBOW_LOG_MODE", "7");
    QCOMPARE(Logging::resolveMode(path), 0);
}

void TestLogging::largerOfIniAndEnvWins() {
    const QString one = writeIni("logMode=1\n");
    const QString two = writeIni("logMode=2\n");
    QVERIFY(!one.isEmpty() && !two.isEmpty());

    qputenv("OXBOW_LOG_MODE", "2");
    QCOMPARE(Logging::resolveMode(one), 2);
    QCOMPARE(Logging::resolveMode(two), 2);

    qputenv("OXBOW_LOG_MODE", "0");
    QCOMPARE(Logging::resolveMode(two), 2);
}

void TestLogging::formatLineCarriesTimestampAndLevel() {
    const QDateTime when(QDate(2026, 10, 22), QTime(18, 51, 34, 123));
    QCOMPARE(Logging::formatLine(Logging::Level::Debug, when, QStringLiteral("a")),
             QStringLiteral("[oxbow] [2026-10-22 18:51:34.123] [DEBUG] a"));
    QCOMPARE(Logging::formatLine(Logging::Level::Info, when, QStringLiteral("b")),
             QStringLiteral("[oxbow] [2026-10-22 18:51:34.123] [INFO] b"));
    QCOMPARE(Logging::formatLine(Logging::Level::Warning, when, QStringLiteral("c")),
             QStringLiteral("[oxbow] [2026-10-22 18:51:34.123] [WARN] c"));
    QCOMPARE(Logging::formatLine(Logging::Level::Error, when, QStringLiteral("d")),
             QStringLiteral("[oxbow] [2026-10-22 18:51:34.123] [ERROR] d"));
}

void TestLogging::filePathUsesTempDirAndPid() {
    QCOMPARE(Logging::filePathForPid(4242),
             QDir(QDir::tempPath()).filePath(QStringLiteral("oxbow.4242.log")));
}

QTEST_GUILESS_MAIN(TestLogging)

#include "tst_Logging.moc"

#include "pch.h"

#include <QtTest/QtTest>

#include "ExternalTool.h"

#include <QTemporaryDir>

// N142 / N149：ExternalTool 两只路径函数的契约测试。两目录（插件目录与 ini 目录）
// 全由调用方注入——本层不读 App::paths()，用例自己摆目录。显式值 trim 后经
// Util::resolveExternalProgram 三步解析定形（判据与自检同源），指向不存在的文件判
// 不可得；空串回落候选表首项，自动态命中随包那一份、插件目录在前。

namespace {

bool touch(const QString &path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("x") == 1;
}

} // namespace

class TestExternalTool : public QObject {
    Q_OBJECT

  private slots:
    // 显式值：trim 后解析定形（绝对写法命中自身 / 裸名靠插件目录顶上 / 不存在判不可得）
    void resolvedPathPrefersExplicit();
    // 自动态：候选表按插件目录 → ini 目录逐档取，出表者恒为真实文件
    void resolvedPathFallsBackToCandidates();
};

void TestExternalTool::resolvedPathPrefersExplicit() {
#ifdef Q_OS_WIN
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString plugin = root.filePath(QStringLiteral("plug"));
    const QString ini = root.filePath(QStringLiteral("ini"));
    const ExternalToolSpec &spec = externalToolSpec(ExternalTool::Ffmpeg);
    const QString exe = root.filePath(QStringLiteral("elsewhere/ffmpeg.exe"));
    QVERIFY(touch(exe));

    // 绝对写法带后缀：trim 掉首尾空白后解析定形为自身
    QCOMPARE(QDir::cleanPath(
                 externalToolResolvedPath(spec, QStringLiteral(" %1 ").arg(exe), plugin, ini)),
             QDir::cleanPath(exe));

    // 裸名写法：显式值过解析，插件目录那份顶上（N149 从"原样透出"改为解析）
    QVERIFY(touch(plugin + QStringLiteral("/ffmpeg.exe")));
    QCOMPARE(QDir::cleanPath(externalToolResolvedPath(spec, QStringLiteral("ffmpeg"), plugin, ini)),
             QDir::cleanPath(plugin + QStringLiteral("/ffmpeg.exe")));

    // 指向不存在的文件判不可得：绝对写法不进 PATH，两个目录入参也不替它顶同名份
    QVERIFY(touch(ini + QStringLiteral("/absent.exe")));
    QVERIFY(externalToolResolvedPath(spec, root.filePath(QStringLiteral("absent.exe")), plugin, ini)
                .isEmpty());
#else
    QSKIP("后缀枚举语义只在 Windows 侧成立");
#endif
}

void TestExternalTool::resolvedPathFallsBackToCandidates() {
#ifdef Q_OS_WIN
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString plugin = root.filePath(QStringLiteral("plug"));
    const QString ini = root.filePath(QStringLiteral("ini"));
    const ExternalToolSpec &spec = externalToolSpec(ExternalTool::Ffmpeg);
    const QString iniExe = ini + QStringLiteral("/ffmpeg.exe");
    const QString pluginExe = plugin + QStringLiteral("/ffmpeg.exe");

    // 两目录同名并存：候选表插件目录在前，随包那一份优先
    QVERIFY(touch(pluginExe));
    QVERIFY(touch(iniExe));
    QCOMPARE(QDir::cleanPath(externalToolResolvedPath(spec, QString(), plugin, ini)),
             QDir::cleanPath(pluginExe));

    // 只摆 ini 目录：候选扫描的下一档即它
    QVERIFY(QFile::remove(pluginExe));
    QCOMPARE(QDir::cleanPath(externalToolResolvedPath(spec, QString(), plugin, ini)),
             QDir::cleanPath(iniExe));

    // 两目录都抽掉：唯一剩下的落点是 PATH，出表者仍须是真实文件（判据恒为"存在"）
    QVERIFY(QFile::remove(iniExe));
    const QString fromPath = externalToolResolvedPath(spec, QString(), plugin, ini);
    if (!fromPath.isEmpty())
        QVERIFY(QFileInfo(fromPath).isFile());
#else
    QSKIP("候选表按后缀与目录次序扫描，语义只在 Windows 侧成立");
#endif
}

QTEST_GUILESS_MAIN(TestExternalTool)
#include "tst_ExternalTool.moc"

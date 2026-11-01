#include "pch.h"

#include <QtTest/QtTest>

#include "ExternalTool.h"

// N146：externalToolResolvedPath 的契约测试——ini 显式值 trim 后原样优先；
// 候选扫描依赖磁盘与 PATH，不进单测。

class TestExternalTool : public QObject {
    Q_OBJECT

  private slots:
    void resolvedPathPrefersExplicit();
};

void TestExternalTool::resolvedPathPrefersExplicit() {
    // ini 显式值原样优先（含空白修剪）；候选扫描依赖磁盘与 PATH，不在单测断言之列
    QCOMPARE(externalToolResolvedPath(externalToolSpec(ExternalTool::Ffmpeg),
                                      QStringLiteral(" D:/tools/ffmpeg.exe "),
                                      QStringLiteral("/cfg")),
             QStringLiteral("D:/tools/ffmpeg.exe"));
}

QTEST_GUILESS_MAIN(TestExternalTool)
#include "tst_ExternalTool.moc"

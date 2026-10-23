// N130：后缀名过滤器行控件测试（复位回填默认清单、text / setText 往返）。
// ctest --test-dir build -C Debug -R ExtensionFilterEdit 跑本套件；
// oxbow_tests_ExtensionFilterEdit <槽名> 跑单个用例。

#include "pch.h"

#include <QtTest/QtTest>

#include "Constants.h"
#include "ui/ExtensionFilterEdit.h"

class TestExtensionFilterEdit : public QObject {
    Q_OBJECT

  private slots:
    void resetsToDefaultList();
    void textRoundTrip();
};

void TestExtensionFilterEdit::resetsToDefaultList() {
    ExtensionFilterEdit edit;
    edit.setText(QStringLiteral("abc"));
    // 复位按钮是控件内唯一 QPushButton：findChild 命中后 click() 直接触发
    auto *resetBtn = edit.findChild<QPushButton *>();
    QVERIFY(resetBtn);
    resetBtn->click();
    QCOMPARE(edit.text(), App::defaultMediaExtensions().join(QLatin1Char(' ')));
}

void TestExtensionFilterEdit::textRoundTrip() {
    ExtensionFilterEdit edit;
    edit.setText(QStringLiteral(" *.mp4  *.mkv "));
    QCOMPARE(edit.text(), QStringLiteral(" *.mp4  *.mkv "));
    edit.setText(QString());
    QVERIFY(edit.text().isEmpty());
}

QTEST_MAIN(TestExtensionFilterEdit)

#include "tst_ExtensionFilterEdit.moc"

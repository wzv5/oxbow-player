// N156：主窗自绘标题栏三态组件测试。
// ctest --test-dir build -C Debug -R WindowTitleBar 跑本套件；
// oxbow_tests_WindowTitleBar <槽名> 跑单个用例（直跑 windows 平台——部署目录
// 不含 offscreen 插件，加 -platform offscreen 会触发"平台插件无法初始化"弹窗）。
// 测试以普通 QWidget 充当宿主——组件构造带显式 QWidget *（主窗类进不了测试库）。
// 像素级断言不可靠（平台样式差），标题回显走结构断言：宿主指针身份 + 重挂不变。

#include "pch.h"

#include <QtTest/QtTest>

#include "Constants.h"
#include "ui/WindowTitleBar.h"

class TestWindowTitleBar : public QObject {
    Q_OBJECT

  private slots:
    // Normal 态：高 32，三控制按钮可见、菜单与退出全屏隐藏
    void normalModeConfiguration();
    // Mini 态：高 miniTitleBarHeight、仅关闭钮；Floating 态：退出全屏 + 关闭
    void miniAndFloatingConfiguration();
    // 菜单按钮随 setMenuButtonVisible 显隐；Mini / Floating 态下发 true 也不显示
    void menuButtonGating();
    // 组件与全部按钮 NoFocus（N117，不入 Tab 链）
    void noFocusPolicy();
    // clicked → 各信号（退出全屏钮同发 maximizeRestoreRequested，语义分流在主窗）
    void buttonClicksEmitSignals();
    // 显式宿主指针身份：重挂进另一个顶层窗后仍读原宿主（window() 已变）
    void hostPointerStableAfterReparent();
    // 最大化 glyph 判据随宿主窗口态翻转（绘制分支与判据同源）
    void maximizeGlyphFollowsWindowState();
    // N156：最大化钮 NC 命中矩形只在 Normal 态给出；按下回灌态可读写、随形态作废
    void maximizeButtonNativeState();

  private:
    // 按钮无句柄暴露，经 tooltip 找回（tooltip 即文案单源）
    static QAbstractButton *buttonByTip(WindowTitleBar &bar, const QString &tip);
};

QAbstractButton *TestWindowTitleBar::buttonByTip(WindowTitleBar &bar, const QString &tip) {
    for (QAbstractButton *b : bar.findChildren<QAbstractButton *>())
        if (b->toolTip() == tip)
            return b;
    return nullptr;
}

void TestWindowTitleBar::normalModeConfiguration() {
    QWidget host;
    WindowTitleBar bar(&host);
    QCOMPARE(bar.minimumSize().height(), 32);
    QVERIFY(buttonByTip(bar, QStringLiteral("Minimize"))->isVisibleTo(&bar));
    QVERIFY(buttonByTip(bar, QStringLiteral("Maximize"))->isVisibleTo(&bar));
    QVERIFY(buttonByTip(bar, QStringLiteral("Close"))->isVisibleTo(&bar));
    QVERIFY(!buttonByTip(bar, QStringLiteral("Menu"))->isVisibleTo(&bar));
    QVERIFY(!buttonByTip(bar, QStringLiteral("Exit Full Screen"))->isVisibleTo(&bar));
}

void TestWindowTitleBar::miniAndFloatingConfiguration() {
    QWidget host;
    WindowTitleBar bar(&host);
    bar.setMode(WindowTitleBar::Mode::Mini);
    QCOMPARE(bar.minimumSize().height(), App::miniTitleBarHeight());
    QVERIFY(buttonByTip(bar, QStringLiteral("Close"))->isVisibleTo(&bar));
    QVERIFY(!buttonByTip(bar, QStringLiteral("Minimize"))->isVisibleTo(&bar));
    QVERIFY(!buttonByTip(bar, QStringLiteral("Maximize"))->isVisibleTo(&bar));
    QVERIFY(!buttonByTip(bar, QStringLiteral("Exit Full Screen"))->isVisibleTo(&bar));
    QVERIFY(!buttonByTip(bar, QStringLiteral("Menu"))->isVisibleTo(&bar));
    bar.setMode(WindowTitleBar::Mode::Floating);
    QCOMPARE(bar.minimumSize().height(), 32);
    QVERIFY(buttonByTip(bar, QStringLiteral("Exit Full Screen"))->isVisibleTo(&bar));
    QVERIFY(buttonByTip(bar, QStringLiteral("Close"))->isVisibleTo(&bar));
    QVERIFY(!buttonByTip(bar, QStringLiteral("Minimize"))->isVisibleTo(&bar));
    QVERIFY(!buttonByTip(bar, QStringLiteral("Maximize"))->isVisibleTo(&bar));
}

void TestWindowTitleBar::menuButtonGating() {
    QWidget host;
    WindowTitleBar bar(&host);
    bar.setMenuButtonVisible(true);
    QVERIFY(buttonByTip(bar, QStringLiteral("Menu"))->isVisibleTo(&bar));
    bar.setMode(WindowTitleBar::Mode::Mini);
    QVERIFY(!buttonByTip(bar, QStringLiteral("Menu"))->isVisibleTo(&bar));
    bar.setMode(WindowTitleBar::Mode::Floating);
    bar.setMenuButtonVisible(true);
    QVERIFY(!buttonByTip(bar, QStringLiteral("Menu"))->isVisibleTo(&bar));
    bar.setMode(WindowTitleBar::Mode::Normal);
    QVERIFY(buttonByTip(bar, QStringLiteral("Menu"))->isVisibleTo(&bar)); // 回 Normal 恢复意愿
}

void TestWindowTitleBar::noFocusPolicy() {
    QWidget host;
    WindowTitleBar bar(&host);
    QCOMPARE(bar.focusPolicy(), Qt::NoFocus);
    for (QAbstractButton *b : bar.findChildren<QAbstractButton *>())
        QCOMPARE(b->focusPolicy(), Qt::NoFocus);
}

void TestWindowTitleBar::buttonClicksEmitSignals() {
    QWidget host;
    WindowTitleBar bar(&host);
    QSignalSpy minimize(&bar, &WindowTitleBar::minimizeRequested);
    QSignalSpy maximize(&bar, &WindowTitleBar::maximizeRestoreRequested);
    QSignalSpy close(&bar, &WindowTitleBar::closeRequested);
    QSignalSpy menu(&bar, &WindowTitleBar::menuRequested);
    buttonByTip(bar, QStringLiteral("Minimize"))->click();
    buttonByTip(bar, QStringLiteral("Maximize"))->click();
    buttonByTip(bar, QStringLiteral("Close"))->click();
    QCOMPARE(minimize.count(), 1);
    QCOMPARE(maximize.count(), 1);
    QCOMPARE(close.count(), 1);
    QCOMPARE(menu.count(), 0);
    bar.setMenuButtonVisible(true);
    buttonByTip(bar, QStringLiteral("Menu"))->click();
    QCOMPARE(menu.count(), 1);
    QCOMPARE(menu.first().at(0).typeId(), int(QMetaType::QPoint)); // globalPos 载荷
    bar.setMode(WindowTitleBar::Mode::Floating);
    buttonByTip(bar, QStringLiteral("Exit Full Screen"))->click();
    QCOMPARE(maximize.count(), 2); // 退出全屏同发 maximizeRestoreRequested
}

void TestWindowTitleBar::hostPointerStableAfterReparent() {
    QWidget host1, host2;
    WindowTitleBar bar(&host1);
    QCOMPARE(bar.hostWindow(), &host1);
    bar.setParent(&host2); // 模拟全屏浮窗重挂：父窗变、宿主指针不变
    QCOMPARE(bar.hostWindow(), &host1);
    QCOMPARE(bar.window(), &host2);
    // 标题回显渲染烟测：现读宿主标题绘制（重挂后仍读原宿主），不崩、不出空图
    host1.setWindowTitle(QStringLiteral("Oxbow Test"));
    QVERIFY(!bar.grab().isNull());
}

void TestWindowTitleBar::maximizeGlyphFollowsWindowState() {
    QWidget host;
    WindowTitleBar bar(&host);
    QVERIFY(!bar.maximizeGlyphRestore());
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    host.showMaximized();
    QTRY_VERIFY(bar.maximizeGlyphRestore());
    host.showNormal();
    QTRY_VERIFY(!bar.maximizeGlyphRestore());
}

void TestWindowTitleBar::maximizeButtonNativeState() {
    QWidget host;
    WindowTitleBar bar(&host); // 第二参缺省：组件是顶层窗，需自行 show 才有布局几何
    bar.resize(400, 32);
    bar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&bar));
    const QRect maxRect = bar.maximizeButtonRect();
    QVERIFY(!maxRect.isEmpty());
    QCOMPARE(maxRect.size(), QSize(46, 32));
    QVERIFY(QRect(QPoint(0, 0), bar.size()).contains(maxRect));
    // 紧贴关闭钮左侧（布局 spacing 0）
    QCOMPARE(maxRect.right() + 1, buttonByTip(bar, QStringLiteral("Close"))->geometry().left());
    // 覆盖态无此按钮；回到 Normal 重新给出
    bar.setMode(WindowTitleBar::Mode::Mini);
    QVERIFY(bar.maximizeButtonRect().isEmpty());
    bar.setMode(WindowTitleBar::Mode::Floating);
    QVERIFY(bar.maximizeButtonRect().isEmpty());
    bar.setMode(WindowTitleBar::Mode::Normal);
    QVERIFY(!bar.maximizeButtonRect().isEmpty());
    // 按下回灌（原生 NC 路径）：可读写，形态切换即作废
    QVERIFY(!bar.maximizeButtonDown());
    bar.setMaximizeButtonDown(true);
    QVERIFY(bar.maximizeButtonDown());
    bar.setMode(WindowTitleBar::Mode::Mini);
    QVERIFY(!bar.maximizeButtonDown());
}

QTEST_MAIN(TestWindowTitleBar)

#include "tst_WindowTitleBar.moc"

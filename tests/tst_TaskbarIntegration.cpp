// N127：任务栏表现面纯推导（TaskbarState.h）单元测试。
// 覆盖：空闲优先、暂停 / 播放徽标、时长未知、比例钳制与非法值。
// 无 GUI 依赖，一类一可执行（R5）：ctest --test-dir build -C Debug -R TaskbarIntegration；
//   oxbow_tests_TaskbarIntegration <槽名> 跑单个用例。

#include "pch.h"

#include <QtTest/QtTest>

#include "platform/TaskbarState.h"

class TestTaskbarIntegration : public QObject {
    Q_OBJECT

  private slots:
    // 空闲压过一切：暂停位为真也不挂徽标、不显示进度、三键置灰
    void stateIdle();
    // 非空闲 + 暂停：暂停档 + 暂停徽标，比例照位置算
    void statePaused();
    // 非空闲 + 未暂停 + 有时长：正常档 + 播放徽标
    void statePlaying();
    // 时长未知（0 / 负 / NaN / Inf）：不显示进度，徽标与三键仍生效
    void stateUnknownDuration();
    // 比例钳制：负位置夹到 0、越界夹到 1、极小 duration 仍为 1、NaN 位置按 0
    void progressClamp();
};

void TestTaskbarIntegration::stateIdle() {
    const TaskbarState state = deriveTaskbarState(true, true, 10.0, 100.0);
    QVERIFY(state.progress == TaskbarProgress::None);
    QVERIFY(!state.showBadge);
    QVERIFY(!state.buttonsEnabled);
    QCOMPARE(state.fraction, 0.0);
}

void TestTaskbarIntegration::statePaused() {
    const TaskbarState state = deriveTaskbarState(false, true, 25.0, 100.0);
    QVERIFY(state.progress == TaskbarProgress::Paused);
    QVERIFY(state.showBadge);
    QVERIFY(state.badgePaused);
    QVERIFY(state.buttonsEnabled);
    QCOMPARE(state.fraction, 0.25);
}

void TestTaskbarIntegration::statePlaying() {
    const TaskbarState state = deriveTaskbarState(false, false, 50.0, 200.0);
    QVERIFY(state.progress == TaskbarProgress::Normal);
    QVERIFY(state.showBadge);
    QVERIFY(!state.badgePaused);
    QVERIFY(state.buttonsEnabled);
    QCOMPARE(state.fraction, 0.25);
}

void TestTaskbarIntegration::stateUnknownDuration() {
    const double values[] = {0.0, -5.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()};
    for (double duration : values) {
        const TaskbarState state = deriveTaskbarState(false, false, 10.0, duration);
        QVERIFY(state.progress == TaskbarProgress::None);
        QVERIFY(state.showBadge);
        QVERIFY(!state.badgePaused);
        QVERIFY(state.buttonsEnabled);
    }

    // 暂停位不因时长未知而丢失
    const TaskbarState paused = deriveTaskbarState(false, true, 10.0, 0.0);
    QVERIFY(paused.progress == TaskbarProgress::None);
    QVERIFY(paused.showBadge);
    QVERIFY(paused.badgePaused);
    QVERIFY(paused.buttonsEnabled);
}

void TestTaskbarIntegration::progressClamp() {
    QCOMPARE(deriveTaskbarState(false, false, -10.0, 100.0).fraction, 0.0);
    QCOMPARE(deriveTaskbarState(false, false, 150.0, 100.0).fraction, 1.0);
    QCOMPARE(deriveTaskbarState(false, false, 0.001, 0.001).fraction, 1.0);
    QCOMPARE(
        deriveTaskbarState(false, false, std::numeric_limits<double>::quiet_NaN(), 100.0).fraction,
        0.0);
}

QTEST_GUILESS_MAIN(TestTaskbarIntegration)

#include "tst_TaskbarIntegration.moc"

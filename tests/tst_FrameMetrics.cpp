// 断言数值按 windowBorderWidth=1 / miniTitleBarHeight=24（窗框 = 2×26）、
// windowResizeBand=6 与 16:9 比例手工推得，常量改动须同步重推

#include "pch.h"

#include <QtTest/QtTest>

#include "Constants.h"
#include "ui/FrameMetrics.h"

using namespace FrameMetrics;

class TestFrameMetrics : public QObject {
    Q_OBJECT

  private slots:
    // 窗框尺寸与两组互逆换算（含奇数尺寸往返）
    void frameSizeAndInverse();
    // 右缘拖拽：宽为准反推高；反推轴（高）以中线为锚
    void rightEdgeDragWidthDriven();
    // 纯上缘拖拽：高为准反推宽、被拖轴对边为锚；等比提案幂等
    void topEdgeDragHeightDriven();
    // 视频区宽下限钳制（minWidth 语义钉为视频区宽）
    void minWidthClamp();
    // 工作区上限钳制（视频域扣窗框）与越界最小移动钳回
    void workAreaCapAndPositionClamp();
    // 吸附：带内贴齐、带外空转、角部两轴同贴、越过工作区不钳回（N68 第 8 条）
    void snapping();
    // 左缘拖拽：宽为准、被拖轴对边（右缘）为锚，最小宽钳制后右缘不动
    void leftEdgeDragAnchorsRight();
    // 下缘拖拽：高为准、被拖轴对边（上缘）为锚；等比提案幂等
    void bottomEdgeDragIdempotent();
    // 左上角：两轴各以对边为锚（右缘 + 下缘固定）
    void topLeftCornerAnchorsOpposite();
    // 热区边组合：四边 strip / 四角双向 / 角优先于边 / 中心空
    void hotZones();

  private:
    static const QRect &workArea(); // 逻辑工作区 (0,0,1920,1040)
    static double ratio();          // 16:9
};

const QRect &TestFrameMetrics::workArea() {
    static const QRect kWork(0, 0, 1920, 1040);
    return kWork;
}

double TestFrameMetrics::ratio() { return 400.0 / 225.0; }

void TestFrameMetrics::frameSizeAndInverse() {
    QCOMPARE(miniFrameSize(), QSize(2 * App::windowBorderWidth(),
                                    2 * App::windowBorderWidth() + App::miniTitleBarHeight()));
    const QSize video(400, 225);
    const QSize client = miniClientSize(video);
    QCOMPARE(client, QSize(402, 251));
    QCOMPARE(miniVideoSize(client), video);
    // 奇数尺寸往返不失真
    const QSize odd(331, 217);
    QCOMPARE(miniVideoSize(miniClientSize(odd)), odd);
}

void TestFrameMetrics::rightEdgeDragWidthDriven() {
    // 提案：右缘外扩至 500 宽（(100,100,500,257)）→ cw = 498 → chTrue = round(498/r) = 280；
    // 右缘拖拽左缘不动（对边为锚），高为反推轴以中线为锚（cy = 228 → top = 75）
    const QRect fixed =
        miniResizedRect(QRect(100, 100, 500, 257), Qt::RightEdge, ratio(), 200, workArea());
    QCOMPARE(fixed, QRect(100, 75, 500, 306));
}

void TestFrameMetrics::topEdgeDragHeightDriven() {
    // 等比提案原样返回（幂等）：窗口 (100,50,402,251) 上缘上移后仍恰为 16:9
    const QRect raw(100, 50, 402, 251);
    QCOMPARE(miniResizedRect(raw, Qt::TopEdge, ratio(), 200, workArea()), raw);
    // 非等比提案：高为准反推宽（cwTrue = round(574*r) = 1020），上缘拖拽下缘不动
    // （对边为锚：top = 80+600-26-574 = 80），宽为反推轴中线锚（cx = 304 → 左出界，
    // 钳回 0）
    const QRect fixed =
        miniResizedRect(QRect(100, 80, 408, 600), Qt::TopEdge, ratio(), 200, workArea());
    QCOMPARE(fixed, QRect(0, 80, 1022, 600));
}

void TestFrameMetrics::minWidthClamp() {
    // 拖窄到 60 宽提案：cw = 58 < 200 → 视频区宽钳到 200 → 窗口宽 202；
    // 右缘拖拽左缘不动；高 = 26 + round(200/r) = 26 + 113 = 139（中线锚）
    const QRect fixed =
        miniResizedRect(QRect(100, 100, 60, 257), Qt::RightEdge, ratio(), 200, workArea());
    QCOMPARE(fixed.left(), 100);
    QCOMPARE(fixed.width(), 202);
    QCOMPARE(fixed.height(), 139);
}

void TestFrameMetrics::workAreaCapAndPositionClamp() {
    // 宽超上限：cw = 2998 → 钳到 round(maxVideoH*r) = 1803（16:9 下高上限先绑定）
    // → 窗口 1805×1040；右缘拖拽左缘 300 不动 → 右缘出界 2105 > 1920 → 最小移动钳回 115
    const QRect fixed =
        miniResizedRect(QRect(300, 100, 3000, 257), Qt::RightEdge, ratio(), 200, workArea());
    QCOMPARE(fixed.width(), 1805);
    QCOMPARE(fixed.height(), 1040);
    QCOMPARE(fixed.left(), 115);
}

void TestFrameMetrics::snapping() {
    // 左缘带内（dLeft = 10 ≤ 16）：贴 0
    QCOMPARE(miniSnappedTopLeft(QRect(10, 500, 408, 257), workArea(), 16), QPoint(0, 500));
    // 带外（dLeft = 20 > 16）：空转
    QCOMPARE(miniSnappedTopLeft(QRect(20, 500, 408, 257), workArea(), 16), QPoint(20, 500));
    // 角部两轴同贴
    QCOMPARE(miniSnappedTopLeft(QRect(10, 8, 408, 257), workArea(), 16), QPoint(0, 0));
    // 右缘带内：右缘距工作区右界 10 → 右移 10 贴齐
    QCOMPARE(miniSnappedTopLeft(QRect(1502, 500, 408, 257), workArea(), 16), QPoint(1512, 500));
    // 底缘带内：距底 12 → 下移 12 贴齐
    QCOMPARE(miniSnappedTopLeft(QRect(100, 771, 408, 257), workArea(), 16), QPoint(100, 783));
    // 越过左界（dLeft < 0）：不钳回——用户拖出工作区不拽回（N68 第 8 条裁决）
    QCOMPARE(miniSnappedTopLeft(QRect(-5, 500, 408, 257), workArea(), 16), QPoint(-5, 500));
}

void TestFrameMetrics::leftEdgeDragAnchorsRight() {
    // 拖窄到 60 宽提案：cw = 58 < 200 → 视频区宽钳到 200 → 窗口宽 202；左缘拖拽
    // 右缘不动（右缘 360 固定 → 左缘 360-202 = 158）；高 = 26 + round(200/r) = 139，
    // 反推轴（高）中线锚（cy = 228 → top = 228-69 = 159）
    const QRect fixed =
        miniResizedRect(QRect(300, 100, 60, 257), Qt::LeftEdge, ratio(), 200, workArea());
    QCOMPARE(fixed, QRect(158, 159, 202, 139));
}

void TestFrameMetrics::bottomEdgeDragIdempotent() {
    // 等比提案原样返回（幂等）：下缘拖拽上缘不动（top = 50），宽为反推轴中线锚
    // （cx = 301 → left = 301-201 = 100）
    const QRect raw(100, 50, 402, 251);
    QCOMPARE(miniResizedRect(raw, Qt::BottomEdge, ratio(), 200, workArea()), raw);
}

void TestFrameMetrics::topLeftCornerAnchorsOpposite() {
    // 左上角：宽为准（含左边即宽驱动），两轴各以对边为锚——右缘 360 固定 → 左 158，
    // 下缘 357 固定 → 上 357-139 = 218；最小宽钳到 200 → 窗口 202×139
    const QRect fixed = miniResizedRect(QRect(300, 100, 60, 257), Qt::TopEdge | Qt::LeftEdge,
                                        ratio(), 200, workArea());
    QCOMPARE(fixed, QRect(158, 218, 202, 139));
}

void TestFrameMetrics::hotZones() {
    const QSize win(800, 600);                                  // band = 6，角 = 12
    QCOMPARE(hotZoneEdges(win, QPoint(400, 300)), Qt::Edges()); // 中心空
    QCOMPARE(hotZoneEdges(win, QPoint(3, 300)), Qt::Edges(Qt::LeftEdge));
    QCOMPARE(hotZoneEdges(win, QPoint(797, 300)), Qt::Edges(Qt::RightEdge));
    QCOMPARE(hotZoneEdges(win, QPoint(400, 3)), Qt::Edges(Qt::TopEdge));
    QCOMPARE(hotZoneEdges(win, QPoint(400, 597)), Qt::Edges(Qt::BottomEdge));
    QCOMPARE(hotZoneEdges(win, QPoint(5, 5)), Qt::Edges(Qt::LeftEdge | Qt::TopEdge));
    QCOMPARE(hotZoneEdges(win, QPoint(795, 5)), Qt::Edges(Qt::RightEdge | Qt::TopEdge));
    QCOMPARE(hotZoneEdges(win, QPoint(5, 595)), Qt::Edges(Qt::LeftEdge | Qt::BottomEdge));
    QCOMPARE(hotZoneEdges(win, QPoint(795, 595)), Qt::Edges(Qt::RightEdge | Qt::BottomEdge));
    // 角优先于边：(10,3) 落在左角区（x<12）又在上边带（y<6）→ 给双向而非单上
    QCOMPARE(hotZoneEdges(win, QPoint(10, 3)), Qt::Edges(Qt::LeftEdge | Qt::TopEdge));
    // 角区外、边带内：(13,3) x≥12 退出左角区 → 只上边
    QCOMPARE(hotZoneEdges(win, QPoint(13, 3)), Qt::Edges(Qt::TopEdge));
}

QTEST_GUILESS_MAIN(TestFrameMetrics)

#include "tst_FrameMetrics.moc"

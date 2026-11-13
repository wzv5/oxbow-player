#pragma once

#include "pch.h"

#include "Constants.h"

// 迷你几何换算与手势判定（逻辑坐标域；物理取整由调用点完成）
namespace FrameMetrics {

// 迷你窗框尺寸 = 视频区四周的可见边框（windowBorderWidth）+ 顶部迷你标题栏：
// 水平 = 左右两条边；垂直 = 上下两条边 + 标题栏
inline QSize miniFrameSize() {
    const int edge = App::windowBorderWidth();
    return QSize(2 * edge, 2 * edge + App::miniTitleBarHeight());
}

// 视频区尺寸 → 迷你客户区尺寸（互逆换算对）
inline QSize miniClientSize(const QSize &videoSize) { return videoSize + miniFrameSize(); }

// 迷你客户区尺寸 → 视频区尺寸（互逆换算对）
inline QSize miniVideoSize(const QSize &clientSize) { return clientSize - miniFrameSize(); }

// 自由缩放提案 → 保比修正后的窗口矩形。rawRect 是窗口矩形（无框下即客户区，
// 逻辑域），edges 是被拖的边组合，videoRatio = 视频宽/高（无流由调用方给
// 16:9，与进入路径同源），minWidth 是视频区宽下限（原 miniMinWidth，语义钉为
// 视频区宽），workArea 是所在屏工作区（逻辑域）。
// 语义与 N68 的 WM_SIZING 修正逐条对应（仅下限单位从客户区宽改为视频区宽）：
// ① 真值维按拖边朝向选定——纯上/下边以高为准反推宽，左右边与四角以宽为准
//    反推高（N68 变更记录第 6 条的实测结论：反着实现四边全成 no-op）；
// ② 真值维先钳入参 minWidth 与工作区上限（上限在视频域：工作区扣窗框）
//    再反推另一维，钳制落在反推之前才不破比例；
// ③ 锚点两轴分治——被拖轴以对边为锚（对边不动），被反推的轴以中线为锚
//    （两侧对称伸缩）；
// ④ 末了按最小移动原则把整窗钳回工作区（越界多少平移多少）。
inline QRect miniResizedRect(const QRect &rawRect, Qt::Edges edges, double videoRatio, int minWidth,
                             const QRect &workArea) {
    const QSize frame = miniFrameSize();
    const bool verticalEdge = edges & (Qt::LeftEdge | Qt::RightEdge);
    const bool horizontalEdge = edges & (Qt::TopEdge | Qt::BottomEdge);
    // 提案的视频区尺寸：真值维钳制与反推都在这个域进行
    const int cw = rawRect.width() - frame.width();
    const int ch = rawRect.height() - frame.height();
    // 工作区上限换到视频域（无框窗口矩形即客户区，扣窗框即视频区上限）
    const int maxVideoW = workArea.width() - frame.width();
    const int maxVideoH = workArea.height() - frame.height();
    int cwTrue;
    int chTrue;
    if (horizontalEdge && !verticalEdge) { // 纯上/下边：高为准，反推宽
        chTrue = qMax(ch, static_cast<int>(qRound(minWidth / videoRatio)));
        chTrue = qMin(chTrue, qMin(maxVideoH, static_cast<int>(qRound(maxVideoW / videoRatio))));
        cwTrue = qRound(chTrue * videoRatio);
    } else { // 左右边与四角：宽为准，反推高
        cwTrue = qMax(cw, minWidth);
        cwTrue = qMin(cwTrue, qMin(maxVideoW, static_cast<int>(qRound(maxVideoH * videoRatio))));
        chTrue = qRound(cwTrue / videoRatio);
    }
    // 锚点两轴分治：被拖轴对边为锚（对边不动），反推轴中线为锚（对称伸缩）
    const int cx = rawRect.x() + rawRect.width() / 2;
    const int cy = rawRect.y() + rawRect.height() / 2;
    QRect result;
    if (edges & Qt::LeftEdge)
        result.setLeft(rawRect.x() + rawRect.width() - frame.width() - cwTrue);
    else if (edges & Qt::RightEdge)
        result.setLeft(rawRect.x());
    else
        result.setLeft(cx - (frame.width() + cwTrue) / 2);
    if (edges & Qt::TopEdge)
        result.setTop(rawRect.y() + rawRect.height() - frame.height() - chTrue);
    else if (edges & Qt::BottomEdge)
        result.setTop(rawRect.y());
    else
        result.setTop(cy - (frame.height() + chTrue) / 2);
    result.setWidth(frame.width() + cwTrue);
    result.setHeight(frame.height() + chTrue);
    // 最小移动钳回工作区（两轴独立，越界多少移多少）；qMax 兜窗口宽于工作区的极端
    const int maxLeft = qMax(workArea.x(), workArea.x() + workArea.width() - result.width());
    const int maxTop = qMax(workArea.y(), workArea.y() + workArea.height() - result.height());
    result.moveTo(qBound(workArea.x(), result.x(), maxLeft),
                  qBound(workArea.y(), result.y(), maxTop));
    return result;
}

// 拖动吸附：判距基准是**未吸附原始位**（起点矩形 + 鼠标总位移，调用方保证不传
// 已吸附结果——N68 WM_MOVING 第①条同款纪律：拿上轮吸附输出判距会自反馈，慢速
// 拖动永远脱不开）。四边独立判距，带内整轴平移，可两轴同贴成角；脱离阈值即
// 带宽本身，无须另设迟滞——基准与鼠标位移一一对应，拖过 snap 自然脱开
inline QPoint miniSnappedTopLeft(const QRect &rawRect, const QRect &workArea, int snapDistance) {
    const int dLeft = rawRect.x() - workArea.x();
    const int dRight = workArea.x() + workArea.width() - (rawRect.x() + rawRect.width());
    const int dTop = rawRect.y() - workArea.y();
    const int dBottom = workArea.y() + workArea.height() - (rawRect.y() + rawRect.height());
    int shiftX = 0;
    if (dLeft >= 0 && dLeft <= snapDistance)
        shiftX = -dLeft;
    else if (dRight >= 0 && dRight <= snapDistance)
        shiftX = dRight;
    int shiftY = 0;
    if (dTop >= 0 && dTop <= snapDistance)
        shiftY = -dTop;
    else if (dBottom >= 0 && dBottom <= snapDistance)
        shiftY = dBottom;
    return rawRect.topLeft() + QPoint(shiftX, shiftY);
}

// 窗口坐标 → 缩放热区边组合（逻辑域）。Native 形态的 WM_NCHITTEST 与自驱动形态
// 的子控件边缘转发共用这一份判定。四角放大到边带两倍见方、判序优先于边 strip
// （抓角需两轴同时落带、目标比单边小，放大补偿瞄准难度）；角外按边带给单向
inline Qt::Edges hotZoneEdges(const QSize &windowSize, const QPoint &windowPos) {
    const int band = App::windowResizeBand();
    const int corner = band * 2;
    const int w = windowSize.width();
    const int h = windowSize.height();
    const int x = windowPos.x();
    const int y = windowPos.y();
    const bool left = x < corner;
    const bool right = x >= w - corner;
    const bool top = y < corner;
    const bool bottom = y >= h - corner;
    Qt::Edges edges;
    if ((left || right) && (top || bottom)) {
        if (left)
            edges |= Qt::LeftEdge;
        if (right)
            edges |= Qt::RightEdge;
        if (top)
            edges |= Qt::TopEdge;
        if (bottom)
            edges |= Qt::BottomEdge;
    } else {
        if (x < band)
            edges |= Qt::LeftEdge;
        if (x >= w - band)
            edges |= Qt::RightEdge;
        if (y < band)
            edges |= Qt::TopEdge;
        if (y >= h - band)
            edges |= Qt::BottomEdge;
    }
    return edges;
}

} // namespace FrameMetrics

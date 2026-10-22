#pragma once

#include "pch.h"

class MpvController;

struct TaskbarIntegrationImpl;

// N127：Windows 任务栏集成——覆盖图标（播放 / 暂停）、播放进度与预览工具条
// （上一 / 播放暂停 / 下一）。只吃 MpvController 的播放态推送与宿主窗口，不读主窗
// 与播放列表。
//
// 一次性工作，坑集中记在这里：
// 1. 任务栏按钮属于宿主顶层窗口。A18 全屏与 N83 迷你模式都经 setWindowFlags 重建
//    平台窗口，HWND 随之换新：进度与覆盖图标必须在 WinIdChange 后重放，工具条必须
//    等窗口可见并收到 TaskbarButtonCreated 后重加。本类不缓存 HWND。
// 2. **任何 ITaskbarList3 方法都要等 TaskbarButtonCreated**（MSDN 明写）。组件因此
//    建在 show() 之前，过滤器先就位才收得到；收到前所有下发一律空转。
// 3. 迷你态是 Qt::Tool，没有任务栏按钮；所有调用按无操作处理，不做特殊分支。
// 4. 工具条点击经 WM_COMMAND + THBN_CLICKED 回来，只能靠应用级原生事件过滤器接，
//    组件不拥有窗口类；三条命令直发 mpv，不经主窗。
// 5. 配色跟**系统**深浅色（读 AppsUseLightTheme），不跟 QStyleHints::colorScheme——
//    后者被 AppearanceManager 按应用配色写过，而预览工具条由系统绘制。
// 6. 预览裁切（SetThumbnailClip）在 Win11 实测被 shell 忽略：调用返回 S_OK、句柄与
//    矩形都正确，预览仍显示整窗（100x100 诊断矩形同样无效）。别再加回来。
class TaskbarIntegration : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

  public:
    TaskbarIntegration(MpvController *controller, QWidget *hostWindow);
    ~TaskbarIntegration() override;

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void reapplyAll();
    void applyProgress(bool force);
    void applyOverlay(bool force);
    void ensureButtons();
    void updateButtons();
    void buildButtonIcons();
    void dropIcons();

    std::unique_ptr<TaskbarIntegrationImpl> m_impl;
};

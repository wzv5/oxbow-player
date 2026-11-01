#pragma once

#include "pch.h"

#include "ai/AiSubtitleJob.h"

// N146：AI 字幕生成对话框——非模态单例，按需创建、关闭即销毁（节律照 SubtitleBrowserDialog；
// WA_QuitOnClose = false，退出收口只归 SHUTDOWN）。任务对象归主窗持有，本类只是视图：
// 自检在对话框打开时算一次并当场定格八项设置快照，此后设置改动不再影响本次会话。
// 关闭窗口等同取消：先对在跑任务 cancel 收口，再销毁视图，不补弹任何提示。
// **不设父窗**（创建时传 nullptr）：带属主的顶层窗进不了任务栏，置空才有任务栏
// 按钮与最小化落点；代价是失去 owned 语义（不随主窗隐藏），销毁链由主窗
// finished→deleteLater 与析构兜底 delete 补齐。
// 布局纪律：键值对用两列网格（QLabel 的 \t 停不到同一列，网格等效"tab 分隔"且全列
// 对齐）；值列水平 Ignored，对话框最小 / 初始宽度固定 260，拉大时空白沉底（末端
// stretch），当前文件值超宽时按 ElideMiddle 折叠（label 的 Resize 事件里现算）。
class AiSubtitleDialog : public QDialog {
    Q_OBJECT
  public:
    // 点开始的现读复核与当前文件行的现读，两个出口都由主窗注入
    using StartContextProvider = std::function<AiSubtitleStartContext()>;
    using CurrentFileProvider = std::function<QString()>;

    AiSubtitleDialog(AiSubtitleJob *job, const StartContextProvider &startContext,
                     const CurrentFileProvider &currentFile, QWidget *parent);

    // 主窗推送：当前文件行无任务时刷新（任务进行中固定为捕获项，不跟随）
    void setCurrentFile(const QString &fileName);

  protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void runSelfCheck();
    void updateStartButton();
    void startGeneration();
    void setStepDisplay(AiSubtitleJob::Step step);
    void setProgress(double ratio);
    void resetAfterTerminal();
    void showMessage(const QString &text);
    void refreshCurrentFileValue();

    AiSubtitleJob *m_job;
    StartContextProvider m_startContext;
    CurrentFileProvider m_currentFile;

    // 自检定格的八项设置快照 + 点开始时的"启用翻译"勾选（此后设置改动不再影响它）
    AiSubtitleJob::StartParams m_snapshot;
    bool m_ffmpegAvailable = false;
    bool m_asrAvailable = false;
    bool m_translationAvailable = false;
    bool m_selfCheckDone = false;
    bool m_running = false; // 视图侧镜像：按钮与勾选框的切换依据
    bool m_closing = false; // 关闭触发的取消：终态回调不再复位 UI
    // 当前文件行现值（完整文件名；折叠显在值 label 上）——无任务时随在播刷新，
    // 点开始即定格为任务捕获项，此后不再跟随切文件
    QString m_currentFileName;

    QLabel *m_ffmpegCheckValue = nullptr;
    QLabel *m_asrCheckValue = nullptr;
    QLabel *m_translationCheckValue = nullptr;
    QCheckBox *m_enableTranslation = nullptr;
    QCheckBox *m_autoClose = nullptr;
    QLabel *m_currentFileValue = nullptr;
    QLabel *m_stepValue = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_startButton = nullptr;
};

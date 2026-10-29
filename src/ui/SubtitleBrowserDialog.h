#pragma once

#include "pch.h"

#include "SubtitleExtractor.h"

class SubtitleBrowserModel; // 行模型，定义在实现文件

// N143：字幕浏览器面板。无模态对话框（show 非 exec），父指针挂主窗；顶部一行工具栏
// （提前 / 偏移编辑框 / 延后 / 重置 / 定位当前行 / 搜索）+ 主体两列列表。取数与解析
// 全在 SubtitleExtractor，本类只管触发与呈现：
//   - 触发点：轨道表变更（切流、切文件后的新轨道表都落在这上面）、停止态清空、面板
//     打开那一刻——全部收口到 refresh()；面板不可见时只存态、不取数，同一请求已在飞
//     或已出表即无事可做。
//   - 当前行：time-pos 推送经 0 间隔单发定时器折叠后按自建时间轴重算，命中缓存 O(1)；
//     只刷新颜色不滚动，定位唯一入口是工具栏按钮。
//   - 遮罩：取数一开始即覆盖全窗，加载与出错共用；加载态带取消按钮，关闭对话框与
//     取消同一收口（关 = 取消）。
class SubtitleBrowserDialog : public QDialog {
    Q_OBJECT
  public:
    SubtitleBrowserDialog(MpvController *controller, SubtitleExtractor *extractor,
                          QWidget *parent = nullptr);

    // 主窗推送的播放态（面板不可见时只存态）
    void setTracks(const QString &mediaPath, const QList<MpvController::TrackInfo> &tracks);
    void setMediaIdle();
    void setTimePos(double seconds);
    void setSubDelay(double delay);

  protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;

  private:
    void buildToolbar(QVBoxLayout *layout);
    void refresh();
    void showMask(const QString &message, bool cancelable);
    void showLoadingMask();
    void hideMask();
    void applyEntries(const QList<SubtitleExtractor::SubtitleEntry> &entries);
    void applyFailure(SubtitleExtractor::Failure reason);
    void computeCurrentRow(double timePos);
    void locateCurrentRow();
    void searchNext();
    void commitOffset();
    void updateOffsetDisplay();
    void retintIcons();
    void copySelectedTexts();

    MpvController *m_controller = nullptr;
    SubtitleExtractor *m_extractor = nullptr; // 主窗持有，不由本对话框析构
    SubtitleBrowserModel *m_model = nullptr;
    QTableView *m_view = nullptr;
    QLineEdit *m_offsetEdit = nullptr;
    QLineEdit *m_searchBox = nullptr;
    QAction *m_searchAction = nullptr; // 搜索框 trailing 装饰图标（retintIcons 供）
    QToolButton *m_earlierButton = nullptr;
    QToolButton *m_laterButton = nullptr;
    QToolButton *m_resetButton = nullptr;
    QToolButton *m_locateButton = nullptr;
    // 遮罩：覆盖整个对话框（含工具栏），加载与出错共用同一控件
    QWidget *m_mask = nullptr;
    QLabel *m_maskLabel = nullptr;
    QProgressBar *m_maskProgress = nullptr; // 转取进度；总时长未知时落忙碌态
    QPushButton *m_maskCancel = nullptr;

    QString m_mediaPath;
    QList<MpvController::TrackInfo> m_tracks;
    bool m_idle = true;
    double m_delay = 0.0;
    QTimer m_timePosCoalescer; // 推送折叠（0 间隔、非周期，以最后一次值为准）
    double m_pendingTimePos = 0.0;
    int m_currentRow = -1;      // 与模型当前行镜像（模型侧为绘制事实源）
    bool m_offsetDirty = false; // 偏移编辑框有未提交输入：observe 推送不回写显示
    // 搜索锚点（照播放列表，另加条目表代数——表重建后旧行号不作续搜）
    QString m_searchQuery;
    qint64 m_generationAtAnchor = 0;
    int m_searchAnchor = -1;
    qint64 m_entriesGeneration = 0;
};

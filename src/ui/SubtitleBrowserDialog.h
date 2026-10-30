#pragma once

#include "pch.h"

#include "SubtitleExtractor.h"

class SubtitleBrowserModel; // 行模型，定义在实现文件

// 正播媒体可否作为内嵌轨的取数输入。判据在调用方一侧——认得在播条目的只有主窗，
// 取数模块不该收到请求之后再回绝；外部轨不看这一位（只看外部字幕文件自身）。
enum class SubtitleSourceAccess {
    Fetchable,    // 实体申报允许第二条解码流：本地文件与 crypt 转接都落在这一档
    NotFetchable, // 其余：远端直链、未放行的转接来源、认不出在播条目
};

// 主窗随轨道表推送现读的在播条目状态，面板保存后转交取数请求
struct SubtitleRequestContext {
    SubtitleSourceAccess access = SubtitleSourceAccess::NotFetchable;
    SubtitleEntryVersion version; // 原样转交，面板不解读
};

// N143：字幕浏览器面板。无模态对话框（show 非 exec），父指针挂主窗；顶部一行工具栏
// （提前 / 偏移编辑框 / 延后 / 重置 / 定位当前行 / 刷新 / 搜索）+ 主体两列列表。取数与
// 解析全在 SubtitleExtractor，本类只管触发与呈现：
//   - 触发点：轨道表变更（切流、切文件后的新轨道表都落在这上面）、停止态清空、面板
//     打开那一刻——全部收口到 runFetch(false)；面板不可见时只存态、不取数，同一请求
//     已在飞或已出表即无事可做。刷新按钮走 runFetch(true)：跳过同请求短路并丢弃同键
//     缓存完整重取（错误终态兼作重试入口）。
//   - 内嵌轨在媒体不可取时根本不发请求，与"没有可用文本轨"同走清空收口。
//   - 当前行：time-pos 推送经 0 间隔单发定时器折叠后按自建时间轴重算，命中缓存 O(1)；
//     只刷新颜色不滚动，定位唯一入口是工具栏按钮。
//   - 遮罩：只覆盖列表区、半透明，加载与出错共用；工具栏不禁用——偏移三件照常生效、
//     刷新照常可点、定位与搜索静默无效；列表被挡指针事件且期间拒绝获焦（Tab 循环不得
//     移入）。加载态带取消按钮，关闭对话框与取消同一收口（关 = 取消）。
class SubtitleBrowserDialog : public QDialog {
    Q_OBJECT
  public:
    SubtitleBrowserDialog(MpvController *controller, SubtitleExtractor *extractor,
                          QWidget *parent = nullptr);

    // 主窗推送的播放态（面板不可见时只存态）
    void setTracks(const QString &mediaPath, const QList<MpvController::TrackInfo> &tracks,
                   const SubtitleRequestContext &context);
    void setMediaIdle();
    void setTimePos(double seconds);
    void setSubDelay(double delay);

  protected:
    void showEvent(QShowEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void buildToolbar(QVBoxLayout *layout);
    void runFetch(bool force);
    void showMask(const QString &message, bool cancelable);
    void showLoadingMask();
    void hideMask();
    void updateMaskTint();
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
    QToolButton *m_reloadButton = nullptr; // 丢弃同键缓存完整重取（兼错误重试）
    QWidget *m_mask = nullptr;             // 只盖列表区，加载与出错共用同一控件
    QLabel *m_maskLabel = nullptr;
    QProgressBar *m_maskProgress = nullptr; // 转取进度；总时长未知时落忙碌态
    QPushButton *m_maskCancel = nullptr;

    QString m_mediaPath;
    QList<MpvController::TrackInfo> m_tracks;
    bool m_idle = true;
    double m_delay = 0.0;
    SubtitleRequestContext m_context;                    // 随轨道表推送下传，取数请求转交
    Qt::FocusPolicy m_viewFocusPolicy = Qt::StrongFocus; // 遮罩由隐转显那一拍现捕，转隐时归还
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

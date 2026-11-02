#pragma once

#include "pch.h"

#include "installer/DependencyCatalog.h"

class DependencyInstallJob;

// N150：可选依赖安装向导（非模态、可最小化、按需创建）。两个页面共用一个内部 Page，
// isComplete / validatePage 由 std::function 提供；每行的三列事实收在 Row 里（行指针 +
// 目录层的 RowFacts + 勾选态），列文案、状态行、计划与确认正文一律由 DependencyCatalog
// 的纯函数算出来。
class DependencyInstallerWizard : public QWizard {
    Q_OBJECT
  public:
    class Page : public QWizardPage {
      public:
        using QWizardPage::QWizardPage;
        bool isComplete() const override { return m_isComplete && m_isComplete(); }
        bool validatePage() override { return !m_validate || m_validate(); }
        void refreshComplete() { emit completeChanged(); }
        std::function<bool()> m_isComplete;
        std::function<bool()> m_validate;
    };

    // executionRefresh：执行收口后由主窗回调（重解析外部工具 / 重预载 MediaInfo）
    DependencyInstallerWizard(DependencyInstallJob *job, const QString &pluginDir,
                              std::function<void()> executionRefresh, QWidget *parent = nullptr);

  protected:
    void reject() override; // Cancel / Esc / 关窗同一入口
    void initializePage(int id) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    struct Row {
        QTreeWidgetItem *item = nullptr;
        DependencyCatalog::RowFacts facts;
    };

    void buildSelectPage();
    void buildExecutePage();
    void populateTree();
    // 开一轮查询会话：清本会话的 host 失败记录、按插件目录重探重查
    void startQuerySession();
    void renderRow(DependencyCatalog::Id id);
    void renderStatus();
    void setCheckState(Row &row, Qt::CheckState state);
    // 三态循环的唯一入口：点击与旁路翻转（键盘一类）都按循环表裁定，默认翻转不算数
    void applyCheckCycle(Row &row);
    Row *rowOfItem(QTreeWidgetItem *item);
    Row *rowOfIndex(int id);
    bool isSettled() const;
    QVector<DependencyCatalog::RowFacts> facts() const;
    QVector<DependencyCatalog::PlanEntry> currentPlan() const;

    bool ensureWritable(); // 探针失败即弹窗并留在第一页
    bool askConfirmation(const QVector<DependencyCatalog::PlanEntry> &plan);
    void connectJob();

    void onProbeReady(int id, const QString &version, const QString &rawOutput);
    void onQueryReady(int id, bool queryOk, const QString &version, qint64 downloadBytes,
                      const QString &error);
    void onStepText(const QString &text);
    void onProgress(qint64 done, qint64 total);
    void onLogLine(const QString &line);
    void onFinished(int installed, int removed, int failed, int skipped);

    DependencyInstallJob *m_job = nullptr;
    QString m_pluginDir;
    std::function<void()> m_executionRefresh;
    std::array<Row, DependencyCatalog::kComponentCount> m_rows;
    bool m_checkGuard = false;
    bool m_queriesRunning = false;
    bool m_executing = false;
    bool m_cancelRequested = false;
    QVector<DependencyCatalog::PlanEntry> m_plan;
    Page *m_selectPage = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_stepLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPlainTextEdit *m_logBox = nullptr;
};

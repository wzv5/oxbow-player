#include "pch.h"

#include "ui/DependencyInstallerWizard.h"

#include "installer/DependencyInstallJob.h"
#include "installer/DependencySwap.h"
#include "installer/InstallerStrings.h"
#include "ui/AppearanceManager.h"

using namespace DependencyCatalog;
using namespace InstallerText;

namespace {

constexpr int kColumnCount = 5; // 名称 / 最新版本 / 已安装版本 / 下载大小 / 说明
constexpr int kNameColumn = 0;
constexpr int kLatestColumn = 1;
constexpr int kInstalledColumn = 2;
constexpr int kSizeColumn = 3;
constexpr int kDescriptionColumn = 4;
// 定宽两列版本 + 一列大小：内容驱动会让宽窄随当期版本号跳，读起来抓不住行
constexpr int kLatestColumnWidth = 110;
constexpr int kInstalledColumnWidth = 110;
constexpr int kSizeColumnWidth = 90;

constexpr int kRowIndexRole = Qt::UserRole;         // 行在 m_rows 里的下标
constexpr int kLogicalCheckRole = Qt::UserRole + 1; // 我们裁定的勾选态（物理态可能被旁路翻走）

// 点击是否落在条目的勾选指示器内
bool inCheckIndicator(QTreeWidget *tree, QTreeWidgetItem *row, const QPoint &globalPos) {
    const QModelIndex index = tree->indexFromItem(row, kNameColumn);
    QStyleOptionViewItem opt;
    opt.initFrom(tree);
    opt.rect = tree->visualRect(index);
    opt.features |= QStyleOptionViewItem::HasCheckIndicator;
    switch (row->checkState(kNameColumn)) {
    case Qt::Checked:
        opt.state |= QStyle::State_On;
        break;
    case Qt::PartiallyChecked:
        opt.state |= QStyle::State_NoChange;
        break;
    case Qt::Unchecked:
        opt.state |= QStyle::State_Off;
        break;
    }
    const QRect indicator =
        tree->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, tree);
    return indicator.isValid() && indicator.contains(tree->viewport()->mapFromGlobal(globalPos));
}

// 说明列超宽省略（全文另挂在条目 tooltip 上）。不设 Q_OBJECT：只覆 paint，无信号与槽
class DescriptionDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem tuned = option;
        initStyleOption(&tuned, index);
        tuned.text =
            QFontMetrics(tuned.font).elidedText(tuned.text, Qt::ElideRight, tuned.rect.width());
        QStyledItemDelegate::paint(painter, tuned, index);
    }
};

} // namespace

DependencyInstallerWizard::DependencyInstallerWizard(DependencyInstallJob *job,
                                                     const QString &pluginDir,
                                                     std::function<void()> executionRefresh,
                                                     QWidget *parent)
    : QWizard(parent, Qt::Window | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint |
                          Qt::WindowSystemMenuHint),
      m_job(job), m_pluginDir(pluginDir), m_executionRefresh(std::move(executionRefresh)) {
    setWindowTitle(wizText(kWizardTitle));
    setWizardStyle(QWizard::ClassicStyle);
    // 末页（执行页）禁用 Back：执行不可逆进页即跑，回去既不重算也不刷新
    setOption(QWizard::DisabledBackButtonOnLastPage);
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    setSizeGripEnabled(true);
    resize(840, 560);
    setButtonText(QWizard::NextButton, wizText(kButtonInstall));
    setButtonText(QWizard::FinishButton, wizText(kButtonClose));

    buildSelectPage();
    buildExecutePage();
    populateTree();
    connectJob();

    // 同一时刻只有一个作业：执行中的作业只附着显示，不另起一轮；查询中或空闲则开新会话
    if (job->phase() == DependencyInstallJob::Phase::Executing) {
        m_executing = true;
        // 构造期直接 setCurrentId 会被随后的 QWizard::restart 重排掉，落到显示后一拍。
        // 这一拍之前作业可能已收口（刚装完就点开）：那时按空闲处理，不带着空计划进执行页
        QTimer::singleShot(0, this, [this] {
            if (m_executing) {
                setCurrentId(1);
                return;
            }
            button(QWizard::CancelButton)->setEnabled(true); // 完成态禁用过 Cancel
            startQuerySession();
        });
    } else {
        startQuerySession();
    }
}

void DependencyInstallerWizard::startQuerySession() {
    m_queriesRunning = true;
    m_job->beginWizardSession(m_pluginDir);
    m_job->startQueries(m_pluginDir);
}

void DependencyInstallerWizard::buildSelectPage() {
    m_selectPage = new Page(this);
    Page *page = m_selectPage;
    page->setTitle(wizText(kPageSelectTitle));
    page->setSubTitle(wizText(kPageSelectSubtitle));

    auto *layout = new QVBoxLayout(page);
    auto *hint = new QLabel(page);
    hint->setWordWrap(true);
    hint->setText(wizText(kHintCheckStates) + QLatin1Char('\n') +
                  wizText(kHintInstallLocation, m_pluginDir.isEmpty()
                                                    ? emptyCell()
                                                    : QDir::toNativeSeparators(m_pluginDir)));
    layout->addWidget(hint);

    m_tree = new QTreeWidget(page);
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    m_tree->setColumnCount(kColumnCount);
    m_tree->setHeaderLabels({wizText(kColumnName), wizText(kColumnLatest),
                             wizText(kColumnInstalled), wizText(kColumnSize),
                             wizText(kColumnDescription)});
    QHeaderView *header = m_tree->header();
    header->setSectionResizeMode(kNameColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kLatestColumn, QHeaderView::Fixed);
    header->setSectionResizeMode(kInstalledColumn, QHeaderView::Fixed);
    header->setSectionResizeMode(kSizeColumn, QHeaderView::Fixed);
    header->setSectionResizeMode(kDescriptionColumn, QHeaderView::Stretch);
    m_tree->setColumnWidth(kLatestColumn, kLatestColumnWidth);
    m_tree->setColumnWidth(kInstalledColumn, kInstalledColumnWidth);
    m_tree->setColumnWidth(kSizeColumn, kSizeColumnWidth);
    m_tree->setItemDelegateForColumn(kDescriptionColumn, new DescriptionDelegate(m_tree));
    m_tree->viewport()->installEventFilter(this);
    layout->addWidget(m_tree, 1);

    m_statusLabel = new QLabel(page);
    layout->addWidget(m_statusLabel);

    page->m_isComplete = [this] { return isSettled() && !currentPlan().isEmpty(); };
    page->m_validate = [this] {
        const QVector<PlanEntry> plan = currentPlan();
        // 可写检查失败弹窗并留在本页、不动计划
        if (planChangesFiles(plan) && !ensureWritable())
            return false;
        if (!askConfirmation(plan))
            return false;
        m_plan = plan;
        return true;
    };
    setPage(0, page);
}

void DependencyInstallerWizard::buildExecutePage() {
    Page *page = new Page(this);
    page->setTitle(wizText(kPageExecuteTitle));
    page->setSubTitle(wizText(kHintClosingPlayer));

    auto *layout = new QVBoxLayout(page);
    m_stepLabel = new QLabel(page);
    layout->addWidget(m_stepLabel);
    m_progressBar = new QProgressBar(page);
    m_progressBar->setRange(0, 1);
    layout->addWidget(m_progressBar);
    m_logBox = new QPlainTextEdit(page);
    m_logBox->setReadOnly(true);
    m_logBox->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logBox->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(m_logBox, 1);
    // 完成态摘要借步骤行显示；Finish 等作业回报 finished 才可点
    page->m_isComplete = [this] { return !m_executing; };
    setPage(1, page);
}

void DependencyInstallerWizard::populateTree() {
    int groupCount = 0;
    const CategoryGroup *groups = categoryGroups(groupCount);
    for (int g = 0; g < groupCount; ++g) {
        auto *categoryRow = new QTreeWidgetItem(m_tree);
        categoryRow->setText(kNameColumn, catalogText(groups[g].title));
        for (int column = 1; column < kColumnCount; ++column)
            categoryRow->setText(column, {});
        categoryRow->setFlags(Qt::ItemIsEnabled); // 分类只承担展开与分组，不带勾选框
        for (Id id : groups[g].items) {
            const Component &comp = component(id);
            const int index = static_cast<int>(id);
            auto *item = new QTreeWidgetItem(categoryRow);
            item->setData(kNameColumn, kRowIndexRole, index);
            item->setText(kNameColumn, QString::fromLatin1(comp.name));
            const QString description = catalogText(comp.descriptionText);
            item->setText(kDescriptionColumn, description);
            item->setToolTip(kDescriptionColumn, description);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            item->setTextAlignment(kSizeColumn, Qt::AlignRight | Qt::AlignVCenter);
            Row &row = m_rows[static_cast<size_t>(index)];
            row.item = item;
            row.facts.installed =
                QFileInfo::exists(DependencySwap::pluginPath(m_pluginDir, installedMarkerOf(comp)));
            // 模型不探测：已安装列由本地事实现算，该行探测侧出生即落定
            row.facts.probeSettled = isModelComponent(comp);
            setCheckState(row, row.facts.installed ? Qt::PartiallyChecked : Qt::Unchecked);
            renderRow(id);
        }
    }
    m_tree->expandAll();
    renderStatus();
}

void DependencyInstallerWizard::renderRow(Id id) {
    Row &row = m_rows[static_cast<size_t>(id)];
    const Component &comp = component(id);
    if (!row.item)
        return;
    row.item->setText(kLatestColumn, latestVersionText(comp, row.facts));
    if (row.facts.querySettled && !isModelComponent(comp) && row.facts.query.version.isEmpty())
        row.item->setToolTip(kLatestColumn, versionFailureTip(row.facts.query.error));
    row.item->setText(kInstalledColumn, installedVersionText(comp, row.facts));
    // 探测原始输出进 tooltip：判不出版本时那是唯一线索
    row.item->setToolTip(kInstalledColumn, row.facts.probeRaw);
    row.item->setText(kSizeColumn, downloadSizeText(row.facts));
    if (row.facts.querySettled && row.facts.query.downloadBytes < 0)
        row.item->setToolTip(kSizeColumn, sizeFailureTip(row.facts.query.error));

    // 有新版本只高亮最新版本列；模型没有版本，永不参与
    if (hasNewerVersion(comp, row.facts))
        row.item->setForeground(kLatestColumn, AppearanceManager::availableForeground(palette()));
    else
        row.item->setData(kLatestColumn, Qt::ForegroundRole, QVariant());
}

void DependencyInstallerWizard::renderStatus() {
    m_statusLabel->setText(statusText(facts()));
    m_selectPage->refreshComplete();
}

void DependencyInstallerWizard::setCheckState(Row &row, Qt::CheckState state) {
    m_checkGuard = true;
    row.item->setCheckState(kNameColumn, state);
    row.item->setData(kNameColumn, kLogicalCheckRole, static_cast<int>(state));
    m_checkGuard = false;
}

void DependencyInstallerWizard::applyCheckCycle(Row &row) {
    const Id id = idAt(row.item->data(kNameColumn, kRowIndexRole).toInt());
    // 查询未落定前未装项锁定为空（该项结果未知）；已装项照常可循环
    if (!isSettled() && !row.facts.installed)
        return;
    const Qt::CheckState logical =
        static_cast<Qt::CheckState>(row.item->data(kNameColumn, kLogicalCheckRole).toInt());
    setCheckState(
        row, nextCheckState(logical, row.facts.installed, isAvailable(component(id), row.facts)));
    m_selectPage->refreshComplete();
}

DependencyInstallerWizard::Row *DependencyInstallerWizard::rowOfItem(QTreeWidgetItem *item) {
    if (!item || !item->parent() || !(item->flags() & Qt::ItemIsUserCheckable))
        return nullptr;
    const QVariant stored = item->data(kNameColumn, kRowIndexRole);
    if (!stored.isValid() || stored.toInt() < 0 || stored.toInt() >= kComponentCount)
        return nullptr;
    return &m_rows[static_cast<size_t>(stored.toInt())];
}

DependencyInstallerWizard::Row *DependencyInstallerWizard::rowOfIndex(int id) {
    if (id < 0 || id >= kComponentCount)
        return nullptr;
    Row &row = m_rows[static_cast<size_t>(id)];
    return row.item ? &row : nullptr;
}

bool DependencyInstallerWizard::isSettled() const { return allRowsSettled(facts()); }

QVector<RowFacts> DependencyInstallerWizard::facts() const {
    QVector<RowFacts> values;
    values.reserve(kComponentCount);
    for (const Row &row : m_rows)
        values.append(row.facts);
    return values;
}

QVector<PlanEntry> DependencyInstallerWizard::currentPlan() const {
    return executablePlan(
        [this](Id id) {
            return static_cast<Qt::CheckState>(
                m_rows[static_cast<size_t>(id)].item->data(kNameColumn, kLogicalCheckRole).toInt());
        },
        facts());
}

bool DependencyInstallerWizard::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_tree->viewport() && event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        Row *row = rowOfItem(m_tree->itemAt(mouse->pos()));
        if (row && inCheckIndicator(m_tree, row->item, mouse->globalPosition().toPoint())) {
            applyCheckCycle(*row);
            return true; // 指示器上的默认翻转一律不算数
        }
    }
    return QWizard::eventFilter(watched, event);
}

void DependencyInstallerWizard::connectJob() {
    connect(m_job, &DependencyInstallJob::probeReady, this,
            &DependencyInstallerWizard::onProbeReady);
    connect(m_job, &DependencyInstallJob::queryReady, this,
            &DependencyInstallerWizard::onQueryReady);
    connect(m_job, &DependencyInstallJob::stepText, this, &DependencyInstallerWizard::onStepText);
    connect(m_job, &DependencyInstallJob::progress, this, &DependencyInstallerWizard::onProgress);
    connect(m_job, &DependencyInstallJob::logLine, this, &DependencyInstallerWizard::onLogLine);
    connect(m_job, &DependencyInstallJob::finished, this, &DependencyInstallerWizard::onFinished);
    // 旁路翻转（键盘一类）一律按循环表纠正回写，与点击同一条裁定。判据只认"物理态与逻辑
    // 态分家"：文本 / tooltip / 前景一类改动同样发 itemChanged，不看这一眼就会替每格多翻一档
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_checkGuard)
            return;
        Row *row = rowOfItem(item);
        if (!row)
            return;
        const Qt::CheckState logical =
            static_cast<Qt::CheckState>(row->item->data(kNameColumn, kLogicalCheckRole).toInt());
        if (row->item->checkState(kNameColumn) == logical)
            return;
        applyCheckCycle(*row);
    });
}

void DependencyInstallerWizard::onProbeReady(int id, const QString &version,
                                             const QString &rawOutput) {
    Row *row = rowOfIndex(id);
    if (!row || row->facts.probeSettled)
        return;
    row->facts.probeVersion = version;
    row->facts.probeRaw = rawOutput;
    row->facts.probeSettled = true;
    renderRow(idAt(id));
    renderStatus();
}

void DependencyInstallerWizard::onQueryReady(int id, bool queryOk, const QString &version,
                                             qint64 downloadBytes, const QString &error) {
    Row *row = rowOfIndex(id);
    if (!row || row->facts.querySettled)
        return;
    row->facts.query.ok = queryOk;
    row->facts.query.version = version;
    row->facts.query.downloadBytes = downloadBytes;
    row->facts.query.error = error;
    row->facts.querySettled = true;
    renderRow(idAt(id));
    renderStatus();
}

void DependencyInstallerWizard::onStepText(const QString &text) { m_stepLabel->setText(text); }

void DependencyInstallerWizard::onProgress(qint64 done, qint64 total) {
    // 字节可能超过 int 上限：进度条走万分数
    constexpr qint64 kScale = 10000;
    m_progressBar->setRange(0, static_cast<int>(kScale));
    m_progressBar->setValue(
        static_cast<int>(qBound<qint64>(0, done * kScale / qMax<qint64>(total, 1), kScale)));
}

void DependencyInstallerWizard::onLogLine(const QString &line) {
    m_logBox->appendPlainText(line);
    m_logBox->verticalScrollBar()->setValue(m_logBox->verticalScrollBar()->maximum());
}

void DependencyInstallerWizard::onFinished(int installed, int removed, int failed, int skipped) {
    m_executing = false;
    m_cancelRequested = false;
    button(QWizard::CancelButton)->setEnabled(false);
    button(QWizard::FinishButton)->setEnabled(true);
    m_stepLabel->setText(
        wizText(kSummaryLine).arg(installed).arg(removed).arg(failed).arg(skipped));
    m_selectPage->refreshComplete();
    if (m_executionRefresh)
        m_executionRefresh();
}

bool DependencyInstallerWizard::ensureWritable() {
    QString error;
    if (!m_pluginDir.isEmpty()) {
        QTemporaryFile probe(QDir(m_pluginDir).filePath(QStringLiteral("oxbow-probe-XXXXXX")));
        probe.setAutoRemove(true);
        if (probe.open())
            return true;
        error = probe.errorString();
    } else {
        error = QStringLiteral("plugin directory unknown");
    }
    QMessageBox::warning(this, wizText(kDialogTitleNotWritable),
                         wizText(kDialogBodyNotWritable).arg(m_pluginDir, error));
    return false;
}

bool DependencyInstallerWizard::askConfirmation(const QVector<PlanEntry> &plan) {
    QString warning;
    if (planChangesFiles(plan)) {
        // 磁盘预检：峰值与可用容量比较，不足只在正文末尾加一行警告、不禁用
        const qint64 peak = planPeakDiskBytes(plan, [this](Id id) {
            return m_rows[static_cast<size_t>(id)].facts.query.downloadBytes;
        });
        const QStorageInfo storage(m_pluginDir);
        if (storage.isValid() && storage.bytesAvailable() >= 0 && peak > 0 &&
            storage.bytesAvailable() < peak)
            warning =
                wizText(kWarnDiskSpace)
                    .arg(displayDownloadSize(peak), displayDownloadSize(storage.bytesAvailable()));
    }

    const auto nameOf = [](Id id) { return QString::fromLatin1(component(id).name); };
    const auto sizeOf = [this](Id id) {
        return displayDownloadSize(m_rows[static_cast<size_t>(id)].facts.query.downloadBytes);
    };
    QString body =
        QStringLiteral("<pre>%1</pre>").arg(confirmationText(plan, nameOf, sizeOf).toHtmlEscaped());
    if (!warning.isEmpty())
        body += QLatin1Char('\n') + warning.toHtmlEscaped();
    return QMessageBox::question(this, windowTitle(), body, QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::Yes) == QMessageBox::Yes;
}

void DependencyInstallerWizard::initializePage(int id) {
    QWizard::initializePage(id);
    if (id == 0) {
        // 完成态处理器禁用过 Cancel：回到第一页必须恢复可点（取消查询 / 关窗）
        button(QWizard::CancelButton)->setEnabled(true);
        return;
    }
    button(QWizard::FinishButton)->setEnabled(false);
    if (m_executing)
        return; // 附着运行中的作业：日志与进度都留着，不另起执行
    // 查询期日志只属于第一页：自启执行前清空日志框
    m_logBox->clear();
    m_executing = true;
    m_job->startExecute(m_pluginDir, m_plan);
}

void DependencyInstallerWizard::reject() {
    if (!m_executing) {
        if (m_queriesRunning)
            m_job->cancel(); // 第一页关窗同样取消在途查询
        QWizard::reject();
        return;
    }
    if (m_cancelRequested)
        return; // 已请求取消：等作业回报 finished 才放行关窗
    const auto answer =
        QMessageBox::question(this, wizText(kConfirmCancelTitle), wizText(kConfirmCancelBody),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    m_cancelRequested = true;
    m_job->cancel();
    m_stepLabel->setText(wizText(kButtonCancelling));
    button(QWizard::CancelButton)->setEnabled(false);
}

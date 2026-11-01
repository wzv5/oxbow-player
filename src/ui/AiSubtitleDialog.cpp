#include "pch.h"

#include "AiSubtitleDialog.h"

#include "AppPaths.h"
#include "AppearanceManager.h"
#include "ExternalTool.h"
#include "ai/AiSubtitleCatalog.h"

namespace {

QString stepName(AiSubtitleJob::Step step) {
    switch (step) {
    case AiSubtitleJob::Step::WaitingToStart:
        return QCoreApplication::translate("AiSubtitleDialog", "Waiting to start");
    case AiSubtitleJob::Step::ExtractingAudio:
        return QCoreApplication::translate("AiSubtitleDialog", "Extracting audio");
    case AiSubtitleJob::Step::InitializingSpeechRecognition:
        return QCoreApplication::translate("AiSubtitleDialog", "Initializing ASR");
    case AiSubtitleJob::Step::SpeechRecognition:
        return QCoreApplication::translate("AiSubtitleDialog", "ASR");
    case AiSubtitleJob::Step::InitializingTranslation:
        return QCoreApplication::translate("AiSubtitleDialog", "Initializing translation");
    case AiSubtitleJob::Step::Translation:
        return QCoreApplication::translate("AiSubtitleDialog", "Translation");
    case AiSubtitleJob::Step::Completed:
        return QCoreApplication::translate("AiSubtitleDialog", "Completed");
    }
    return {};
}

} // namespace

AiSubtitleDialog::AiSubtitleDialog(AiSubtitleJob *job, const StartContextProvider &startContext,
                                   const CurrentFileProvider &currentFile, QWidget *parent)
    : QDialog(parent), m_job(job), m_startContext(startContext), m_currentFile(currentFile) {
    setWindowTitle(tr("Generate Subtitles"));
    setAttribute(Qt::WA_QuitOnClose, false); // 退出收口只归 SHUTDOWN
    // 构造期预置（先于 show，setWindowFlags 会 withdraw 窗口）：本窗不设父窗、
    // 类型位为 Qt::Window——带属主或 Dialog 位的顶层窗都不进任务栏，两者都避开
    // 才有任务栏按钮与最小化落点；最小化提示位一并给足
    setWindowFlags((windowFlags() & ~Qt::WindowType_Mask) | Qt::Window |
                   Qt::WindowMinimizeButtonHint);

    auto *layout = new QVBoxLayout(this);

    // 键值行用两列网格：值列水平 Ignored（不把长文本反灌进对话框最小宽度），
    // 全列对齐等效"tab 分隔"；分割线与两个勾选框也在网格里，保证各行顺序
    auto *grid = new QGridLayout;
    const auto makeRow = [this, grid](const QString &key, QLabel *&value, int row) {
        grid->addWidget(new QLabel(key, this), row, 0);
        value = new QLabel(this);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        grid->addWidget(value, row, 1);
    };
    // 键不带冒号：同一个词在翻译表里只成一条
    makeRow(tr("FFmpeg"), m_ffmpegCheckValue, 0);
    makeRow(tr("ASR"), m_asrCheckValue, 1);
    makeRow(tr("Translation"), m_translationCheckValue, 2);
    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    grid->addWidget(separator, 3, 0, 1, 2);
    makeRow(tr("Current file"), m_currentFileValue, 4);
    makeRow(tr("Step"), m_stepValue, 5);
    m_enableTranslation = new QCheckBox(tr("Enable translation"), this);
    m_autoClose = new QCheckBox(tr("Close dialog when finished"), this);
    grid->addWidget(m_enableTranslation, 6, 0, 1, 2);
    grid->addWidget(m_autoClose, 7, 0, 1, 2);
    grid->setColumnStretch(1, 1);
    m_currentFileValue->installEventFilter(this); // 值列 Resize 现算文件名折叠
    layout->addLayout(grid);

    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(false); // 只有条体，不显示任何数值文案
    layout->addWidget(m_progress);

    m_startButton = new QPushButton(tr("Start"), this);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    buttonRow->addWidget(m_startButton);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);
    layout->addStretch(1); // 拉大时空白沉底：行间距不随窗口高度摊开

    connect(m_enableTranslation, &QCheckBox::toggled, this,
            [this] { updateStartButton(); }); // 勾选框切换即时重算启用态
    connect(m_startButton, &QPushButton::clicked, this, [this] {
        if (m_running)
            m_job->cancel(); // 取消不弹提示，canceled 回调里直接复位
        else
            startGeneration();
    });

    // 任务信号 → 视图（job 与本类同在 GUI 线程，直连）
    connect(m_job, &AiSubtitleJob::stepChanged, this,
            [this](AiSubtitleJob::Step step) { setStepDisplay(step); });
    // 进度判据全在任务侧（八档唯一表），视图照收
    connect(m_job, &AiSubtitleJob::progressChanged, this,
            [this](double ratio) { setProgress(ratio); });
    connect(m_job, &AiSubtitleJob::rateLimitWaiting, this, [this](int) {
        // 限速等待：步骤仍停在"翻译"，进度不动（下一句的 progressChanged 恢复）
        m_stepValue->setText(tr("Waiting for rate limit…"));
    });
    connect(m_job, &AiSubtitleJob::succeeded, this, [this] {
        m_running = false;
        if (m_autoClose->isChecked()) {
            close(); // 已勾自动关闭：不弹提示、直接关闭（job 已 idle，close 不触发取消）
            return;
        }
        showMessage(tr("Subtitles generated successfully."));
        resetAfterTerminal();
    });
    connect(m_job, &AiSubtitleJob::failed, this, [this](const QString &reason) {
        m_running = false;
        // 步骤行停在出事的那一段（m_step 未动）；即使勾了自动关闭也照样弹提示
        showMessage(tr("Failed to generate subtitles: %1").arg(reason));
        resetAfterTerminal();
    });
    connect(m_job, &AiSubtitleJob::canceled, this, [this] {
        m_running = false;
        if (m_closing)
            return;           // 关闭触发的取消：视图即毁，不复位不提示
        resetAfterTerminal(); // 用户主动收尾：不弹提示，直接复位到等待开始
    });

    // 自检：打开时算一次（singleShot 让"checking"先上屏一轮）；判完定格快照
    m_ffmpegCheckValue->setText(tr("checking…"));
    m_asrCheckValue->setText(tr("checking…"));
    m_translationCheckValue->setText(tr("checking…"));
    m_stepValue->setText(tr("Self-check"));
    m_stepValue->setForegroundRole(QPalette::Link);
    m_startButton->setEnabled(false);
    m_enableTranslation->setEnabled(false);
    setCurrentFile(m_currentFile ? m_currentFile() : QString());
    QTimer::singleShot(0, this, [this] { runSelfCheck(); });

    // 宽度纪律：最小与初始都是 260；高度按内容，用户可调（多余空间沉底）
    setMinimumWidth(260);
    resize(260, sizeHint().height());
}

void AiSubtitleDialog::setCurrentFile(const QString &fileName) {
    if (m_running)
        return; // 点开始后固定为任务捕获项
    m_currentFileName = fileName;
    refreshCurrentFileValue();
}

void AiSubtitleDialog::refreshCurrentFileValue() {
    if (m_currentFileName.isEmpty()) {
        m_currentFileValue->setText(tr("—"));
        m_currentFileValue->setForegroundRole(QPalette::WindowText);
        m_currentFileValue->setToolTip({});
        return;
    }
    // 超宽折叠（ElideMiddle 保住扩展名可见）；完整名进 tooltip。值色走调色板
    // Link 画刷（前景角色），主题切换自动跟随，无需手动重染
    const int width = m_currentFileValue->width();
    m_currentFileValue->setText(width > 0 ? m_currentFileValue->fontMetrics().elidedText(
                                                m_currentFileName, Qt::ElideMiddle, width)
                                          : m_currentFileName);
    m_currentFileValue->setForegroundRole(QPalette::Link);
    m_currentFileValue->setToolTip(m_currentFileName);
}

bool AiSubtitleDialog::eventFilter(QObject *watched, QEvent *event) {
    // 值列宽度只在布局时变：Resize 现算折叠，容忍对话框被拉窄
    if (watched == m_currentFileValue && event->type() == QEvent::Resize &&
        !m_currentFileName.isEmpty())
        refreshCurrentFileValue();
    return QDialog::eventFilter(watched, event);
}

void AiSubtitleDialog::closeEvent(QCloseEvent *event) {
    m_closing = true; // 关闭窗口等同取消：先收口任务，再销毁视图（不补弹提示）
    if (m_running)
        m_job->cancel();
    QDialog::closeEvent(event);
}

void AiSubtitleDialog::runSelfCheck() {
    // 快照：判完当场定格，此后设置改动不再影响本次对话框会话
    const QString pluginDir = App::paths().pluginDir();
    QSettings settings(App::paths().iniFile(), QSettings::IniFormat);
    m_snapshot.settings = AiSubtitleCatalog::readSettings(settings);
    m_snapshot.enableTranslation = false; // 勾选框不进快照，点开始时现读

    // ffmpeg：只判解析出的路径非空（不查文件存在、不跑进程）——ini 显式值原样
    // 优先（存在性归运行机制），自动发现返回空串才判不可用
    m_ffmpegAvailable = !externalToolResolvedPath(
                             externalToolSpec(ExternalTool::Ffmpeg),
                             settings.value(externalToolIniKey(ExternalTool::Ffmpeg)).toString(),
                             App::paths().iniDir())
                             .isEmpty();
    // ASR 与翻译：判据分域各立（内置档判插件目录下定形后的程序与模型文件，其余档
    // 只判用户那一串非空——不跑进程、不查 PATH、不探端口可用性）
    m_asrAvailable = AiSubtitleCatalog::asrAvailable(m_snapshot.settings.asrMode,
                                                     m_snapshot.settings.asrCommand, pluginDir);
    m_translationAvailable = AiSubtitleCatalog::translationAvailable(
        m_snapshot.settings.translationMode, m_snapshot.settings.translationApi, pluginDir);

    m_selfCheckDone = true;
    const auto setCheckValue = [this](QLabel *label, bool available) {
        label->setText(available ? tr("available") : tr("unavailable"));
        // 语义色住 AppearanceManager（暗色档随之提亮）；调色板在自检这一帧现取，
        // 与快照同寿命——本次会话不跟随其后的主题切换
        QPalette palette = label->palette();
        palette.setColor(label->foregroundRole(),
                         available ? AppearanceManager::availableForeground(palette)
                                   : AppearanceManager::unavailableForeground(palette));
        label->setPalette(palette);
    };
    setCheckValue(m_ffmpegCheckValue, m_ffmpegAvailable);
    setCheckValue(m_asrCheckValue, m_asrAvailable);
    setCheckValue(m_translationCheckValue, m_translationAvailable);
    m_enableTranslation->setEnabled(true);
    updateStartButton();
    setStepDisplay(AiSubtitleJob::Step::WaitingToStart);

    // 判完若有任一项不可用：弹一个自检失败提示框（每次打开至多一次，与勾选态无关）；
    // 确认后照常等待开始。不列具体项——三行自检值当场就是明细
    if (!m_ffmpegAvailable || !m_asrAvailable || !m_translationAvailable)
        showMessage(tr("Some dependencies are unavailable. Please check the related settings."));
}

void AiSubtitleDialog::updateStartButton() {
    // 运行中恒可点（Cancel）；待发车启用态 = ffmpeg / ASR 可用 且（未勾"启用翻译"
    // 或翻译可用）；自检不通过保持禁用
    const bool checksPass = m_ffmpegAvailable && m_asrAvailable &&
                            (!m_enableTranslation->isChecked() || m_translationAvailable);
    m_startButton->setEnabled(m_running || (m_selfCheckDone && checksPass));
    m_startButton->setText(m_running ? tr("Cancel") : tr("Start"));
}

void AiSubtitleDialog::startGeneration() {
    // 点开始那一刻现读在播路径与实体复核（对话框非模态，打开后可能已经切了文件）；
    // 不通过则不发车并显示原因
    const AiSubtitleStartContext context = m_startContext();
    if (!context.error.isEmpty()) {
        showMessage(tr("Cannot start: %1").arg(context.error));
        return;
    }
    m_currentFileName = context.displayName; // 当前文件行定格为本次任务的捕获项
    refreshCurrentFileValue();
    m_snapshot.enableTranslation = m_enableTranslation->isChecked();
    m_running = true;
    m_enableTranslation->setEnabled(false); // 点开始即禁用，到终态才恢复
    updateStartButton();
    m_job->start(m_snapshot, context.identity);
}

void AiSubtitleDialog::setStepDisplay(AiSubtitleJob::Step step) {
    m_stepValue->setText(stepName(step)); // 进度随步骤一并由任务侧发来，视图不另判
}

void AiSubtitleDialog::setProgress(double ratio) {
    if (ratio < 0) {
        m_progress->setRange(0, 0); // 不定形态（提取 / 两段初始化）
        m_progress->setValue(0);
        return;
    }
    m_progress->setRange(0, 100);
    m_progress->setValue(qBound(0, qRound(ratio * 100.0), 100));
}

void AiSubtitleDialog::resetAfterTerminal() {
    m_running = false;
    m_job->reset(); // 终态确认后复位：步骤与进度都随任务的信号回"等待开始"
    m_enableTranslation->setEnabled(true);
    setCurrentFile(m_currentFile ? m_currentFile() : QString());
    updateStartButton();
}

void AiSubtitleDialog::showMessage(const QString &text) {
    QMessageBox box(this);
    box.setWindowTitle(tr("Generate Subtitles"));
    box.setText(text);
    box.setIcon(QMessageBox::Information);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

#include "pch.h"

#include "SettingsDialog.h"

#include "MpvConf.h"
#include "MpvController.h"
#include "Util.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(QWidget *parent, MpvController *controller,
                               const QString &pluginIniPath)
    : QDialog(parent), m_controller(controller), m_pluginIniPath(pluginIniPath) {
    // mpv.conf 路径（resolveMpvConfPath 上溯两级即 portable_config，必存在目录）
    m_mpvConfPath = Util::resolveMpvConfPath(QFileInfo(pluginIniPath).absolutePath());
    m_confCache = MpvConf::open(m_mpvConfPath);

    setWindowTitle(tr("Settings"));
    buildUi();
    applyToControls();

    // 初始化快照：回填后的值即为初始已应用值。
    m_appliedSnapshot = m_pending;
    // 初始 Apply 按钮禁用；修改任一控件后 enableApplyButton() 启用
    m_buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);

    // 按钮接线（Apply / OK / Cancel）
    connect(m_buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            [this] { applyAll(); });
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, [this] {
        applyAll();
        accept();
    });
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SettingsDialog::buildUi() {
    auto *root = new QHBoxLayout(this);

    // 左侧分类列表（固定宽度 140，纯文本）
    m_categoryList = new QListWidget(this);
    m_categoryList->setFixedWidth(140);
    m_categoryList->setIconSize(QSize(0, 0)); // 无图标
    m_categoryList->setSpacing(2);
    m_categoryList->addItem(tr("Basic"));
    m_categoryList->addItem(tr("Playback"));
    m_categoryList->addItem(tr("Video"));
    root->addWidget(m_categoryList);

    // 右侧容器（堆栈 + 底部按钮）
    auto *rightPane = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    m_pageStack = new QStackedWidget(rightPane);
    m_pageStack->addWidget(buildBasicPage());    // index 0
    m_pageStack->addWidget(buildPlaybackPage()); // index 1
    m_pageStack->addWidget(buildVideoPage());    // index 2
    rightLayout->addWidget(m_pageStack, 1);

    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Ok | QDialogButtonBox::Cancel, rightPane);
    rightLayout->addWidget(m_buttonBox);
    root->addWidget(rightPane, 1);

    // 切换分类同步堆栈
    connect(m_categoryList, &QListWidget::currentRowChanged, m_pageStack,
            &QStackedWidget::setCurrentIndex);
    m_categoryList->setCurrentRow(0);
}

QWidget *SettingsDialog::buildBasicPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // 语言
    auto *langLabel = new QLabel(tr("Language"), page);
    layout->addWidget(langLabel);
    m_langCombo = new QComboBox(page);
    m_langCombo->setProperty("iniKey", QStringLiteral("language"));
    populateLanguageCombo(m_langCombo, QString());
    connect(m_langCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::onControlChanged);
    m_pending.insert(m_langCombo, QString());
    layout->addWidget(m_langCombo);

    layout->addSpacing(8);

    // 音量滚轮步进
    auto *wheelLabel = new QLabel(tr("Wheel Step"), page);
    layout->addWidget(wheelLabel);
    m_wheelStepCombo = new QComboBox(page);
    m_wheelStepCombo->setProperty("iniKey", QStringLiteral("volumeWheelStep"));
    for (int step : {1, 2, 5, 10})
        m_wheelStepCombo->addItem(QString::number(step), step);
    connect(m_wheelStepCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::onControlChanged);
    m_pending.insert(m_wheelStepCombo, 1);
    layout->addWidget(m_wheelStepCombo);

    // 音量方向键步进
    auto *keyLabel = new QLabel(tr("Arrow Key Step"), page);
    layout->addWidget(keyLabel);
    m_keyStepCombo = new QComboBox(page);
    m_keyStepCombo->setProperty("iniKey", QStringLiteral("volumeKeyStep"));
    for (int step : {1, 2, 5, 10})
        m_keyStepCombo->addItem(QString::number(step), step);
    connect(m_keyStepCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::onControlChanged);
    m_pending.insert(m_keyStepCombo, 5);
    layout->addWidget(m_keyStepCombo);

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildPlaybackPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // 音量上限（mpv 选项，写 conf + 下发 mpv）
    auto *maxLabel = new QLabel(tr("Max Volume"), page);
    layout->addWidget(maxLabel);
    m_volumeMaxSpin = new QSpinBox(page);
    m_volumeMaxSpin->setProperty("mpvKey", QStringLiteral("volume-max"));
    m_volumeMaxSpin->setRange(100, 1000); // mpv volume-max 实际允许范围
    connect(m_volumeMaxSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            &SettingsDialog::onControlChanged);
    m_pending.insert(m_volumeMaxSpin, 130); // mpv 默认 130
    layout->addWidget(m_volumeMaxSpin);

    layout->addSpacing(8);

    // 循环模式（mpv 选项，仅下发，不写 conf）
    auto *loopLabel = new QLabel(tr("Loop Playback"), page);
    layout->addWidget(loopLabel);
    m_loopModeCombo = new QComboBox(page);
    // loopMode 是三态复合 mpv 选项：需写双 key（loop-file / loop-playlist）到 conf
    // + 下发双属性到 mpv。applyAll() 检测到 mpvKey=="loopMode" 走复合路径。
    // 第 0 项为"自定义（不干涉）"——用户手动改 mpv.conf 后组合不在预设 3 种时回填到该项，
    // 不下发 mpv、不写 conf（避免覆盖用户的意图）；data 为空串以与其他预设区分
    m_loopModeCombo->setProperty("mpvKey", QStringLiteral("loopMode"));
    m_loopModeCombo->addItem(tr("Custom (do not override)"), QString()); // 空串 = 自定义占位
    m_loopModeCombo->addItem(tr("No Loop"), QStringLiteral("none"));
    m_loopModeCombo->addItem(tr("Loop One"), QStringLiteral("file"));
    m_loopModeCombo->addItem(tr("Loop Playlist"), QStringLiteral("playlist"));
    connect(m_loopModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::onControlChanged);
    m_pending.insert(m_loopModeCombo, QString()); // 默认自定义（不干涉）
    layout->addWidget(m_loopModeCombo);

    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildVideoPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // 硬件解码（mpv 选项，写 conf + 下发 mpv）
    auto *hwdecLabel = new QLabel(tr("Hardware Decoding"), page);
    layout->addWidget(hwdecLabel);
    m_hwdecCombo = new QComboBox(page);
    m_hwdecCombo->setProperty("mpvKey", QStringLiteral("hwdec"));
    // 可编辑下拉：mpv hwdec 支持很多后端名（auto-safe/d3d11va/dxva2/vulkan/cuda/...），
    // 预设几项供快选，用户也可直接输入完整后端名。readControlValue 查 "comboValueFromText" 取
    // currentText
    m_hwdecCombo->setProperty("comboValueFromText", true);
    m_hwdecCombo->setEditable(true);
    const QStringList presets{
        QStringLiteral("auto"),  QStringLiteral("auto-safe"), QStringLiteral("d3d11va"),
        QStringLiteral("dxva2"), QStringLiteral("no"),
    };
    for (const QString &p : presets)
        m_hwdecCombo->addItem(p);
    connect(m_hwdecCombo, &QComboBox::currentTextChanged, this, &SettingsDialog::onControlChanged);
    m_pending.insert(m_hwdecCombo, QStringLiteral("auto"));
    layout->addWidget(m_hwdecCombo);

    layout->addStretch(1);
    return page;
}

void SettingsDialog::populateLanguageCombo(QComboBox *combo, const QString &currentLocale) {
    Q_UNUSED(currentLocale); // INI 读出在 applyToControls 统一处理
    QStringList locales;
    locales << QStringLiteral("en");
    QDir dir(QStringLiteral(":/i18n"));
    const QStringList qmFiles =
        dir.entryList(QStringList() << QStringLiteral("mpvqt_*.qm"), QDir::Files);
    for (const QString &f : qmFiles) {
        if (f.startsWith(QStringLiteral("mpvqt_")) && f.endsWith(QStringLiteral(".qm"))) {
            QString loc = f.chopped(3).mid(6);
            if (!loc.isEmpty() && !locales.contains(loc))
                locales << loc;
        }
    }
    std::sort(locales.begin(), locales.end(),
              [](const QString &a, const QString &b) { return a < b; });

    combo->addItem(tr("Auto"), QString()); // 空串 data = Auto
    for (const QString &loc : locales) {
        QString display;
        if (loc == QStringLiteral("en")) {
            display = QStringLiteral("English");
        } else {
            QLocale qloc(loc);
            display = qloc.nativeLanguageName();
            if (display.isEmpty())
                display = loc;
            else {
                display[0] = display[0].toUpper();
                const QString terr = qloc.nativeTerritoryName();
                if (!terr.isEmpty() && !display.contains(terr))
                    display = QStringLiteral("%1 (%2)").arg(display, terr);
            }
        }
        combo->addItem(display, loc);
    }
}

void SettingsDialog::applyToControls() {
    // 构造期回填必须包裹在 m_blockApply 守卫内，防止回填过程触发 valueChanged
    // 污染 m_pending
    m_blockApply = true;

    // === 插件本体选项：从 INI 读 ===
    QSettings ini(m_pluginIniPath, QSettings::IniFormat);
    QString curLang = ini.value(QStringLiteral("language")).toString().trimmed();
    if (curLang.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0)
        curLang.clear();
    int langIdx = m_langCombo->findData(curLang);
    m_langCombo->setCurrentIndex(langIdx >= 0 ? langIdx : 0); // 默认 Auto
    m_pending[m_langCombo] = m_langCombo->currentData();

    int wheelStep = ini.value(QStringLiteral("volumeWheelStep"), 1).toInt();
    int wsIdx = m_wheelStepCombo->findData(wheelStep);
    m_wheelStepCombo->setCurrentIndex(wsIdx >= 0 ? wsIdx : 0);
    m_pending[m_wheelStepCombo] = m_wheelStepCombo->currentData();

    int keyStep = ini.value(QStringLiteral("volumeKeyStep"), 5).toInt();
    int ksIdx = m_keyStepCombo->findData(keyStep);
    m_keyStepCombo->setCurrentIndex(ksIdx >= 0 ? ksIdx : m_keyStepCombo->findData(5));
    m_pending[m_keyStepCombo] = m_keyStepCombo->currentData();

    // === mpv 选项（铁律）：仅从 MpvController 取，不读 mpv.conf ===
    // 音量上限
    double volumeMax = m_controller->propertyDouble("volume-max", -1.0);
    int maxValue = (volumeMax >= 0) ? qRound(volumeMax) : 130; // mpv 默认 130
    if (maxValue < 100)
        maxValue = 100;
    if (maxValue > 1000)
        maxValue = 1000;
    m_volumeMaxSpin->setValue(maxValue);
    m_pending[m_volumeMaxSpin] = maxValue;

    // 循环模式：反查 loop-file / loop-playlist；不在预设 3 种组合时回填到"自定义"
    QString loopFile = m_controller->propertyString("loop-file");
    QString loopPlaylist = m_controller->propertyString("loop-playlist");
    QString loopMode; // 空串 = 自定义
    if (loopFile == QLatin1String("inf") && loopPlaylist == QLatin1String("no"))
        loopMode = QStringLiteral("file");
    else if (loopFile == QLatin1String("no") && loopPlaylist == QLatin1String("inf"))
        loopMode = QStringLiteral("playlist");
    else if (loopFile == QLatin1String("no") && loopPlaylist == QLatin1String("no"))
        loopMode = QStringLiteral("none");
    // 其余任意组合（inf/inf、其它值、缺一）均保持空串 → 自定义
    int loopIdx = m_loopModeCombo->findData(loopMode);
    m_loopModeCombo->setCurrentIndex(loopIdx >= 0 ? loopIdx : 0);
    m_pending[m_loopModeCombo] = m_loopModeCombo->currentData();

    // 硬件解码（可编辑下拉，直接显示实际值，不匹配预设时不补全）
    QString hwdec = m_controller->propertyString("hwdec");
    if (hwdec.isEmpty())
        hwdec = QStringLiteral("auto"); // mpv 默认
    m_hwdecCombo->setCurrentText(hwdec);
    m_pending[m_hwdecCombo] = m_hwdecCombo->currentText();

    m_blockApply = false;
}

QVariant SettingsDialog::readControlValue(QWidget *control) const {
    if (auto *combo = qobject_cast<QComboBox *>(control)) {
        // 标记 comboValueFromText：可编辑下拉（用户可能输入自定义值）取 currentText
        if (combo->property("comboValueFromText").isValid())
            return combo->currentText();
        // 不可编辑下拉：兑底 data 无效时取 currentText（addItems 不设 data 的情况）
        const QVariant d = combo->currentData();
        return d.isValid() ? d : combo->currentText();
    }
    if (auto *spin = qobject_cast<QSpinBox *>(control))
        return spin->value();
    return {};
}

void SettingsDialog::onControlChanged() {
    if (m_blockApply)
        return;
    QWidget *w = qobject_cast<QWidget *>(sender());
    if (!w)
        return;
    auto it = m_pending.find(w);
    if (it == m_pending.end())
        return;
    it.value() = readControlValue(w);
    updateApplyButtonState();
}

void SettingsDialog::updateApplyButtonState() {
    // dirty：任一控件值与快照不同
    bool anyDirty = false;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        const auto snapIt = m_appliedSnapshot.find(it.key());
        if (snapIt == m_appliedSnapshot.end() || snapIt.value() != it.value()) {
            anyDirty = true;
            break;
        }
    }
    m_dirty = anyDirty;
    if (m_buttonBox && m_buttonBox->button(QDialogButtonBox::Apply))
        m_buttonBox->button(QDialogButtonBox::Apply)->setEnabled(m_dirty);
}

void SettingsDialog::applyLoopModeOption(const QString &mode) {
    if (mode == QLatin1String("file")) {
        m_controller->setProperty(QStringLiteral("loop-file"), QStringLiteral("inf"));
        m_controller->setProperty(QStringLiteral("loop-playlist"), QStringLiteral("no"));
    } else if (mode == QLatin1String("playlist")) {
        m_controller->setProperty(QStringLiteral("loop-file"), QStringLiteral("no"));
        m_controller->setProperty(QStringLiteral("loop-playlist"), QStringLiteral("inf"));
    } else {
        m_controller->setProperty(QStringLiteral("loop-file"), QStringLiteral("no"));
        m_controller->setProperty(QStringLiteral("loop-playlist"), QStringLiteral("no"));
    }
}

bool SettingsDialog::applyAll() {
    bool ok = true;
    bool mpvConfDirty = false;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        QWidget *w = it.key();
        const QVariant &val = it.value();

        // 与快照比对：未变则跳过下发与写盘
        const auto snapIt = m_appliedSnapshot.find(w);
        const bool changed = (snapIt == m_appliedSnapshot.end() || snapIt.value() != val);
        if (!changed)
            continue;

        if (w->property("mpvKey").isValid()) {
            const QString k = w->property("mpvKey").toString();
            if (k == QStringLiteral("loopMode")) {
                // 复合路径：空 data = 自定义（不干涉），跳过下发与写 conf
                const QString mode = val.toString();
                if (!mode.isEmpty()) {
                    applyLoopModeOption(mode);
                    QString loopFileVal, loopPlaylistVal;
                    if (mode == QLatin1String("file")) {
                        loopFileVal = QStringLiteral("inf");
                        loopPlaylistVal = QStringLiteral("no");
                    } else if (mode == QLatin1String("playlist")) {
                        loopFileVal = QStringLiteral("no");
                        loopPlaylistVal = QStringLiteral("inf");
                    } else {
                        loopFileVal = QStringLiteral("no");
                        loopPlaylistVal = QStringLiteral("no");
                    }
                    if (m_confCache) {
                        m_confCache->setGlobal("loop-file", loopFileVal);
                        m_confCache->setGlobal("loop-playlist", loopPlaylistVal);
                        mpvConfDirty = true;
                    }
                }
            } else {
                m_controller->setProperty(k, val.toString());
                if (m_confCache) {
                    m_confCache->setGlobal(k, val.toString());
                    mpvConfDirty = true;
                }
            }
        } else if (w->property("iniKey").isValid()) {
            const QString k = w->property("iniKey").toString();
            QSettings s(m_pluginIniPath, QSettings::IniFormat);
            s.setValue(k, val);
        } else {
            qWarning("SettingsDialog: pending control missing property tag");
        }
    }
    if (mpvConfDirty && m_confCache)
        ok &= m_confCache->commit();
    // 应用完成：更新快照为当前已应用值；Apply 按钮随之禁用
    if (ok) {
        m_appliedSnapshot = m_pending;
        m_dirty = false;
        if (m_buttonBox && m_buttonBox->button(QDialogButtonBox::Apply))
            m_buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
    }
    return ok;
}
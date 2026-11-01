#include "pch.h"

#include "ExternalToolPathCombo.h"

#include "ExternalTool.h"

#include "AppPaths.h"

ExternalToolPathCombo::ExternalToolPathCombo(const ExternalToolSpec &spec, const QString &configDir,
                                             QWidget *parent)
    : QWidget(parent), m_spec(spec), m_pluginDir(App::paths().pluginDir()), m_configDir(configDir),
      m_autoLabel(tr("<Auto>")) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_combo = new QComboBox(this);
    m_combo->setEditable(true);
    m_combo->setInsertPolicy(QComboBox::InsertPolicy::NoInsert);
    // 候选是完整路径：默认的尺寸策略会把 minimumSizeHint 抬到最长候选，把对话框钳到
    // 路径宽度。改按字符数预算，完整路径交给下拉弹层展示
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_combo->setMinimumContentsLength(24);
    // 占位串挂内部行编辑：QComboBox 自己的占位只在 currentIndex == -1 时显示，
    // 而自动项恒占第 0 位；行编辑的占位是"文本为空即显示"
    m_combo->lineEdit()->setPlaceholderText(m_autoLabel);
    m_combo->addItem(m_autoLabel, QString());
    for (const QString &path : externalToolCandidates(m_spec, m_pluginDir, m_configDir))
        m_combo->addItem(path, path);
    m_combo->setCurrentIndex(0);
    m_combo->clearEditText();
    layout->addWidget(m_combo, 1);

    auto *browseButton = new QPushButton(tr("Browse…"), this);
    layout->addWidget(browseButton);

    connect(m_combo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        // 选中自动项会把该项文案灌进行编辑，清空让占位串接管
        if (m_combo->currentIndex() == 0 && text == m_autoLabel)
            m_combo->clearEditText();
        emit changed();
    });
    connect(browseButton, &QPushButton::clicked, this, &ExternalToolPathCombo::browse);
}

QString ExternalToolPathCombo::value() const {
    const QString text = m_combo->currentText().trimmed();
    if (text.isEmpty() || text == m_autoLabel)
        return QString();
    return text;
}

void ExternalToolPathCombo::setPath(const QString &path) {
    const QSignalBlocker blocker(m_combo);
    if (path.isEmpty()) {
        m_combo->setCurrentIndex(0);
        m_combo->clearEditText();
        return;
    }
    // 呈现形态只有这一处出口：候选表出表即原生形，这里把 ini 里可能存的任一种写法
    // （正斜杠 / 反斜杠）归到同一形，否则同一份库在下拉框里可能出现两种写法、精确匹配
    // 也认不出候选项
    const QString shown = QDir::toNativeSeparators(QDir::cleanPath(path));
    const int idx = m_combo->findData(shown);
    if (idx >= 0)
        m_combo->setCurrentIndex(idx);
    else
        m_combo->setEditText(shown);
}

void ExternalToolPathCombo::browse() {
    QString startDir;
    const QString current = value();
    if (!current.isEmpty()) {
        startDir = QFileInfo(current).absolutePath();
    } else {
        if (!m_pluginDir.isEmpty() && QFileInfo::exists(m_pluginDir))
            startDir = m_pluginDir;
        else {
            const QStringList candidates = externalToolCandidates(m_spec, m_pluginDir, m_configDir);
            if (!candidates.isEmpty())
                startDir = QFileInfo(candidates.first()).absolutePath();
        }
    }
    const QString selected =
        QFileDialog::getOpenFileName(this, QString(), startDir, externalToolBrowseFilter(m_spec));
    if (selected.isEmpty())
        return;
    setPath(selected);
    emit changed();
}

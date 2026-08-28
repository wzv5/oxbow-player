#include "pch.h"
#include "ListSettingsDialog.h"

#include "Constants.h"
#include "Util.h"
#include "DirProvider.h"
#include "StaticProvider.h"

namespace {
// 扩展名编辑区文本 → 规范清单：按空白拆分、去点、转小写、去重保序
QStringList parseExtensions(const QString &text)
{
    QStringList out;
    for (const QString &token : text.split(QRegularExpression(QStringLiteral("\\s+")),
                                           Qt::SkipEmptyParts)) {
        QString ext = token;
        while (ext.startsWith(QLatin1Char('.')))
            ext.remove(0, 1);
        ext = ext.toLower();
        if (!ext.isEmpty() && !out.contains(ext))
            out << ext;
    }
    return out;
}
} // namespace

ListSettingsDialog::ListSettingsDialog(QWidget *parent, const QString &currentName,
                                       const PlaylistProvider *current,
                                       const QStringList &occupiedNames)
    : QDialog(parent), m_occupiedNames(occupiedNames), m_current(current)
{
    setWindowTitle(tr("列表设置"));
    auto *layout = new QFormLayout(this);

    // ——第一步：仅创建控件与布局，全部取中性初值——
    m_nameEdit = new QLineEdit(currentName);
    layout->addRow(tr("列表名称"), m_nameEdit);

    layout->addRow(new QLabel(tr("列表提供者")));

    m_staticRadio = new QRadioButton(tr("本地静态列表"));
    layout->addRow(m_staticRadio);

    m_dirRadio = new QRadioButton(tr("本地目录"));
    auto *rootRow = new QHBoxLayout;
    m_rootEdit = new QLineEdit;
    auto *browseBtn = new QPushButton(tr("浏览…"));
    rootRow->addWidget(m_rootEdit, 1);
    rootRow->addWidget(browseBtn);
    m_recursiveCheck = new QCheckBox(tr("包含子目录"));
    m_extensionsEdit = new QLineEdit;
    auto *extRow = new QHBoxLayout;
    extRow->addWidget(m_extensionsEdit, 1);
    // 恢复默认清单（↺ U+21BA）：与 NavigatorPanel 复位按钮符号一致
    auto *extResetBtn = new QPushButton(QStringLiteral("\u21ba"));
    extResetBtn->setToolTip(tr("恢复默认扩展名清单"));
    extResetBtn->setFixedWidth(28);
    extRow->addWidget(extResetBtn);
    auto *dirPage = new QWidget;
    auto *dirLayout = new QVBoxLayout(dirPage);
    dirLayout->setContentsMargins(16, 0, 0, 0);
    dirLayout->addLayout(rootRow);
    dirLayout->addWidget(m_recursiveCheck);
    dirLayout->addWidget(new QLabel(tr("媒体扩展名（空格分隔，留空不过滤）：")));
    dirLayout->addLayout(extRow);
    layout->addRow(m_dirRadio);
    layout->addRow(dirPage);

    auto *group = new QButtonGroup(this);
    group->addButton(m_staticRadio);
    group->addButton(m_dirRadio);

    connect(browseBtn, &QPushButton::clicked, this, &ListSettingsDialog::browseRoot);
    // 恢复默认清单：预填 Constants 默认值（不校验、可继续编辑）
    connect(extResetBtn, &QPushButton::clicked, this, [this] {
        m_extensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
    });
    // 切换单选联动目录参数区启停（populate 的程序性 setChecked 同样触发本信号）
    connect(m_staticRadio, &QRadioButton::toggled, dirPage, &QWidget::setDisabled);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ListSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addRow(buttons);

    // ——第二步：控件就绪后统一回填入口提供者数据（信号已接线，联动自然生效）——
    populateFromCurrent();
}

void ListSettingsDialog::populateFromCurrent()
{
    if (!m_current) {
        // 新建列表：静态默认，扩展名预填全局默认清单
        m_staticRadio->setChecked(true);
        m_extensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        return;
    }
    if (m_current->type() == PlaylistProvider::Type::Dir) {
        auto *d = static_cast<const DirProvider *>(m_current);
        m_dirRadio->setChecked(true);
        // m_root 已为原生分隔符（来源处处理），不再额外 toNativeSeparators（B15）
        m_rootEdit->setText(d->rootPath());
        m_recursiveCheck->setChecked(d->recursive());
        m_extensionsEdit->setText(d->extensions().join(QLatin1Char(' ')));
    } else {
        auto *s = static_cast<const StaticProvider *>(m_current);
        Q_UNUSED(s); // 静态无参数区：仅选中单选，扩展名框预填默认清单
        m_staticRadio->setChecked(true);
        m_extensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
    }
}

QString ListSettingsDialog::listName() const
{
    return m_nameEdit->text().trimmed();
}

PlaylistProvider::Type ListSettingsDialog::providerType() const
{
    return m_dirRadio->isChecked() ? PlaylistProvider::Type::Dir : PlaylistProvider::Type::Static;
}

QString ListSettingsDialog::dirRoot() const
{
    // 路径来源处（用户可手动输入，可能相对或分隔符不规范）：相对→绝对、转原生分隔符，
    // 统一走 resolveLocalPath；网络/流保持原样（需求 #6 读写原样）
    const QString text = m_rootEdit->text().trimmed();
    if (text.isEmpty())
        return text;
    return Util::resolveLocalPath(text);
}

bool ListSettingsDialog::dirRecursive() const
{
    return m_recursiveCheck->isChecked();
}

QStringList ListSettingsDialog::dirExtensions() const
{
    return parseExtensions(m_extensionsEdit->text());
}

std::optional<std::unique_ptr<PlaylistProvider>> ListSettingsDialog::takeProvider()
{
    return std::move(m_result);
}

void ListSettingsDialog::browseRoot()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("选择目录"),
                                                          dirRoot());
    // Qt 来源（目录选择）：拿到即转原生分隔符（B15）
    if (!dir.isEmpty())
        m_rootEdit->setText(Util::nativePath(dir));
}

void ListSettingsDialog::accept()
{
    const QString name = listName();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("列表设置"), tr("列表名称不能为空。"));
        return;
    }
    if (name == QLatin1String("默认")) {
        QMessageBox::warning(this, tr("列表设置"), tr("不能使用默认列表的名称。"));
        return;
    }
    if (m_occupiedNames.contains(name)) {
        QMessageBox::warning(this, tr("列表设置"), tr("该名称已被其他列表使用。"));
        return;
    }
    if (providerType() == PlaylistProvider::Type::Dir) {
        const QString root = dirRoot();
        if (root.isEmpty()) {
            QMessageBox::warning(this, tr("列表设置"), tr("请选择绑定目录。"));
            return;
        }
        // 本地目录提供者仅接受真实本地路径；网络/流路径（http/ftp/stream 等，含 :// 且非 file:）拒绝。
        // 用串前缀判定，避免 QUrl::scheme() 误判盘符、QUrl::fromUserInput 误判相对名 "a" 为非本地。
        if (Util::isNetworkStreamPath(root)) {
            QMessageBox::warning(this, tr("列表设置"), tr("本地目录提供者不支持网络/流路径。"));
            return;
        }
        if (!QDir(root).exists()) {
            QMessageBox::warning(this, tr("列表设置"), tr("目录不存在：%1").arg(root));
            return;
        }
    }

    // 存量同类型且参数未变 → 不产出新提供者（调用方仅应用改名）。
    // 先按类型分支再下探子类，不依赖 RTTI
    bool unchanged = false;
    if (m_current && m_current->type() == providerType()) {
        if (providerType() == PlaylistProvider::Type::Dir) {
            auto *d = static_cast<const DirProvider *>(m_current);
            unchanged = d->rootPath() == dirRoot() && d->recursive() == dirRecursive()
                        && d->extensions() == dirExtensions();
        } else {
            unchanged = true; // 静态无参数，同类型即未变
        }
    }
    if (unchanged) {
        QDialog::accept();
        return;
    }

    // 构建新提供者；排序规则默认由调用方在接入点按类型施加
    if (providerType() == PlaylistProvider::Type::Dir)
        m_result = std::make_unique<DirProvider>(dirRoot(), dirRecursive(), dirExtensions());
    else
        m_result = std::make_unique<StaticProvider>();
    QDialog::accept();
}

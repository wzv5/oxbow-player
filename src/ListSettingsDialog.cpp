#include "ListSettingsDialog.h"
#include "pch.h"

#include "Constants.h"
#include "DirProvider.h"
#include "StaticProvider.h"
#include "Util.h"
#include "WebDavProvider.h"

namespace {
// 扩展名编辑区文本 → 规范清单：按空白拆分、去点、转小写、去重保序
QStringList parseExtensions(const QString &text) {
    QStringList out;
    for (const QString &token :
         text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
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
                                       const QStringList &occupiedNames,
                                       const std::shared_ptr<ServiceRegistry> &registry)
    : QDialog(parent), m_occupiedNames(occupiedNames), m_current(current), m_registry(registry) {
    setWindowTitle(tr("List Settings"));
    auto *layout = new QFormLayout(this);

    // ——第一步：仅创建控件与布局，全部取中性初值——
    m_nameEdit = new QLineEdit(currentName);
    layout->addRow(tr("List Name"), m_nameEdit);

    layout->addRow(new QLabel(tr("List Provider")));

    m_staticRadio = new QRadioButton(tr("Local Static List"));
    layout->addRow(m_staticRadio);

    m_dirRadio = new QRadioButton(tr("Local Directory"));
    auto *rootRow = new QHBoxLayout;
    m_rootEdit = new QLineEdit;
    auto *browseBtn = new QPushButton(tr("Browse…"));
    rootRow->addWidget(m_rootEdit, 1);
    rootRow->addWidget(browseBtn);
    m_recursiveCheck = new QCheckBox(tr("Include Subdirectories"));
    // 目录页扩展名编辑区（N37 前即有）：整组直接堆叠在目录参数之下
    m_dirExtensionsEdit = new QLineEdit;
    auto *dirExtRow = new QHBoxLayout;
    dirExtRow->addWidget(m_dirExtensionsEdit, 1);
    auto *dirExtResetBtn = new QPushButton(QStringLiteral("\u21ba"));
    dirExtResetBtn->setToolTip(tr("Reset to default extension list"));
    dirExtResetBtn->setFixedWidth(28);
    dirExtRow->addWidget(dirExtResetBtn);
    auto *dirPage = new QWidget;
    auto *dirLayout = new QVBoxLayout(dirPage);
    dirLayout->setContentsMargins(16, 0, 0, 0);
    dirLayout->addLayout(rootRow);
    dirLayout->addWidget(m_recursiveCheck);
    dirLayout->addWidget(new QLabel(tr("Media extensions (space-separated; leave empty for no "
                                       "filter):")));
    dirLayout->addLayout(dirExtRow);
    layout->addRow(m_dirRadio);
    layout->addRow(dirPage);

    // ——N37：WebDAV 参数区（扩展名组同形堆叠在本页参数之下）——
    m_webdavRadio = new QRadioButton(tr("WebDAV Directory"));
    auto *urlRow = new QHBoxLayout;
    m_urlEdit = new QLineEdit;
    urlRow->addWidget(new QLabel(tr("Directory URL")), 0);
    urlRow->addWidget(m_urlEdit, 1);
    auto *userRow = new QHBoxLayout;
    m_userEdit = new QLineEdit;
    userRow->addWidget(new QLabel(tr("Username")), 0);
    userRow->addWidget(m_userEdit, 1);
    auto *passwordRow = new QHBoxLayout;
    m_passwordEdit = new QLineEdit;
    m_passwordEdit->setEchoMode(QLineEdit::EchoMode::Password);
    passwordRow->addWidget(new QLabel(tr("Password")), 0);
    passwordRow->addWidget(m_passwordEdit, 1);
    m_davRecursiveCheck = new QCheckBox(tr("Include Subdirectories"));
    m_davExtensionsEdit = new QLineEdit;
    auto *davExtRow = new QHBoxLayout;
    davExtRow->addWidget(m_davExtensionsEdit, 1);
    auto *davExtResetBtn = new QPushButton(QStringLiteral("\u21ba"));
    davExtResetBtn->setToolTip(tr("Reset to default extension list"));
    davExtResetBtn->setFixedWidth(28);
    davExtRow->addWidget(davExtResetBtn);
    auto *webdavPage = new QWidget;
    auto *webdavLayout = new QVBoxLayout(webdavPage);
    webdavLayout->setContentsMargins(16, 0, 0, 0);
    webdavLayout->addLayout(urlRow);
    webdavLayout->addLayout(userRow);
    webdavLayout->addLayout(passwordRow);
    webdavLayout->addWidget(m_davRecursiveCheck);
    webdavLayout->addWidget(new QLabel(tr("Media extensions (space-separated; leave empty for no "
                                          "filter):")));
    webdavLayout->addLayout(davExtRow);
    layout->addRow(m_webdavRadio);
    layout->addRow(webdavPage);

    auto *group = new QButtonGroup(this);
    group->addButton(m_staticRadio);
    group->addButton(m_dirRadio);
    group->addButton(m_webdavRadio);

    connect(browseBtn, &QPushButton::clicked, this, &ListSettingsDialog::browseRoot);
    // 恢复默认清单：预填 Constants 默认值（不校验、可继续编辑）；两页各自复位
    connect(dirExtResetBtn, &QPushButton::clicked, this, [this] {
        m_dirExtensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
    });
    connect(davExtResetBtn, &QPushButton::clicked, this, [this] {
        m_davExtensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
    });
    // 切换单选联动参数区启停（populate 的程序性 setChecked 同样触发本信号）：
    // 目录页 / WebDAV 页各随自己的单选启停（扩展名组已属各自页面，随页启停）
    auto updateEnabling = [this, dirPage, webdavPage] {
        dirPage->setEnabled(m_dirRadio->isChecked());
        webdavPage->setEnabled(m_webdavRadio->isChecked());
    };
    connect(m_staticRadio, &QRadioButton::toggled, this, updateEnabling);
    connect(m_dirRadio, &QRadioButton::toggled, this, updateEnabling);
    connect(m_webdavRadio, &QRadioButton::toggled, this, updateEnabling);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ListSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addRow(buttons);

    // ——第二步：控件就绪后统一回填入口提供者数据（信号已接线，联动自然生效）——
    populateFromCurrent();
    updateEnabling(); // 防御：回填未触发 toggled（初值即目标态）时也保证联动正确
}

void ListSettingsDialog::populateFromCurrent() {
    if (!m_current) {
        // 新建列表：静态默认，两页扩展名框均预填全局默认清单
        m_staticRadio->setChecked(true);
        m_dirExtensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        m_davExtensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        return;
    }
    if (m_current->type() == PlaylistProvider::Type::Dir) {
        auto *d = static_cast<const DirProvider *>(m_current);
        m_dirRadio->setChecked(true);
        // m_root 已为原生分隔符（来源处处理），不再额外 toNativeSeparators（B15）
        m_rootEdit->setText(d->rootPath());
        m_recursiveCheck->setChecked(d->recursive());
        m_dirExtensionsEdit->setText(d->extensions().join(QLatin1Char(' ')));
        m_davExtensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
    } else if (m_current->type() == PlaylistProvider::Type::WebDav) {
        // N37：WebDAV 参数回填（六项全量；davUrl 已含尾斜杠，URL 框原样回显）
        auto *w = static_cast<const WebDavProvider *>(m_current);
        m_webdavRadio->setChecked(true);
        m_urlEdit->setText(w->rootUrl());
        m_userEdit->setText(w->user());
        m_passwordEdit->setText(w->password());
        m_davRecursiveCheck->setChecked(w->recursive());
        m_davExtensionsEdit->setText(w->extensions().join(QLatin1Char(' ')));
        m_dirExtensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
    } else {
        auto *s = static_cast<const StaticProvider *>(m_current);
        Q_UNUSED(s); // 静态无参数区：仅选中单选，两页扩展名框预填默认清单
        m_staticRadio->setChecked(true);
        m_dirExtensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        m_davExtensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
    }
}

QString ListSettingsDialog::listName() const { return m_nameEdit->text().trimmed(); }

PlaylistProvider::Type ListSettingsDialog::providerType() const {
    if (m_dirRadio->isChecked())
        return PlaylistProvider::Type::Dir;
    if (m_webdavRadio->isChecked())
        return PlaylistProvider::Type::WebDav; // N37
    return PlaylistProvider::Type::Static;
}

QString ListSettingsDialog::dirRoot() const {
    // 路径来源处（用户可手动输入，可能相对或分隔符不规范）：相对→绝对、转原生分隔符，
    // 统一走 resolveLocalPath；网络/流保持原样（需求 #6 读写原样）
    const QString text = m_rootEdit->text().trimmed();
    if (text.isEmpty())
        return text;
    return Util::resolveLocalPath(text);
}

bool ListSettingsDialog::dirRecursive() const { return m_recursiveCheck->isChecked(); }

QStringList ListSettingsDialog::dirExtensions() const {
    return parseExtensions(m_dirExtensionsEdit->text());
}

// N37：URL 的来源处（§3.1 第 1 条）——trim → 经 Util::nativePath 规范化（网络分支）
// → 补尾斜杠。尾斜杠是 WebDAV 的基目录语义（resolved 展开相对 href 的前提），
// 属该来源自身的规则，不进通用函数
QString ListSettingsDialog::davUrl() const {
    QString text = m_urlEdit->text().trimmed();
    if (text.isEmpty())
        return text;
    text = Util::nativePath(text);
    // 尾斜杠补在 path 上：基目录语义只对路径成立，直接补在串尾会把分隔符塞进
    // query（"…/dav?token=1/"）。query / fragment 原样保留在后
    const qsizetype q = text.indexOf(QLatin1Char('?'));
    const qsizetype f = text.indexOf(QLatin1Char('#'));
    qsizetype cut = text.size();
    if (q >= 0)
        cut = qMin(cut, q);
    if (f >= 0)
        cut = qMin(cut, f);
    QString head = text.left(cut);
    if (!head.endsWith(QLatin1Char('/')))
        head += QLatin1Char('/');
    return head + text.mid(cut);
}

QString ListSettingsDialog::davUser() const { return m_userEdit->text().trimmed(); }

QString ListSettingsDialog::davPassword() const { return m_passwordEdit->text(); }

bool ListSettingsDialog::davRecursive() const { return m_davRecursiveCheck->isChecked(); }

QStringList ListSettingsDialog::davExtensions() const {
    return parseExtensions(m_davExtensionsEdit->text());
}

std::optional<std::shared_ptr<PlaylistProvider>> ListSettingsDialog::takeProvider() {
    return std::move(m_result);
}

void ListSettingsDialog::browseRoot() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select Directory"), dirRoot());
    // Qt 来源（目录选择）：拿到即转原生分隔符（B15）
    if (!dir.isEmpty())
        m_rootEdit->setText(Util::nativePath(dir));
}

void ListSettingsDialog::accept() {
    const QString name = listName();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("List Settings"), tr("List name cannot be empty."));
        return;
    }
    if (name == tr("Default")) {
        QMessageBox::warning(this, tr("List Settings"),
                             tr("Cannot use the name of the default list."));
        return;
    }
    if (m_occupiedNames.contains(name)) {
        QMessageBox::warning(this, tr("List Settings"),
                             tr("This name is already used by another list."));
        return;
    }
    if (providerType() == PlaylistProvider::Type::Dir) {
        const QString root = dirRoot();
        if (root.isEmpty()) {
            QMessageBox::warning(this, tr("List Settings"), tr("Please select a bound directory."));
            return;
        }
        // 本地目录提供者仅接受真实本地路径；网络/流路径（http/ftp/stream 等，含 :// 且非
        // file:）拒绝。 用串前缀判定，避免 QUrl::scheme() 误判盘符、QUrl::fromUserInput 误判相对名
        // "a" 为非本地。
        if (Util::isNetworkStreamPath(root)) {
            QMessageBox::warning(this, tr("List Settings"),
                                 tr("Local directory provider does not support network or "
                                    "stream paths."));
            return;
        }
        if (!QDir(root).exists()) {
            QMessageBox::warning(this, tr("List Settings"),
                                 tr("Directory does not exist: %1").arg(root));
            return;
        }
    }
    // N37：WebDAV 校验——URL 非空 + QUrl 可解析 + scheme ∈ {http, https}；
    // 不得内嵌 userinfo（提示改用用户名 / 密码栏，避免凭据出现两个来源）；
    // 不做连通性探测——交由异步枚举，失败即占位行 + 右键刷新重试
    if (providerType() == PlaylistProvider::Type::WebDav) {
        const QString url = davUrl();
        if (url.isEmpty()) {
            QMessageBox::warning(this, tr("List Settings"), tr("Please enter a directory URL."));
            return;
        }
        const QUrl parsed(url, QUrl::ParsingMode::TolerantMode);
        if (!parsed.isValid() || (parsed.scheme().toLower() != QLatin1String("http") &&
                                  parsed.scheme().toLower() != QLatin1String("https"))) {
            QMessageBox::warning(this, tr("List Settings"),
                                 tr("Directory URL must be a valid http(s) URL."));
            return;
        }
        if (!parsed.userName(QUrl::ComponentFormattingOption::FullyDecoded).isEmpty() ||
            !parsed.password(QUrl::ComponentFormattingOption::FullyDecoded).isEmpty()) {
            QMessageBox::warning(this, tr("List Settings"),
                                 tr("Directory URL must not embed credentials; "
                                    "use the username and password fields instead."));
            return;
        }
    }

    // 存量同类型且参数未变 → 不产出新提供者（调用方仅应用改名）。
    // 先按类型分支再下探子类，不依赖 RTTI
    bool unchanged = false;
    if (m_current && m_current->type() == providerType()) {
        if (providerType() == PlaylistProvider::Type::Dir) {
            auto *d = static_cast<const DirProvider *>(m_current);
            unchanged = d->rootPath() == dirRoot() && d->recursive() == dirRecursive() &&
                        d->extensions() == dirExtensions();
        } else if (providerType() == PlaylistProvider::Type::WebDav) {
            // N37：URL / 用户名 / 密码 / 递归 / 扩展名全等方判未变
            auto *w = static_cast<const WebDavProvider *>(m_current);
            unchanged = w->rootUrl() == davUrl() && w->user() == davUser() &&
                        w->password() == davPassword() && w->recursive() == davRecursive() &&
                        w->extensions() == davExtensions();
        } else {
            unchanged = true; // 静态无参数，同类型即未变
        }
    }
    if (unchanged) {
        QDialog::accept();
        return;
    }

    // 构建新提供者（registry 经基类持有）；排序规则默认由调用方在接入点按类型施加
    if (providerType() == PlaylistProvider::Type::Dir)
        m_result =
            std::make_shared<DirProvider>(dirRoot(), dirRecursive(), dirExtensions(), m_registry);
    else if (providerType() == PlaylistProvider::Type::WebDav)
        m_result = std::make_shared<WebDavProvider>(davUrl(), davUser(), davPassword(),
                                                    davRecursive(), davExtensions(), m_registry);
    else
        m_result = std::make_shared<StaticProvider>(m_registry);
    QDialog::accept();
}

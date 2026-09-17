#include "pch.h"

#include "DirProvider.h"

#include "Constants.h"
#include "LocalFileEntity.h"
#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "TraversalPolicy.h"
#include "Util.h"

namespace {
// 非 Q_OBJECT 类的用户可见串：英文源 + QT_TRANSLATE_NOOP 标注翻译上下文
// （供 lupdate 提取），运行期经 QCoreApplication::translate 取值——无译文时即
// 英文原文（N34/HttpClient 同款约定）
QString trText(const char *source) { return QCoreApplication::translate("DirProvider", source); }

// 扩展名 → QDir 名称过滤器（*.mp4 形态）
QStringList toNameFilters(const QStringList &extensions) {
    QStringList filters;
    filters.reserve(extensions.size());
    for (const QString &ext : extensions)
        filters << QStringLiteral("*.") + ext;
    return filters;
}
// junction 链解析上限：病态链 / 自指链不至于把枚举拖死
constexpr int kJunctionChainGuard = 32;

// 目录真实路径（去重键唯一来源）：先解开 NTFS junction 链，再取 canonical
// （解析符号链接 / . / ..），解析失败退回原路径——使"同一目录经多条路径可达"
// 收敛为同一个键。
// 实测（Qt 6.11.2 / Windows 11）：canonicalFilePath() 不解析 junction——链接的
// canonical 仍是链接自身路径，直接拿它当键等于没去重，junction 环会被当成普通
// 目录逐层展开；故此处显式经 junctionTarget() 解到真实目录再取 canonical
QString canonicalDirPath(const QString &path) {
    QFileInfo info(path);
    for (int i = 0; i < kJunctionChainGuard && info.isJunction(); ++i) {
        QString target = info.junctionTarget();
        if (target.isEmpty())
            break;
        // reparse 目标可能带 \??\ / \\?\ 前缀，统一转成 Qt 路径形态再续解
        if (target.startsWith(QStringLiteral("\\\\?\\")) ||
            target.startsWith(QStringLiteral("\\??\\")))
            target = target.mid(4);
        info = QFileInfo(QDir::fromNativeSeparators(target));
    }
    const QString canon = info.canonicalFilePath();
    return canon.isEmpty() ? info.absoluteFilePath() : canon;
}
} // namespace

DirProvider::DirProvider(const QString &root, bool recursive, const QStringList &extensions,
                         const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_root(root), m_recursive(recursive), m_extensions(extensions) {}

// 本地目录的参数页（N44 §2.5.2）：根路径行 + 浏览按钮 + 递归 + 扩展名行，内容与
// 重构前的目录参数区逐项一致；原 16px 左缩进取消（四行布局下参数区不再挂在某个
// 单选之下）。页面类只在本文件可见，经 createDirSettingsPage 产出
class DirSettingsPage : public ProviderSettingsPage {
    Q_OBJECT
  public:
    DirSettingsPage(const DirProvider *seed, const std::shared_ptr<ServiceRegistry> &registry,
                    QWidget *parent)
        : ProviderSettingsPage(parent), m_seed(seed), m_registry(registry) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);

        auto *rootRow = new QHBoxLayout;
        m_rootEdit = new QLineEdit;
        auto *browseBtn = new QPushButton(tr("Browse…"));
        rootRow->addWidget(m_rootEdit, 1);
        rootRow->addWidget(browseBtn);
        form->addRow(tr("Path"), rootRow);

        m_recursiveCheck = new QCheckBox(tr("Include Subdirectories"));
        form->addRow(m_recursiveCheck); // 单控件行：占第 1 列起、跨两列

        m_extensionsEdit = new QLineEdit;
        auto *extRow = new QHBoxLayout;
        extRow->addWidget(m_extensionsEdit, 1);
        auto *extResetBtn = new QPushButton(QStringLiteral("↺"));
        extResetBtn->setToolTip(tr("Reset to default extension list"));
        extResetBtn->setFixedWidth(28);
        extRow->addWidget(extResetBtn);
        // 说明文字移到编辑框下方单独一行（原在标题的括号里），灰色显示：
        // 标题只写 Filter，扩展名这一层含义由本行说明承担
        auto *extNote =
            new QLabel(tr("Space-separated file extensions; leave empty for no filter"));
        // 不开 wordWrap：备注按单行计入宽度，由它把表单区（进而对话框）撑宽。
        // 开换行会让 QLabel 变成"用高度换宽度"，布局反而不为它撑宽、把它压到折行
        extNote->setEnabled(false); // 禁用态即取调色板的灰，跟随主题不硬编码颜色
        // 备注右侧留 12px：它是本列最宽的一项，不留就会直接贴住窗口右边
        auto *noteRow = new QHBoxLayout;
        noteRow->addWidget(extNote);
        noteRow->addSpacing(12);
        auto *extField = new QVBoxLayout;
        extField->addLayout(extRow);
        extField->addLayout(noteRow);
        form->addRow(tr("Filter"), extField);

        layout->addLayout(form);
        // 表单区在对话框里被纵向拉伸：不加尾部伸缩项，多余空间会被均摊到各行之间
        // （表现为行间距异常大），故把余量全部压到内容之下
        layout->addStretch(1);

        // 回填（构造期自持，取代原对话框的 populateFromCurrent）：m_root 已为原生
        // 分隔符（来源处处理），不再额外 toNativeSeparators（B15）；
        // 无回显来源（新建 / 换类型）时扩展名框预填全局默认清单
        if (m_seed) {
            m_rootEdit->setText(m_seed->rootPath());
            m_recursiveCheck->setChecked(m_seed->recursive());
            m_extensionsEdit->setText(m_seed->extensions().join(QLatin1Char(' ')));
        } else {
            m_extensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        }

        connect(browseBtn, &QPushButton::clicked, this, &DirSettingsPage::browseRoot);
        // 复位：预填默认清单（不校验、可继续编辑）
        connect(extResetBtn, &QPushButton::clicked, this, [this] {
            m_extensionsEdit->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        });
    }

    std::optional<QString> validate() const override {
        return DirProvider::validateInput(m_rootEdit->text());
    }

    bool hasChanges() const override {
        return !m_seed || rootValue() != m_seed->rootPath() ||
               m_recursiveCheck->isChecked() != m_seed->recursive() ||
               extensionsValue() != m_seed->extensions();
    }

    std::shared_ptr<PlaylistProvider> build() const override {
        return std::make_shared<DirProvider>(rootValue(), m_recursiveCheck->isChecked(),
                                             extensionsValue(), m_registry);
    }

  private:
    // 取规范化参数值（hasChanges 与 build 共用同一出口）
    QString rootValue() const { return DirProvider::normalizeRoot(m_rootEdit->text()); }
    QStringList extensionsValue() const {
        return DirProvider::parseExtensions(m_extensionsEdit->text());
    }

    void browseRoot() {
        const QString dir =
            QFileDialog::getExistingDirectory(this, tr("Select Directory"), rootValue());
        // Qt 来源（目录选择）：拿到即转原生分隔符（B15）
        if (!dir.isEmpty())
            m_rootEdit->setText(Util::nativePath(dir));
    }

    const DirProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QLineEdit *m_rootEdit = nullptr;
    QCheckBox *m_recursiveCheck = nullptr;
    QLineEdit *m_extensionsEdit = nullptr;
};

ProviderSettingsPage *createDirSettingsPage(const PlaylistProvider *seed,
                                            const std::shared_ptr<ServiceRegistry> &registry,
                                            QWidget *parent) {
    // 同类型由调用方（对话框）保证：seed 非空即本类型
    return new DirSettingsPage(seed ? static_cast<const DirProvider *>(seed) : nullptr, registry,
                               parent);
}

QString DirProvider::typeKey() const { return QLatin1String(kDirProviderKey); }

QString DirProvider::displayName() {
    return QCoreApplication::translate("DirProvider", kDirProviderName);
}

QString DirProvider::normalizeRoot(const QString &text) {
    // 路径来源处（用户可手动输入，可能相对或分隔符不规范）：相对→绝对、转原生分隔符，
    // 统一走 resolveLocalPath；网络/流保持原样（需求 #6 读写原样）
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QString();
    return Util::resolveLocalPath(trimmed);
}

std::optional<QString> DirProvider::validateInput(const QString &rawRoot) {
    // 入参是控件原始文本，内部先定形——不存在"必须先定形"的前置条件
    const QString root = normalizeRoot(rawRoot);
    if (root.isEmpty())
        return trText(QT_TRANSLATE_NOOP("DirProvider", "Please select a bound directory."));
    // 本地目录列表仅接受真实本地路径；网络/流路径（http/ftp/stream 等，含 :// 且非
    // file:）拒绝。用串前缀判定，避免 QUrl::scheme() 误判盘符、
    // QUrl::fromUserInput 误判相对名 "a" 为非本地
    if (Util::isNetworkStreamPath(root))
        return trText(QT_TRANSLATE_NOOP(
            "DirProvider", "Local directory lists do not support network or stream paths."));
    if (!QDir(root).exists())
        return trText(QT_TRANSLATE_NOOP("DirProvider", "Directory does not exist: %1")).arg(root);
    return std::nullopt;
}

QStringList DirProvider::parseExtensions(const QString &text) {
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

// 枚举并造实体：成功带实体有序表与警告，失败带错误文案；
// N29 起不预热：collect 产 QFileInfoList（枚举一次 stat 顺手取得），遍历中经
// 注入口把文件元数据零成本注入实体（口径与惰性求值共用 fileMetaFrom）。
// N37：失败面收缩为"根未绑定 / 不存在 / 不可读"三项；递归中个别子目录不可读
// 为部分成功，随 warnings 回传（不再整次失败进占位）
std::expected<PlaylistProvider::Enumeration, QString> DirProvider::enumerate() const {
    if (m_root.isEmpty())
        return std::unexpected(trText(QT_TRANSLATE_NOOP("DirProvider", "No directory bound.")));
    const QDir rootDir(m_root);
    if (!rootDir.exists())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("DirProvider", "Directory does not exist: %1")).arg(m_root));
    if (!rootDir.isReadable())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("DirProvider", "Directory is not readable: %1")).arg(m_root));
    // 扩展名空表＝不过滤；过滤器一次构造逐层复用
    // N36：本批遍历参数经注册表策略快照取用（GUI 线程装配、任务线程只读，N36 修订 1）；
    // 起始处解析一次贯穿整次遍历，设置变更只影响下一次枚举。resolve 为空按缺省兜底
    int maxDepth = App::recursiveDepthLimitDefault();
    if (const auto policy = registry()->resolve<TraversalPolicy>())
        maxDepth = policy->depthLimit;
    QFileInfoList infos;
    QStringList warnings;
    // N36 修订 3：已访问目录的 canonical 路径集——符号链接环 / 同一目录经多条路径可达
    // 的剪枝依据，根先入集
    QSet<QString> visited;
    visited.insert(canonicalDirPath(m_root));
    collect(rootDir, m_extensions.isEmpty() ? QStringList() : toNameFilters(m_extensions),
            m_recursive, 0, maxDepth, visited, infos, warnings);
    // N37：warnings 随结果回传（部分成功），不再触发整次失败
    EntityList entities;
    entities.reserve(infos.size());
    for (const QFileInfo &fi : infos) {
        // 路径加工（绝对化 + 原生分隔符，B15）统一由 fromPlayUrl 内的
        // resolveLocalPath 出口负责，此处不再预处理；registry 转交（本地路径忽略）
        auto entity = PlaylistEntity::fromPlayUrl(fi.absoluteFilePath(), registry());
        injectFileMeta(*entity, LocalFileEntity::fileMetaFrom(fi)); // 零成本注入
        entities.push_back(std::move(entity));
    }
    return Enumeration{std::move(entities), std::move(warnings)};
}

bool DirProvider::supportsModify() const { return false; }

// 目录表不可增删，但可排序（规则随参数一并持久化）
bool DirProvider::supportsSort() const { return true; }

void DirProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // N44：类型键经基类取 typeKey()，不再写字面量
    settings.setValue(QStringLiteral("dirRoot"), m_root);
    settings.setValue(QStringLiteral("dirRecursive"), m_recursive);
    settings.setValue(QStringLiteral("dirExtensions"), m_extensions);
    // 目录绑定列表持久化排序键（含 None——显式记录用户未选择规则）
    settings.setValue(QStringLiteral("sortKey"), sortKey());
    settings.setValue(QStringLiteral("sortAscending"), sortAscending());
}

std::shared_ptr<PlaylistProvider>
DirProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    auto provider = std::make_shared<DirProvider>(
        settings.value(QStringLiteral("dirRoot")).toString(),
        settings.value(QStringLiteral("dirRecursive")).toBool(),
        settings.value(QStringLiteral("dirExtensions")).toStringList(), registry);
    provider->setSortRule(settings.value(QStringLiteral("sortKey"), 0).toInt(),
                          settings.value(QStringLiteral("sortAscending"), true).toBool());
    return provider;
}

void DirProvider::collect(const QDir &dir, const QStringList &filters, bool recursive, int depth,
                          int maxDepth, QSet<QString> &visited, QFileInfoList &out,
                          QStringList &warnings) {
    // 文件层：命中扩展名的条目信息追加（N29：整个 QFileInfo 留给注入，
    // 路径加工统一在 fromPlayUrl 的 resolveLocalPath 出口做）
    const QFileInfoList files =
        dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::NoSort);
    for (const QFileInfo &fi : files)
        out << fi;
    if (!recursive)
        return;
    // 目录层：先序深入子目录（平铺语义，次序无关紧要——视图层统一重排）；
    // 不可读子目录记 warning 后跳过（部分成功语义）
    const QFileInfoList subDirs =
        dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &fi : subDirs) {
        // N36：子目录深度超过上限不再深入（上限 n = 最多深入 n 层，0 = 不递归）；
        // 截断静默，不记 warning（超深是配置使然，不是错误）
        if (depth + 1 > maxDepth)
            continue;
        // N36 修订 3：已访问过（符号链接环，或同一目录经多条路径可达）即静默跳过，
        // 不记 warning——这是防御，不是错误；深度上限之外的第二道闸门
        const QString key = canonicalDirPath(fi.absoluteFilePath());
        if (visited.contains(key))
            continue;
        if (!QDir(fi.absoluteFilePath()).isReadable()) {
            warnings << trText(QT_TRANSLATE_NOOP("DirProvider",
                                                 "Subdirectory is not readable, skipped: %1"))
                            .arg(fi.absoluteFilePath());
            continue;
        }
        visited.insert(key);
        collect(fi.absoluteFilePath(), filters, recursive, depth + 1, maxDepth, visited, out,
                warnings);
    }
}

// N44：本文件定义了 Q_OBJECT 类（DirSettingsPage），CMake AUTOMOC 要求显式包含
// 同名 .moc（否则 configure 期直接报错），按源文件名而非类名
#include "DirProvider.moc"

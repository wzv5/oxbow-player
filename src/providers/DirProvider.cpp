#include "pch.h"

#include "DirProvider.h"

#include "Constants.h"
#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "TraversalPolicy.h"
#include "Util.h"
#include "entities/LocalFileEntity.h"
#include "ui/ExtensionFilterEdit.h"

namespace {
QString trText(const char *source) { return QCoreApplication::translate("DirProvider", source); }

// 与 WebDAV 参数页的选项文案逐字相同，两处不得各自措辞
constexpr char kBrowseLayered[] = QT_TRANSLATE_NOOP("DirProvider", "Layered Browsing");
constexpr char kBrowseFilesOnly[] = QT_TRANSLATE_NOOP("DirProvider", "Files Only");
constexpr char kBrowseFilesRecursive[] =
    QT_TRANSLATE_NOOP("DirProvider", "Files Only (Include Subdirectories)");

// junction 链解析上限：病态链 / 自指链不至于把枚举拖死
constexpr int kJunctionChainGuard = 32;

// 目录真实路径（去重键唯一来源）：先解开 NTFS junction 链，再取 canonical
// （解析符号链接 / . / ..），解析失败退回原路径，使同一目录经多条路径可达时
// 收敛为同一个键。
// Qt 6.11.2 / Windows 11 实测：canonicalFilePath() 不解析 junction——链接的
// canonical 仍是链接自身路径，直接当键等于没去重，junction 环会被逐层展开；
// 故先经 junctionTarget() 解到真实目录再取 canonical
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

DirProvider::DirProvider(const QString &root, BrowseMode mode, const QStringList &extensions,
                         const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_root(root), m_browseMode(mode), m_extensions(extensions) {}

// 页面类只在本文件可见，经 createDirSettingsPage 产出
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

        m_modeCombo = new QComboBox;
        m_modeCombo->addItem(trText(kBrowseLayered));
        m_modeCombo->addItem(trText(kBrowseFilesOnly));
        m_modeCombo->addItem(trText(kBrowseFilesRecursive));
        form->addRow(tr("Browse Mode"), m_modeCombo);

        m_extFilter = new ExtensionFilterEdit;
        form->addRow(tr("Filter"), m_extFilter);
        auto *extNote =
            new QLabel(tr("Space-separated file extensions; leave empty for no filter"));
        extNote->setEnabled(false);
        form->addRow(extNote);

        layout->addLayout(form);
        // 表单区在对话框里被纵向拉伸：不压尾部伸缩项，多余空间会被均摊到各行之间
        layout->addStretch(1);

        if (m_seed) {
            m_rootEdit->setText(m_seed->rootPath());
            m_modeCombo->setCurrentIndex(int(m_seed->browseMode()));
            m_extFilter->setText(m_seed->extensions().join(QLatin1Char(' ')));
        } else {
            m_modeCombo->setCurrentIndex(int(DirProvider::BrowseMode::Layered));
            m_extFilter->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        }

        connect(browseBtn, &QPushButton::clicked, this, &DirSettingsPage::browseRoot);
    }

    std::optional<QString> validate() const override {
        return DirProvider::validateInput(m_rootEdit->text());
    }

    bool hasChanges() const override {
        return !m_seed || rootValue() != m_seed->rootPath() ||
               modeValue() != m_seed->browseMode() || extensionsValue() != m_seed->extensions();
    }

    std::shared_ptr<PlaylistProvider> build() const override {
        return std::make_shared<DirProvider>(rootValue(), modeValue(), extensionsValue(),
                                             m_registry);
    }

  private:
    QString rootValue() const { return DirProvider::normalizeRoot(m_rootEdit->text()); }
    DirProvider::BrowseMode modeValue() const {
        return DirProvider::browseModeFromInt(m_modeCombo->currentIndex());
    }
    QStringList extensionsValue() const { return Util::parseExtensions(m_extFilter->text()); }

    void browseRoot() {
        const QString dir =
            QFileDialog::getExistingDirectory(this, tr("Select Directory"), rootValue());
        // Qt 来源的路径，拿到即转原生分隔符
        if (!dir.isEmpty())
            m_rootEdit->setText(Util::Url::toNative(dir));
    }

    const DirProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QLineEdit *m_rootEdit = nullptr;
    QComboBox *m_modeCombo = nullptr;
    ExtensionFilterEdit *m_extFilter = nullptr;
};

ProviderSettingsPage *createDirSettingsPage(const PlaylistProvider *seed,
                                            const std::shared_ptr<ServiceRegistry> &registry,
                                            QWidget *parent) {
    return new DirSettingsPage(seed ? static_cast<const DirProvider *>(seed) : nullptr, registry,
                               parent);
}

QString DirProvider::typeKey() const { return QLatin1String(kDirProviderKey); }

QString DirProvider::displayName() {
    return QCoreApplication::translate("DirProvider", kDirProviderName);
}

QString DirProvider::normalizeRoot(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QString();
    return Util::Url::toIdentity(trimmed);
}

std::optional<QString> DirProvider::validateInput(const QString &rawRoot) {
    const QString root = normalizeRoot(rawRoot);
    if (root.isEmpty())
        return trText(QT_TRANSLATE_NOOP("DirProvider", "Please select a bound directory."));
    // 用串前缀判定而非 QUrl::scheme()：后者会把盘符误判成 scheme，fromUserInput
    // 会把相对名误判成主机
    if (Util::Url::isNetworkStream(root))
        return trText(QT_TRANSLATE_NOOP(
            "DirProvider", "Local directory playlists do not support network or stream paths."));
    if (!QDir(root).exists())
        return trText(QT_TRANSLATE_NOOP("DirProvider", "Directory does not exist: %1")).arg(root);
    return std::nullopt;
}

QString DirProvider::resolveLayerPath(const QString &root, const QStringList &parent) {
    QString path = root;
    for (const QString &seg : parent)
        path = QDir(path).filePath(seg);
    return path;
}

DirProvider::BrowseMode DirProvider::browseModeFromInt(int value) {
    switch (value) {
    case int(BrowseMode::Layered):
        return BrowseMode::Layered;
    case int(BrowseMode::FilesOnly):
        return BrowseMode::FilesOnly;
    case int(BrowseMode::FilesRecursive):
        return BrowseMode::FilesRecursive;
    }
    return BrowseMode::Layered;
}

// 预切分数字段后按下标排序：比较器里重复 segmentName 会把 O(n) 摊成 O(n log n)
void DirProvider::sortEntitiesByNumericName(EntityList &entities) {
    const int n = int(entities.size());
    if (n < 2)
        return;
    std::vector<Util::SegmentedName> segs;
    segs.reserve(n);
    for (const auto &e : entities)
        segs.push_back(Util::segmentName(e->fileName()));
    std::vector<int> order;
    order.reserve(n);
    for (int i = 0; i < n; ++i)
        order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&segs](int a, int b) { return Util::naturalCompare(segs[a], segs[b]) < 0; });
    EntityList sorted;
    sorted.reserve(n);
    for (int i : order)
        sorted.push_back(std::move(entities[i]));
    entities = std::move(sorted);
}

void DirProvider::sortContainersByNumericTitle(QList<Container> &containers) {
    const int n = int(containers.size());
    if (n < 2)
        return;
    std::vector<Util::SegmentedName> segs;
    segs.reserve(n);
    for (const Container &c : containers)
        segs.push_back(Util::segmentName(c.title));
    std::vector<int> order;
    order.reserve(n);
    for (int i = 0; i < n; ++i)
        order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&segs](int a, int b) { return Util::naturalCompare(segs[a], segs[b]) < 0; });
    QList<Container> sorted;
    sorted.reserve(n);
    for (int i : order)
        sorted.push_back(std::move(containers[i]));
    containers = std::move(sorted);
}

// 平铺模式收到非空 parent 属调用方违约：Q_ASSERT 守候，Release 返回空结果
std::expected<PlaylistProvider::Enumeration, QString>
DirProvider::enumerate(const QStringList &parent) const {
    if (m_root.isEmpty())
        return std::unexpected(trText(QT_TRANSLATE_NOOP("DirProvider", "No directory bound.")));
    if (!parent.isEmpty() && m_browseMode != BrowseMode::Layered) {
        Q_ASSERT(false);
        return Enumeration{};
    }
    const QString layerPath = resolveLayerPath(m_root, parent);
    const QDir layerDir(layerPath);
    if (!layerDir.exists())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("DirProvider", "Directory does not exist: %1"))
                .arg(layerPath));
    if (!layerDir.isReadable())
        return std::unexpected(
            trText(QT_TRANSLATE_NOOP("DirProvider", "Directory is not readable: %1"))
                .arg(layerPath));
    Enumeration out;
    if (m_browseMode == BrowseMode::Layered) {
        const QFileInfoList subDirs =
            layerDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
        out.containers.reserve(subDirs.size());
        for (const QFileInfo &fi : subDirs) {
            Container c;
            c.id = fi.fileName();
            c.title = fi.fileName();
            c.path = Util::Url::toNative(fi.absoluteFilePath());
            // 供容器参与日期 / 大小排序
            c.meta = LocalFileEntity::fileMetaFrom(fi);
            out.containers.push_back(std::move(c));
        }
        sortContainersByNumericTitle(out.containers);
    }

    QFileInfoList infos;
    if (m_browseMode == BrowseMode::FilesRecursive) {
        // 策略在 GUI 线程装配、任务线程只读；起始处取一次快照，设置变更只影响
        // 下一次枚举。resolve 为空按缺省兜底
        int maxDepth = App::recursiveDepthLimitDefault();
        if (const auto policy = registry()->resolve<TraversalPolicy>())
            maxDepth = policy->depthLimit;
        // canonical 集：符号链接环与同目录多路径的剪枝依据
        QSet<QString> visited;
        visited.insert(canonicalDirPath(layerPath));
        collect(layerPath, m_extensions, true, 0, maxDepth, visited, infos, out.warnings);
    } else {
        infos = Util::listFiles(layerPath, m_extensions);
    }
    out.entities.reserve(infos.size());
    for (const QFileInfo &fi : infos) {
        // 工厂不代做定形：absoluteFilePath 已绝对，这里只补原生分隔符
        auto entity =
            PlaylistEntity::fromPath(Util::Url::toNative(fi.absoluteFilePath()), registry());
        injectFileMeta(*entity, LocalFileEntity::fileMetaFrom(fi));
        out.entities.push_back(std::move(entity));
    }
    sortEntitiesByNumericName(out.entities);
    return out;
}

bool DirProvider::supportsModify() const { return false; }

bool DirProvider::supportsSort() const { return true; }

void DirProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // 类型键经基类取 typeKey()
    settings.setValue(QStringLiteral("dirRoot"), m_root);
    settings.setValue(QStringLiteral("dirBrowseMode"), int(m_browseMode));
    settings.setValue(QStringLiteral("dirExtensions"), m_extensions);
    // 含 None：显式记录用户未选择规则
    settings.setValue(QStringLiteral("sortKey"), int(sortRule().key));
    settings.setValue(QStringLiteral("sortAscending"), sortRule().ascending);
}

std::shared_ptr<PlaylistProvider>
DirProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    auto provider = std::make_shared<DirProvider>(
        settings.value(QStringLiteral("dirRoot")).toString(),
        browseModeFromInt(settings.value(QStringLiteral("dirBrowseMode"), 0).toInt()),
        settings.value(QStringLiteral("dirExtensions")).toStringList(), registry);
    // 缺键落 {None, 升序}
    const SortRule rule{static_cast<SortKey>(settings.value(QStringLiteral("sortKey"), 0).toInt()),
                        settings.value(QStringLiteral("sortAscending"), true).toBool()};
    provider->setSortRule(rule);
    return provider;
}

void DirProvider::collect(const QString &dirPath, const QStringList &extensions, bool recursive,
                          int depth, int maxDepth, QSet<QString> &visited, QFileInfoList &out,
                          QStringList &warnings) {
    const QFileInfoList files = Util::listFiles(dirPath, extensions);
    for (const QFileInfo &fi : files)
        out << fi;
    if (!recursive)
        return;
    const QDir dir(dirPath);
    // 不可读子目录记 warning 后跳过（部分成功语义）
    const QFileInfoList subDirs =
        dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &fi : subDirs) {
        // 上限 n = 最多深入 n 层，0 = 不递归；截断静默，超深属配置而非错误
        if (depth + 1 > maxDepth)
            continue;
        // 环或同目录多路径：静默跳过，不记 warning
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
        collect(fi.absoluteFilePath(), extensions, recursive, depth + 1, maxDepth, visited, out,
                warnings);
    }
}

// 本文件定义 Q_OBJECT 类（DirSettingsPage），AUTOMOC 要求显式包含同名 .moc
#include "DirProvider.moc"

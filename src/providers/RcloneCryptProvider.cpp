#include "pch.h"

#include "RcloneCryptProvider.h"

#include "Constants.h"
#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "TraversalPolicy.h"
#include "Util.h"
#include "entities/RcloneCryptEntity.h"
#include "streams/FileByteStream.h"
#include "streams/RcloneCryptByteStream.h"
#include "streams/StreamEndpoint.h"
#include "ui/ExtensionFilterEdit.h"

namespace {

// 非 Q_OBJECT 类的用户可见串：英文源 + QT_TRANSLATE_NOOP 标注翻译上下文，
// 运行期经 QCoreApplication::translate 取值（WebDavProvider 同款约定）
QString trText(const char *source) {
    return QCoreApplication::translate("RcloneCryptProvider", source);
}

// 三档浏览模式的源文案：与 DirProvider / WebDavProvider 的选项文案逐字相同
// （交叉验收项，N46 §2.1），翻译上下文归属本提供者
constexpr char kBrowseLayered[] = QT_TRANSLATE_NOOP("RcloneCryptProvider", "Layered Browsing");
constexpr char kBrowseFilesOnly[] = QT_TRANSLATE_NOOP("RcloneCryptProvider", "Files Only");
constexpr char kBrowseFilesRecursive[] =
    QT_TRANSLATE_NOOP("RcloneCryptProvider", "Files Only (Include Subdirectories)");

// 根目录 + 密文相对路径 → 文件系统路径：内部一律 '/' 分段，落到文件系统前才转
// 原生分隔符（N98 §4，Util::Url::toNative 口径）
QString toNativePath(const QString &root, const QString &cipherRel) {
    if (cipherRel.isEmpty())
        return root;
    return Util::Url::toNative(root + QLatin1Char('/') + cipherRel);
}

// ——提供者初始排序（N45 §2.7 同形）：文件名数字化，稳定排序保等键原序——
// 排序键预切分一次存数组再按下标排（DirProvider 同形）：比较器里反复 segmentName
// 会把 O(n) 次切分放大成 O(n log n) 次
void sortEntitiesByNumericName(RcloneCryptProvider::EntityList &entities) {
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
    RcloneCryptProvider::EntityList sorted;
    sorted.reserve(n);
    for (int i : order)
        sorted.push_back(std::move(entities[i]));
    entities = std::move(sorted);
}

void sortContainersByNumericTitle(QList<PlaylistProvider::Container> &containers) {
    const int n = int(containers.size());
    if (n < 2)
        return;
    std::vector<Util::SegmentedName> segs;
    segs.reserve(n);
    for (const PlaylistProvider::Container &c : containers)
        segs.push_back(Util::segmentName(c.title));
    std::vector<int> order;
    order.reserve(n);
    for (int i = 0; i < n; ++i)
        order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&segs](int a, int b) { return Util::naturalCompare(segs[a], segs[b]) < 0; });
    QList<PlaylistProvider::Container> sorted;
    sorted.reserve(n);
    for (int i : order)
        sorted.push_back(std::move(containers[i]));
    containers = std::move(sorted);
}

// ——N98 §6：来源解析点（来源私有的东西与参数页同级定义，不外露）——
// 持不可变参数与派生好的密钥（构造期快照）；openStream 在 demux 线程把身份
// 解码回明文相对路径、逐段现算密文路径，FileByteStream 打开后包解密流。
// 密钥未就绪（列表从未枚举过）按失败显影，不现场派生（N98 §3：端点创建
// 不触发派生）
class RcloneCryptStreamEndpoint final : public StreamEndpoint {
  public:
    RcloneCryptStreamEndpoint(std::shared_ptr<const RcloneCrypt> cipher, QString root)
        : m_cipher(std::move(cipher)), m_root(std::move(root)) {}

    std::expected<std::shared_ptr<ByteStream>, QString>
    openStream(const QString &identity) const override {
        if (!m_cipher)
            return std::unexpected(
                QStringLiteral("crypt key not derived (list was never enumerated)"));
        if (!identity.startsWith(QLatin1String(kRcloneCryptScheme)))
            return std::unexpected(QStringLiteral("unexpected identity form: %1").arg(identity));
        const QString encodedRel =
            identity.mid(int(sizeof(kRcloneCryptScheme) - 1)); // 剥 scheme 留编码相对路径
        const QString plainRel = RcloneCryptProvider::decodeIdentity(encodedRel);
        // 密文路径现算：文件路径走文件形态（末段版本串处理与枚举侧互逆）
        const QString cipherRel =
            QString::fromStdString(m_cipher->encryptFileName(plainRel.toStdString()));
        const QString fsPath = toNativePath(m_root, cipherRel);
        auto file = FileByteStream::open(fsPath);
        if (!file)
            return std::unexpected(file.error());
        auto stream = RcloneCryptByteStream::open(m_cipher, *file);
        if (!stream)
            return std::unexpected(stream.error());
        return stream.value();
    }

  private:
    std::shared_ptr<const RcloneCrypt> m_cipher; // 构造后不可变（N52 §2.3 契约 5）
    QString m_root;
};

} // namespace

RcloneCryptProvider::RcloneCryptProvider(const QString &root, const QString &password,
                                         const QString &salt, RcloneCrypt::NameMode nameMode,
                                         RcloneCrypt::NameEncoding encoding, bool dirNameEncrypt,
                                         const QString &suffix, bool strictNames,
                                         bool passBadBlocks, BrowseMode browseMode,
                                         const QStringList &extensions,
                                         const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_root(root), m_password(password), m_salt(salt),
      m_nameMode(nameMode), m_nameEncoding(encoding), m_encodingRaw(int(encoding)),
      m_dirNameEncrypt(dirNameEncrypt), m_suffix(suffix), m_strictNames(strictNames),
      m_passBadBlocks(passBadBlocks), m_browseMode(browseMode), m_extensions(extensions) {}

QString RcloneCryptProvider::typeKey() const { return QLatin1String(kRcloneCryptProviderKey); }

QString RcloneCryptProvider::displayName() {
    return QCoreApplication::translate("RcloneCryptProvider", kRcloneCryptProviderName);
}

// N98 §6：恒为 Required——直发分支由基类三态表保证不可达，directPlayUrl 不覆写
PlaylistProvider::TransitCapability RcloneCryptProvider::transitCapability() const {
    return TransitCapability::Required;
}

std::shared_ptr<StreamEndpoint> RcloneCryptProvider::createStreamEndpoint() const {
    return std::make_shared<RcloneCryptStreamEndpoint>(cipherIfReady(), m_root);
}

bool RcloneCryptProvider::supportsModify() const { return false; }

bool RcloneCryptProvider::supportsSort() const { return true; }

RcloneCrypt::Params RcloneCryptProvider::cipherParams() const {
    RcloneCrypt::Params p;
    p.password = m_password.toStdString();
    p.salt = m_salt.toStdString();
    p.mode = m_nameMode;
    p.encoding = m_nameEncoding;
    p.dirNameEncrypt = m_dirNameEncrypt;
    p.suffix = m_suffix.toStdString();
    p.passBadBlocks = m_passBadBlocks;
    return p;
}

std::expected<std::shared_ptr<const RcloneCrypt>, QString>
RcloneCryptProvider::cipherForEnum() const {
    QMutexLocker lock(&m_cipherMutex);
    if (!m_cipherError.isEmpty())
        return std::unexpected(m_cipherError);
    if (m_cipher)
        return m_cipher;
    // 派生（scrypt，实测 100–200ms、峰值 16 MiB）在此完成：enumerate 天然在
    // 工作线程（N98 §3），互斥锁保证同一提供者至多派生一次
    auto created = RcloneCrypt::create(cipherParams());
    if (!created) {
        m_cipherError = QString::fromStdString(created.error());
        return std::unexpected(m_cipherError);
    }
    m_cipher = *created;
    return m_cipher;
}

std::shared_ptr<const RcloneCrypt> RcloneCryptProvider::cipherIfReady() const {
    QMutexLocker lock(&m_cipherMutex);
    return m_cipher;
}

bool RcloneCryptProvider::extensionMatches(const QString &plainName) const {
    if (m_extensions.isEmpty())
        return true; // 空表不过滤
    const int dot = plainName.lastIndexOf(QLatin1Char('.'));
    if (dot < 0)
        return false;
    // 清单恒为无点小写（parseExtensions 口径），解密后的名字大小写原样
    return m_extensions.contains(plainName.mid(dot + 1).toLower());
}

std::expected<RcloneCryptProvider::Enumeration, QString>
RcloneCryptProvider::enumerate(const QStringList &parent) const {
    // 配置数据异常（如编码取值域外）不放行任何一次枚举——错误经既有通道上抛，
    // 视图落占位行；此时密钥派生根本不必发生
    if (!m_configError.isEmpty())
        return std::unexpected(m_configError);

    auto cipher = cipherForEnum();
    if (!cipher)
        return std::unexpected(cipher.error());

    // TraversalPolicy 的深度上限只约束递归形态（N36：resolve 缺省按装配错误兜底）
    const int depthLimit = [this] {
        const auto policy = registry() ? registry()->resolve<TraversalPolicy>() : nullptr;
        return policy ? policy->depthLimit : App::recursiveDepthLimitDefault();
    }();

    const QString plainRel = parent.join(QLatin1Char('/')); // 空即根层
    Enumeration out;

    if (m_browseMode == BrowseMode::FilesOnly || m_browseMode == BrowseMode::FilesRecursive) {
        // 平铺形态：只对空 parent 响应（平铺提供者收到非空 parent 属调用方违约，
        // Debug 断言显影、release 降级空结果——基类契约的既有先例）
        Q_ASSERT(parent.isEmpty());
        if (!parent.isEmpty())
            return out;
        QStringList warnings;
        const QString cipherRoot =
            QString::fromStdString(cipher.value()->encryptDirName(std::string()));
        const QString rootDir = toNativePath(m_root, cipherRoot);
        QSet<QString> visited;
        const bool recursive = m_browseMode == BrowseMode::FilesRecursive;
        if (auto err = collectLayer(cipher.value(), rootDir, QString(), false, recursive, 0,
                                    depthLimit, visited, out, warnings))
            return std::unexpected(*err);
        out.warnings = std::move(warnings);
        sortEntitiesByNumericName(out.entities);
        return out;
    }

    // 分层形态：parent 是明文段序列（视图只对本提供者产出的容器 id 发起调用）
    const QString cipherRel =
        QString::fromStdString(cipher.value()->encryptDirName(plainRel.toStdString()));
    const QString dirPath = toNativePath(m_root, cipherRel);
    QStringList warnings;
    QSet<QString> visited;
    if (auto err = collectLayer(cipher.value(), dirPath, plainRel, true, false, 0, depthLimit,
                                visited, out, warnings))
        return std::unexpected(*err);
    out.warnings = std::move(warnings);
    sortContainersByNumericTitle(out.containers);
    sortEntitiesByNumericName(out.entities);
    return out;
}

std::optional<QString> RcloneCryptProvider::collectLayer(
    const std::shared_ptr<const RcloneCrypt> &cipher, const QString &cipherDir,
    const QString &plainRel, bool makeContainers, bool recurse, int depth, int depthLimit,
    QSet<QString> &visited, Enumeration &out, QStringList &warnings) const {
    QDir dir(cipherDir);
    // 递归形态按 canonical 路径剪环（N36 修订 3 同款）
    if (recurse) {
        const QString canon = dir.canonicalPath();
        if (!canon.isEmpty()) {
            if (visited.contains(canon))
                return std::nullopt;
            visited.insert(canon);
        }
    }
    if (!dir.exists())
        return trText(QT_TRANSLATE_NOOP("RcloneCryptProvider", "Encrypted directory not found: %1"))
            .arg(cipherDir);

    const QFileInfoList entries =
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (entry.isDir()) {
            auto plain = cipher->decryptDirName(name.toStdString());
            if (!plain) {
                if (m_strictNames) {
                    return trText(QT_TRANSLATE_NOOP("RcloneCryptProvider",
                                                    "Undecryptable directory name detected: %1"))
                        .arg(name);
                }
                warnings << trText(QT_TRANSLATE_NOOP("RcloneCryptProvider",
                                                     "Skipped undecryptable directory name: %1"))
                                .arg(name);
                continue;
            }
            const QString plainName = QString::fromStdString(*plain);
            const QString childPlain =
                plainRel.isEmpty() ? plainName : plainRel + QLatin1Char('/') + plainName;
            if (makeContainers) {
                // 容器：id 与 title 取明文段名，path 展示位留空（Jellyfin 同口径）
                Container container;
                container.id = plainName;
                container.title = plainName;
                out.containers.push_back(std::move(container));
            } else if (recurse && depth < depthLimit) {
                // 深度超过上限不再深入，静默截断（N36 同款）。
                // 子目录密文路径只加密**子段名**——encryptDirName 是逐段语义，
                // 传整段相对路径会把父段前缀重复拼进已加密父目录下
                const QString childCipher =
                    QString::fromStdString(cipher->encryptDirName(plainName.toStdString()));
                if (auto err =
                        collectLayer(cipher, toNativePath(cipherDir, childCipher), childPlain,
                                     false, true, depth + 1, depthLimit, visited, out, warnings))
                    return err;
            }
            continue;
        }

        // 版本串密文名经 decryptFileName 解出"明文名 + 版本串"（版本串在解码后
        // 加回），与现行文件身份不同、无冲突，照常列出
        auto plain = cipher->decryptFileName(name.toStdString());
        if (!plain) {
            if (m_strictNames) {
                return trText(QT_TRANSLATE_NOOP("RcloneCryptProvider",
                                                "Undecryptable file name detected: %1"))
                    .arg(name);
            }
            warnings << trText(QT_TRANSLATE_NOOP("RcloneCryptProvider",
                                                 "Skipped undecryptable file name: %1"))
                            .arg(name);
            continue;
        }
        const QString plainName = QString::fromStdString(*plain);
        if (!extensionMatches(plainName))
            continue;

        // 身份：URN（编码相对路径）；根目录归提供者，不进身份串
        const QString rel =
            plainRel.isEmpty() ? plainName : plainRel + QLatin1Char('/') + plainName;
        const QString identity = QLatin1String(kRcloneCryptScheme) + encodeIdentity(rel);
        auto entity = std::make_shared<RcloneCryptEntity>(identity, registry());
        injectDisplayName(*entity, plainName);
        injectFileName(*entity, plainName);
        // 文件级元数据：size=明文长度（换算失败按值未知）、mtime=密文文件的 mtime
        FileMeta meta;
        const auto plainSize = RcloneCrypt::decryptedSize(entry.size());
        if (plainSize)
            meta.size = *plainSize;
        meta.modified = entry.lastModified();
        injectFileMeta(*entity, meta);
        out.entities.push_back(std::move(entity));
    }
    return std::nullopt;
}

// ——参数页（N44 口径）：页面类只在本文件可见，经 createRcloneCryptSettingsPage 产出。
// 一行一参（N98 §7）：根目录（浏览按钮）、浏览模式、口令、salt、名字加密模式、编码、
// 目录名加密、suffix、strict_names、pass_bad_blocks、扩展名清单。
// transitCapability()==Required ⇒ enableTransitOption(nullopt)：只在页底加一行说明、
// 不给勾选框（N52 §2.8）。口令与 salt 用 Password 回显掩码但存原文（同 WebDAV）
class RcloneCryptSettingsPage : public ProviderSettingsPage {
    Q_OBJECT
  public:
    RcloneCryptSettingsPage(const RcloneCryptProvider *seed,
                            const std::shared_ptr<ServiceRegistry> &registry, QWidget *parent)
        : ProviderSettingsPage(parent), m_seed(seed), m_registry(registry) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);

        m_rootEdit = new QLineEdit;
        auto *rootRow = new QHBoxLayout;
        rootRow->addWidget(m_rootEdit, 1);
        auto *browseBtn = new QPushButton(QStringLiteral("…"));
        browseBtn->setFixedWidth(28);
        browseBtn->setToolTip(tr("Browse for encrypted directory"));
        rootRow->addWidget(browseBtn);
        form->addRow(tr("Encrypted Directory"), rootRow);

        m_modeCombo = new QComboBox;
        m_modeCombo->addItem(trText(kBrowseLayered));
        m_modeCombo->addItem(trText(kBrowseFilesOnly));
        m_modeCombo->addItem(trText(kBrowseFilesRecursive));
        form->addRow(tr("Browse Mode"), m_modeCombo);

        m_passwordEdit = new QLineEdit;
        m_passwordEdit->setEchoMode(QLineEdit::EchoMode::Password);
        form->addRow(tr("Password"), m_passwordEdit);
        m_saltEdit = new QLineEdit;
        m_saltEdit->setEchoMode(QLineEdit::EchoMode::Password);
        form->addRow(tr("Salt"), m_saltEdit);

        m_nameModeCombo = new QComboBox;
        m_nameModeCombo->addItem(tr("Standard"));
        m_nameModeCombo->addItem(tr("Obfuscate"));
        m_nameModeCombo->addItem(tr("Off"));
        form->addRow(tr("Filename Encryption"), m_nameModeCombo);

        m_encodingCombo = new QComboBox;
        m_encodingCombo->addItem(tr("base32"));
        m_encodingCombo->addItem(tr("base64"));
        form->addRow(tr("Filename Encoding"), m_encodingCombo);

        m_dirNamesCheck =
            new QCheckBox(tr("Encrypt directory names (only when filename encryption is on)"));
        form->addRow(QString(), m_dirNamesCheck);

        m_suffixEdit = new QLineEdit;
        auto *suffixNote = new QLabel(tr("Only used when filename encryption is off"));
        suffixNote->setEnabled(false);
        auto *suffixField = new QVBoxLayout;
        suffixField->addWidget(m_suffixEdit);
        suffixField->addWidget(suffixNote);
        form->addRow(tr("Suffix"), suffixField);

        m_strictCheck = new QCheckBox(tr("Fail on undecryptable names (strict names)"));
        form->addRow(QString(), m_strictCheck);
        m_passBadBlocksCheck =
            new QCheckBox(tr("Deliver zeroed block on MAC failure (pass bad blocks)"));
        form->addRow(QString(), m_passBadBlocksCheck);

        m_extFilter = new ExtensionFilterEdit;
        auto *extNote =
            new QLabel(tr("Space-separated file extensions; leave empty for no filter"));
        extNote->setEnabled(false);
        auto *noteRow = new QHBoxLayout;
        noteRow->addWidget(extNote);
        noteRow->addSpacing(12);
        auto *extField = new QVBoxLayout;
        extField->addWidget(m_extFilter);
        extField->addLayout(noteRow);
        form->addRow(tr("Filter"), extField);

        layout->addLayout(form);
        // N52 §2.8：Required 类型不给勾选框，只在页底加一行说明
        enableTransitOption(std::nullopt);
        layout->addStretch(1);

        if (m_seed) {
            m_rootEdit->setText(m_seed->rootPath());
            m_modeCombo->setCurrentIndex(int(m_seed->browseMode()));
            m_passwordEdit->setText(m_seed->password());
            m_saltEdit->setText(m_seed->salt());
            m_nameModeCombo->setCurrentIndex(int(m_seed->nameMode()));
            m_encodingCombo->setCurrentIndex(int(m_seed->nameEncoding()));
            m_dirNamesCheck->setChecked(m_seed->dirNameEncrypt());
            m_suffixEdit->setText(m_seed->suffix());
            m_strictCheck->setChecked(m_seed->strictNames());
            m_passBadBlocksCheck->setChecked(m_seed->passBadBlocks());
            m_extFilter->setText(m_seed->extensions().join(QLatin1Char(' ')));
        } else {
            m_modeCombo->setCurrentIndex(int(RcloneCryptProvider::BrowseMode::Layered));
            m_nameModeCombo->setCurrentIndex(int(RcloneCrypt::NameMode::Standard));
            m_encodingCombo->setCurrentIndex(int(RcloneCrypt::NameEncoding::Base32));
            m_dirNamesCheck->setChecked(true);
            m_suffixEdit->setText(QStringLiteral(".bin"));
            m_extFilter->setText(App::defaultMediaExtensions().join(QLatin1Char(' ')));
        }

        connect(browseBtn, &QPushButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Encrypted Directory"),
                                                                  m_rootEdit->text());
            if (!dir.isEmpty())
                m_rootEdit->setText(dir);
        });
    }

    std::optional<QString> validate() const override {
        return RcloneCryptProvider::validateInput(m_rootEdit->text());
    }

    bool hasChanges() const override {
        return !m_seed || rootValue() != m_seed->rootPath() ||
               passwordValue() != m_seed->password() || saltValue() != m_seed->salt() ||
               nameModeValue() != m_seed->nameMode() ||
               nameEncodingValue() != m_seed->nameEncoding() ||
               m_dirNamesCheck->isChecked() != m_seed->dirNameEncrypt() ||
               suffixValue() != m_seed->suffix() ||
               m_strictCheck->isChecked() != m_seed->strictNames() ||
               m_passBadBlocksCheck->isChecked() != m_seed->passBadBlocks() ||
               modeValue() != m_seed->browseMode() || extensionsValue() != m_seed->extensions();
    }

    std::shared_ptr<PlaylistProvider> build() const override {
        return std::make_shared<RcloneCryptProvider>(
            rootValue(), passwordValue(), saltValue(), nameModeValue(), nameEncodingValue(),
            m_dirNamesCheck->isChecked(), suffixValue(), m_strictCheck->isChecked(),
            m_passBadBlocksCheck->isChecked(), modeValue(), extensionsValue(), m_registry);
    }

  private:
    // 取规范化参数值（hasChanges 与 build 共用同一出口；幂等回显硬要求）
    QString rootValue() const { return RcloneCryptProvider::normalizeRoot(m_rootEdit->text()); }
    QString passwordValue() const { return m_passwordEdit->text(); }
    QString saltValue() const { return m_saltEdit->text(); }
    RcloneCrypt::NameMode nameModeValue() const {
        return RcloneCryptProvider::nameModeFromInt(m_nameModeCombo->currentIndex());
    }
    RcloneCrypt::NameEncoding nameEncodingValue() const {
        return RcloneCryptProvider::nameEncodingFromInt(m_encodingCombo->currentIndex());
    }
    QString suffixValue() const { return m_suffixEdit->text(); }
    RcloneCryptProvider::BrowseMode modeValue() const {
        return RcloneCryptProvider::browseModeFromInt(m_modeCombo->currentIndex());
    }
    QStringList extensionsValue() const {
        return RcloneCryptProvider::parseExtensions(m_extFilter->text());
    }

    const RcloneCryptProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QLineEdit *m_rootEdit = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_saltEdit = nullptr;
    QComboBox *m_nameModeCombo = nullptr;
    QComboBox *m_encodingCombo = nullptr;
    QCheckBox *m_dirNamesCheck = nullptr;
    QLineEdit *m_suffixEdit = nullptr;
    QCheckBox *m_strictCheck = nullptr;
    QCheckBox *m_passBadBlocksCheck = nullptr;
    ExtensionFilterEdit *m_extFilter = nullptr;
};

ProviderSettingsPage *
createRcloneCryptSettingsPage(const PlaylistProvider *seed,
                              const std::shared_ptr<ServiceRegistry> &registry, QWidget *parent) {
    return new RcloneCryptSettingsPage(
        seed ? static_cast<const RcloneCryptProvider *>(seed) : nullptr, registry, parent);
}

// ——静态纯函数——

QString RcloneCryptProvider::normalizeRoot(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return trimmed;
    return Util::Url::toIdentity(trimmed);
}

std::optional<QString> RcloneCryptProvider::validateInput(const QString &rawRoot) {
    const QString root = normalizeRoot(rawRoot);
    if (root.isEmpty())
        return trText(QT_TRANSLATE_NOOP("RcloneCryptProvider", "Encrypted directory must be set"));
    if (Util::Url::isNetworkStream(root))
        return trText(
            QT_TRANSLATE_NOOP("RcloneCryptProvider", "Encrypted directory must be a local path"));
    return std::nullopt;
}

QStringList RcloneCryptProvider::parseExtensions(const QString &text) {
    QStringList out;
    const QStringList parts =
        text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (QString part : parts) {
        if (part.startsWith(QLatin1Char('.')))
            part.remove(0, 1);
        part = part.toLower();
        if (!part.isEmpty() && !out.contains(part))
            out.append(part);
    }
    return out;
}

RcloneCrypt::NameMode RcloneCryptProvider::nameModeFromInt(int value) {
    if (value < 0 || value > 2)
        return RcloneCrypt::NameMode::Standard;
    return static_cast<RcloneCrypt::NameMode>(value);
}

RcloneCrypt::NameEncoding RcloneCryptProvider::nameEncodingFromInt(int value) {
    if (value != 0 && value != 1)
        return RcloneCrypt::NameEncoding::Base32;
    return static_cast<RcloneCrypt::NameEncoding>(value);
}

std::optional<RcloneCrypt::NameEncoding> RcloneCryptProvider::nameEncodingFromIni(int value) {
    if (value == 0)
        return RcloneCrypt::NameEncoding::Base32;
    if (value == 1)
        return RcloneCrypt::NameEncoding::Base64;
    return std::nullopt; // 含 2=base32768（本期不实现）与一切乱值
}

RcloneCryptProvider::BrowseMode RcloneCryptProvider::browseModeFromInt(int value) {
    if (value < 0 || value > 2)
        return RcloneCryptProvider::BrowseMode::Layered;
    return static_cast<RcloneCryptProvider::BrowseMode>(value);
}

QString RcloneCryptProvider::encodeIdentity(const QString &plainRelativePath) {
    if (plainRelativePath.isEmpty())
        return plainRelativePath;
    const QStringList segs = plainRelativePath.split(QLatin1Char('/'));
    QStringList encoded;
    encoded.reserve(segs.size());
    for (const QString &seg : segs)
        encoded.append(QString::fromUtf8(QUrl::toPercentEncoding(seg)));
    return encoded.join(QLatin1Char('/'));
}

QString RcloneCryptProvider::decodeIdentity(const QString &encodedRelativePath) {
    if (encodedRelativePath.isEmpty())
        return encodedRelativePath;
    const QStringList segs = encodedRelativePath.split(QLatin1Char('/'));
    QStringList plain;
    plain.reserve(segs.size());
    for (const QString &seg : segs)
        plain.append(QUrl::fromPercentEncoding(seg.toUtf8()));
    return plain.join(QLatin1Char('/'));
}

// ——INI 序列化——

void RcloneCryptProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings); // N44：类型键经基类取 typeKey()
    settings.setValue(QStringLiteral("rclonecrypt_root"), m_root);
    settings.setValue(QStringLiteral("rclonecrypt_password"), m_password);
    settings.setValue(QStringLiteral("rclonecrypt_salt"), m_salt);
    settings.setValue(QStringLiteral("rclonecrypt_name_mode"), int(m_nameMode));
    settings.setValue(QStringLiteral("rclonecrypt_encoding"), m_encodingRaw);
    settings.setValue(QStringLiteral("rclonecrypt_dir_names"), m_dirNameEncrypt);
    settings.setValue(QStringLiteral("rclonecrypt_suffix"), m_suffix);
    settings.setValue(QStringLiteral("rclonecrypt_strict_names"), m_strictNames);
    settings.setValue(QStringLiteral("rclonecrypt_pass_bad_blocks"), m_passBadBlocks);
    settings.setValue(QStringLiteral("rclonecrypt_browse_mode"), int(m_browseMode));
    settings.setValue(QStringLiteral("rclonecrypt_extensions"), m_extensions);
    settings.setValue(QStringLiteral("sortKey"), int(sortRule().key));
    settings.setValue(QStringLiteral("sortAscending"), sortRule().ascending);
}

std::shared_ptr<PlaylistProvider>
RcloneCryptProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    const int rawEncoding = settings.value(QStringLiteral("rclonecrypt_encoding"), 0).toInt();
    const auto encoding = nameEncodingFromIni(rawEncoding);
    auto provider = std::make_shared<RcloneCryptProvider>(
        settings.value(QStringLiteral("rclonecrypt_root")).toString(),
        settings.value(QStringLiteral("rclonecrypt_password")).toString(),
        settings.value(QStringLiteral("rclonecrypt_salt")).toString(),
        nameModeFromInt(settings.value(QStringLiteral("rclonecrypt_name_mode"), 0).toInt()),
        encoding.value_or(RcloneCrypt::NameEncoding::Base32),
        settings.value(QStringLiteral("rclonecrypt_dir_names"), true).toBool(),
        settings.value(QStringLiteral("rclonecrypt_suffix")).toString(),
        settings.value(QStringLiteral("rclonecrypt_strict_names"), false).toBool(),
        settings.value(QStringLiteral("rclonecrypt_pass_bad_blocks"), false).toBool(),
        browseModeFromInt(settings.value(QStringLiteral("rclonecrypt_browse_mode"), 0).toInt()),
        settings.value(QStringLiteral("rclonecrypt_extensions")).toStringList(), registry);
    // 域外编码不静默降成 base32：提供者照常出生（根路径、其余参数与存量条目不因此丢），
    // 但带上配置错误——一次枚举都不放行。参数页重开时下拉框只有两个合法项，改回去
    // 保存即修复
    if (!encoding) {
        provider->setEncodingRaw(rawEncoding);
        provider->m_configError =
            trText(QT_TRANSLATE_NOOP("RcloneCryptProvider", "Unsupported filename encoding: %1"))
                .arg(rawEncoding);
    }
    const SortRule rule{static_cast<SortKey>(settings.value(QStringLiteral("sortKey"), 0).toInt()),
                        settings.value(QStringLiteral("sortAscending"), true).toBool()};
    provider->setSortRule(rule);
    return provider;
}

#include "RcloneCryptProvider.moc"

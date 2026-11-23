#include "pch.h"

#include "ScriptListProvider.h"

#include "Logging.h"
#include "ProviderSettingsPage.h"
#include "ServiceRegistry.h"
#include "playlist/ScriptListChannel.h"
#include "playlist/ScriptListSpec.h"

namespace {

// 枚举在任务线程上跑，文案显式取 translate 上下文
QString scriptListText(const char *source) {
    return QCoreApplication::translate("ScriptListProvider", source);
}

constexpr char kTextNoScript[] =
    QT_TRANSLATE_NOOP("ScriptListProvider", "No script is providing this list.");
constexpr char kTextScriptGone[] =
    QT_TRANSLATE_NOOP("ScriptListProvider", "The script providing this list is no longer running.");
constexpr char kTextTimeout[] =
    QT_TRANSLATE_NOOP("ScriptListProvider", "The script list did not respond in time.");
constexpr char kTextInvalidData[] =
    QT_TRANSLATE_NOOP("ScriptListProvider", "The script list returned invalid data.");
constexpr char kTextSkipped[] = QT_TRANSLATE_NOOP(
    "ScriptListProvider",
    "Some items were skipped because they do not have a valid absolute path or URL.");

} // namespace

ScriptListProvider::ScriptListProvider(const QString &scriptId,
                                       const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistProvider(registry), m_scriptId(scriptId) {}

QString ScriptListProvider::typeKey() const { return QLatin1String(kScriptListProviderKey); }

bool ScriptListProvider::supportsModify() const { return false; }

bool ScriptListProvider::supportsSort() const { return true; }

SortRule ScriptListProvider::defaultSortRule() const { return {SortKey::EnumerationOrder, true}; }

QString ScriptListProvider::displayName() {
    return QCoreApplication::translate("ScriptListProvider", kScriptListProviderName);
}

std::expected<PlaylistProvider::Enumeration, QString>
ScriptListProvider::enumerate(const QStringList &parent) const {
    // 现取通道：列表恢复早于通道出生；局部强引用保证调用期间服务不被拆解
    const auto channel = registry()->resolve<ScriptListChannel>();
    if (!channel) {
        OXBOW_LOG_WARNING() << "ScriptListProvider: ScriptListChannel is not assembled";
        return std::unexpected(scriptListText(kTextNoScript));
    }
    if (m_scriptId.isEmpty())
        return std::unexpected(scriptListText(kTextNoScript));

    // 首载给脚本一个注册窗口（插件可能先于脚本就绪），等过一次即止
    if (m_firstLoad.exchange(false)) {
        if (!channel->waitRegistered(m_scriptId, channel->idWaitTimeout()))
            return std::unexpected(scriptListText(kTextNoScript));
    }

    const auto reply =
        channel->requestLayer(m_scriptId, parent.join(QLatin1Char('/'))); // 根层为空串
    if (!reply) {
        switch (reply.error()) {
        case ScriptListChannel::Error::NotRegistered:
            return std::unexpected(scriptListText(kTextNoScript));
        case ScriptListChannel::Error::SendFailed:
            return std::unexpected(scriptListText(kTextScriptGone));
        case ScriptListChannel::Error::Timeout:
            return std::unexpected(scriptListText(kTextTimeout));
        case ScriptListChannel::Error::Closed:
            // 退出窗口期，视图已死；映射并入数据非法
            return std::unexpected(scriptListText(kTextInvalidData));
        }
    }

    const ScriptListSpec::LayerData layer = ScriptListSpec::parseLayer(reply->toUtf8());
    if (!layer.valid)
        return std::unexpected(scriptListText(kTextInvalidData));
    // 脚本自定错误：整层按失败占位，文案原样上屏（脚本自己的语言，不经 translate）
    if (!layer.error.isEmpty())
        return std::unexpected(layer.error);

    Enumeration out;
    out.containers.reserve(layer.containers.size());
    for (const ScriptListSpec::ContainerData &container : layer.containers) {
        // 容器没有文件名；meta 留给排序，自报面内不给即未知
        out.containers.append({container.id, container.title, std::nullopt, container.path});
    }

    out.entities.reserve(layer.items.size());
    for (const ScriptListSpec::ItemData &item : layer.items) {
        PlaylistEntity::Ptr entity = PlaylistEntity::fromPath(item.url, registry());
        if (!entity)
            continue; // parseLayer 已卡定形门禁，此处是契约兜底
        // 注入次序固定；名字键先后不影响终态（setTitle 置旗后 setFilename 不再联动）
        if (item.fileSize || item.fileCreated || item.fileModified) {
            FileMeta meta;
            if (item.fileSize)
                meta.size = *item.fileSize;
            if (item.fileCreated)
                meta.created = QDateTime::fromSecsSinceEpoch(*item.fileCreated);
            if (item.fileModified)
                meta.modified = QDateTime::fromSecsSinceEpoch(*item.fileModified);
            injectFileMeta(*entity, meta);
        }
        if (!item.filename.isEmpty())
            injectFileName(*entity, item.filename);
        if (!item.displayName.isEmpty())
            injectDisplayName(*entity, item.displayName);
        if (!item.options.isEmpty())
            injectPlayOptions(*entity, item.options);
        const bool anyMeta = !item.metaFormat.isEmpty() || item.metaDuration.has_value() ||
                             item.metaRecordedDate.isValid() || !item.metaTitle.isEmpty() ||
                             !item.metaAlbum.isEmpty() || !item.metaPerformer.isEmpty();
        if (anyMeta) {
            PlaylistEntity::MediaMeta meta;
            meta.format = item.metaFormat;
            meta.duration = item.metaDuration;
            meta.recordedDate = item.metaRecordedDate;
            meta.title = item.metaTitle;
            meta.album = item.metaAlbum;
            meta.performer = item.metaPerformer;
            injectMediaMeta(*entity, meta); // 注入视同已求值，实算不再发生
        }
        out.entities.push_back(std::move(entity));
    }

    // 跳过项 warning 只住根层
    if (parent.isEmpty() && layer.skippedCount > 0)
        out.warnings.append(scriptListText(kTextSkipped));
    return out;
}

void ScriptListProvider::saveSettings(QSettings &settings) const {
    writeProviderKey(settings);
    settings.setValue(QStringLiteral("scriptId"), m_scriptId);
}

std::shared_ptr<PlaylistProvider>
ScriptListProvider::load(QSettings &settings, const std::shared_ptr<ServiceRegistry> &registry) {
    const QString id = settings.value(QStringLiteral("scriptId")).toString();
    // 缺失 / 空 / 非法一律视为未绑定；恒返回非空，工厂返空会回落静态表
    return std::make_shared<ScriptListProvider>(ScriptListSpec::isValidListId(id) ? id : QString(),
                                                registry);
}

// 页面类只在本文件可见；候选为本次会话已注册的数据源
class ScriptListSettingsPage : public ProviderSettingsPage {
    Q_OBJECT
  public:
    ScriptListSettingsPage(const ScriptListProvider *seed,
                           const std::shared_ptr<ServiceRegistry> &registry, QWidget *parent)
        : ProviderSettingsPage(parent), m_seed(seed), m_registry(registry) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);

        // 可编辑下拉：首项恒为空串（取不到通道时等同普通输入框）
        m_idCombo = new QComboBox;
        m_idCombo->setEditable(true);
        m_idCombo->setInsertPolicy(QComboBox::NoInsert);
        m_idCombo->addItem(QString());
        if (const auto channel = registry ? registry->resolve<ScriptListChannel>() : nullptr)
            m_idCombo->addItems(channel->registeredIds());
        form->addRow(tr("ID"), m_idCombo);

        auto *note =
            new QLabel(tr("Bind a list created by a script; see docs/script-interface.md"));
        note->setEnabled(false);
        form->addRow(note);

        layout->addLayout(form);
        layout->addStretch(1);

        if (m_seed)
            m_idCombo->setCurrentText(m_seed->scriptId());
    }

    std::optional<QString> validate() const override {
        if (ScriptListSpec::isValidListId(idValue()))
            return std::nullopt;
        return tr("The data source id may only contain letters, digits, dots, dashes and "
                  "underscores, and must be 1 to 64 characters long.");
    }

    bool hasChanges() const override { return !m_seed || idValue() != m_seed->scriptId(); }

    std::shared_ptr<PlaylistProvider> build() const override {
        return std::make_shared<ScriptListProvider>(idValue(), m_registry);
    }

  private:
    // 输入 trim 后再判
    QString idValue() const { return m_idCombo->currentText().trimmed(); }

    const ScriptListProvider *m_seed = nullptr;
    std::shared_ptr<ServiceRegistry> m_registry;
    QComboBox *m_idCombo = nullptr;
};

ProviderSettingsPage *createScriptListSettingsPage(const PlaylistProvider *seed,
                                                   const std::shared_ptr<ServiceRegistry> &registry,
                                                   QWidget *parent) {
    return new ScriptListSettingsPage(
        seed ? static_cast<const ScriptListProvider *>(seed) : nullptr, registry, parent);
}

// AUTOMOC 要求显式包含本文件 Q_OBJECT 类的 .moc
#include "ScriptListProvider.moc"

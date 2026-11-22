#pragma once

#include "PlaylistProvider.h"

// 类型键与显示名源文案（显示名经 translate 取值）
inline constexpr char kDirProviderKey[] = "dir";
inline constexpr char kDirProviderName[] = QT_TRANSLATE_NOOP("DirProvider", "Local Directory");

class ProviderSettingsPage;

// 本地目录提供者：绑定一个目录，按扩展名清单同步枚举。排序由视图按每 tab 规则
// 施加；提供者返回前按文件名数字化排好初始序。扩展名只过滤文件，分层模式的
// 子目录恒为容器（空目录也显示，否则深层内容无从抵达）。
class DirProvider : public PlaylistProvider {
  public:
    // 枚举值序即参数页下拉框选项序，亦即 INI 持久化取值
    enum class BrowseMode {
        Layered,        // 只列举该层，子目录 → 容器（新建列表默认）
        FilesOnly,      // 只列举根内文件，平铺
        FilesRecursive, // 递归 DFS 拉平
    };

    // registry 经基类持有，必填；目录枚举只产本地路径，当前未用，为接口统一保留
    DirProvider(const QString &root, BrowseMode mode, const QStringList &extensions,
                const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    bool supportsSort() const override;
    SortRule defaultSortRule() const override { return {SortKey::Modified, true}; }

    static QString displayName();

    // trim → 空即空串 → Util::Url::toIdentity（相对转绝对、转原生分隔符；网络 /
    // 流路径原样）。幂等是硬要求，否则"原样回显未改动"会被 hasChanges 误判
    static QString normalizeRoot(const QString &text);
    static std::optional<QString> validateInput(const QString &rawRoot);
    // 空白拆分、去前导点、转小写、去重保序；空文本 ⇒ 空表（不过滤）
    static QStringList parseExtensions(const QString &text);

    // 空序列即根本身；不 cleanPath，保持 root 形态稳定
    static QString resolveLayerPath(const QString &root, const QStringList &parent);
    static BrowseMode browseModeFromInt(int value);

    QString rootPath() const { return m_root; }
    BrowseMode browseMode() const { return m_browseMode; }
    QStringList extensions() const { return m_extensions; }

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

  private:
    // 仅 FilesRecursive 使用，一层内命中扩展名的条目追加 out、不可读子目录记 warning
    // 后跳过。depth 自根 0，超 maxDepth 静默截断；visited 为 canonical 集，环与重复
    // 访问在此剪枝
    static void collect(const QString &dirPath, const QStringList &extensions, bool recursive,
                        int depth, int maxDepth, QSet<QString> &visited, QFileInfoList &out,
                        QStringList &warnings);

    static void sortEntitiesByNumericName(EntityList &entities);
    static void sortContainersByNumericTitle(QList<Container> &containers);

    QString m_root; // 绝对路径、原生分隔符
    BrowseMode m_browseMode = BrowseMode::Layered;
    QStringList m_extensions; // 无点小写扩展名；空表表示不过滤
};

// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createDirSettingsPage(const PlaylistProvider *seed,
                                            const std::shared_ptr<ServiceRegistry> &registry,
                                            QWidget *parent);

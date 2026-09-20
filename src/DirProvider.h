#pragma once

#include "PlaylistProvider.h"

// N44：身份常量（类型键 + 显示名源文案），登记表与 writeProviderKey 共用同一点定义
inline constexpr char kDirProviderKey[] = "dir";
inline constexpr char kDirProviderName[] = QT_TRANSLATE_NOOP("DirProvider", "Local Directory");

class ProviderSettingsPage;

// 本地目录提供者（N9）：绑定一个目录，按扩展名清单同步枚举。
// N45：浏览模式三选一——分层浏览（单层列举，子目录产容器）/ 仅文件（平铺，
// 现状 recursive = false 的逐字保留）/ 仅文件（含子目录）（递归 DFS 拉平，
// 现状 recursive = true 的逐字保留）。排序由视图层按每 tab 规则施加；
// 提供者返回前按文件名数字化排好初始序（N45 §2.7，与视图 SortKey::None
// 显示序逐项一致，一致性由规格与验收约束）。
// N26：enumerate 现场造实体返回；N29 起枚举只造实体不预热——collect 产出
// QFileInfoList，遍历中经注入口把顺手取得的文件元数据零成本注入实体。
// 原静态执行体 enumerateDir 撤销，内容来源统一为本实例方法
// （工作线程经裸指针调用，见 PlaylistWidget::startLoadAsync）
class DirProvider : public PlaylistProvider {
  public:
    // N45 §2.11：浏览模式（三选一）。枚举值序即参数页下拉框的选项序，
    // 亦即 INI 的持久化取值（int 不透明存储）
    enum class BrowseMode {
        Layered,        // 分层浏览：只列举该层，子目录 → 容器（新建列表默认）
        FilesOnly,      // 仅文件：只列举根内文件，平铺
        FilesRecursive, // 仅文件（含子目录）：递归 DFS 拉平
    };

    // registry 经基类持有，必填无缺省（目录枚举只产本地路径，当前未用；
    // 为与 Static 统一接口）
    DirProvider(const QString &root, BrowseMode mode, const QStringList &extensions,
                const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    // 显式 true：目录表内容以枚举为准不可增删，但仍可排序并持久化规则
    bool supportsSort() const override;

    // 当前语言下的显示名（无译文时即英文源）；登记表以函数指针引用（N44）
    static QString displayName();

    // ——以下三个静态纯函数（N44）：参数设置页的取值 / 校验与单元测试共用同一出口——
    // 根路径定形：trim → 空即空串 → Util::resolveSourcePath（相对转绝对、转原生
    // 分隔符；网络 / 流路径原样）。**幂等是硬要求**——已规范形态的输入再定形
    // 必须结果不变，否则"原样回显 + 未改动"会被 hasChanges 误判成有改动
    static QString normalizeRoot(const QString &text);
    // 参数校验：入参是控件**原始文本**（内部先 normalizeRoot，故不存在"必须先定形"
    // 的前置条件）。返回可直接展示的错误文案；通过返回 nullopt
    static std::optional<QString> validateInput(const QString &rawRoot);
    // 扩展名编辑区文本 → 规范清单：按空白拆分、去前导点、转小写、去重保序；
    // 空文本 ⇒ 空表（不过滤）
    static QStringList parseExtensions(const QString &text);

    // ——以下两个静态纯函数（N45）：parent 路径的解析与浏览模式的取值——
    // 单元测试与 enumerate / INI 读取共用同一出口（N44 先例）
    // parent 路径段序列 → 目标层绝对路径：逐段拼回 root 即得（容器 id 取目录名，
    // 单段不含路径；视图只对提供者自己返回过的 id 发起调用，段内容可信）。
    // 空序列即根本身，原样返回
    static QString resolveLayerPath(const QString &root, const QStringList &parent);
    // INI 的 int 取值 → 浏览模式：取值域外（损坏 / 未知值）降级 Layered
    // （新建列表默认，§2.11），不做旧键兼容
    static BrowseMode browseModeFromInt(int value);

    QString rootPath() const { return m_root; }
    BrowseMode browseMode() const { return m_browseMode; }
    QStringList extensions() const { return m_extensions; }

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

  private:
    // 递归收集辅助（仅"仅文件（含子目录）"模式使用）：dir 内命中过滤器的条目信息
    // 追加至 out；不可读子目录记入 warnings 后跳过。
    // N36：depth 为 dir 自身深度（根为 0）。子目录深度超过 maxDepth 不再深入，
    // 静默截断不记 warning；maxDepth == 0 为不递归（只收根内文件），
    // n 为最多深入 n 层子目录（N36 修订 2）
    // N36 修订 3：visited 为已访问目录的 canonical 路径集，环与重复访问在此剪枝
    // （静默跳过）；由 enumerate 建立并先入根
    static void collect(const QDir &dir, const QStringList &filters, bool recursive, int depth,
                        int maxDepth, QSet<QString> &visited, QFileInfoList &out,
                        QStringList &warnings);

    // ——提供者初始排序（N45 §2.7）——文件名数字化（Util::segmentName +
    // naturalCompare），稳定排序保等键原序；实体按 fileName、容器按 title。
    // 与视图 SortKey::None 显示序逐项一致由规格与验收约束，不共用代码
    static void sortEntitiesByNumericName(EntityList &entities);
    static void sortContainersByNumericTitle(QList<Container> &containers);

    QString m_root;                                // 绑定目录（绝对路径）
    BrowseMode m_browseMode = BrowseMode::Layered; // 浏览模式（新建默认分层）
    QStringList m_extensions;                      // 无点小写扩展名；空表表示不过滤
};

// 参数页工厂（N44）：页面类定义在 DirProvider.cpp，本处只声明工厂。
// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createDirSettingsPage(const PlaylistProvider *seed,
                                            const std::shared_ptr<ServiceRegistry> &registry,
                                            QWidget *parent);

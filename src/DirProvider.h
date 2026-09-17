#pragma once

#include "PlaylistProvider.h"

// N44：身份常量（类型键 + 显示名源文案），登记表与 writeProviderKey 共用同一点定义
inline constexpr char kDirProviderKey[] = "dir";
inline constexpr char kDirProviderName[] = QT_TRANSLATE_NOOP("DirProvider", "Local Directory");

class ProviderSettingsPage;

// 本地目录提供者（N9）：绑定一个目录，可选含子目录，按扩展名清单同步枚举。
// 枚举结果平铺为单一序列（子目录不分组），排序由视图层按每 tab 规则施加。
// N26：enumerate 现场造实体返回；N29 起枚举只造实体不预热——collect 产出
// QFileInfoList，遍历中经注入口把顺手取得的文件元数据零成本注入实体。
// 原静态执行体 enumerateDir 撤销，内容来源统一为本实例方法
// （工作线程经裸指针调用，见 PlaylistWidget::startLoadAsync）
class DirProvider : public PlaylistProvider {
  public:
    // registry 经基类持有，必填无缺省（目录枚举只产本地路径，当前未用；
    // 为与 Static 统一接口）
    DirProvider(const QString &root, bool recursive, const QStringList &extensions,
                const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    std::expected<Enumeration, QString> enumerate() const override;
    bool supportsModify() const override;
    // 显式 true：目录表内容以枚举为准不可增删，但仍可排序并持久化规则
    bool supportsSort() const override;

    // 当前语言下的显示名（无译文时即英文源）；登记表以函数指针引用（N44）
    static QString displayName();

    // ——以下三个静态纯函数（N44）：参数设置页的取值 / 校验与单元测试共用同一出口——
    // 根路径定形：trim → 空即空串 → Util::resolveLocalPath（相对转绝对、转原生
    // 分隔符；网络 / 流路径原样）。**幂等是硬要求**——已规范形态的输入再定形
    // 必须结果不变，否则"原样回显 + 未改动"会被 hasChanges 误判成有改动
    static QString normalizeRoot(const QString &text);
    // 参数校验：入参是控件**原始文本**（内部先 normalizeRoot，故不存在"必须先定形"
    // 的前置条件）。返回可直接展示的错误文案；通过返回 nullopt
    static std::optional<QString> validateInput(const QString &rawRoot);
    // 扩展名编辑区文本 → 规范清单：按空白拆分、去前导点、转小写、去重保序；
    // 空文本 ⇒ 空表（不过滤）
    static QStringList parseExtensions(const QString &text);

    QString rootPath() const { return m_root; }
    bool recursive() const { return m_recursive; }
    QStringList extensions() const { return m_extensions; }

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

  private:
    // 递归收集辅助：dir 内命中过滤器的条目信息追加至 out；不可读子目录记入 warnings 后跳过。
    // N36：depth 为 dir 自身深度（根为 0）。子目录深度超过 maxDepth 不再深入，
    // 静默截断不记 warning；maxDepth == 0 为不递归（只收根内文件），
    // n 为最多深入 n 层子目录（N36 修订 2）
    // N36 修订 3：visited 为已访问目录的 canonical 路径集，环与重复访问在此剪枝
    // （静默跳过）；由 enumerate 建立并先入根
    static void collect(const QDir &dir, const QStringList &filters, bool recursive, int depth,
                        int maxDepth, QSet<QString> &visited, QFileInfoList &out,
                        QStringList &warnings);

    QString m_root;           // 绑定目录（绝对路径）
    bool m_recursive = false; // 是否包含子目录（平铺）
    QStringList m_extensions; // 无点小写扩展名；空表表示不过滤
};

// 参数页工厂（N44）：页面类定义在 DirProvider.cpp，本处只声明工厂。
// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createDirSettingsPage(const PlaylistProvider *seed,
                                            const std::shared_ptr<ServiceRegistry> &registry,
                                            QWidget *parent);

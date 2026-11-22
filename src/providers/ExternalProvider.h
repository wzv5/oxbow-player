#pragma once

#include "pch.h"

#include "HttpClient.h"
#include "PlaylistProvider.h"

// 类型键与显示名源文案（显示名经 translate 取值）
inline constexpr char kExternalProviderKey[] = "external";
inline constexpr char kExternalProviderName[] = QT_TRANSLATE_NOOP("ExternalProvider", "External");

class ProviderSettingsPage;

// 外部播放列表提供者：tab 绑定一个路径参数（本地清单文件或 http(s) 地址，定性看
// 内容不看后缀）。先取回整份内容，再按流清单标签白名单判别"一条流还是一份清单"：
// 命中则只回清单自身一项（由 mpv 按 HLS 整条播）；否则按行语法分 m3u / 分组文本
// （tvbox 系）/ 裸清单三支解析——根层 = 具名组容器 + 无组名条目，各组内容随同一次
// 枚举进 descendants，一次交付全树。
// 能力位与目录 / WebDAV 同形；转接不覆写（Unsupported）。默认排序沿用基类
// {None, true}——外部表没有修改时间可排，作者序有语义。
// 无状态：除参数与排序规则外无成员可变状态，每次 enumerate 都重新取源。
// 取源时限与体上限沿用全局值；HttpClient 由枚举内 resolve 现取，取不到即装配错误。
class ExternalProvider : public PlaylistProvider {
  public:
    // sourcePath 为定形后的本地绝对路径或 http(s) URL
    ExternalProvider(const QString &sourcePath, const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    // 整树交付：非空 parent 属调用方违约（Debug 断言，Release 空结果）
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    bool supportsSort() const override;
    // 恒真（类型级事实）；流分支的"树只有根层"是合法整树交付
    bool enumeratesWholeTree() const override;

    static QString displayName();

    // trim → 网络形态走 toNative，本地形态走 toIdentity（相对转绝对、原生分隔符、
    // file:// 折盘符）。不补尾斜杠——外部路径指向一个文件。幂等是硬要求
    static QString normalizeSource(const QString &text);
    // 非空；定形后 isLocal 或 isHttp；含 :// 而 isHttp 为假者拒；本地档必须是
    // 磁盘上存在的普通文件。不设扩展名白名单，远端不做探测
    static std::optional<QString> validateInput(const QString &rawText);
    // 分号拆、逐段 trim、去空段、组内去重保序；组名内含分号者被拆作两组，是
    // iptv-org 约定
    static QStringList splitGroupTitles(const QString &groupTitles);

    // 解析纯函数：不 I/O、不碰实体与 registry
    struct Entry {
        QString target;      // 按基址定形后的身份串
        QString title;       // #EXTINF 逗号后的显示名，可空
        QString tvgName;     // 只进显示名回退链
        QStringList groups;  // 序即出现序，一条目可入多组
        QVariantMap options; // 条目级 mpv 选项；空 = 无
    };
    // entries 按文件序；groupOrder 为具名组首次出现序；isStreamSource 置位时其余
    // 整份为空；warnings 为结构性异常说明，每类至多一条、不带条数
    struct Parsed {
        QList<Entry> entries;
        QStringList groupOrder;
        bool isStreamSource = false;
        QStringList warnings;
    };
    // text 为 UTF-8 全文（BOM 与行尾在本函数内处理）；baseSource 为相对目标基址
    // ——本地清单 = 所在目录，http 清单 = 清单 URL。相对目标经 resolved 展开，
    // 不剥 userinfo
    static Parsed parseSource(const QString &text, const QString &baseSource);

    QString sourcePath() const { return m_sourcePath; }

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

  private:
    QString m_sourcePath;
};

// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createExternalSettingsPage(const PlaylistProvider *seed,
                                                 const std::shared_ptr<ServiceRegistry> &registry,
                                                 QWidget *parent);

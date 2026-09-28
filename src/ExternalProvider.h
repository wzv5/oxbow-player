#pragma once

#include "pch.h"

#include "HttpClient.h"
#include "PlaylistProvider.h"

// N44：身份常量（类型键 + 显示名源文案），登记表与 writeProviderKey 共用同一点定义
inline constexpr char kExternalProviderKey[] = "external";
inline constexpr char kExternalProviderName[] = QT_TRANSLATE_NOOP("ExternalProvider", "External");

class ProviderSettingsPage;

// 外部播放列表提供者（N63）：tab 绑定一个路径参数（本地清单文件或 http(s) 地址，
// 后缀不限——.m3u / .m3u8 / .txt / 无后缀皆可，定性看内容不看后缀）。枚举在内部
// 先取回整份内容，再按流清单标签白名单（§2.2，含头部区位次限定）判别"这份东西
// 是一条流还是一份清单"：命中即只回清单自身一项（由 mpv 按 HLS 整条播）；不命中
// 则按行语法分 m3u / 分组文本（tvbox 系）/ 裸清单三支解析成一层分组清单——
// 根层 = 具名组容器 + 无组名条目，每个具名组的层内容随同一次枚举进 descendants，
// 一次交付全树（enumeratesWholeTree 恒真，N64 两态化的第一个真实取用者）。
//
// 能力位与目录 / WebDAV 同形（supportsModify 假、supportsSort 真）；转接不覆写
// （基类默认 Unsupported）⇒ transit 键不落盘、参数页无转接行、下发恒走直发形态
// （条目 URL 原样给 mpv，含 userinfo 也如实）。默认排序沿用基类 {None, true}——
// 外部表没有修改时间可排，作者序有语义（§2.9），不做提供者初始排序。
//
// 无状态纪律（§2.7）：实例内除参数与排序规则外无可变状态——没有缓存、没有锁、
// 没有失效判据。每次 enumerate 都重新取源；"缓存何时作废"在权威整树语义下问题
// 本身消失（交付即真、缺席即删），提供者"构造后不可变"的并发安全前提原样保住。
//
// 取源时限与体上限沿用全局值（remoteTimeoutSec / maxBodyBytes），不新增旋钮；
// HttpClient 由装配顶点按 INI 建好，枚举内 registry()->resolve 现取（N35 纪律，
// resolve 不到即装配错误：断言 + 整次失败，不读 INI 兜底）。
class ExternalProvider : public PlaylistProvider {
  public:
    // sourcePath 为参数页来源处定形的路径（normalizeSource：本地绝对路径或
    // http(s) URL）；registry 经基类持有，必填无缺省
    ExternalProvider(const QString &sourcePath, const std::shared_ptr<ServiceRegistry> &registry);

    QString typeKey() const override;
    // 整树交付：只应答空 parent（非空即调用方违约，Q_ASSERT 守候 + 空结果，
    // 不解析不应答）；每次调用都重新取源
    std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const override;
    bool supportsModify() const override;
    // 显式 true：内容以取源为准不可增删，但可排序并持久化规则（与目录表同形）
    bool supportsSort() const override;
    // 恒真（类型级事实，不因内容是频道清单 / 流 / 分组文本而翻转，§2.4 / §2.7）；
    // 流分支那份"树只有根层"是合法整树交付，不是退化成逐层
    bool enumeratesWholeTree() const override;

    // 当前语言下的显示名（无译文时即英文源）；登记表以函数指针引用（N44）
    static QString displayName();

    // ——以下静态纯函数（N44 惯例）：参数设置页 / INI 读侧与单元测试共用同一出口——
    // 路径定形：trim → 网络形态走 Util::Url::toNative（http(s) 收规范形态），本地
    // 形态走 Util::Url::toIdentity（相对→绝对、原生分隔符、file:// 折盘符）。
    // 不补尾斜杠——外部路径指向一个文件，不是目录基址。**幂等是硬要求**（与
    // DirProvider::normalizeRoot 同一条纪律，否则"原样回显 + 未改动"会被
    // hasChanges 误判成有改动）
    static QString normalizeSource(const QString &text);
    // 参数校验：入参是控件**原始文本**（内部先定形，不设"必须先定形"的前置条件）。
    // 判据四条：非空；定形后 isLocal 或 isHttp；含 :// 而 isHttp 为假者拒
    // （rtsp:// 之类的"清单地址"本提供者取不回）；本地档必须是磁盘上存在的普通
    // 文件（目录与不存在者当场拒）。**不设扩展名白名单**——后缀不能定性正是本篇
    // 立足点，填进去的若真不是清单，由枚举期的零条目警告回答。远端不做任何探测
    // （承 N37"不做连通性探测"）。返回可直接展示的错误文案；通过返回 nullopt
    static std::optional<QString> validateInput(const QString &rawText);
    // group-title 的多值拆分（iptv-org 约定）：分号拆、逐段 trim、去空段、组内去重
    // 保序。组名内含分号者被拆作两组——m3u 无转义机制可表达，是既定约定非本篇选择
    static QStringList splitGroupTitles(const QString &groupTitles);

    // ——m3u / 分组文本解析纯函数（本篇唯一"自己写"的部分，与
    // WebDavProvider::parseMultistatus 同法定位：纯函数、不 I/O、不碰实体与
    // registry，单元测试直接喂字符串；用完即弃，不落进任何成员——§2.7 无状态）——
    // 单个条目的解析产出：target 为按基址定形后的条目身份串（喂工厂前必已定形）；
    // title 为 #EXTINF 逗号后的显示名（分组文本支即行首名），可空；tvgName 为属性
    // 区 tvg-name 的值，只进显示名回退链（§2.5），可空；groups 为组名列表
    // （序即出现序，一条目入多组 = m3u 的 group-title 分号多值语义）
    struct Entry {
        QString target;
        QString title;
        QString tvgName;
        QStringList groups;
        // N65：条目级单文件选项——键 = mpv 选项名（成型出口做纯键名映射，此后
        // 链路上不再有第二个名字表），值 = 取值原样（不 trim 内部、不转义、不拼
        // 前缀）。解析内成型，产出即可直接交给 mpv；空 map = 无选项
        QVariantMap options;
    };
    // 一次解析的产出：entries 按文件序；groupOrder 为去重后的具名组序（首次出现
    // 序，trim 后按大小写敏感相等并组）；isStreamSource 为流清单标签白名单命中
    // 旗标（§2.2，按位次限定命中）——置位时 entries / groupOrder / warnings 整份
    // 为空（解析产出整份丢弃，只由 enumerate 回清单自身一项）；warnings 为结构性
    // 异常的无计数说明（每类至多一条、串里不带条数，照 DavListing::warnings 纪律）
    struct Parsed {
        QList<Entry> entries;
        QStringList groupOrder;
        bool isStreamSource = false;
        QStringList warnings;
    };
    // text 为解码后的清单全文（UTF-8，BOM 与行尾在本函数内处理）；baseSource 为
    // 相对目标的定形基址——本地清单 = 清单文件所在目录，http 清单 = 清单 URL
    // （相对目标经 QUrl::resolved 展开，同 WebDAV 的展开机制、不剥 userinfo——
    // 本提供者不注入也不剥，A8：下发与条目 URL 逐字节相同）
    static Parsed parseSource(const QString &text, const QString &baseSource);

    QString sourcePath() const { return m_sourcePath; }

    void saveSettings(QSettings &settings) const override;

    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

  private:
    QString m_sourcePath; // 清单路径 / URL（入口处定形，构造后不可变）
};

// 参数页工厂（N44）：页面类定义在 ExternalProvider.cpp，本处只声明工厂。
// seed 为同类型存量提供者（新建 / 换类型传 nullptr）
ProviderSettingsPage *createExternalSettingsPage(const PlaylistProvider *seed,
                                                 const std::shared_ptr<ServiceRegistry> &registry,
                                                 QWidget *parent);

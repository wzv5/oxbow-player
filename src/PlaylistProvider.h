#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

class QSettings;
class StreamEndpoint;   // N52 L2：来源解析点（纯头接口，实现文件使用）
class StreamRelayIf;    // N52 L4：转接发布接缝（StreamRelay.h 的抽象基，纯头无实现体）
class PlaybackReporter; // N74：播放上报报告者（寿命归本提供者，注册点在列表侧）

// 播放列表提供者抽象接口（N9）：条目来源策略与列表视图解耦。
// N26 起以播放项实体为交换媒介：enumerate 返回实体有序表（所有权一次性移交视图，
// 实体在提供者侧现场构造，遍历中顺手已持有的元数据可经注入口主动注入）；
// 视图层据此建行并施加排序规则；
// 序列化（存什么、怎么存）完全由具体提供者决定——含各自的排序键读写
class PlaylistProvider {
  public:
    using EntityList = PlaylistEntity::List;

    // ——N37：枚举结果——条目 + 非致命警告。warnings 为"部分来源失败 / 被跳过"的可读
    // 文案（条目已产出，不判整次失败）；unexpected 语义不变：来源整体不可用，
    // 视图进占位行。
    // N45：增容器数组。容器组恒在实体组之前是模型不变式（不是显示偏好），
    // 两数组顺序完备：显示序 = containers 序接 entities 序
    struct Container {
        QString id; // 提供者内稳定，作为 parent 路径段
        QString title;
        std::optional<FileMeta> meta; // 复用 FileMeta，供容器参与排序
        // N47：该层完整路径（纯展示，供属性对话框 / 复制 / 打开；空 = 未知）。
        // 本地为绝对路径，WebDAV 为无凭据完整 URL；不参与排序、寻址与持久化
        QString path;
    };
    // ——N64：一层内容的纯内容底座——根层与子孙层共用同一内容表达，containers
    // 恒在实体组之前这条模型不变式在此只写一次。不含 warnings、不含错误字段：
    // 子层不表达错误与警告（缺席即失败，解释性文案归根层 warnings）
    struct LevelData {
        QList<Container> containers;
        EntityList entities;
    };
    // ——N37：枚举结果——条目 + 非致命警告。warnings 为"部分来源失败 / 被跳过"的
    // 可读文案（条目已产出，不判整次失败），只住根层；unexpected 语义不变：
    // 来源整体不可用，视图进占位行。
    // N45：增容器数组（继承自 LevelData）。两数组顺序完备：显示序 = containers 序
    // 接 entities 序。
    // N64 两态化：逐层形态（默认）descendants 恒空、语义一字不变；整树形态
    // （enumeratesWholeTree()==true）每次枚举交付其所知整棵树——descendants 键 =
    // 相对表根的全路径段序列（与视图层缓存的层键同形同键），值 = 该层纯内容。
    // 扁平一张表无递归，QMap 的字典序不承载显示语义；不存在部分交付
    struct Enumeration : LevelData {
        QStringList warnings;
        QMap<QStringList, LevelData> descendants;
    };

    // ——N35 服务注册表（基类统一持有，供子类 enumerate 造实体时注入）——
    // 构造注入，必填无缺省：所有提供者诞生点必须显式传入可用实例。
    // Static 经此向 fromPath 转交，使持久化的 http 条目重载后仍得同类型实体
    explicit PlaylistProvider(const std::shared_ptr<ServiceRegistry> &registry);

    virtual ~PlaylistProvider();

    // 类型键（N44）：提供者类型的唯一身份，取值即各提供者头文件的键常量
    // （kXxxProviderKey）。INI 的 provider 键写侧经 writeProviderKey 取它、
    // 读侧经登记表条目比对它，两侧共用同一点定义。
    // 取代原 Type 枚举 + type()：类型身份不再需要一处集中维护的枚举，
    // 新增提供者不必回改基类
    virtual QString typeKey() const = 0;
    // 枚举条目（同步）：内容唯一来源。返回 expected——成功带实体有序表与警告
    // （空目录成功返回空表，与失败区分；N37 起部分来源失败随 warnings 回传），
    // 失败带错误文案。
    // N45：parent 为相对根的路径段序列（不含根本身），空即根层——决定枚举哪一层。
    // 契约：视图只对提供者自己返回过的容器 id 发起非空 parent 调用；平铺形态的
    // 提供者收到非空 parent 属调用方违约——Q_ASSERT 守候，release 降级返回空结果
    // （照 fromPath 对"http 加空 registry"的既有先例），不得静默忽略。
    // 提供者不产出"上一级"项，该行由视图合成
    virtual std::expected<Enumeration, QString> enumerate(const QStringList &parent = {}) const = 0;
    // 视图 → 提供者内容回传（只读引用）。基类默认实现忽略——不可修改的提供者
    // 不参与回传。回传是内容落盘的唯一入口：savePlaylists 对可修改提供者逐个回传
    // 后随即序列化，回传与落盘不得拆开（N27）。
    // N28：回传另有守卫——未加载（含首载在途）的表实体集合为空，回传会清空提供者
    // 自持内容造成永久丢数据，故整段跳过；落盘侧照走 saveSettings
    // 不变式：实现方只许从实体提取标量（本轮即 path()），不得保存实体指针——
    // 实体归视图层，视图 refresh() / 重建后提供者持有的实例即成陈旧副本
    virtual void setItems(const EntityList &entities) { Q_UNUSED(entities); }
    // 能力位：是否支持修改（添加与删除）。不可修改的提供者内容以枚举为准，
    // 视图侧整组禁用增删（失效清理走刷新）
    virtual bool supportsModify() const = 0;
    // 能力位：是否支持排序。默认随 supportsModify——允许修改即允许排序，反之不成立
    // （目录表可排序而不可增删，须显式覆写 true；可修改而不可排序的 m3u 类
    // 预留实现须显式覆写 false）
    virtual bool supportsSort() const { return supportsModify(); }

    // ——N64 枚举形态两态化——查询位：true = 本提供者一次枚举交付整棵树
    // （descendants 全量、键相对表根），视图对其只发起空 parent 调用（初载与
    // 刷新都落根层），并把交付结果当权威内容整树重建（缺席即删）。默认
    // false = 逐层（现状语义逐字保持，实现零改动义务）。
    // 显式声明而非由结果推断：空树与逐层空结果形态完全相同，推断不出来。
    // 两态是类型级事实，视图不存模式位——决策点现查，换源随提供者自动切换。
    // 规模判据归声明方自担：整树意味着全部子孙层实体在首载 materialize——
    // 目录与 WebDAV 逐层取本来就便宜，声明逐层；几百到几千条的一次性清单
    // 可整树（首载一次取回，之后钻层零取源）；大库按同判据自决。
    //
    // 整树形态契约义务（违约处置：Debug Q_ASSERT 显影，Release 降级不崩溃）：
    // 1. 逐层提供者带 descendants = 违约：Release 丢弃 descendants，按逐层语义处理；
    // 2. 整树提供者不响应非空 parent——常规通路只被空 parent 调用，保证这一点是
    //    视图层的义务；提供者收到非空 parent = 调用方违约，Release 降级返回空结果
    //    （照平铺提供者收到非空 parent 的既有先例），不得尝试应答；
    // 3. 键前缀一致性：每个 descendants 键的祖先链必须由本次交付内容构成——首段
    //    ∈ 根层 containers，逐段往下落在交付层的 containers；调用层自身与根层不出现在
    //    descendants 中。Release 非法键逐项忽略（链断即整枝不写）。同层容器 id 不得
    //    重复（重复即键寻址歧义，QMap 会静默后者覆盖前者、丢一整层内容）：Debug
    //    断言显影；Release 保留交付原样——同层两行同 id 容器、寻址命中同一子层，
    //    不崩不丢层，前缀校验的 id 集按先到者算；
    // 4. 错误与警告只住根层：LevelData 无此字段，解析失败的组以缺席表达，
    //    解释性文案进根层 warnings。
    // 深度不设限：descendants 是扁平表 + 义务 3 的前缀校验，任意深度的树与
    // 一层树走同一条写入通路
    virtual bool enumeratesWholeTree() const { return false; }

    // ——N52 §2.8：下发形态三态能力 + 列表级偏好——
    // 转接不是"能力位 + 开关"两个布尔：Required × 未勾选是语义上不存在却可被
    // 合法构造的状态（只能转接的来源漏勾即把 mpv 打不开的直接形态发出去），
    // 故能力升为三态枚举，有效决策收成 usesTransit() 一个派生值：
    //
    //   transitCapability()   | 列表偏好（INI transit） | usesTransit()
    //   ----------------------+-------------------------+------------------------------
    //   Unsupported（默认）   | 任意                    | false（偏好值被忽略）
    //   Optional（WebDAV）    | true / false            | 跟随偏好
    //   Required（Emby 一类） | 任意                    | true（偏好值被忽略，关不掉）
    //
    // 由这张表换来的两条不变式（写进头文件即接口的一部分）：
    // 1. usesTransit()==true 时 directPlayUrl() 永不被调用——"凭据不进播放 URL"
    //    与"令牌不进播放 URL"不是两条各守一次的纪律，而是同一条路径不可达；
    // 2. Required 的提供者不必覆写 directPlayUrl——基类恒等实现即"这条分支永不走"。
    enum class TransitCapability { Unsupported, Optional, Required };
    virtual TransitCapability transitCapability() const { return TransitCapability::Unsupported; }
    // 产出来源解析点（Unsupported 时必空；Optional/Required 返空 = 编程错误）。
    // 端点持不可变参数与注册表，无 QObject、任意线程可析构（N52 §2.3 契约 5）
    virtual std::shared_ptr<StreamEndpoint> createStreamEndpoint() const;

    // 唯一有效决策（非虚，全仓只有它读那个持久化偏好）
    bool usesTransit() const;

    // ——喂 mpv 的播放形态（模板方法，非虚公开出口）——
    // usesTransit() 为假 → 走 directPlayUrl()（直发形态）；为真 → 铸 oxbow:// 转接
    // URI 并把端点发布进注册表（发布即下发，N52 §2.6）。这样"把直接形态误当播放
    // 串下发"在编译期就不可能——directPlayUrl 降为 protected，外部拿不到那个钩子。
    // 实现必须轻（playTargets 是整表逐条调用）：一次哈希查找 + 一次前缀判定 +
    // 一次百分号编码，零网络。仅在 GUI 线程经 Playlist::playTargets 调用。
    // N57 §2.4：列表身份由调用方按值传入——提供者不认识列表、也不持有它的身份，
    // 转接铸造与端点发布所需的身份只是入参（nil 属编程错误，Debug 断言显影）。
    // N65：出口升为 playTarget——串与它的逐文件选项在同一句 return 里出生
    // （直发支无选项；转接支随串交出 cache=yes，N52 §2.10 语义一字不改、落点从
    // 控制器内的 scheme 反推搬回铸造点——谁知道事实谁就交选项）；
    // playUrl() 保留为薄包装（playTarget(...).url），四个"只要串"的消费者语义不变
    PlayTarget playTarget(const QUuid &listUuid, const QString &path) const;
    QString playUrl(const QUuid &listUuid, const QString &path) const {
        return playTarget(listUuid, path).url;
    }

    // 转接偏好（持久化值）：Optional 返偏好值；Required / Unsupported 返 nullopt
    // （形态不由偏好决定）。参数页回显与 hasChanges 共用此出口
    std::optional<bool> transitPreference() const;
    // 偏好写入（对话框 accept 期调用；形态约束由能力表承载，写值本身不设防）
    void setTransitPreference(bool preferred);

    // ——N74 播放上报：报告者实例（可空）——
    // 提供者造它、但不注册：投递路由需要列表身份，而按 N57 §2.4 提供者不认识也
    // 不镜像列表身份，注册点因此落在列表绑定点（Playlist 构造与换源两处）。
    // 空即该提供者不出报告者（静态 / 目录 / WebDAV / rclone crypt 一族）
    const std::shared_ptr<PlaybackReporter> &reporter() const { return m_reporter; }

    // ——INI 序列化：完全委托实现——写 provider 类型键、自身参数与排序键
    virtual void saveSettings(QSettings &settings) const = 0;

    // N62 §2.3：复制出口（非虚，基类一处实现）——经唯一序列化器往返：saveSettings
    // 写入调用方提供的 staging QSettings、load 从同一处读回，随后把源当下的排序规则
    // 原样交入（静态表不持久化排序键，纯往返会落成类型默认；目录与 WebDAV 的排序
    // 两键随往返本就忠实）。saveSettings 是"这份配置由哪些项构成"的唯一真相（N44 起
    // 类型键经 writeProviderKey 单点写），复制不另设通路，子类零改动。
    // staging 的介质与分组由调用方管理（暂存组从不落盘，见 PlaylistWidget::duplicateList）
    std::shared_ptr<PlaylistProvider> duplicated(QSettings &staging) const;

    // 工厂：按 group 内 provider 键经登记表（ProviderCatalog）分派到对应实现的
    // load；键缺失 / 未知 → 回落登记表第 0 项（默认提供者，现状即静态）。
    // registry 转交对应实现（缺省空）
    // N28：直接产出 shared_ptr——提供者所有权模型即共享（视图容器 + 枚举线程），
    // 调用点无需再经 unique_ptr 中间形态转换
    static std::shared_ptr<PlaylistProvider> load(QSettings &settings,
                                                  const std::shared_ptr<ServiceRegistry> &registry);

    // ——排序规则（N57 §2.9：值对象化）——规则**值**归提供者持久化，键的定义归实体侧
    // （取值域见 `PlaylistEntity.h` 的 `SortKey`）。默认规则由提供者自述，视图不再
    // 绕能力位推导：基类不排序，目录与 WebDAV 覆写为按修改时间升序，静态沿用基类
    SortRule sortRule() const { return m_sortRule; }
    void setSortRule(SortRule rule) { m_sortRule = rule; }
    virtual SortRule defaultSortRule() const { return {}; }

  private:
    SortRule m_sortRule;                         // N57 §2.9：规则值（键 + 方向）
    std::shared_ptr<ServiceRegistry> m_registry; // N35：服务注册表（基类统一持有）
    // N74：播放上报报告者（可空，基类统一持有——列表侧注册时要按同一份交出）
    std::shared_ptr<PlaybackReporter> m_reporter;
    // N57 §2.4：列表身份不再镜像于此（归 Playlist / PlaylistRegistry 持有）。
    // 转接偏好与端点：端点惰性建一次并缓存（参数不可变故无需重建；playUrl 仅
    // GUI 线程调用，缓存无需加锁）
    bool m_transitPreferred = false;
    mutable std::shared_ptr<StreamEndpoint> m_transitEndpoint;

  protected:
    // ——N48：喂 mpv 的播放形态（实体 path() → 实际下发给 mpv 的串）——
    // N52 降为 protected：公开出口只有非虚的 playUrl（模板方法），usesTransit()
    // 为假才会走到这里——"凭据不进播放 URL"（N48）与"令牌不进播放 URL"（Emby）
    // 由同一条不可达路径承载。默认恒等：内容原样下发（本地文件、静态表、通用
    // http 项皆如此）。WebDavProvider 重写为在 URL 的 userinfo 段注入当前凭据——
    // 凭据只在"PROPFIND 请求"与"喂 mpv 播放"两个使用点注入，实体 path() / 持久化 /
    // 各项比对恒为无凭据形态。该产出只喂 mpv（不进实体、不落盘、不参与比对），
    // 故允许经 QUrl 取编码形态；反向的剥离（回显比对、落键）走
    // Util::Url::stripCredentials 的纯字符串实现
    virtual QString directPlayUrl(const QString &path) const { return path; }

    // 写 INI 的 provider 键（N44）：取值即 typeKey()。各提供者 saveSettings 首行
    // 调用它——写侧（typeKey()）与读侧（登记表条目）共用同一点定义的键常量，
    // 杜绝"键写错导致重启后退化为默认提供者"的静默失败。
    // N52：本函数同时落转接偏好（transit），两键同点写出；N57 §2.4：列表身份
    // （uuid 键）改由 Playlist::save 一侧写，本函数不再认识身份。
    // transit 键只对 Optional 类型有意义，Unsupported / Required 不写（并清掉
    // 可能的存量键）——偏好不决定形态，形态由三态表承载
    void writeProviderKey(QSettings &settings) const;

    // 子类经此取构造期注入的注册表（enumerate 造实体时转交 fromPath）
    const std::shared_ptr<ServiceRegistry> &registry() const { return m_registry; }

    // N74：子类构造期交出报告者实例（寿命随本提供者；注册表只持 weak，注册由
    // 列表侧完成，见 reporter()）
    void setReporter(std::shared_ptr<PlaybackReporter> reporter) {
        m_reporter = std::move(reporter);
    }

    // ——N29 元数据注入口（唯一授权通道）——
    // 实体的 setFileMeta / setMediaMeta 为 protected 且仅友元（本类）可及；
    // 友元不继承，具体提供者经此静态转发写入——访问发生在本类成员函数体内，
    // 友元在此生效。新增提供者无需改实体头文件，只调转发函数
    static void injectFileMeta(PlaylistEntity &e, const FileMeta &m) { e.setFileMeta(m); }
    static void injectMediaMeta(PlaylistEntity &e, const PlaylistEntity::MediaMeta &m) {
        e.setMediaMeta(m);
    }
    // N63 / N70：显示名注入（外部清单的频道名 / 分组文本的行首名 / Jellyfin 的服务端
    // 标题）——实体身份串是远端 URL 或 URN 时 deriveNames 派生出的只有末段或 itemId
    // 噪声，显示名须由提供者在枚举期主动写入。与上面两个注入口同一条授权通道、同一个
    // 类内写法（setTitle 为 protected 且友元不继承，转发必须有）。
    // 本出口只写显示名（列表行文本、搜索命中、窗口标题与 OSD 读它）；名称类排序键读
    // 的是另一个字段，另有 injectFileName
    static void injectDisplayName(PlaylistEntity &e, const QString &name) { e.setTitle(name); }
    // N70：文件名注入——写 fileName 并随之重算 suffix（SortKey::FileName /
    // FileNameNumeric / Extension 三个键的取值来源）。
    // 联动规则在实体侧：title 未被显式设置过时本出口顺带把 title 同步成同一串，setTitle
    // 置旗后不再联动——故两个出口无论谁先调用，两个终值都相同（调用顺序无关）
    static void injectFileName(PlaylistEntity &e, const QString &name) { e.setFilename(name); }
    // N65：条目级单文件选项注入——与上面三个注入口同一条授权通道、同一个类内写法。
    // 选项是解析期一次成型的"mpv 选项名 → 取值"map，本出口只搬运不加工
    static void injectPlayOptions(PlaylistEntity &e, QVariantMap options) {
        e.setPlayOptions(std::move(options));
    }
};

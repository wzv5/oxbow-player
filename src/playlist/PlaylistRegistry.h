#pragma once

#include "pch.h"

#include "Playlist.h"

class PlaybackReporterRegistry; // N74：下传给每个 Playlist 作报告者注册点（可空）

// 默认列表的保留身份（N57 §2.5）：max UUID 全 F。version 位使合法铸造永不产出它，
// 持久列表也碰不上它（它本身恒在注册表内，任何接回该值的请求即判撞车而改用新铸）；
// 全零另有含义——QUuid::fromString 对空串与非法串返回全零，故全零即"无身份"
inline const QUuid kDefaultListUuid =
    QUuid::fromString(QStringLiteral("ffffffff-ffff-ffff-ffff-ffffffffffff"));

// 默认列表名的源文案与取值（§2.5 拆串）：现状 tr("Default") 在 PlaylistWidget 上下文里
// 同时承载"tab 0 名"与流属性表的"默认流"标签两组不相干语义，默认列表名因此与一个它
// 并不属于的语境绑死。沿用仓内"常量式源文案 + 取值函数"分工（displayName 先例）；
// 英文名不变，zh 译文由维护者按既有分工补条目
inline constexpr char kDefaultListNameSrc[] = QT_TRANSLATE_NOOP("PlaylistRegistry", "Default");
QString defaultListDisplayName();

// 播放列表注册表（N57 §2.2 / §2.3）：列表模型的唯一所有者，会话级、仅 GUI 线程读写
// （§2.10 口径 1，不设 mutex）。结构为「默认列表恒存在」+「有序持久列表」，vector
// 顺序即落盘节序，是权威源（tab 序是它的镜像）。持久列表数量为个位数，按身份查询
// 就是对序列的一次线性扫描——不设哈希索引。
// 不认识 ServiceRegistry 与提供者登记表：默认列表的后备提供者由装配顶点造好交入，
// 循环引用在类型上不可表达；也不进服务注册表（可变模型对象与"resolve 任意线程"
// 的线程契约不符）。
class PlaylistRegistry {
  public:
    // 收成品提供者，不自调提供者工厂；默认列表在此以保留身份与默认名直接建成，
    // 不经过 append（append 是持久列表的登记入口）。
    // N74：报告者注册表句柄只是过一道手——真正的注册点在 Playlist 一侧（列表身份
    // 归它持有）。缺省空即不注册（测试自建的表、或装配顶点没给）
    explicit PlaylistRegistry(std::shared_ptr<PlaylistProvider> defaultProvider,
                              std::shared_ptr<PlaybackReporterRegistry> reporters = nullptr);
    PlaylistRegistry(const PlaylistRegistry &) = delete;
    PlaylistRegistry &operator=(const PlaylistRegistry &) = delete;

    Playlist *defaultList() const { return m_default.get(); } // 恒非空

    // 按身份直查（默认列表与全部持久列表两段都查）；入参为空或 nil 恒不命中——
    // 本类不产生 nil 身份的列表。持久列表数个位数，线性扫描即够（不设哈希索引，
    // 占用判定与查询共用同一份扫描）
    Playlist *find(const QUuid &uuid) const;

    // `find` 的薄封装：空返回即无此列表（书签编辑器据此显示 Unknown）。前提是
    // "空名列表不存在"——加载跳过无名节、`append` 与重名校验拒空，两处共同保证
    QString nameOf(const QUuid &uuid) const;

    // 全部列表名（**含默认列表名**），可选剔除一个身份：重名校验与"下一个默认名
    // 序号"的唯一采集源，判据只在此处，视图与对话框不再各扫一遍 tab
    QStringList names(const QUuid &except = QUuid()) const;

    // 落盘遍历只走这两处：结构上不含默认列表，无需条件跳过
    int persistentCount() const;
    // 按权威顺序取。越界属调用方违约（Debug 断言显影）；Release 返 nullptr 只为把
    // 崩溃降级成一次可判空的取值，不作为可依赖的分支（调用方一律在
    // `0 <= i < persistentCount()` 内取）
    Playlist *persistentAt(int i) const;

    // 持久列表的唯一登记入口：一次成型构造、登记进序列尾部、返回裸指针。
    // storedUuid 为 nil 或已被占用则内部另铸新值，否则接回存量值——铸身份与
    // 占用判定是本类的实现，不设公开出口
    Playlist *append(QString name, std::shared_ptr<PlaylistProvider> provider,
                     const QUuid &storedUuid);
    // 只作用于持久列表（默认列表断言拒绝摘除）；摘除即列表对象的唯一析构点。
    // N55 §2.6 实施约束 1：摘除后归属该列表的显示中实体（默认 tab 收编层）的弱引用
    // 仍可能临时 lock 成功（在途任务 lambda 持有的 shared_ptr 副本令销毁延后）——
    // 来源失效清扫（PlaylistWidget::applySourceRetired，由本函数全仓唯一调用点
    // PlaylistWidget::removeTabAt 同栈触发）把这批实体先于任何再解析移除；漏调即
    // "每次推送重复收编"回来，本注释指回 N55 §2.6 兜（文档，不是机制）
    bool remove(const QUuid &uuid);

    // N62 §2.2：改序唯一出口——把 uuid 对应的持久列表移到持久域**终位下标** to
    // （摘出后插回该位，前移后移两方向同义；0 <= to < persistentCount()）。
    // 契约即"不可行即 false"：越界与不在册一律 false，不设断言——nil、未登记身份、
    // 默认列表的保留身份（结构上不在 vector 内）同档不命中。成功即权威序已变，
    // tab 条镜像与落盘由调用方随后完成
    bool moveList(const QUuid &uuid, int to);

  private:
    // 新铸一个不与任何在册列表相撞的身份
    QUuid mintUuid() const;

    std::unique_ptr<Playlist> m_default;
    std::vector<std::unique_ptr<Playlist>> m_persistent;
    std::shared_ptr<PlaybackReporterRegistry> m_reporters; // N74：过一道手，可空
};

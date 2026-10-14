#pragma once

#include "pch.h"

#include "PlaybackReporter.h"
#include "PlaylistEntity.h"

// N74：报告者注册表——"字符串键 → 多实例、按注册顺序、弱持有、过期自清理"，
// 每条表项另带**归属列表身份**（投递路由键，非认领判据）。
// 不并入 ServiceRegistry（那是"类型键 → 唯一实例、provide 即覆盖、强持有"的 IoC，
// N35）：形状不同，塞进同一个类会让它那份很短的契约长出第二套语义（强 / 弱、
// 单 / 多、覆盖 / 追加）。也不注册进驱动器本身：驱动器是挂线程、连控制器、持
// 状态机的 QObject，本表是"纯数据 + 一把互斥锁"——分开付两条很短的契约，注册方
// 的构造期依赖就只到注册表递送为止，测试里也可单独立起一张表而不带线程与控制器。
// 本表自身作为一个服务经 ServiceRegistry 递送（MainWindow 构造期 provide）。
//
// 线程契约：add 仅 GUI 线程（列表绑定点：Playlist 构造与换源）；snapshot /
// hasEntry 任意线程可调，互斥锁内完成。
//
// 报告者实例的寿命归提供者（与它所属列表同生共死）：本表只持 weak_ptr，不延长
// 任何对象寿命，也不会在工作线程上析构 GUI 侧对象——取快照时 lock 失败者即过期
// 并顺手清理。重新授权 = 新实例随换源再注册一次、旧实例无人引用即失效，无需
// 注销点；duplicated 产出的实例同样注册、同样随其析构失效。
class PlaybackReporterRegistry {
  public:
    // 表项快照：种类键（报告者种类，注册方给出，Jellyfin 用 kJellyfinProviderKey，
    // 与设置目录的开关键一一对应）+ 归属列表身份（路由用，nil = 无归属）+
    // 已 lock 的报告者。同键可有多个实例（同一服务器建两个列表即两个报告者都在册、
    // 归属各异，驱动器据此把事件只投给来源那一条列表的报告者）。
    // 顺序 = 注册顺序——驱动器按此顺序逐个调用。
    struct Entry {
        QString key;
        QUuid listUuid;
        std::shared_ptr<PlaybackReporter> reporter;
    };

    // 注册（GUI 线程）。listUuid 是该报告者所属列表的身份，由列表绑定点交出——
    // 提供者按 N57 §2.4 不认识列表身份，所以它不能是注册方。nil 表示无归属
    // （测试自立的表 / 不经列表注册的遗留形态），驱动器对它一律投。
    // 同键多实例并存（不同列表各一册）不去重；同 (键, 归属) 只留最新一册——换源
    // 后旧实例可能被在途任务吊着仍存活，顶掉才不会重复投递与串凭据。
    void add(const QString &key, const QUuid &listUuid,
             const std::shared_ptr<PlaybackReporter> &reporter);

    // 取快照：lock 失败者即过期并顺手清理，故本函数带副作用、非 const。
    // 交出的 shared_ptr 只把寿命维持到快照自身析构（扇出中途提供方析构即安全）。
    std::vector<Entry> snapshot();

    // 在册布尔：add 时顺手置位、不复位（过期清理不回拨）。驱动器线程启动判据的
    // 第二个条件供 GUI 侧现查用——过期残留至多造成一次空转的线程启动，无害。
    bool hasEntry() const;

    // 投递路由判据（纯谓词，住这儿而非驱动器：它是"表项归属 ↔ 事件来源"这条规则，
    // 随本表的形状成立；驱动器链接 mpv 侧不可实例化、放那儿就测不到）。
    // 归属为 nil（不经列表绑定点的注册）→ 一律投；来源为 nil（正播条目解析不出
    // 归属）→ 有归属者一律**不投**，宁漏不串；否则相等才投。
    static bool claimsSource(const QUuid &ownerUuid, const QUuid &sourceUuid);

    // 归属解析（同族纯谓词）：来源列表身份的**来源侧**判定，与 `claimsSource` 的
    // 表项侧判定合成"宁漏不串"的完整路由规则——两半必须同住一处，判据住主窗转发
    // 槽即零覆盖。入参：正播实体（可空——越界 / 层不在缓存 / 异步推进中）、已
    // unwrap 折回的条目身份、正播 tab 的列表身份（可空）。实体在位 → `matches`
    // 键级判等相符才交 `ownerUuid()`，不符交 nil（载荷与现读行号不原子的错配防线，
    // 故意宽松的 matches 仍认得出形态漂移）；实体缺位 → 正播列表非 nil 且非默认
    // 列表才回落该列表身份（真列表根层项的归属恒等于它自己，回落不引入错配），
    // 默认 tab 混得进收编项（归属是别家列表），歧义区不猜——三种落空全交 nil。
    static QUuid resolveSourceUuid(const PlaylistEntity::Ptr &entity, const QString &identity,
                                   const QUuid &playingListUuid);

  private:
    struct Row {
        QString key;
        QUuid listUuid;
        std::weak_ptr<PlaybackReporter> reporter;
    };

    mutable std::mutex m_mutex;
    // 顺序 = 注册顺序（同键插到该键组尾，即整表注册序）
    std::vector<Row> m_entries;
    bool m_hasEntry = false;
};

#pragma once

#include "pch.h"

#include "PlaylistEntity.h"
#include "PlaylistProvider.h"

// 播放列表（N57 §2.2）：会话级一等对象——身份、名字与提供者三件套齐备才诞生，
// 唯一所有者是 PlaylistRegistry（对外只出裸指针、不透明所有权）。无视图依赖、
// 不可拷贝；默认列表是其中的真实一员，只是不落盘、不可改名换源摘除。
// 线程口径（§2.10）：仅 GUI 线程读写，不设 mutex。
class Playlist {
  public:
    // 一次成型：不存在"无提供者的中间态"，uuid 断言非 nil、provider 断言非空。
    // 本类不自铸身份也不反问注册表：新建与加载均由调用方交值
    Playlist(const QUuid &uuid, QString name, std::shared_ptr<PlaylistProvider> provider);
    Playlist(const Playlist &) = delete;
    Playlist &operator=(const Playlist &) = delete;

    const QUuid &uuid() const { return m_uuid; } // 恒非 nil
    bool isDefault() const;                      // 即 uuid() == kDefaultListUuid

    const QString &name() const { return m_name; }
    // 改名一律走视图的显式方法（tab 文本是名字的镜像，§2.6 不变式）；默认列表拒绝
    void setName(QString name);

    // 返回引用是硬要求：N55 §2.2 起逐条取它的是视图侧的入列盖章（每个实体一份弱引用，
    // 按值返回即每项两次原子引用计数）；playTargets 反而不再经它——形态由实体问自身归属
    const std::shared_ptr<PlaylistProvider> &provider() const { return m_provider; }
    // 换身即替换，身份与名字不动；不向提供者下推任何东西。默认列表拒绝。
    // N55 §2.6 实施约束 1：换身恰好等于"来源参数变了"，换身后旧实例若仍被在途任务
    // 的 shared_ptr 吊着，归属该列表的显示中实体的弱引用仍能 lock 成功——来源失效
    // 清扫（PlaylistWidget::applySourceRetired，由本函数全仓唯一调用点
    // PlaylistWidget::openListSettings 同栈触发）保证这批实体先于新参数首次 publish
    // 被移除；漏调即静默拿到别家端点，本注释指回 N55 §2.6 兜（文档，不是机制）
    void setProvider(std::shared_ptr<PlaylistProvider> provider);

    // 播放形态：逐项走实体的归属出口 entity->playTarget()（N55 §2.3 通路 + N65
    // 整体出口）——本类既不交自身身份也不取提供者，收编进默认列表的转接项因此
    // 仍由原列表加工。凭据注入与转接铸造仍在提供者侧（§2.4）；空槽位回空串 +
    // 空 map 以保队列对位。N65：**本函数是全仓唯一的选项合并点**——提供者产出
    // 为底、条目级 playOptions 逐键覆盖，"条目级覆盖提供者级"的顺序在此写死
    // （两族键不同源，规则今天不改变任何输出，只是把确定性写出来）
    QVector<PlayTarget> playTargets(const PlaylistEntity::List &entities) const;

    // 列表自己那两键（名字与身份）——一个 INI 组要三段合写，其余由编排点接续
    // （`PlaylistWidget::writeTabGroup`）。身份字面形态恒为 WithoutBraces 小写串
    void save(QSettings &settings) const;

  private:
    QUuid m_uuid;
    QString m_name;
    std::shared_ptr<PlaylistProvider> m_provider; // 恒非空
};

#pragma once
#include "pch.h"

class BookmarkManager : public QObject {
    Q_OBJECT
  public:
    struct Bookmark {
        double time = 0.0;
        QString title;
    };

    // N54：书签来源——它出生时所属的列表与所在层。N77 起来源随组：组标识的 uuid 分量
    // 与来源身份恒等（冗余但保留结构，load / save 沿用既有两键形状），levelIds 与展示
    // 串仍首见即定。listUuid 为 nil 即整条来源无效（旧 INI / 损坏数据；N57 §2.5 后
    // 默认 tab 出生的书签记默认列表的保留身份，不再是无归属），此时 levelIds 不参与
    // 解读——空 levelIds 配上有效身份表示根层，两者须可区分。
    // N57 §2.4：身份在内存中恒为 QUuid（判等全在代码侧，大小写与花括号不参与），
    // 字面串形态只在 load / save 各一处转换
    struct Origin {
        QUuid listUuid;       // 列表身份（Playlist::uuid），nil = 无身份
        QStringList levelIds; // 队列根层 id 序列（enumerate 的 parent 形态）

        // 有效性只看身份：空 levelIds 配有效身份是"根层"，与整条无来源可区分
        bool isValid() const { return !listUuid.isNull(); }
    };

    // N77：分组标识 = (列表身份, 内容键)。书签的"承诺"（回哪个列表）是出生时的列表
    // 事实，不再由内容键全局共享——跨列表同内容各成一组，写侧零串扰。listUuid 可为
    // nil（旧 INI 自带缺键 / 无效身份的条目），nil 独立成组、不并默认组，跳转路由
    // 形状因此不变
    struct Group {
        QUuid listUuid;
        QString key;

        bool operator==(const Group &o) const { return listUuid == o.listUuid && key == o.key; }
        bool operator!=(const Group &o) const { return !(*this == o); }
        bool operator<(const Group &o) const {
            if (listUuid != o.listUuid)
                return listUuid < o.listUuid;
            return key < o.key;
        }
    };

    // N77 §2.3：合并读的单组快照（按 key 的本地合并读查询逐组返回书签与来源；
    // 展示串不随行——读侧展示经 displayPath(组) 按需现取）
    struct GroupEntries {
        Group group;
        Origin origin; // 该组来源（uuid 分量与组标识恒等）
        QList<Bookmark> bookmarks;
    };

    explicit BookmarkManager(const QString &configFile, QObject *parent = nullptr);

    void load();
    void save() const;

    // N77：会话内全部组（组序权威，save 按此落盘；编辑器逐组陈列）
    QList<Group> allFiles() const { return m_order; }
    QList<Bookmark> bookmarks(const Group &group) const { return m_data.value(group); }
    // UI 展示用：组 → 首见原始路径（保留大小写/分隔符形态；N56 §2.4：书签层
    // 完全豁免显示纪律——存串取自实体 path()，用户凭据原样保留并展示，load 不为
    // 已有值做处理）
    QString displayPath(const Group &group) const { return m_displayOf.value(group, group.key); }
    // N54：组 → 来源；缺键（无身份 / 旧数据）返回空 Origin
    Origin bookmarkOrigin(const Group &group) const { return m_originOf.value(group); }

    // N77 §2.3：同键各组是否全部本地（判据单点：各组展示串经 Util::Url::isLocal）。
    // 键空间按形态分域，本地路径与远端 URL 归一后键不同，同键各组本地性恒一致；
    // 不满足（理论不可达，防御保留）即调用方走严格口径
    bool allGroupsLocal(const QString &key) const;
    // N77 §2.3：按 key 的本地合并读——同键全部组按组序逐组返回，供显示豁免拼合并
    // 视图。仅在 allGroupsLocal(key) 为真时调用
    QList<GroupEntries> groupsByKey(const QString &key) const;

    void addBookmark(const QString &rawPath, double time, const QString &title,
                     const Origin &origin);
    // N16：按索引改写书签（时间/标题）；校验交给调用方。改后重排+落盘+发 changed
    void updateBookmark(const Group &group, int index, double time, const QString &title);
    void removeBookmark(const Group &group, int index);
    void removeFile(const Group &group);
    void clearAll();
    // 移除失效项。本地项看磁盘：不存在即失效（判据单点 Util::Url::isMissingLocal）。
    // 远端项看归属（N54）：本篇不做 INI 兼容与迁移，故"没有可用归属"就是数据异常——
    // 既无来源身份、或来源身份在现存列表里查不到，这类项按 §2.2 跳转必被拒（远端不
    // 回退），留着只是列表里的死条目。N77：谓参逐组以其自身 uuid 调用（组标识的
    // uuid 分量与来源身份恒等）
    // N57 §2.4：谓参改为**必传**（形参换 QUuid）——原"未接即不删"那一档在仓内从无
    // 调用形态（唯一调用方恒接入谓词），只是多一条无人走的分支；锁着它的两条测试用例
    // 随之改写。存活判据与列表名现查同出一条查询（空返回即无此列表），故由调用方就地
    // 派生，不另设第二份判据
    using ListAlivePredicate = std::function<bool(const QUuid &listUuid)>;
    int removeInvalid(const ListAlivePredicate &listAlive);

    // N77 §2.5：组过户——本地跳转落默认 tab 时再归属。目标组 (kDefaultListUuid, key)
    // 缺席 ⇒ 整体迁移（组 uuid 改默认、来源 uuid 同步改写、levelIds 清空：空配有效
    // 身份即根层）；已存在 ⇒ 合并（书签并入后按时间稳定排序，不去重；展示串保留目标
    // 既有；源组层引用丢弃）。只改分组记账与落盘，不碰播放、tab 与 mpv
    void transferToDefault(const QUuid &fromUuid, const QString &key);

  signals:
    void changed();

  private:
    QString m_configFile;
    QMap<Group, QList<Bookmark>> m_data;
    QMap<Group, QString> m_displayOf; // 组 → 原始路径（N15 实测修正：展示保原样）
    QMap<Group, Origin> m_originOf;   // N54：组 → 来源（与展示路径同纪律：首见即定）
    QList<Group> m_order;
};

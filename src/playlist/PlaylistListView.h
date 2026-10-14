#pragma once

#include "pch.h"

#include "Playlist.h"

// 面板级共享时长仓的类型名（**归属 `PlaylistWidget`**，§2.1：跨 tab 共享状态不入
// 视图、不入注册表；委托按指针引用，视图经它现查时长）
using DurationStore = QHash<QString, std::optional<double>>;

// 一个 tab 的视图侧全部状态（N57 §2.2 / §2.8）：全仓唯一的列表页诞生点
// （PlaylistWidget::createView）出生它，构造期一次注入所属列表。本篇把原先六张
// "以控件指针为键的平行表"收进本类成为成员——对象即状态，无键、无增删同步点、
// 无漏删可能（不是把六张表合成第七张，而是不再有表）。
//
// 只搬状态，不搬编排：行构建与换入、播放标记、四步式、异步推进、遮罩与工具条仍归
// PlaylistWidget；随状态一起搬走的只有纯读写层 / 路径 / 记忆簿记的那些函数。
//
// N45 层级模型口径原样保留：层在缓存即"已加载"（不设并行标志），`ensureLevel` 的
// "只取不插 / 插入即宣告已加载"两条纪律一字不动（对 N58 的契约①②）。
// 线程口径（§2.10）：状态仅 GUI 线程读写；析构路径不得解引用所属列表。
class PlaylistListView : public QListWidget {
    // Q_OBJECT 必需：viewAt 用 qobject_cast<PlaylistListView *> 辨页，无自身
    // metaobject 时会退回基类的静态元对象，任何 QWidget 都会被误判命中
    Q_OBJECT
  public:
    // ——N45 §2.5 层级模型（自 PlaylistWidget 私有嵌套搬来，定义一字不动）——
    // 路径段：id 与标题两列。队列根与浏览路径同形（§2.1）；标题仅供 tooltip
    // 拼相对路径（§2.4.3），不进层缓存键
    struct PathSeg {
        QString id;
        QString title;
    };
    // 单层内容与状态。**"本层是否已加载"不设独立标志——层在本缓存中存在即已加载**
    // （归一化旧的 entriesLoaded 标志与 m_entities 容器两条并行来源）；
    // loadError 非空 = 本层枚举失败（内容一律清空，行上只剩骨架与占位行）
    struct Level {
        // ——N58 §2.2 层元素包装体：序号是"这一层这一次枚举的产物"，与层同寿——
        // 包装体即层内元素本身：序号随层缓存逐出消失，不进实体接口、不进提供者接口
        // （Container 是值类型，加字段等于把内部簿记抬进提供者接口）。收编 clone()
        // 的实体来自别的列表，包装体随新层构造、序号当场重赋
        struct EntityEntry {
            PlaylistEntity::Ptr entity;
            int enumerationIndex = 0;
        };
        struct ContainerEntry {
            PlaylistProvider::Container container;
            int enumerationIndex = 0;
        };
        QList<ContainerEntry> containers; // 容器组（恒在实体组之前，§2.2）
        QList<EntityEntry> entities;      // 实体所有权仓（序 == 实体行序）
        bool fileMetaLoaded = false;      // 本层 fileMeta 已全表读取完成
        bool mediaMetaLoaded = false;     // 本层 mediaMeta 已全表读取完成
        QString lastSelectedKey;          // 本层上次选中项（实体 key 或容器 id），切层换入时恢复
        QString loadError;                // 非空 = 本层枚举失败（占位行显示中）
    };
    // 单个 tab 的播放记忆（默认 tab 仅进程内有效，持久化时跳过它）。
    // N9/N26：lastPath 为记忆条目 key（Util::Url::key 归一形态，引用条目身份
    // 而非某次字符串形态；旧 INI 读取时归一桥接），恢复时序号校验优先
    struct Memory {
        int lastPos = -1;
        double lastTimePos = -1.0;
        QString lastPath;
    };

    explicit PlaylistListView(Playlist *list, QWidget *parent = nullptr);
    ~PlaylistListView() override;

    // 恒非空（构造期注入，断言守候）。析构路径不得取它（§2.10 口径 3）
    Playlist *list() const { return m_list; }

    // ——层缓存（外层键已消失，内层 QMap 键形态不变：enumerate 的 parent 序列）——
    // 层定位（可写，缺省插入空层）：**仅限建行 / 回传 / 直写 / 失败登记路径**——
    // 插入即宣告"已加载"，读路径误用会把未加载层变成幽灵已加载层
    Level &ensureLevel(const QStringList &ids);
    // 层定位：不在缓存返回 nullptr（只取不插——读路径一律用它）
    const Level *findLevel(const QStringList &ids) const;
    // 可写重载：改本层的簿记字段（层选中记忆、失败文案）而不插新层
    Level *findLevel(const QStringList &ids);
    // 当前浏览层（ensure 语义）：承接旧 entitiesOf 的插入语义
    Level &levelOf();
    // 层已加载 = 在缓存且 loadError 为空（失败层存在但不算已加载）
    bool layerLoaded(const QStringList &ids) const;
    // 层的失败文案；层不在缓存返回空串（未加载 ≠ 失败）
    QString levelErrorOf(const QStringList &ids) const;
    // 无条件删单层、不连带子孙（下面"仅失败层"档的底座；本表内暂无独立调用方，
    // 由 §2.2 出口清单要求保留）
    void eraseLevel(const QStringList &ids);
    // 只删"已失败"的那一层（未加载与成功层都不动）——N54 §2.4 的重试入口
    void eraseFailedLevel(const QStringList &ids);
    // 刷新整枝（N45 §2.5）：删浏览层的全部**子孙**层、留浏览层自身
    void invalidateDescendantsOf(const QStringList &browseIds);
    // 本层两类"已全表读取"标志复位（N20 跨表驱逐由 PlaylistWidget 逐 tab 调用）
    void resetMetaFlags();
    // 全缓存层的遍历口：排序的执行面（规则变更时队列根层与其余已缓存层一并重排，
    // N45 §2.6）留在 PlaylistWidget，状态在本类——只给引用，不给第二份容器
    QMap<QStringList, Level> &levels();
    // 当前浏览层的实体仓（引用；所有权在 Level）
    QList<Level::EntityEntry> &entries();
    // 按当前数组下标重编号（N58 §2.3 唯一纪律）：凡层内容集合发生变化的路径，
    // 全部写入动作完成之后调用一次。调用点全在 PlaylistWidget（写入侧），故不收私有
    static void renumberEnumerationOrder(Level &lvl);

    // ——路径与显示层——
    QStringList browsePathIds() const; // enumerate 的 parent 形态（根层即空序列）
    QStringList queueRootIds() const;
    QList<PathSeg> browsePath() const; // 含标题列（"上一级"与容器行的 tooltip 用）
    QList<PathSeg> queueRoot() const;
    // 纯替换。"替换前保存离开层选中"的纪律在调用方（PlaylistWidget::setBrowsePath
    // 与钻取 / 返回同一口径），此处不设回调
    void setBrowsePath(const QList<PathSeg> &path);
    void setQueueRoot(const QList<PathSeg> &root);
    // 行内容当前对应的层：浏览路径可先行漂移（在途 / 层未在缓存），二者分开跟踪
    QStringList displayedIds() const;
    void setDisplayedIds(const QStringList &ids);

    // ——续播记忆（默认 tab 仅进程内有效，持久化跳过）——
    // 不设"有无记忆"的并行标志：原表的 contains 判据在各调用点恒与
    // `lastPos >= 0` 合取（缺项读值即默认值 -1 / 空），成员化后该判据由值本身承担
    Memory &memory();
    const Memory &memory() const;
    void setMemory(const Memory &m);
    void clearMemory();         // 整条作废（回到默认值）
    void clearResumeProgress(); // B9：清进度、留最后播放项

    // ——换源清理（§2.7 换身行）：层 / 浏览路径 / 队列根 / 显示层 / 记忆全清 + 行清——
    void resetForNewSource();

  private:
    Playlist *m_list;
    // "行变更 → 整表重绘"的接线句柄：析构开头主动断开（原因见 .cpp 构造函数与
    // 析构函数的注释），不等 ~QObject
    QList<QMetaObject::Connection> m_rowRepaints;
    QMap<QStringList, Level> m_levels;
    QList<PathSeg> m_browsePath;
    QList<PathSeg> m_queueRoot;
    QStringList m_displayedIds;
    Memory m_memory;
};

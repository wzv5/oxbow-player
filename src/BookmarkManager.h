#pragma once
#include "pch.h"

class BookmarkManager : public QObject {
    Q_OBJECT
  public:
    struct Bookmark {
        double time = 0.0;
        QString title;
    };

    // N54：书签来源——它出生时所属的列表与所在层。文件级属性（同归一键下的多条
    // 书签共享一个来源，与 m_displayOf 同层），跳转据此"回绑定的列表 → 换入该层 →
    // 定位条目"。listUuid 为 nil 即整条来源无效（旧 INI / 损坏数据；N57 §2.5 后
    // 默认 tab 出生的书签改记默认列表的保留身份，不再是无归属），此时 levelIds
    // 不参与解读——空 levelIds 配上有效身份表示根层，两者须可区分。
    // N57 §2.4：身份在内存中恒为 QUuid（判等全在代码侧，大小写与花括号不参与），
    // 字面串形态只在 load / save 各一处转换
    struct Origin {
        QUuid listUuid;       // 列表身份（Playlist::uuid），nil = 无身份
        QStringList levelIds; // 队列根层 id 序列（enumerate 的 parent 形态）

        // 有效性只看身份：空 levelIds 配有效身份是"根层"，与整条无来源可区分
        bool isValid() const { return !listUuid.isNull(); }
    };

    explicit BookmarkManager(const QString &configFile, QObject *parent = nullptr);

    void load();
    void save() const;

    QStringList allFiles() const { return m_order; }
    QList<Bookmark> bookmarks(const QString &path) const;
    // UI 展示用：归一化键 → 首见原始路径（保留大小写/分隔符形态；N56 §2.4：书签层
    // 完全豁免显示纪律——存串取自实体 path()，用户凭据原样保留并展示，load 不为
    // 已有值做处理）
    QString displayPath(const QString &normalizedKey) const {
        return m_displayOf.value(normalizedKey, normalizedKey);
    }
    // N54：归一化键 → 来源；缺键（无身份 / 旧数据）返回空 Origin
    Origin bookmarkOrigin(const QString &normalizedKey) const {
        return m_originOf.value(normalizedKey);
    }

    void addBookmark(const QString &rawPath, double time, const QString &title,
                     const Origin &origin);
    // N16：按索引改写书签（时间/标题）；校验交给调用方。改后重排+落盘+发 changed
    void updateBookmark(const QString &path, int index, double time, const QString &title);
    void removeBookmark(const QString &path, int index);
    void removeFile(const QString &path);
    void clearAll();
    // 移除失效项。本地项看磁盘：不存在即失效（判据单点 Util::Url::isMissingLocal）。
    // 远端项看归属（N54）：本篇不做 INI 兼容与迁移，故"没有可用归属"就是数据异常——
    // 既无来源身份、或来源身份在现存列表里查不到，这类项按 §2.2 跳转必被拒（远端不
    // 回退），留着只是列表里的死条目。
    // N57 §2.4：谓参改为**必传**（形参换 QUuid）——原"未接即不删"那一档在仓内从无
    // 调用形态（唯一调用方恒接入谓词），只是多一条无人走的分支；锁着它的两条测试用例
    // 随之改写。存活判据与列表名现查同出一条查询（空返回即无此列表），故由调用方就地
    // 派生，不另设第二份判据
    using ListAlivePredicate = std::function<bool(const QUuid &listUuid)>;
    int removeInvalid(const ListAlivePredicate &listAlive);

  signals:
    void changed();

  private:
    QString m_configFile;
    QMap<QString, QList<Bookmark>> m_data;
    QMap<QString, QString> m_displayOf; // 归一化键 → 原始路径（N15 实测修正：展示保原样）
    QMap<QString, Origin> m_originOf;   // N54：归一化键 → 来源（与展示路径同纪律：首见即定）
    QStringList m_order;
};

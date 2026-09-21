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
    // 定位条目"。listUuid 为空即整条来源无效（默认 tab 出生 / 旧 INI），此时
    // levelIds 不参与解读——空 levelIds 配上有效 uuid 表示根层，两者须可区分。
    struct Origin {
        QString listUuid;     // 列表身份（PlaylistProvider::listUuid），空 = 无身份
        QStringList levelIds; // 队列根层 id 序列（enumerate 的 parent 形态）

        // 有效性只看 uuid：空 levelIds 配有效 uuid 是"根层"，与整条无来源可区分
        bool isValid() const { return !listUuid.isEmpty(); }
    };

    explicit BookmarkManager(const QString &configFile, QObject *parent = nullptr);

    void load();
    void save() const;

    QStringList allFiles() const { return m_order; }
    QList<Bookmark> bookmarks(const QString &path) const;
    // UI 展示用：归一化键 → 首见原始路径（保留大小写/分隔符形态；N48：凭据段不保留
    // ——新增书签在入库前剥除 userinfo，load 不为已有值做处理）
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
    // 移除失效项。本地项看磁盘：不存在即失效（判据单点 Util::isMissingLocalSource）。
    // 远端项看归属（N54）：本篇不做 INI 兼容与迁移，故"没有可用归属"就是数据异常——
    // 既无来源 uuid、或来源 uuid 在现存列表里查不到，这类项按 §2.2 跳转必被拒（远端不
    // 回退），留着只是列表里的死条目。listAlive 未接时"列表是否还在"这一半无从判定，
    // 只按"无来源"那一半清，绝不把有来源的项误删
    using ListAlivePredicate = std::function<bool(const QString &listUuid)>;
    int removeInvalid(const ListAlivePredicate &listAlive = {});

  signals:
    void changed();

  private:
    QString m_configFile;
    QMap<QString, QList<Bookmark>> m_data;
    QMap<QString, QString> m_displayOf; // 归一化键 → 原始路径（N15 实测修正：展示保原样）
    QMap<QString, Origin> m_originOf;   // N54：归一化键 → 来源（与展示路径同纪律：首见即定）
    QStringList m_order;
};

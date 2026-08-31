#pragma once
#include "pch.h"

class BookmarkManager : public QObject {
    Q_OBJECT
  public:
    struct Bookmark {
        double time = 0.0;
        QString title;
    };

    explicit BookmarkManager(const QString &configFile, QObject *parent = nullptr);

    void load();
    void save() const;

    QStringList allFiles() const { return m_order; }
    QList<Bookmark> bookmarks(const QString &path) const;
    // UI 展示用：归一化键 → 首见原始路径（保留大小写/分隔符形态）
    QString displayPath(const QString &normalizedKey) const {
        return m_displayOf.value(normalizedKey, normalizedKey);
    }

    void addBookmark(const QString &rawPath, double time, const QString &title);
    // N16：按索引改写书签（时间/标题）；校验交给调用方。改后重排+落盘+发 changed
    void updateBookmark(const QString &path, int index, double time, const QString &title);
    void removeBookmark(const QString &path, int index);
    void removeFile(const QString &path);
    void clearAll();
    // 一键移除不存在文件
    int removeInvalid();

  signals:
    void changed();

  private:
    QString m_configFile;
    QMap<QString, QList<Bookmark>> m_data;
    QMap<QString, QString> m_displayOf; // 归一化键 → 原始路径（N15 实测修正：展示保原样）
    QStringList m_order;
};

#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

#include "MediaInfoHelper.h"

// 本地文件实体（N26）：QFileInfo stat、MediaInfoHelper 解析、系统图标均就地求值；
// MediaInfo 依赖关在实体内部。
// canComputeMediaMeta = MediaInfoHelper::available()，进程期恒定。
class LocalFileEntity : public PlaylistEntity {
  public:
    explicit LocalFileEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<LocalFileEntity>(*this); }

    bool isLocalFile() const override { return true; }
    bool exists() const override { return QFileInfo::exists(path()); }
    // 不可读 / 不存在属"值未知"，不是能力缺失
    bool canComputeFileMeta() const override { return true; }
    bool canComputeMediaMeta() const override { return MediaInfoHelper::available(); }
    // 本地文件随机 seek 零成本（N107）
    bool supportsSecondaryDecode() const override { return true; }

    // 文件级元数据的唯一求值口径：不存在 / 不可读即三字段全空（N29）
    static FileMeta fileMetaFrom(const QFileInfo &fi) {
        FileMeta meta;
        if (!fi.exists() || !fi.isReadable())
            return meta;
        meta.size = qMax<qint64>(0, fi.size());
        meta.created = fi.birthTime();
        meta.modified = fi.lastModified();
        return meta;
    }

  protected:
    FileMeta computeFileMeta() const override { return fileMetaFrom(QFileInfo(path())); }

    // 解析失败返回默认 Data，同样入缓存，防同一坏文件反复解析
    MediaMeta computeMediaMeta() const override { return MediaInfoHelper::parse(path()); }

    // 不存在取通用图标；仅 GUI 线程
    QIcon computeIcon() const override {
        static QFileIconProvider provider;
        const QFileInfo fi(path());
        if (!fi.exists())
            return provider.icon(QFileIconProvider::File);
        return provider.icon(fi);
    }
};

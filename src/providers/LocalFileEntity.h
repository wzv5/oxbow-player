#pragma once

#include "pch.h"

#include "playlist/PlaylistEntity.h"

// 本地文件实体（N26）：元数据全部就地求值——QFileInfo 一次 stat 取文件级字段，
// MediaInfoHelper 同步解析媒体属性，QFileIconProvider 取系统图标。
// 把 MediaInfo 依赖关在实体内部（视图层不再认识该 dll）：
// supportsMediaMeta() 返回 dll 可用性，行为与重构前全局判定完全一致。
class LocalFileEntity : public PlaylistEntity {
  public:
    // registry 必填（经基类持有，本地实体不用；无缺省，调用点显式传）
    explicit LocalFileEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<LocalFileEntity>(*this); }

    bool isLocalFile() const override { return true; }
    bool exists() const override { return QFileInfo::exists(path()); }
    // 文件不存在属"值未知"（求值结果三字段全空），非能力缺失
    bool supportsFileMeta() const override { return true; }
    bool supportsMediaMeta() const override { return MediaInfoHelper::available(); }
    // N107：本地文件随机 seek 零成本、无对手方，恒可另起一路解码
    bool supportsSecondaryDecode() const override { return true; }

    // N29：fileMeta 求值口径的唯一载体——惰性求值与提供者注入共用一份，防两处漂移
    // （"不存在 / 不可读 → 三字段全空"、"size 取 qMax(0, …)"）
    static FileMeta fileMetaFrom(const QFileInfo &fi);

  protected:
    FileMeta computeFileMeta() const override;
    MediaMeta computeMediaMeta() const override;
    QIcon computeIcon() const override;
};

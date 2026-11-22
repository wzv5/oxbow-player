#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

// 兜底实体（N26）：网络 / 流路径的落点；path 原样透传，不求值。
class UnknownEntity : public PlaylistEntity {
  public:
    explicit UnknownEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<UnknownEntity>(*this); }

    bool isLocalFile() const override { return false; }
    bool exists() const override { return true; } // 不可判定，不参与失效清理
    bool canComputeFileMeta() const override { return false; }
    bool canComputeMediaMeta() const override { return false; }

  protected:
    FileMeta computeFileMeta() const override { return {}; }   // 恒不被调
    MediaMeta computeMediaMeta() const override { return {}; } // 恒不被调

    // 通用文件图标；仅 GUI 线程
    QIcon computeIcon() const override {
        static QFileIconProvider provider;
        return provider.icon(QFileIconProvider::File);
    }
};

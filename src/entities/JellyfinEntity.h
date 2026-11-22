#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

// jellyfin:// 的 scheme 常量，工厂与提供者共用
inline constexpr char kJellyfinScheme[] = "jellyfin://";

// Jellyfin 叶子实体（N69）：工厂 jellyfin:// 的落点；IsFolder=false，入列由扩展名
// 过滤，可进入的层走容器。身份 URN jellyfin://<serverId>/<type>/<itemId>，取流 URL
// 由铸造点现拼。元数据只来自枚举期注入，canCompute* 为假（N178）；显示名与文件名
// 分别注入（N70），Episode 带集号前缀（N72）。
class JellyfinEntity : public PlaylistEntity {
  public:
    JellyfinEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<JellyfinEntity>(*this); }

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

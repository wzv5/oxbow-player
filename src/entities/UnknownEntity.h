#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

// 兜底实体（N26）：工厂对网络 / 流路径产出。插件不做任何额外加工，
// path() 原样喂 mpv / 持久化 / 回显比对。完整远端实体（服务端标题、延迟解析、
// 远端元数据）归后续篇；未知实体是其最小替身，保证网络项有实体可落
// （回刷收编的 mpv 列表即现实引用者）。
class UnknownEntity : public PlaylistEntity {
  public:
    // registry 必填（经基类持有，兜底实体不用；无缺省，调用点显式传）
    explicit UnknownEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<UnknownEntity>(*this); }

    bool isLocalFile() const override { return false; }
    bool exists() const override { return true; } // 不可判定，不参与失效清理
    bool supportsFileMeta() const override { return false; }
    bool supportsMediaMeta() const override { return false; }

  protected:
    FileMeta computeFileMeta() const override {
        return {}; // 恒不被调（supportsFileMeta 为假）
    }
    MediaMeta computeMediaMeta() const override {
        return {}; // 恒不被调（supportsMediaMeta 为假）
    }
    // 通用文件图标（与重构前网络路径表现一致）；仅 GUI 线程调用。
    // provider 无状态，函数内 static 复用一次构造
    QIcon computeIcon() const override {
        static QFileIconProvider provider;
        return provider.icon(QFileIconProvider::File);
    }
};

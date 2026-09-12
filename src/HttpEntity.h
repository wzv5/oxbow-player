#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

// ServiceRegistry 前向声明见 PlaylistEntity.h（本头仅以引用形态使用，无需包含）

// 通用 http(s) 播放项实体（N35）：工厂对 http:// / https:// 前缀的分派落点。
// 支持文件级元数据（PROPFIND 注入为主路，惰性 HEAD 为回退），不支持媒体元数据
// （恒走静态空回落）。playUrl 恒为可播放的完整 URL（含 userinfo），不做脱敏——
// UI 一律直接显示完整 URL，与本地路径的处理一致。
//
// 实体内 registry 由基类统一持有（构造注入，身份稳定，重装配是在同一注册表内
// 覆盖 HttpClient 实例），不保存 client：每次求值现查现用，设置下一次求值即生效
// （N35 §一-4）。
class HttpEntity : public PlaylistEntity {
  public:
    // registry 必须非空（构造内断言）：http(s) + 空 registry 属调用错误，
    // 工厂已在分派处挡住（Debug 断言、Release 降级 UnknownEntity）
    HttpEntity(QString playUrl, const std::shared_ptr<ServiceRegistry> &registry);

    Ptr clone() const override { return std::make_shared<HttpEntity>(*this); }

    bool isLocalFile() const override { return false; }
    bool exists() const override { return true; } // 不可判定，不参与失效清理
    bool supportsFileMeta() const override { return true; }
    bool supportsMediaMeta() const override { return false; }

  protected:
    // N35 修订 2：同步阻塞求值（exec），无取消通道——取消语义为结果静默丢弃
    // （调用方不等在途项、不采用其结果），体内零轮询；仅任务线程调用
    FileMeta computeFileMeta() const override;
    MediaMeta computeMediaMeta() const override; // 恒不被调（supportsMediaMeta 为假）
    QIcon computeIcon() const override;

  private:
    // registry 存于基类（registry() 取用），clone 经拷贝构造携带同一实例
};

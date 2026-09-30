#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

// 身份 scheme 常量唯一持有者：工厂分派（PlaylistEntity::fromPath）与提供者铸造
// （JellyfinProvider）共用同一点定义，杜绝"键写错导致分派落空"的静默失败
inline constexpr char kJellyfinScheme[] = "jellyfin://";

// Jellyfin 条目实体（N69）：工厂对 jellyfin:// 前缀的分派落点，JellyfinProvider
// 枚举的叶子——判据是 IsFolder=false（Movie / Episode / Audio / Video / Photo …
// 一律同形），哪类叶子入列由扩展名清单过滤，不由类型白名单裁决。可进入的层
// （库 / Series / Season / Folder …）走层缓存容器机制，不落实体。
// 身份是 URN 形态 jellyfin://<serverId>/<type>/<itemId>——serverId / itemId 两个
// 持久标识 + 类型段（小写，取流 URL 由铸造点从身份一步现拼），与服务器地址、
// 凭据、取流形态完全解耦（N69 设计：直连与转码两种未来都由铸造点从同一身份
// 现拼，身份零改动；服务器换地址不影响任何已存身份串）。
// 元数据值只来自提供者注入（/Items 的 fields 显式字段随 N39 注入通道写入），
// 实体自身不发起任何请求；未注入时空求值返回结果，与"值未知"同口径（WebDavEntity
// 先例）。能力位恒真不随单次枚举结果变化：刷新 = 重新枚举 = 重新注入。
// 服务端标题由提供者枚举期经注入口写入（URN 尾段 itemId 无语义，派生名只是
// 占位）。key() 用基类默认派生：URN 与地址、凭据解耦且形态恒定，无需重写。
class JellyfinEntity : public PlaylistEntity {
  public:
    // registry 经基类持有（构造契约同基类：必填，无缺省）
    JellyfinEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<JellyfinEntity>(*this); }

    bool isLocalFile() const override { return false; }
    bool exists() const override { return true; } // 不可判定，不参与失效清理
    bool supportsFileMeta() const override { return true; }
    bool supportsMediaMeta() const override { return true; }

  protected:
    // 恒不被调（枚举期注入覆盖全部取用点；仅 refresh() 清缓存后、下次枚举前的
    // 窗口内可能命中，返回空值即"值未知"）
    FileMeta computeFileMeta() const override { return {}; }
    MediaMeta computeMediaMeta() const override { return {}; }
    QIcon computeIcon() const override;
};

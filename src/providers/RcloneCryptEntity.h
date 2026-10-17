#pragma once

#include "pch.h"

#include "HttpEntity.h"

// 身份 scheme 常量唯一持有者：工厂分派（PlaylistEntity::fromPath）与提供者铸造
// （RcloneCryptProvider）共用同一点定义，杜绝"键写错导致分派落空"的静默失败
// （kJellyfinScheme 先例）
inline constexpr char kRcloneCryptScheme[] = "rclonecrypt://";

// rclone crypt 条目实体（N98）：工厂对 rclonecrypt:// 前缀的分派落点，
// RcloneCryptProvider 枚举的叶子。身份是 URN 形态
// rclonecrypt://<percent-encoded 相对路径>（段内 URL 编码、'/' 分隔符保留原样，
// N98 §6 要点 4——编码形态使 StreamTransit::mint 的 userinfo 守卫结构性免疫，
// '@' 已编成 %40）。
// 元数据值只来自提供者注入（size=明文长度、mtime=密文文件的 mtime），实体自身
// 不发起任何求值；能力位恒真不随单次枚举结果变化（WebDavEntity 同形：支持文件级
// 元数据、不支持媒体元数据，显示名与文件名由提供者经注入口写入）。
// 仅头文件实现——体量只有一个能力位重写；图标用 HttpEntity 的通用文件图标。
class RcloneCryptEntity : public HttpEntity {
  public:
    // registry 经基类持有（构造契约同基类：必填，无缺省）
    RcloneCryptEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : HttpEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<RcloneCryptEntity>(*this); }

    bool supportsFileMeta() const override { return true; }
    // N107：底层是本地文件解密流，随机 seek 零成本——看着像远端（身份串经转接），
    // 判据却与远端相反，点名写出免得被按"转接即昂贵"误改回去
    bool supportsSecondaryDecode() const override { return true; }
};

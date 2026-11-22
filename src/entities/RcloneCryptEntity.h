#pragma once

#include "pch.h"

#include "HttpEntity.h"

// rclonecrypt:// 的 scheme 常量，工厂与提供者共用
inline constexpr char kRcloneCryptScheme[] = "rclonecrypt://";

// rclone crypt 条目实体（N98）：工厂 rclonecrypt:// 的落点。身份 URN 编码相对路径
// （'/' 保留，段内字符 URL 编码）。元数据只来自提供者注入（明文长度 / 密文 mtime）；
// canCompute* 随 HttpEntity 为假。
class RcloneCryptEntity : public HttpEntity {
  public:
    RcloneCryptEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : HttpEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<RcloneCryptEntity>(*this); }

    // 底层本地解密流，随机 seek 零成本（N107）
    bool supportsSecondaryDecode() const override { return true; }
};

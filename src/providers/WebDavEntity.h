#pragma once

#include "pch.h"

#include "HttpEntity.h"

// WebDAV 条目实体（N39）：WebDavProvider 枚举的落点。与通用 http 项（HttpEntity）
// 的唯一差别是具备文件级元数据——值只来自提供者注入（PROPFIND 的 getcontentlength /
// getlastmodified），实体自身不发起任何请求；未注入时继承来的恒空求值返回结果，
// 与本地文件"文件不存在即值未知"同口径（值未知 ≠ 能力缺失，故能力位不随单次
// 枚举结果变化）。
// 其余全部继承 HttpEntity：显示名 percent-decode、通用文件图标、不支持媒体元数据、
// 存在性不可判定。仅头文件实现——体量只有一个能力位重写。
class WebDavEntity : public HttpEntity {
  public:
    // registry 必填（同 HttpEntity：构造内断言非空）
    WebDavEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : HttpEntity(std::move(path), registry) {}

    Ptr clone() const override { return std::make_shared<WebDavEntity>(*this); }

    bool supportsFileMeta() const override { return true; }
};

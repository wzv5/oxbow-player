#pragma once

#include "pch.h"

#include "PlaylistEntity.h"
#include "UnknownEntity.h"

#include "Util.h"

// 标准输入实体：mpv 以单独的 "-" 从 stdin 读媒体数据时的落点，只经回显工厂
// （fromMpvEcho 的 stdin 分支）产生。身份串恒为 mpv 原样的 "-"——回显比对与
// playUrl 靠它逐字节一致，不换成展示名；显示面在构造期定死为不翻译的 "stdin"。
// 能力位沿用兜底实体；标准输入是一次性数据流，书签在添加入口拒收
class StdinEntity : public UnknownEntity {
  public:
    explicit StdinEntity(const std::shared_ptr<ServiceRegistry> &registry)
        : UnknownEntity(QString::fromLatin1(Util::Url::kMpvStdinPath), registry) {
        setFilename(QStringLiteral("stdin")); // 标题随文件名派生
        setDisplayUrl(QStringLiteral("stdin"));
    }

    Ptr clone() const override { return std::make_shared<StdinEntity>(*this); }
};

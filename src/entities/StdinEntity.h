#pragma once

#include "pch.h"

#include "PlaylistEntity.h"
#include "UnknownEntity.h"

#include "Util.h"

// stdin 实体：mpv 的 "-" 回显落点；身份恒为 "-"，显示面定死为 "stdin"。
// 无自主求值；一次性数据流，书签入口拒收。
class StdinEntity : public UnknownEntity {
  public:
    explicit StdinEntity(const std::shared_ptr<ServiceRegistry> &registry)
        : UnknownEntity(QString::fromLatin1(Util::Url::kMpvStdinPath), registry) {
        setFilename(QStringLiteral("stdin")); // 标题随文件名派生
        setDisplayUrl(QStringLiteral("stdin"));
    }

    Ptr clone() const override { return std::make_shared<StdinEntity>(*this); }
};

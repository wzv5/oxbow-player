#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

#include "Util.h"

// ServiceRegistry 前向声明见 PlaylistEntity.h

// 通用 http(s) 实体（N35）：工厂对 http(s) 的分派落点，也承接手工 URL / 回显收编。
// 不自主求值（HEAD 耗时长、数据不准），canCompute* 为假；注入值照常落缓存（N178）。
// path 为完整可播放 URL，不脱敏；WebDAV 来源由提供者保证无 userinfo（N48）。
class HttpEntity : public PlaylistEntity {
  public:
    HttpEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
        : PlaylistEntity(std::move(path), registry) {
        Q_ASSERT(registry != nullptr);

        // 显示名取末段并 percent-decode，解后为空回退基类派生。构造参数名遮蔽
        // 基类访问器，取 path() 须显式限定
        const QString decoded = Util::Url::fileName(PlaylistEntity::path());
        if (!decoded.isEmpty())
            setFilename(decoded);
    }

    Ptr clone() const override { return std::make_shared<HttpEntity>(*this); }

    bool isLocalFile() const override { return false; }
    bool exists() const override { return true; } // 不可判定，不参与失效清理
    // 无自主求值；注入值由基类缓存承载（N178）
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

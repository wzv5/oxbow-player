#pragma once

#include "pch.h"

#include "PlaylistEntity.h"

// ServiceRegistry 前向声明见 PlaylistEntity.h（本头仅以引用形态使用，无需包含）

// 通用 http(s) 播放项实体（N35）：工厂对 http:// / https:// 前缀的分派落点，
// 也是手工打开 URL / mpv 播放列表收编等非注入来源的落点。
// N39 起不支持文件级元数据：原惰性 HEAD 求值（经注册表现查 client 发 HEAD 取
// content-length / last-modified）已删除——耗时长、数据不准，实际无使用价值。
// 该类型因此不具备任何取得元数据的途径，能力位如实为假，使排序菜单的文件级
// 排序项对纯 http 表自动灰掉；有元数据可注入的 http 实体见 WebDavEntity。
// 不支持媒体元数据（恒走静态空回落）。playUrl 恒为可播放的完整 URL（含
// userinfo），不做脱敏——UI 一律直接显示完整 URL，与本地路径的处理一致。
//
// registry 仍由基类统一持有（工厂契约要求 http(s) 必带非空实例），但本类不再
// 使用：元数据求值通道已删，未来远端元数据的落点亦在本类，届时重新启用。
class HttpEntity : public PlaylistEntity {
  public:
    // registry 必须非空（构造内断言）：http(s) + 空 registry 属调用错误，
    // 工厂已在分派处挡住（Debug 断言、Release 降级 UnknownEntity）
    HttpEntity(QString playUrl, const std::shared_ptr<ServiceRegistry> &registry);

    Ptr clone() const override { return std::make_shared<HttpEntity>(*this); }

    bool isLocalFile() const override { return false; }
    bool exists() const override { return true; } // 不可判定，不参与失效清理
    // N39：无取得元数据的途径（惰性 HEAD 已删），故为假——fileMeta() 走静态空回落、
    // cachedFileMeta() 恒空、注入口忽略写入、预读骨架跳过本类实体
    bool supportsFileMeta() const override { return false; }
    bool supportsMediaMeta() const override { return false; }

  protected:
    // N39：恒不被调（supportsFileMeta 为假）。原同步阻塞 HEAD 求值已删，见类注释
    FileMeta computeFileMeta() const override;
    MediaMeta computeMediaMeta() const override; // 恒不被调（supportsMediaMeta 为假）
    QIcon computeIcon() const override;

  private:
    // registry 存于基类（registry() 取用），clone 经拷贝构造携带同一实例
};

#include "pch.h"

#include "HttpEntity.h"

#include "Util.h"

HttpEntity::HttpEntity(QString path, const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistEntity(std::move(path), registry) {
    Q_ASSERT(registry != nullptr);

    // 显示名（N35 §2.2）：取末段并 percent-decode 后 setFilename（基类 deriveNames
    // 用 QFileInfo 取末段、不解码，否则列表出现 "%20"、"%E4%B8%AD" 之类）。逻辑自
    // N37 起收编进 Util::Url::fileName（标题 / OSD 取名共用同一出口）；解后为空
    // （如以 / 结尾）回退基类行为（title 已按 path() 派生）。
    // 构造参数名遮蔽基类同名访问器，取 path() 须显式限定
    const QString decoded = Util::Url::fileName(PlaylistEntity::path());
    if (!decoded.isEmpty())
        setFilename(decoded);
}

// N39：恒不被调（supportsFileMeta 为假）。原惰性 HEAD 求值已删——http 项的
// 元数据只由掌握信息的提供者注入（注入型实体见 WebDavEntity），实体自身不再
// 发起任何请求
FileMeta HttpEntity::computeFileMeta() const { return {}; }

// 恒不被调（supportsMediaMeta 为假）
PlaylistEntity::MediaMeta HttpEntity::computeMediaMeta() const { return {}; }

// 通用文件图标（与 UnknownEntity 同形，N35 §2.2）；仅 GUI 线程调用。
// provider 无状态，函数内 static 复用一次构造
QIcon HttpEntity::computeIcon() const {
    static QFileIconProvider provider;
    return provider.icon(QFileIconProvider::File);
}

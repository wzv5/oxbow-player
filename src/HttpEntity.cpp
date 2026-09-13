#include "pch.h"

#include "HttpEntity.h"

#include "HttpClient.h"
#include "ServiceRegistry.h"
#include "Util.h"

HttpEntity::HttpEntity(QString playUrl, const std::shared_ptr<ServiceRegistry> &registry)
    : PlaylistEntity(std::move(playUrl), registry) {
    Q_ASSERT(registry != nullptr);

    // 显示名（N35 §2.2）：取末段并 percent-decode 后 setFilename（基类 deriveNames
    // 用 QFileInfo 取末段、不解码，否则列表出现 "%20"、"%E4%B8%AD" 之类）。逻辑自
    // N37 起收编进 Util::playUrlFileName（标题 / OSD 取名共用同一出口）；解后为空
    // （如以 / 结尾）回退基类行为（title 已落 playUrl）。
    // 构造参数名遮蔽基类同名访问器，取 playUrl 须显式限定
    const QString decoded = Util::playUrlFileName(PlaylistEntity::playUrl());
    if (!decoded.isEmpty())
        setFilename(decoded);
}

// 文件级元数据回退路（N35 §2.4 经修订 2）：未注入时向 registry 现查 client，
// exec 阻塞求值（N34 文件头："无取消需求的调用点用 exec"——仅任务线程调用）。
// 取消语义为结果静默丢弃：调用方不等在途项、不采用其结果，故无取消通道、
// 体内零轮询。失败或超时返回空 FileMeta 入缓存不再重试。缺陷短路由 exec
// 内部处理（N34），调用方不持缺陷表知识。
FileMeta HttpEntity::computeFileMeta() const {
    // 装配错误：启动期必已 provide（N35 §2.3），永不命中；断言守候 + fail-safe
    const std::shared_ptr<HttpClient> client =
        registry() ? registry()->resolve<HttpClient>() : nullptr;
    if (!client) {
        Q_ASSERT(false);
        return {};
    }

    HttpClient::Request req;
    req.url = QUrl(playUrl()); // 绝对 URL：与 baseUrl 解绑，注册表 client 不绑 host
    req.method = QByteArrayLiteral("HEAD");
    req.headersOnly = true;

    const HttpClient::Response resp = client->exec(req);

    FileMeta meta;
    if (!resp.canceled && resp.status >= 200 && resp.status < 300) {
        const QByteArray len = resp.headers.value(QByteArrayLiteral("content-length"));
        if (!len.isEmpty()) {
            bool ok = false;
            const qint64 sz = len.toLongLong(&ok);
            if (ok && sz >= 0)
                meta.size = sz;
        }
        const QByteArray lm = resp.headers.value(QByteArrayLiteral("last-modified"));
        if (!lm.isEmpty()) {
            // 解析失败返回 invalid：不置 optional（等价于头缺失，按"值未知"处理）。
            // N37：解析函数迁入 Util（WebDAV 的 getlastmodified 共用），行为不变
            const QDateTime modified = Util::parseHttpDate(lm);
            if (modified.isValid())
                meta.modified = modified;
        }
    }
    return meta;
}

// 恒不被调（supportsMediaMeta 为假）
PlaylistEntity::MediaMeta HttpEntity::computeMediaMeta() const { return {}; }

// 通用文件图标（与 UnknownEntity 同形，N35 §2.2）；仅 GUI 线程调用。
// provider 无状态，函数内 static 复用一次构造
QIcon HttpEntity::computeIcon() const {
    static QFileIconProvider provider;
    return provider.icon(QFileIconProvider::File);
}

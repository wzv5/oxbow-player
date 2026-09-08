#include "pch.h"

#include "LocalFileEntity.h"

#include "MediaInfoHelper.h"

// 不存在或不可读：三字段全空（"值未知"），不抛错。
// N29：口径抽为静态 fileMetaFrom，惰性求值与提供者注入共用一份
FileMeta LocalFileEntity::fileMetaFrom(const QFileInfo &fi) {
    FileMeta meta;
    if (!fi.exists() || !fi.isReadable())
        return meta;
    meta.size = qMax<qint64>(0, fi.size());
    meta.created = fi.birthTime();
    meta.modified = fi.lastModified();
    return meta;
}

FileMeta LocalFileEntity::computeFileMeta() const { return fileMetaFrom(QFileInfo(playUrl())); }

// MediaInfoHelper::parse 打开失败返回默认 Data（字段皆空/0），
// 失败结果同样入实体缓存，防同一坏文件反复解析
PlaylistEntity::MediaMeta LocalFileEntity::computeMediaMeta() const {
    return MediaInfoHelper::parse(playUrl());
}

// 文件不存在 / 失败取通用图标；仅 GUI 线程调用。
// provider 无状态，函数内 static 复用一次构造（千行目录免千次构造）
QIcon LocalFileEntity::computeIcon() const {
    static QFileIconProvider provider;
    const QFileInfo fi(playUrl());
    if (!fi.exists())
        return provider.icon(QFileIconProvider::File);
    return provider.icon(fi);
}

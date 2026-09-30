#include "pch.h"

#include "JellyfinEntity.h"

// 通用文件图标（与 HttpEntity / UnknownEntity 同形）；仅 GUI 线程调用。
// 无封面展示位（N69 非目标：/Images/Primary 不接入），通用图标即现状口径
QIcon JellyfinEntity::computeIcon() const {
    static QFileIconProvider provider;
    return provider.icon(QFileIconProvider::File);
}

#include "Version.h"

// 全仓唯一 include 生成头的翻译单元（N84 §1）：revision.h 由构建期脚本
// cmake/OxbowRevision.cmake 写入 <build>/generated/，勿手改。
#include "revision.h"

namespace App {

QString versionInfo() {
    return QStringLiteral("%1 (%2)").arg(QStringLiteral(OXBOW_VERSION_STRING),
                                         QStringLiteral(OXBOW_REVISION_STRING));
}

QString buildDateTime() { return QStringLiteral(OXBOW_BUILD_TIME); }

QString qtVersion() { return QString::fromLatin1(qVersion()); }

} // namespace App

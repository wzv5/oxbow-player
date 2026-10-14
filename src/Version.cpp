#include "Version.h"

// 全仓唯一 include 生成头的翻译单元（N84 §1）：revision.h 由构建期脚本
// cmake/OxbowRevision.cmake 写入 <build>/generated/，勿手改。
// 单元测试库形态（OXBOW_BUILDING_TESTS）不吃生成头：OXBOW_BUILD_TIME 每轮必变，
// 吃了它测试库就逐轮重编、全部测试 exe 逐轮重链。三宏回落内置值，取值不被任何
// 测试断言观测；测试如需断言版本串，断的是这里的回退值。
#ifdef OXBOW_BUILDING_TESTS
#define OXBOW_VERSION_STRING "oxbow-test"
#define OXBOW_REVISION_STRING "test"
#define OXBOW_BUILD_TIME "1970-01-01T00:00:00Z"
#else
#include "revision.h"
#endif

namespace App {

QString versionInfo() {
    return QStringLiteral("%1 (%2)").arg(QStringLiteral(OXBOW_VERSION_STRING),
                                         QStringLiteral(OXBOW_REVISION_STRING));
}

QString buildDateTime() { return QStringLiteral(OXBOW_BUILD_TIME); }

QString qtVersion() { return QString::fromLatin1(qVersion()); }

} // namespace App

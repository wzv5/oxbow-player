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

QDateTime buildDateTime() {
    return QDateTime::fromString(QStringLiteral(OXBOW_BUILD_TIME), Qt::ISODate);
}

QString qtVersion() { return QString::fromLatin1(qVersion()); }

namespace {
QString s_mpvVersion; // N102：MpvController 构造期填充一次，GUI 线程读写
} // namespace

void setMpvVersion(const QString &v) {
    // mpv-version 属性实测形如 "mpv v0.41.0-dev-g41f6a6450"：入库剥去宿主名前缀
    // 与版本 tag 的 `v` 引导（只在后接数字时剥，避免吃掉非版本串的首字母），只留
    // 版本段，取值侧零处理
    QString s = v;
    if (s.startsWith(QLatin1String("mpv ")))
        s.remove(0, 4);
    if (s.size() > 1 && s.at(0) == QLatin1Char('v') && s.at(1).isDigit())
        s.remove(0, 1);
    s_mpvVersion = std::move(s);
}

QString mpvVersion() { return s_mpvVersion; }

} // namespace App

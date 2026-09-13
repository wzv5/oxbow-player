// DirProvider 单元测试（N36）：递归深度上限（修订 2：0 = 不递归、n = 最多深入
// n 层子目录）与符号链接环 / 同一目录重复访问的剪枝（修订 3）。
// 目录树全部在 QTemporaryDir 内现场搭建（含指向祖先的链接），枚举结果按
// playUrl 集合断言——无素材依赖、无网络、无 mpv，GUILESS 环境可直接跑。
// 环用两种链接各建一遍，环境限制（建不出来 / 未被 QDir 视为目录）一律 QSKIP：
//   · 目录符号链接 —— mklink /D（需开发者模式或管理员，且卷须支持）。不用
//     QFile::link：它在 Windows 上建的是 .lnk 快捷方式，isDir() 为 false，
//     环场景根本不成立（实测 Qt 6.11.2 / Windows 11）
//   · junction —— mklink /J，普通用户即可，Windows 上最常见的目录链接形态
// junction 用例是防御的硬探针：Qt 的 canonicalFilePath() 不解析 junction，去重键
// 必须先解开 junction 链（见 DirProvider::canonicalDirPath），否则环会被当成普通
// 目录逐层展开——表现为结果重复收录（曾实测出 41 条，应为 2 条）。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R DirProvider 跑本套件；
//   mpvqt_tests_DirProvider symlinkLoop* 只跑环相关。

#include "pch.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <utility>

#include "Constants.h"
#include "DirProvider.h"
#include "ServiceRegistry.h"
#include "TraversalPolicy.h"

namespace {
// 建目录成功返回路径，失败返回空串（用例自行 QVERIFY）
QString makeDir(const QString &parent, const QString &name) {
    const QString path = QDir(parent).filePath(name);
    return QDir().mkpath(path) ? path : QString();
}

// 建一个媒体文件（内容无关，扩展名命中过滤即可）
QString makeMedia(const QString &dir, const QString &name) {
    const QString path = QDir(dir).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(QByteArray(16, 'x'));
    f.close();
    return path;
}

// 五级目录树：a 在根，b/c/d/e 依次位于第 1/2/3/4 级子目录。
// 深度用例共用——"上限 n 收录到第几层"照此树逐层比对
struct Tree {
    QString a, b, c, d, e; // 任一项为空即建树失败
    QString sub;           // 第 1 级子目录（环用例在此挂链接）
};

Tree buildTree(const QString &root) {
    Tree t;
    t.a = makeMedia(root, QStringLiteral("a.mp4"));
    t.sub = makeDir(root, QStringLiteral("sub"));
    t.b = makeMedia(t.sub, QStringLiteral("b.mp4"));
    const QString lvl2 = makeDir(t.sub, QStringLiteral("lvl2"));
    t.c = makeMedia(lvl2, QStringLiteral("c.mp4"));
    const QString lvl3 = makeDir(lvl2, QStringLiteral("lvl3"));
    t.d = makeMedia(lvl3, QStringLiteral("d.mp4"));
    const QString lvl4 = makeDir(lvl3, QStringLiteral("lvl4"));
    t.e = makeMedia(lvl4, QStringLiteral("e.mp4"));
    return t;
}

// 带指定深度上限的注册表：resolve 侧取值唯一来源（缺省走常量，故显式装配）
// 真目录符号链接：经 cmd 内建的 mklink /D（需开发者模式或管理员，且卷须支持）。
// 不用 QFile::link——它在 Windows 上建的是 .lnk 快捷方式而非符号链接，建出来
// isDir() 为 false，环场景根本不成立（实测于 Qt 6.11.2 / Windows 11）
bool makeDirSymlink(const QString &target, const QString &linkPath) {
#ifdef Q_OS_WIN
    QProcess p;
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    p.start(QStringLiteral("cmd"),
            {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/D"),
             QDir::toNativeSeparators(linkPath), QDir::toNativeSeparators(target)});
    if (!p.waitForFinished(30000))
        return false;
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
#else
    return QFile::link(target, linkPath); // Unix：QFile::link 即符号链接
#endif
}

#ifdef Q_OS_WIN
// Windows junction（NTFS mount point）：mklink /J 建的目录链接，普通用户即可创建
// ——无需 SeCreateSymbolicLinkPrivilege，是 Windows 上最常见的目录链接形态。
// Qt 无创建 junction 的 API（QFile::link 建的是 .lnk 快捷方式），故经 cmd 内建的
// mklink；输出丢向空设备，只取退出码。junction 仅存在于 Windows，整段随之条件编译
bool makeJunction(const QString &target, const QString &linkPath) {
    QProcess p;
    p.setStandardOutputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    p.start(QStringLiteral("cmd"),
            {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
             QDir::toNativeSeparators(linkPath), QDir::toNativeSeparators(target)});
    if (!p.waitForFinished(30000))
        return false;
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}
#endif // Q_OS_WIN

std::shared_ptr<ServiceRegistry> registryWithDepth(int depth) {
    auto registry = std::make_shared<ServiceRegistry>();
    auto policy = std::make_shared<TraversalPolicy>();
    policy->depthLimit = depth;
    registry->provide<TraversalPolicy>(std::move(policy));
    return registry;
}

// 枚举并抽出 playUrl（统一分隔符后升序）——集合语义，便于逐层比对。
// N37：enumerate 返回类型改为 Enumeration，取 entities 字段（warnings 非本套件关注点）
std::expected<QStringList, QString> enumerateUrls(const QString &root, int depth) {
    // 递归开、扩展名只认 mp4：与本轮被测的遍历口径对齐
    DirProvider provider(root, true, {QStringLiteral("mp4")}, registryWithDepth(depth));
    const auto result = provider.enumerate();
    if (!result.has_value())
        return std::unexpected(result.error());
    QStringList urls;
    urls.reserve(int(result->entities.size()));
    for (const auto &e : result->entities)
        urls << QDir::fromNativeSeparators(e->playUrl()); // 加工后为原生分隔符
    urls.sort();
    return urls;
}

// 期望值同款归一：playUrl 经加工为原生分隔符，比较前统一
QString norm(const QString &path) { return QDir::fromNativeSeparators(path); }
} // namespace

class TestDirProvider : public QObject {
    Q_OBJECT

  private slots:
    // N36 修订 3：自指符号链接（root/loop -> root）——环被剪枝，各文件恰好一次
    void symlinkLoopToRootIsPruned();
    // N36 修订 3：指向祖先的符号链接（root/sub/back -> root/sub）——同一目录
    // 只访问一次，其内文件不重复收录
    void symlinkToAncestorIsVisitedOnce();
#ifdef Q_OS_WIN
    // N36 修订 3：同样两个环，改用 Windows junction 构造（mklink /J，无需特权，
    // 也是 Windows 上最常见的目录链接形态）——顺带实测 Qt 的 canonicalFilePath
    // 是否解析 junction：不解析则本用例会表现为重复收录
    void junctionLoopToRootIsPruned();
    void junctionToAncestorIsVisitedOnce();
#endif // Q_OS_WIN
    // N36 修订 3：无符号链接的普通树不受 visited 影响（等价性）
    void plainTreeVisitedOnceEach();
    // N36 修订 2：0 = 不递归，只收根内媒体文件
    void depthZeroSkipsSubdirectories();
    // N36 修订 2：上限 1 = 只深入一层子目录
    void depthOneStopsAfterFirstLevel();
    // N36 修订 2：默认 3 = 第三级子目录仍收录，第四级不出现
    void defaultDepthCollectsThreeLevels();
    // N37：不可读子目录由"整次失败"改为"枚举成功 + 警告"（失败面收缩为
    // 根未绑定 / 不存在 / 不可读三项）。Windows 下 Qt 默认不查 NTFS ACL
    // （qt_ntfs_permission_lookup 默认关闭），QDir::isReadable() 恒为真——运行时
    // 同样识别不出被拒目录（它被当成空目录静默处理），非仅"测试造不出来"；
    // 已知不修，见主仓 AGENTS.md 持久陷阱。该用例仅在可 chmod 的平台执行
    void unreadableSubdirectoryYieldsWarning();
};

void TestDirProvider::symlinkLoopToRootIsPruned() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    const QString a = makeMedia(root, QStringLiteral("a.mp4"));
    const QString sub = makeDir(root, QStringLiteral("sub"));
    const QString b = makeMedia(sub, QStringLiteral("b.mp4"));
    QVERIFY(!a.isEmpty());
    QVERIFY(!sub.isEmpty());
    QVERIFY(!b.isEmpty());

    // 自指链接：无剪枝时会在深度上限内反复展开（每层都再收录一次 a / b）
    const QString loop = QDir(root).filePath(QStringLiteral("loop"));
    if (!makeDirSymlink(root, loop))
        QSKIP("创建目录符号链接失败（Windows 需开发者模式或管理员，且卷须支持符号链接）");
    if (!QFileInfo(loop).isDir())
        QSKIP("符号链接未被 QDir 视为目录，环场景不成立");

    // 深度给到取值域上界：环有充分展开空间，防不住就是重复收录
    auto result = enumerateUrls(root, App::recursiveDepthLimitMax());
    // 失败文案只在失败路径读取：expected 有值时调 error() 无意义
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(*result, QStringList({norm(a), norm(b)}));
    // 先删链接本身：留着会让 QTemporaryDir 的递归清理失败并留下垃圾目录
    QDir().rmdir(loop);
}

void TestDirProvider::symlinkToAncestorIsVisitedOnce() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    const QString a = makeMedia(root, QStringLiteral("a.mp4"));
    const QString sub = makeDir(root, QStringLiteral("sub"));
    const QString b = makeMedia(sub, QStringLiteral("b.mp4"));
    QVERIFY(!a.isEmpty());
    QVERIFY(!sub.isEmpty());
    QVERIFY(!b.isEmpty());

    // 指向自身所在目录：canonical 与 sub 相同，第二次到达即被剪枝
    const QString back = QDir(sub).filePath(QStringLiteral("back"));
    if (!makeDirSymlink(sub, back))
        QSKIP("创建目录符号链接失败（Windows 需开发者模式或管理员，且卷须支持符号链接）");
    if (!QFileInfo(back).isDir())
        QSKIP("符号链接未被 QDir 视为目录，环场景不成立");

    auto result = enumerateUrls(root, App::recursiveDepthLimitMax());
    // 失败文案只在失败路径读取：expected 有值时调 error() 无意义
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(*result, QStringList({norm(a), norm(b)}));
    QDir().rmdir(back); // 同前：先摘链接再让临时目录清理
}

#ifdef Q_OS_WIN
void TestDirProvider::junctionLoopToRootIsPruned() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    const QString a = makeMedia(root, QStringLiteral("a.mp4"));
    const QString sub = makeDir(root, QStringLiteral("sub"));
    const QString b = makeMedia(sub, QStringLiteral("b.mp4"));
    QVERIFY(!a.isEmpty());
    QVERIFY(!sub.isEmpty());
    QVERIFY(!b.isEmpty());

    // 自指 junction：root/loop -> root（mklink /J，无需特权）
    const QString loop = QDir(root).filePath(QStringLiteral("loop"));
    if (!makeJunction(root, loop))
        QSKIP("创建 junction 失败（卷非 NTFS 或 mklink 不可用）");
    const QFileInfo loopInfo(loop);
    if (!loopInfo.isDir())
        QSKIP("junction 未被 QDir 视为目录，环场景不成立");
    QVERIFY(loopInfo.isJunction()); // 实测链接类型，便于失败时定位

    auto result = enumerateUrls(root, App::recursiveDepthLimitMax());
    // 失败文案只在失败路径读取：expected 有值时调 error() 无意义
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(*result, QStringList({norm(a), norm(b)}));
    QDir().rmdir(loop); // 先摘链接：否则 QTemporaryDir 递归清理失败并留下垃圾
}

void TestDirProvider::junctionToAncestorIsVisitedOnce() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    const QString a = makeMedia(root, QStringLiteral("a.mp4"));
    const QString sub = makeDir(root, QStringLiteral("sub"));
    const QString b = makeMedia(sub, QStringLiteral("b.mp4"));
    QVERIFY(!a.isEmpty());
    QVERIFY(!sub.isEmpty());
    QVERIFY(!b.isEmpty());

    // root/sub/back -> root/sub：canonical 与 sub 相同，第二次到达即被剪枝
    const QString back = QDir(sub).filePath(QStringLiteral("back"));
    if (!makeJunction(sub, back))
        QSKIP("创建 junction 失败（卷非 NTFS 或 mklink 不可用）");
    const QFileInfo backInfo(back);
    if (!backInfo.isDir())
        QSKIP("junction 未被 QDir 视为目录，环场景不成立");
    QVERIFY(backInfo.isJunction());

    auto result = enumerateUrls(root, App::recursiveDepthLimitMax());
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(*result, QStringList({norm(a), norm(b)}));
    QDir().rmdir(back); // 同前：先摘链接再让临时目录清理
}
#endif // Q_OS_WIN

void TestDirProvider::plainTreeVisitedOnceEach() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const Tree t = buildTree(tmp.path());
    QVERIFY(!t.a.isEmpty());
    QVERIFY(!t.b.isEmpty());
    QVERIFY(!t.c.isEmpty());
    QVERIFY(!t.d.isEmpty());
    QVERIFY(!t.e.isEmpty());

    // 无符号链接：每个目录的 canonical 路径互不相同，visited 只增不命中——
    // 深度给足时全树收录且每个文件恰好一次（加防御前后行为一致）
    auto result = enumerateUrls(tmp.path(), App::recursiveDepthLimitMax());
    // 失败文案只在失败路径读取：expected 有值时调 error() 无意义
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(*result, QStringList({norm(t.a), norm(t.b), norm(t.c), norm(t.d), norm(t.e)}));
}

void TestDirProvider::depthZeroSkipsSubdirectories() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const Tree t = buildTree(tmp.path());
    QVERIFY(!t.a.isEmpty());
    QVERIFY(!t.b.isEmpty());

    // 0 = 不递归（修订 2 起不再表示"不限"）：根内文件收录，子目录一概不进
    auto result = enumerateUrls(tmp.path(), 0);
    // 失败文案只在失败路径读取：expected 有值时调 error() 无意义
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(*result, QStringList({norm(t.a)}));
}

void TestDirProvider::depthOneStopsAfterFirstLevel() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const Tree t = buildTree(tmp.path());
    QVERIFY(!t.a.isEmpty());
    QVERIFY(!t.b.isEmpty());
    QVERIFY(!t.c.isEmpty());

    // 上限 1：root/sub 这一层收录，再深一层（lvl2）不进
    auto result = enumerateUrls(tmp.path(), 1);
    // 失败文案只在失败路径读取：expected 有值时调 error() 无意义
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(*result, QStringList({norm(t.a), norm(t.b)}));
}

void TestDirProvider::defaultDepthCollectsThreeLevels() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const Tree t = buildTree(tmp.path());
    QVERIFY(!t.a.isEmpty());
    QVERIFY(!t.b.isEmpty());
    QVERIFY(!t.c.isEmpty());
    QVERIFY(!t.d.isEmpty());
    QVERIFY(!t.e.isEmpty());

    // 默认 3：第三级子目录（root/sub/lvl2/lvl3）内的 d 仍收录，
    // 第四级（lvl4）内的 e 不出现
    QCOMPARE(App::recursiveDepthLimitDefault(), 3);
    auto result = enumerateUrls(tmp.path(), App::recursiveDepthLimitDefault());
    // 失败文案只在失败路径读取：expected 有值时调 error() 无意义
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(*result, QStringList({norm(t.a), norm(t.b), norm(t.c), norm(t.d)}));
}

void TestDirProvider::unreadableSubdirectoryYieldsWarning() {
#ifdef Q_OS_WIN
    QSKIP("Windows 下 Qt 默认不查 NTFS ACL，QDir::isReadable() 恒为真，运行时同样"
          "识别不出被拒目录（已知不修，见 AGENTS.md）；失败面收缩的语义仅在此平台钉死");
#else
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.path();
    const QString a = makeMedia(root, QStringLiteral("a.mp4"));
    const QString sub = makeDir(root, QStringLiteral("sub"));
    const QString b = makeMedia(sub, QStringLiteral("b.mp4"));
    QVERIFY(!a.isEmpty());
    QVERIFY(!sub.isEmpty());
    QVERIFY(!b.isEmpty());

    // 剥夺子目录全部权限位；守卫确认确实不可读（运行环境可能忽略权限位）
    QFile::setPermissions(sub, QFile::Permissions());
    if (QDir(sub).isReadable())
        QSKIP("权限剥夺未生效，无法构造不可读子目录场景");

    DirProvider provider(root, true, {QStringLiteral("mp4")},
                         registryWithDepth(App::recursiveDepthLimitMax()));
    const auto result = provider.enumerate();
    // N37：不再整次失败——根内文件照常收录，子目录失败随 warnings 回传
    if (!result.has_value())
        QFAIL(qPrintable(result.error()));
    QCOMPARE(int(result->entities.size()), 1);
    QCOMPARE(result->entities.front()->playUrl(), a);
    QVERIFY(!result->warnings.isEmpty());

    // 恢复权限位，让 QTemporaryDir 的递归清理得以完成
    QFile::setPermissions(sub, QFile::Permissions(QFile::ReadOwner | QFile::WriteOwner |
                                                  QFile::ExeOwner | QFile::ReadUser |
                                                  QFile::WriteUser | QFile::ExeUser));
#endif
}

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN），无 GUI 依赖
QTEST_GUILESS_MAIN(TestDirProvider)

#include "tst_DirProvider.moc"

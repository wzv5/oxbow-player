// InputConf 单元测试（N89）：input.conf 保真读写的边界 case 覆盖。
// 覆盖清单（验收 23）：文件不存在、同名重复行、行首 # 认领与不认领、
// CRLF / BOM / 无末尾换行、写入失败、行级增删改后的保真回写。
// 不含同键分组（归对话框，不进测试库）。测试无副作用：QTemporaryFile 隔离路径。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R InputConf 跑本套件；
//   oxbow_tests_InputConf <槽名> 跑单个用例。

#include "pch.h"

#include <QtTest/QtTest>

#include "InputConf.h"
#include "MpvKeyName.h"

class TestInputConf : public QObject {
    Q_OBJECT

  private slots:
    // 文件不存在 → open 返回空实例 + commit 写出首行
    void openMissingFileCreatesEmptyInstance();
    // 未编辑行逐字节回写（注释 / 空行 / 缩进 / 行尾空白 / 中间空白）
    void untouchedLinesRoundTripByteExact();
    // 行首 # 认领：#f ignore、#  x cycle pause 等不规整形态命中，字段还原正确
    void claimedDisabledLines();
    // 不认领：剥完仍是 # 开头、解不出键名、两类 Directive 的注释形态
    void unclaimedCommentLines();
    // Directive 两类（default-bindings start / <键> {区名} <命令>）：不进 KeyValue、
    // 原样回写；default-bindings stop 按解不出的行显示（变更记录 3）
    void directiveLinesKeptAsIs();
    // CRLF 保真：未编辑行、禁用插桩行、重组行与新增行都保住 \r，不混形
    void crlfPreserved();
    // BOM 解析时跳过；serialize 不写 BOM
    void bomStrippedOnRead();
    // 无末尾换行：原样再现；追加行终止正常、删掉追加行即回到原形态（净零不留痕迹）
    void missingTrailingNewlineHandled();
    // 存在但读不进（指向目录）→ open 交 nullopt，绝不当空模型用
    void unreadableTargetYieldsNullopt();
    // 同名重复行：各成一行，禁用只作用于目标行
    void duplicateKeyLinesIndependent();
    // 禁用 / 启用严格互逆：缩进还原、不规整前缀剥离
    void disableEnableRoundTrip();
    // 行级改写只重组被编辑行，其余逐字节不动；value 按命令串整体处理
    void fieldEditReassemblesOnlyEditedLine();
    // appendLine 落在文件末尾、commit 原子写出生成合法文件
    void appendLineWritesAtEnd();
    // 文件只读 → commit 失败（Windows 置只读）
    void readOnlyFileCommitFails();
};

// 工具：把内容写入临时文件并返回路径（UTF-8）
static QString writeTempFile(const QString &content) {
    QTemporaryFile *tf = new QTemporaryFile(QDir::tempPath() + "/input_conf_XXXXXX.conf");
    tf->setAutoRemove(true);
    if (!tf->open()) {
        delete tf;
        return {};
    }
    tf->write(content.toUtf8());
    const QString path = tf->fileName();
    tf->close();
    tf->setAutoRemove(false); // 让文件存续到调用方读完
    delete tf;
    return path;
}

static QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// 键名合法性判定器（变更记录 2）：与生产同源——经 MpvKeyName 反解认合法
// （MpvKeyName 纯 Qt 实现，测试库直链，不用替身：替身会在 "# regular comment"
// 这类散文注释上误判成合法键名，实测教训）
static bool keyParseable(const QString &key) { return !MpvKey::sequenceFromName(key).isEmpty(); }

void TestInputConf::openMissingFileCreatesEmptyInstance() {
    const QString path = QDir::tempPath() + "/input_conf_should_not_exist_xyz.conf";
    QFile::remove(path);

    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    QVERIFY(c->lines().isEmpty());
    QCOMPARE(c->appendLine(QStringLiteral("f"), QStringLiteral("cycle fullscreen"), QString()), 0);
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), QStringLiteral("f cycle fullscreen\n"));
}

void TestInputConf::untouchedLinesRoundTripByteExact() {
    const QString original = "# 顶部说明\n"
                             "\n"
                             "   # 缩进注释\n"
                             "f cycle fullscreen   \n"     // 行尾空白保真
                             "v  cycle   sub-visibility\n" // 中间空白保真
                             "#default-bindings start\n"
                             "\t\n"; // 空白行
    const QString path = writeTempFile(original);

    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    QVERIFY(c->commit()); // 不做任何编辑
    QCOMPARE(readAll(path), original);
}

void TestInputConf::claimedDisabledLines() {
    // #f ignore、#  x cycle pause（不规整形态）都命中认领；字段还原正确
    const QString content = QStringLiteral("#f ignore\n"
                                           "#  x cycle pause\n"
                                           "# regular comment\n");
    const QString path = writeTempFile(content);
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    const QList<InputConfLine> &lines = c->lines();
    QCOMPARE(lines.size(), 3);
    QVERIFY(lines[0].kind == InputConfLine::Kind::KeyValue && lines[0].disabled);
    QCOMPARE(lines[0].key, QStringLiteral("f"));
    QCOMPARE(lines[0].command, QStringLiteral("ignore"));
    QVERIFY(lines[1].kind == InputConfLine::Kind::KeyValue && lines[1].disabled);
    QCOMPARE(lines[1].indent, QString());
    QCOMPARE(lines[1].key, QStringLiteral("x"));
    QCOMPARE(lines[1].command, QStringLiteral("cycle pause"));
    QVERIFY(lines[2].kind == InputConfLine::Kind::Comment); // 不认领
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), content); // 未编辑逐字节回写
}

void TestInputConf::unclaimedCommentLines() {
    // 剥完仍是 # 开头、解不出键名、两类 Directive 的注释形态 → 纯注释保留
    const QString content = QStringLiteral("##f ignore\n"
                                           "# 说明 f ignore\n"
                                           "#default-bindings start\n"
                                           "#{encode} quit\n"
                                           "# onlykey\n");
    const QString path = writeTempFile(content);
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    const QList<InputConfLine> &lines = c->lines();
    QCOMPARE(lines.size(), 5);
    for (const InputConfLine &l : lines)
        QVERIFY2(l.kind == InputConfLine::Kind::Comment, qPrintable(l.raw));
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), content);
}

void TestInputConf::directiveLinesKeptAsIs() {
    const QString content = QStringLiteral("default-bindings start\n"
                                           "f {encode} quit\n"
                                           "f cycle fullscreen\n"
                                           "default-bindings stop\n");
    const QString path = writeTempFile(content);
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    const QList<InputConfLine> &lines = c->lines();
    QCOMPARE(lines.size(), 4);
    QVERIFY(lines[0].kind == InputConfLine::Kind::Directive);
    QVERIFY(lines[1].kind == InputConfLine::Kind::Directive);
    QVERIFY(lines[2].kind == InputConfLine::Kind::KeyValue);
    QCOMPARE(lines[2].key, QStringLiteral("f"));
    QCOMPARE(lines[2].command, QStringLiteral("cycle fullscreen"));
    // default-bindings stop 不存在该语法（变更记录 3）：按解不出的行显示
    QVERIFY(lines[3].kind == InputConfLine::Kind::KeyValue);
    QCOMPARE(lines[3].key, QStringLiteral("default-bindings"));
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), content); // 原样保留、不吞不改写
}

void TestInputConf::crlfPreserved() {
    const QString path = QDir::tempPath() + "/input_conf_crlf.conf";
    QFile::remove(path);
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("# header\r\nf cycle fullscreen\r\nv cycle sub-visibility\r\n");
    }
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    QVERIFY(c->lines().at(1).raw.endsWith(QLatin1Char('\r'))); // \r 留在 raw 里
    c->disableLine(1); // 缩进后插 "# "，其后正文一字节不改（含 \r）
    QVERIFY(c->commit());
    QCOMPARE(readAll(path),
             QStringLiteral("# header\r\n# f cycle fullscreen\r\nv cycle sub-visibility\r\n"));
    c->enableLine(1); // 启用还原
    QVERIFY(c->commit());
    QCOMPARE(readAll(path),
             QStringLiteral("# header\r\nf cycle fullscreen\r\nv cycle sub-visibility\r\n"));
    // 被编辑的行按字段重组时，该行的 \r 必须带走——否则 CRLF 文件被改成混形换行
    c->setLineFields(2, QStringLiteral("V"), QStringLiteral("cycle sub-visibility"), QString());
    QVERIFY(c->commit());
    QCOMPARE(readAll(path),
             QStringLiteral("# header\r\nf cycle fullscreen\r\nV cycle sub-visibility\r\n"));
    // 新增行随文件既有形态（CRLF）
    c->appendLine(QStringLiteral("x"), QStringLiteral("ignore"), QString());
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), QStringLiteral("# header\r\nf cycle fullscreen\r\n"
                                           "V cycle sub-visibility\r\nx ignore\r\n"));
}

void TestInputConf::unreadableTargetYieldsNullopt() {
    // 目录存在却读不进内容：open 必须交 nullopt，调用方据此锁死编辑——若当空模型用，
    // 一次 commit 就拿新内容去盖一份没能读进内存的目标
    const auto c = InputConf::open(QDir::tempPath(), keyParseable);
    QVERIFY(!c.has_value());
}

void TestInputConf::bomStrippedOnRead() {
    const QString path = QDir::tempPath() + "/input_conf_bom.conf";
    QFile::remove(path);
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const char bom[] = {'\xEF', '\xBB', '\xBF'};
        f.write(bom, 3);
        f.write("f cycle fullscreen\n");
    }
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    QCOMPARE(c->lines().size(), 1);
    QCOMPARE(c->lines().at(0).key, QStringLiteral("f")); // BOM 未污染首行键名
    QVERIFY(c->commit());
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray data = f.readAll();
    QVERIFY(!data.startsWith('\xEF'));
    QCOMPARE(data, QByteArray("f cycle fullscreen\n"));
}

void TestInputConf::missingTrailingNewlineHandled() {
    const QString content = QStringLiteral("f cycle fullscreen\nv cycle sub-visibility");
    const QString path = writeTempFile(content);
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    QVERIFY(c->lines().size() == 2);
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), content); // 无末尾换行原样再现，不追加

    // 追加行：原末行的换行符由 serialize 按位置现算（他行字节不动），新末行正常终止
    QCOMPARE(c->appendLine(QStringLiteral("x"), QStringLiteral("ignore"), QString()), 2);
    QVERIFY(c->commit());
    QCOMPARE(readAll(path),
             QStringLiteral("f cycle fullscreen\nv cycle sub-visibility\nx ignore\n"));

    // 删掉刚追加的行即回到原文件形态——增删往返是净零操作，不得留下多余的末尾换行
    c->removeLine(2);
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), content);
}

void TestInputConf::duplicateKeyLinesIndependent() {
    const QString content = QStringLiteral("f cycle fullscreen\nf ignore\n");
    const QString path = writeTempFile(content);
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    QCOMPARE(c->lines().size(), 2);
    QCOMPARE(c->lines().at(0).key, QStringLiteral("f"));
    QCOMPARE(c->lines().at(1).key, QStringLiteral("f"));
    c->disableLine(1); // 只作用于目标行
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), QStringLiteral("f cycle fullscreen\n# f ignore\n"));
    // 两行各自成行：数据层不做同键合并（归并是对话框视图层的事）
}

void TestInputConf::disableEnableRoundTrip() {
    // 带缩进的一行：禁用写成缩进后 "# "，启用后缩进原样回来
    const QString content = QStringLiteral("    f cycle volume\n");
    const QString path = writeTempFile(content);
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    QCOMPARE(c->lines().at(0).indent, QStringLiteral("    "));
    c->disableLine(0);
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), QStringLiteral("    # f cycle volume\n"));
    c->enableLine(0);
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), content); // 互逆：逐字节还原

    // 不规整被注释形态启用：删掉认领时实际剥掉的那一段（"#"+其后一段连续空白）
    const QString path2 = writeTempFile(QStringLiteral("#f ignore\n"));
    auto c2 = InputConf::open(path2, keyParseable);
    QVERIFY(c2.has_value());
    QVERIFY(c2->lines().at(0).disabled);
    c2->enableLine(0);
    QVERIFY(c2->commit());
    QCOMPARE(readAll(path2), QStringLiteral("f ignore\n"));
}

void TestInputConf::fieldEditReassemblesOnlyEditedLine() {
    const QString content = QStringLiteral("# head\n"
                                           "f  cycle   fullscreen\n" // 中间空白：未编辑行保真
                                           "# tail\n");
    const QString path = writeTempFile(content);
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    // value 是命令串整体：多命令（; 连接）不得按首个 token 截断
    QCOMPARE(c->lines().at(1).command, QStringLiteral("cycle   fullscreen"));
    c->setLineFields(1, QStringLiteral("f"),
                     QStringLiteral("cycle fullscreen ; show-text \"hi # x\""),
                     QStringLiteral("my note")); // 引号内的 # 不起注释
    QVERIFY(c->commit());
    // 只有被编辑的行被重组（规范化单空格），其余行逐字节不动
    QCOMPARE(readAll(path), QStringLiteral("# head\n"
                                           "f cycle fullscreen ; show-text \"hi # x\" # my note\n"
                                           "# tail\n"));
    // 引号内的 # 原文保留在命令里
    auto c2 = InputConf::open(path, keyParseable);
    QVERIFY(c2.has_value());
    QCOMPARE(c2->lines().at(1).command, QStringLiteral("cycle fullscreen ; show-text \"hi # x\""));
    QCOMPARE(c2->lines().at(1).comment, QStringLiteral("my note"));
}

void TestInputConf::appendLineWritesAtEnd() {
    const QString content = QStringLiteral("# head\nf cycle fullscreen\n\n# tail\n");
    const QString path = writeTempFile(content);
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    QCOMPARE(c->appendLine(QStringLiteral("v"), QStringLiteral("cycle sub-visibility"), QString()),
             4); // 追加到全部既有行（含空行与尾注释）之后
    QVERIFY(c->commit());
    QCOMPARE(readAll(path),
             QStringLiteral("# head\nf cycle fullscreen\n\n# tail\nv cycle sub-visibility\n"));
    // 写盘原子性由 QSaveFile 保证：读回能重新解析
    auto c2 = InputConf::open(path, keyParseable);
    QVERIFY(c2.has_value());
    QCOMPARE(c2->lines().size(), 5);
}

void TestInputConf::readOnlyFileCommitFails() {
#ifdef Q_OS_WIN
    const QString path = writeTempFile(QStringLiteral("f cycle fullscreen\n"));
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::ReadUser));
    auto c = InputConf::open(path, keyParseable);
    QVERIFY(c.has_value());
    c->setLineFields(0, QStringLiteral("f"), QStringLiteral("ignore"), QString());
    QVERIFY(!c->commit());                                           // 期望失败
    QCOMPARE(readAll(path), QStringLiteral("f cycle fullscreen\n")); // 原文件未被写坏
    // 还原权限避免清理失败
    QFile::setPermissions(path, QFile::WriteOwner | QFile::ReadOwner | QFile::WriteUser |
                                    QFile::ReadUser);
#else
    QSKIP("Windows-only test");
#endif
}

QTEST_GUILESS_MAIN(TestInputConf)

#include "tst_InputConf.moc"

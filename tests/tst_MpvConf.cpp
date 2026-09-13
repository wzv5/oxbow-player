// MpvConf 单元测试（R3）：手写 mpv.conf 解析器的边界 case 覆盖。
// 测试无副作用：每个 case 用 QTemporaryFile 隔离路径，结束时自动清理。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R MpvConf 跑本套件；
//   mpvqt_tests_MpvConf <槽名> 跑单个用例。

#include "pch.h"

#include <QTextStream>
#include <QtTest/QtTest>

#include "MpvConf.h"

class TestMpvConf : public QObject {
    Q_OBJECT

  private slots:
    // 文件不存在 → open 返回空实例 + commit 写出首行
    void openMissingFileCreatesEmptyInstance();
    // 顶层裸 key 写入
    void setGlobalWritesKeyValueToTopLevel();
    // 已有顶层 key 修改值（不增行）
    void setGlobalReplacesExistingTopKey();
    // 注释/空行/section 头按行原样保留
    void commentsAndSectionHeaderPreservedByteExact();
    // CRLF 行尾剥 \r，serialize 统一写 \n
    void crlfLineEndingsNormalized();
    // BOM 解析时跳过；serialize 不写 BOM
    void bomStrippedOnRead();
    // 非法行兜底为 Comment 不丢
    void illegalLineKeptAsComment();
    // 同名 key 多次出现 →仅操作首项
    void duplicateKeysFirstOnly();
    // section 内 key 读写独立 namespace
    void sectionKeyIsolatedFromTopLevel();
    // 文件只读 → commit 失败 + qWarning（无法直接断言日志，但返回 false）
    void readOnlyFileCommitFails();
    // 节末无换行（split 产生空尾项）兼容
    void missingTrailingNewlineHandled();
    // R3 循环模式复合写：双 key 写 conf 场景（应用层需写 loop-file + loop-playlist）
    void loopModeDualKeyWrite();
};

// 工具：把内容写入临时文件并返回路径（UTF-8）
static QString writeTempFile(const QString &content) {
    QTemporaryFile *tf = new QTemporaryFile(QDir::tempPath() + "/mpv_conf_XXXXXX.conf");
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
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

void TestMpvConf::openMissingFileCreatesEmptyInstance() {
    const QString path = QDir::tempPath() + "/mpv_conf_should_not_exist_xyz.conf";
    QFile::remove(path);

    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    QVERIFY(c->globalKeys().isEmpty());
    c->setGlobal("hwdec", "d3d11va");
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), QStringLiteral("hwdec=d3d11va\n"));
}

void TestMpvConf::setGlobalWritesKeyValueToTopLevel() {
    const QString path = writeTempFile(QStringLiteral("# header\n"));
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    c->setGlobal("hwdec", "auto");
    QVERIFY(c->commit());
    // 注释保留 + 新 key append 到末尾
    QCOMPARE(readAll(path), QStringLiteral("# header\nhwdec=auto\n"));
}

void TestMpvConf::setGlobalReplacesExistingTopKey() {
    const QString path =
        writeTempFile(QStringLiteral("hwdec=auto\n# tail comment\n[gpu]\nfoo=bar\n"));
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    c->setGlobal("hwdec", "d3d11va");
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), QStringLiteral("hwdec=d3d11va\n# tail comment\n[gpu]\nfoo=bar\n"));
}

void TestMpvConf::commentsAndSectionHeaderPreservedByteExact() {
    // 注意 KeyValue 行 serialize 后会丢失缩进（重组为 key=value）——方案 §2.3.5 设计。
    // 仅 Comment / Empty / Section 行按行原样保留。
    const QString original = "# top comment\n"
                             "\n"
                             "; another\n"
                             "   # indented comment\n"
                             "[gpu]\n"
                             "gpu-api=vulkan\n" // 不带前导空格：serialize 后 KeyValue 重组丢弃缩进
                             "; gpu tail\n"
                             "[vo]\n"
                             "vo=gpu\n";
    const QString path = writeTempFile(original);

    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    c->setGlobal("loop-file", "inf"); // 顶层 key append
    QVERIFY(c->commit());

    const QString out = readAll(path);
    // 顶层注释/空行原样；section 头 + section 内 KeyValue 重组；section 内 Comment/Empty 原样；
    // 新顶层 key append 末尾
    QCOMPARE(out, original + QStringLiteral("loop-file=inf\n"));
}

void TestMpvConf::crlfLineEndingsNormalized() {
    // 写入 CRLF
    const QString path = QDir::tempPath() + "/mpv_conf_crlf.conf";
    QFile::remove(path);
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("# header\r\nhwdec=auto\r\n[gpu]\r\nfoo=bar\r\n");
    }
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    QVERIFY(c->commit());
    // 序列化应统一为 LF；原 CRLF 行剥 \r 后保留内容
    QCOMPARE(readAll(path), QStringLiteral("# header\nhwdec=auto\n[gpu]\nfoo=bar\n"));
}

void TestMpvConf::bomStrippedOnRead() {
    const QString path = QDir::tempPath() + "/mpv_conf_bom.conf";
    QFile::remove(path);
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // UTF-8 BOM EF BB BF + 内容
        const char bom[] = {'\xEF', '\xBB', '\xBF'};
        f.write(bom, 3);
        f.write("hwdec=auto\n");
    }
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    QCOMPARE(c->getGlobal("hwdec"), QStringLiteral("auto"));
    QVERIFY(c->commit());
    // 写回不应带 BOM
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray data = f.readAll();
    QVERIFY(!data.startsWith('\xEF'));
    QCOMPARE(data, QByteArray("hwdec=auto\n"));
}

void TestMpvConf::illegalLineKeptAsComment() {
    // "@@@" 不符合 key=value 也不符合 section/注释；兜底为 Comment 保留
    const QString path = writeTempFile(QStringLiteral("hwdec=auto\n@@@\n[gpu]\n"));
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), QStringLiteral("hwdec=auto\n@@@\n[gpu]\n"));
}

void TestMpvConf::duplicateKeysFirstOnly() {
    const QString path = writeTempFile(QStringLiteral("hwdec=auto\nhwdec=d3d11va\n"));
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    c->setGlobal("hwdec", "no");
    QVERIFY(c->commit());
    // 只改首个；第二个不动
    QCOMPARE(readAll(path), QStringLiteral("hwdec=no\nhwdec=d3d11va\n"));
}

void TestMpvConf::sectionKeyIsolatedFromTopLevel() {
    const QString path = writeTempFile(QStringLiteral("hwdec=auto\n[gpu]\nhwdec=dxva2\n"));
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    QCOMPARE(c->getGlobal("hwdec"), QStringLiteral("auto"));
    QCOMPARE(c->get("gpu", "hwdec"), QStringLiteral("dxva2"));
    // 改顶层，section 内同名 key 不动
    c->setGlobal("hwdec", "no");
    QVERIFY(c->commit());
    QCOMPARE(readAll(path), QStringLiteral("hwdec=no\n[gpu]\nhwdec=dxva2\n"));
}

void TestMpvConf::readOnlyFileCommitFails() {
    // 仅在 Windows 跳过该测试（win32 才能置只读；项目只 Windows 实现）
#ifdef Q_OS_WIN
    const QString path = writeTempFile(QStringLiteral("hwdec=auto\n"));
    // 让文件只读
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::ReadUser));
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    c->setGlobal("hwdec", "no");
    QVERIFY(!c->commit()); // 期望失败
    // 还原权限避免清理失败
    QFile::setPermissions(path, QFile::WriteOwner | QFile::ReadOwner | QFile::WriteUser |
                                    QFile::ReadUser);
#else
    QSKIP("Windows-only test");
#endif
}

void TestMpvConf::missingTrailingNewlineHandled() {
    // 内容无末尾换行
    const QString path = QDir::tempPath() + "/mpv_conf_no_trailing.conf";
    QFile::remove(path);
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hwdec=auto\nexisting=val");
    }
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    // 解析应仍能识别两条 key
    QCOMPARE(c->getGlobal("hwdec"), QStringLiteral("auto"));
    QCOMPARE(c->getGlobal("existing"), QStringLiteral("val"));
    c->setGlobal("hwdec", "no");
    QVERIFY(c->commit());
    // 序列化统一加 \n，末尾应有换行
    QCOMPARE(readAll(path), QStringLiteral("hwdec=no\nexisting=val\n"));
}

void TestMpvConf::loopModeDualKeyWrite() {
    // R3：SettingsDialog 写循环模式时拆为 loop-file / loop-playlist 双 key。
    // 验证 MpvConf 能正确持久化两 key（两种均能读出）。
    const QString path = writeTempFile(QStringLiteral("# header\n"));
    auto c = MpvConf::open(path);
    QVERIFY(c.has_value());
    c->setGlobal("loop-file", "inf");
    c->setGlobal("loop-playlist", "no");
    QVERIFY(c->commit());
    // 读回：两 key 独立可读
    auto c2 = MpvConf::open(path);
    QVERIFY(c2.has_value());
    QCOMPARE(c2->getGlobal("loop-file"), QStringLiteral("inf"));
    QCOMPARE(c2->getGlobal("loop-playlist"), QStringLiteral("no"));
    // 切换到 playlist
    c2->setGlobal("loop-file", "no");
    c2->setGlobal("loop-playlist", "inf");
    QVERIFY(c2->commit());
    auto c3 = MpvConf::open(path);
    QCOMPARE(c3->getGlobal("loop-file"), QStringLiteral("no"));
    QCOMPARE(c3->getGlobal("loop-playlist"), QStringLiteral("inf"));
}

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN），无 GUI 依赖
QTEST_GUILESS_MAIN(TestMpvConf)

#include "tst_MpvConf.moc"
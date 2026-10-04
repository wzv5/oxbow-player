// PlaylistFile 单元测试（N80 / N81）：两档写出（PLS / M3U）、两档读取（只取
// url）与后缀分派的边界 case 覆盖。测试无副作用：每个 case 用 QTemporaryFile /
// QTemporaryDir 隔离，结束时自动清理。一类一可执行（R5）：
//   ctest --test-dir build -C Debug -R PlaylistFile 跑本套件；
//   mpvqt_tests_PlaylistFile <槽名> 跑单个用例。

#include "pch.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "PlaylistFile.h"

using PlaylistFile::Entry;

namespace {

// 建一个不存在的目标路径（临时目录内），后缀自定
QString tempPath(const QString &suffix) {
    static QTemporaryDir dir;
    return dir.filePath(QStringLiteral("out.%1").arg(suffix));
}

// 落一份字节内容为源文件（读取用例），后缀自定。打开失败不在此断言——QVERIFY
// 的失败返回是 void 语义，进不了带返回值函数；读侧 readFile 会以 error 显影，
// 用例照样失败可见
QString feedPath(const QString &suffix, const QByteArray &data) {
    static QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("in.%1").arg(suffix));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
    return path;
}

// 读回全部字节（文件必须存在）
QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

} // namespace

class TestPlaylistFile : public QObject {
    Q_OBJECT

  private slots:
    // PLS：段头 / NumberOfEntries / FileN/TitleN/LengthN 自 1 起 / Version=2 收尾；
    // Length 恒写 -1（时长不导出）
    void plsWritesEntryKeysFromOne();
    // M3U：首行 #EXTM3U，每条先 #EXTINF 后 url；时长位恒写 -1
    void m3uWritesExtInfThenUrl();
    // m3u8 后缀分派到同一份 M3U 内容
    void m3u8SuffixProducesSameM3uContent();
    // 后缀分派不区分大小写；后缀不受支持回 error（形如 Unsupported file format: .txt）
    void suffixDispatchAndUnsupportedSuffix();
    // 两档均 UTF-8 无 BOM、LF 换行；中文路径 / 标题正常
    void utf8NoBomLfWithChineseContent();
    // 目标目录不存在 → 打不开回 error
    void openFailureReturnsError();
    // 目标只读 → 提交失败回 error（Windows-only，win32 才能置只读）
    void readOnlyTargetFailsCommit();
    // 空条目表：PLS 产出 NumberOfEntries=0；M3U 只有 #EXTM3U
    void emptyTrackListStillWritesSkeleton();
    // ——读取（N81）——
    // PLS：FileN 按键号升序输出（乱序键 / 大写 FILE 键都认），NumberOfEntries
    // 不读——条数声明不信任
    void plsReadsFileKeysOrderedAndCaseInsensitive();
    // PLS：空值键跳过；同号键按文件顺序都收；只有 [playlist] 无 File 键回空表
    void plsSkipsEmptyValuesAndIgnoresCount();
    // PLS：首行 BOM 剥离（无段头、首行即 File 键的形态才可见）；中文值原样取出
    void plsStripsLeadingBom();
    // M3U：CRLF 行尾 / 无末尾换行 / 注释行（含 #EXTINF）与空行一律跳过
    void m3uReadsUrlsSkippingCommentsAndBlanks();
    // M3U：UTF-8 BOM 不产幽灵首条；中文内容原样取出
    void m3uStripsLeadingBom();
    // M3U：零有效条目回空表而非 error
    void m3uZeroEntriesReturnsEmpty();
    // 读取分派：大写后缀认档；.txt 回 error；后缀认得却取不到（目录 / 缺失文件）回
    // Cannot open file
    void readDispatchSuffixAndOpenFailure();
};

void TestPlaylistFile::plsWritesEntryKeysFromOne() {
    const QString path = tempPath(QStringLiteral("pls"));
    const QList<Entry> tracks = {
        {QStringLiteral("D:\\a\\one.mkv"), QStringLiteral("One")},
        {QStringLiteral("https://host/two.m3u8"), QStringLiteral("Two")},
    };
    const auto result = PlaylistFile::writeFile(path, tracks);
    QVERIFY(result.has_value());

    const QByteArray data = readAll(path);
    const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
    QCOMPARE(lines.at(0), QStringLiteral("[playlist]"));
    QCOMPARE(lines.at(1), QStringLiteral("NumberOfEntries=2"));
    QCOMPARE(lines.at(2), QStringLiteral("File1=D:\\a\\one.mkv"));
    QCOMPARE(lines.at(3), QStringLiteral("Title1=One"));
    QCOMPARE(lines.at(4), QStringLiteral("Length1=-1"));
    QCOMPARE(lines.at(5), QStringLiteral("File2=https://host/two.m3u8"));
    QCOMPARE(lines.at(6), QStringLiteral("Title2=Two"));
    QCOMPARE(lines.at(7), QStringLiteral("Length2=-1"));
    QCOMPARE(lines.at(8), QStringLiteral("Version=2"));
    QVERIFY(lines.at(9).isEmpty()); // 收尾换行
    QCOMPARE(lines.size(), 10);
}

void TestPlaylistFile::m3uWritesExtInfThenUrl() {
    const QString path = tempPath(QStringLiteral("m3u"));
    const QList<Entry> tracks = {
        {QStringLiteral("D:\\a\\one.mkv"), QStringLiteral("One")},
        {QStringLiteral("https://host/two.m3u8"), QStringLiteral("Two")},
    };
    QVERIFY(PlaylistFile::writeFile(path, tracks).has_value());

    const QByteArray data = readAll(path);
    const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
    QCOMPARE(lines.at(0), QStringLiteral("#EXTM3U"));
    QCOMPARE(lines.at(1), QStringLiteral("#EXTINF:-1,One"));
    QCOMPARE(lines.at(2), QStringLiteral("D:\\a\\one.mkv"));
    QCOMPARE(lines.at(3), QStringLiteral("#EXTINF:-1,Two"));
    QCOMPARE(lines.at(4), QStringLiteral("https://host/two.m3u8"));
    QCOMPARE(lines.size(), 6);
}

void TestPlaylistFile::m3u8SuffixProducesSameM3uContent() {
    const QList<Entry> tracks = {{QStringLiteral("D:\\a\\one.mkv"), QStringLiteral("One")}};
    const QString m3uPath = tempPath(QStringLiteral("m3u"));
    const QString m3u8Path = tempPath(QStringLiteral("m3u8"));
    QVERIFY(PlaylistFile::writeFile(m3uPath, tracks).has_value());
    QVERIFY(PlaylistFile::writeFile(m3u8Path, tracks).has_value());
    // 同一档：两份内容逐字节一致，后缀只是文件命名偏好
    QCOMPARE(readAll(m3u8Path), readAll(m3uPath));
}

void TestPlaylistFile::suffixDispatchAndUnsupportedSuffix() {
    const QList<Entry> tracks = {{QStringLiteral("u"), QStringLiteral("t")}};
    // 大写后缀同样分派（不区分大小写）
    const QString upper = tempPath(QStringLiteral("PLS"));
    QVERIFY(PlaylistFile::writeFile(upper, tracks).has_value());
    QVERIFY(readAll(upper).startsWith("[playlist]"));

    // 后缀不受支持 → error 显影（调用方认了模块不认的后缀不会静默出错）
    const QString bad = tempPath(QStringLiteral("txt"));
    const auto result = PlaylistFile::writeFile(bad, tracks);
    QVERIFY(!result.has_value());
    QVERIFY(result.error().contains(QStringLiteral("Unsupported file format: .txt")));
}

void TestPlaylistFile::utf8NoBomLfWithChineseContent() {
    // 中文目录 / 中文标题：QTemporaryDir 路径内含盘符与 ASCII，文件名带中文
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("清单.pls"));
    const QList<Entry> tracks = {
        {QStringLiteral("D:\\视频\\第一集.mkv"), QStringLiteral("第一集")}};
    QVERIFY(PlaylistFile::writeFile(path, tracks).has_value());

    const QByteArray data = readAll(path);
    QVERIFY(!data.startsWith("\xEF\xBB\xBF")); // 无 BOM
    QVERIFY(!data.contains('\r'));             // LF 换行
    QVERIFY(data.contains("第一集"));          // UTF-8 中文原样
}

void TestPlaylistFile::openFailureReturnsError() {
    QTemporaryDir dir;
    // 目录不存在：open 即失败，error = 具体原因
    const QString path =
        dir.filePath(QStringLiteral("no_such_dir") + QLatin1Char('/') + QStringLiteral("out.pls"));
    const QList<Entry> tracks = {{QStringLiteral("u"), QStringLiteral("t")}};
    const auto result = PlaylistFile::writeFile(path, tracks);
    QVERIFY(!result.has_value());
    QVERIFY(result.error().contains(QStringLiteral("Cannot open file")));
}

void TestPlaylistFile::readOnlyTargetFailsCommit() {
#ifdef Q_OS_WIN
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ro.pls"));
    { // 先落一个文件再置只读
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("placeholder");
    }
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::ReadUser));
    const QList<Entry> tracks = {{QStringLiteral("u"), QStringLiteral("t")}};
    const auto result = PlaylistFile::writeFile(path, tracks);
    // 还原权限避免清理失败
    QFile::setPermissions(path, QFile::WriteOwner | QFile::ReadOwner | QFile::WriteUser |
                                    QFile::ReadUser);
    QVERIFY(!result.has_value());
    // Qt6 起 QSaveFile::open() 对已存在的只读目标即早失败（errorString
    // "Existing file … is not writable"），本模块包装为 "Cannot open file: …"；
    // 走不到 rename / commit 阶段，勿断言 "Commit failed"
    QVERIFY(result.error().contains(QStringLiteral("Cannot open file")));
    QVERIFY(result.error().contains(QStringLiteral("not writable")));
#else
    QSKIP("Windows-only test");
#endif
}

void TestPlaylistFile::emptyTrackListStillWritesSkeleton() {
    const QList<Entry> tracks;
    const QString pls = tempPath(QStringLiteral("pls"));
    QVERIFY(PlaylistFile::writeFile(pls, tracks).has_value());
    QCOMPARE(QString::fromUtf8(readAll(pls)),
             QStringLiteral("[playlist]\nNumberOfEntries=0\nVersion=2\n"));

    const QString m3u = tempPath(QStringLiteral("m3u"));
    QVERIFY(PlaylistFile::writeFile(m3u, tracks).has_value());
    QCOMPARE(QString::fromUtf8(readAll(m3u)), QStringLiteral("#EXTM3U\n"));
}

// ——读取（N81）——

void TestPlaylistFile::plsReadsFileKeysOrderedAndCaseInsensitive() {
    // File2 在 File1 之前、大写 FILE 键、NumberOfEntries 谎报条数——条数以实扫
    // 的 File 键为准、按键内数字升序输出；Title / Length / NumberOfEntries 不读
    const QString path = feedPath(QStringLiteral("pls"), "NumberOfEntries=1\n"
                                                         "File2=https://h/b.mkv\n"
                                                         "FILE1=D:\\a\\one.mkv\n"
                                                         "Title2=B\n");
    const auto result = PlaylistFile::readFile(path);
    QVERIFY(result.has_value());
    QCOMPARE(result.value(),
             (QStringList{QStringLiteral("D:\\a\\one.mkv"), QStringLiteral("https://h/b.mkv")}));
}

void TestPlaylistFile::plsSkipsEmptyValuesAndIgnoresCount() {
    // 空值 File 键跳过；同号键（畸形文件）按文件顺序都收；只有 [playlist] 无
    // File 键 → 空表而非 error
    const QString path = feedPath(QStringLiteral("pls"), "[playlist]\n"
                                                         "NumberOfEntries=3\n"
                                                         "File1=D:\\a.mkv\n"
                                                         "File1=D:\\a2.mkv\n"
                                                         "File2=\n"
                                                         "File3=D:\\b.mkv\n");
    const auto result = PlaylistFile::readFile(path);
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), (QStringList{QStringLiteral("D:\\a.mkv"), QStringLiteral("D:\\a2.mkv"),
                                          QStringLiteral("D:\\b.mkv")}));

    const QString none = feedPath(QStringLiteral("pls"), "[playlist]\nVersion=2\n");
    const auto empty = PlaylistFile::readFile(none);
    QVERIFY(empty.has_value());
    QVERIFY(empty.value().isEmpty());
}

void TestPlaylistFile::plsStripsLeadingBom() {
    // 无段头的 PLS（首行即 File 键）才看得见剥离：不剥则该键行带 FEFF 前缀不匹配、
    // 整条静默丢失。中文值原样取出（读出侧本模块不做任何加工）
    const QString path = feedPath(QStringLiteral("pls"),
                                  // BOM 与键行分作相邻字面量：\xBF 后紧跟 'F' 会被
                                  // 十六进制转义吞进同一个字节（MSVC C2022）
                                  QByteArrayLiteral("\xEF\xBB\xBF"
                                                    "File1=D:\\视频\\第一集.mkv\n"
                                                    "File2=https://h/b.mkv\n"));
    const auto result = PlaylistFile::readFile(path);
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), (QStringList{QStringLiteral("D:\\视频\\第一集.mkv"),
                                          QStringLiteral("https://h/b.mkv")}));
}

void TestPlaylistFile::m3uReadsUrlsSkippingCommentsAndBlanks() {
    // CRLF 行尾、无末尾换行、注释行（含 #EXTINF，不解析不配对）与空行一律跳过，
    // 其余行剔除首尾空白即 url
    const QString path = feedPath(QStringLiteral("m3u"),
                                  "#EXTM3U\r\n"
                                  "\r\n"
                                  "#EXTINF:-1,One\r\n"
                                  "D:\\a\\one.mkv\r\n"
                                  "  https://h/two.mkv  \r\n"
                                  "https://h/three.mkv"); // 无末尾换行
    const auto result = PlaylistFile::readFile(path);
    QVERIFY(result.has_value());
    QCOMPARE(result.value(),
             (QStringList{QStringLiteral("D:\\a\\one.mkv"), QStringLiteral("https://h/two.mkv"),
                          QStringLiteral("https://h/three.mkv")}));
}

void TestPlaylistFile::m3uStripsLeadingBom() {
    // UTF-8 BOM 开头：不剥则首行 "#EXTM3U" 带 FEFF 前缀、不再是注释行，落成
    // 幽灵条目；中文路径原样取出
    const QString path = feedPath(QStringLiteral("m3u"),
                                  QByteArrayLiteral("\xEF\xBB\xBF#EXTM3U\nD:\\视频\\第一集.mkv\n"));
    const auto result = PlaylistFile::readFile(path);
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), (QStringList{QStringLiteral("D:\\视频\\第一集.mkv")}));
}

void TestPlaylistFile::m3uZeroEntriesReturnsEmpty() {
    // 只有注释与空行：空表而非 error（"读成功但没东西"，处置归调用方）
    const QString path = feedPath(QStringLiteral("m3u"), "#EXTM3U\n#EXTINF:-1,One\n\n");
    const auto result = PlaylistFile::readFile(path);
    QVERIFY(result.has_value());
    QVERIFY(result.value().isEmpty());
}

void TestPlaylistFile::readDispatchSuffixAndOpenFailure() {
    // 大写后缀同样分派（不区分大小写）
    const QString upper = feedPath(QStringLiteral("M3U"), "http://h/a.mkv\n");
    const auto ok = PlaylistFile::readFile(upper);
    QVERIFY(ok.has_value());
    QCOMPARE(ok.value(), (QStringList{QStringLiteral("http://h/a.mkv")}));

    // 后缀不受支持 → error 显影（经 All Files 选了别的东西不猜着解析）
    const QString bad = feedPath(QStringLiteral("txt"), "http://h/a.mkv\n");
    const auto err = PlaylistFile::readFile(bad);
    QVERIFY(!err.has_value());
    QVERIFY(err.error().contains(QStringLiteral("Unsupported file format: .txt")));

    // 目录路径 → open 即失败，error = 具体原因
    QTemporaryDir dir;
    // 打不开两支：目录本身、后缀认得但盘上没有的文件。基址一律取 dir.filePath 自造
    // 的名字——QTemporaryDir 的默认名 qt_temp.XXXXXX 尾段会被 QFileInfo 当后缀，
    // 直接喂 dir.path() 落的是"后缀不受支持"支而非 open 失败支（上一版即栽在这）
    const QString dirPath = dir.filePath(QStringLiteral("in.pls"));
    QVERIFY(QDir().mkpath(dirPath));
    const auto dirErr = PlaylistFile::readFile(dirPath);
    QVERIFY(!dirErr.has_value());
    QVERIFY(dirErr.error().contains(QStringLiteral("Cannot open file")));

    const auto missErr = PlaylistFile::readFile(dir.filePath(QStringLiteral("gone.m3u")));
    QVERIFY(!missErr.has_value());
    QVERIFY(missErr.error().contains(QStringLiteral("Cannot open file")));
}

// 一类一可执行（R5）：自带 main（QTEST_GUILESS_MAIN），无 GUI 依赖
QTEST_GUILESS_MAIN(TestPlaylistFile)

#include "tst_PlaylistFile.moc"

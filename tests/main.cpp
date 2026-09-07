// mpvqt_tests 统一入口（N26）：测试文件多于一个后，QTEST_x_MAIN 各自带 main
// 会产生重复符号。各测试文件只提供套件工厂，此处逐个 qExec 并汇总退出码，
// 保持"单个 exe 跑全部"的既有约定。新增测试类时：在对应 cpp 加工厂函数，
// 并在此登记一行
#include <QCoreApplication>
#include <QtTest/QtTest>

#include <vector>

// 各测试文件提供的套件构造（类定义于各自 cpp，此处仅入口装配）
QObject *createTestMpvConf();
QObject *createTestPlaylistEntity();

namespace {
using SuiteFactory = QObject *(*)();

// -o <path[,fmt]> 的输出文件会被多个套件先后打开、后写覆盖先写：
// 第 2 个及之后的套件把路径改写为 <stem>_suiteN<ext>[,fmt]（无扩展名则直接追加），
// 使各套件报告独立落盘
QByteArray suiteOutputArg(const QByteArray &opt, int suite) {
    const int comma = opt.indexOf(',');
    const QByteArray fmt = comma >= 0 ? opt.mid(comma) : QByteArray();
    QByteArray path = comma >= 0 ? opt.left(comma) : opt;
    const QByteArray suffix = "_suite" + QByteArray::number(suite);
    const int dot = path.lastIndexOf('.');
    if (dot > 0)
        path.insert(dot, suffix);
    else
        path += suffix;
    return path + fmt;
}
} // namespace

int main(int argc, char *argv[]) {
    // 两个套件均无 GUI 依赖（QTEST_GUILESS_MAIN 同款环境）
    QCoreApplication app(argc, argv);

    const SuiteFactory suites[] = {createTestMpvConf, createTestPlaylistEntity};
    const int suiteCount = int(sizeof(suites) / sizeof(suites[0]));

    int status = 0;
    for (int s = 0; s < suiteCount; ++s) {
        // 逐套件重建 argv：-o 输出按套件改名，避免报告互相覆盖
        std::vector<QByteArray> storage;
        std::vector<char *> args;
        args.push_back(argv[0]);
        for (int i = 1; i < argc; ++i) {
            const QByteArray arg(argv[i]);
            if (arg == "-o" && i + 1 < argc) {
                storage.push_back(s == 0 ? QByteArray(argv[i + 1])
                                         : suiteOutputArg(QByteArray(argv[i + 1]), s + 1));
                args.push_back(const_cast<char *>(storage.back().constData()));
                ++i; // 消费 -o 的值
            } else {
                storage.push_back(arg);
                args.push_back(const_cast<char *>(storage.back().constData()));
            }
        }
        QObject *suite = suites[s]();
        status |= QTest::qExec(suite, int(args.size()), args.data());
        delete suite;
    }
    return status;
}

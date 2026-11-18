// 字体资源在静态库内，不被引用不入链接——initTestCase 显式 Q_INIT_RESOURCE 兜底。

#include "pch.h"

#include <QtTest/QtTest>

#include "Constants.h"
#include "ui/MaterialIconCatalog.h"

namespace {

bool isLigatureChar(QChar ch) {
    return (ch >= u'a' && ch <= u'z') || (ch >= u'0' && ch <= u'9') || ch == u'_';
}

} // namespace

class TestMaterialIconCatalog : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { Q_INIT_RESOURCE(fonts); }

    // 数字锁字体事实，换字体时此处失败即是提示
    void parsesAllLigatureNames() {
        const QStringList &names = MaterialIconCatalog::names();
        QCOMPARE(names.size(), 4319);
        for (const QString &name : names) {
            QVERIFY2(name.size() >= 2, qPrintable(name));
            for (const QChar ch : name)
                QVERIFY2(isLigatureChar(ch), qPrintable(name));
        }
        QStringList sorted = names;
        std::sort(sorted.begin(), sorted.end());
        QCOMPARE(names, sorted);
    }

    void builtinNamesPresent() {
        const QStringList builtins = {
            App::iconMyLocation(),
            App::iconSearch(),
            App::iconFormatListBulleted(),
            App::iconSettings(),
            App::iconAdd(),
            App::iconDeleteForever(),
            App::iconPlayArrow(),
            App::iconPause(),
            App::iconStop(),
            App::iconSkipPrevious(),
            App::iconSkipNext(),
            App::iconFastForward(),
            App::iconFastRewind(),
            App::iconStream(),
            App::iconVolumeUp(),
            App::iconVolumeOff(),
            App::iconKeyboardArrowUp(),
            App::iconKeyboardArrowDown(),
            App::iconRefresh(),
            App::iconSettingsBackupRestore(),
        };
        QCOMPARE(builtins.size(), 20);
        for (const QString &name : builtins)
            QVERIFY2(MaterialIconCatalog::contains(name), qPrintable(name));
    }

    void invalidNamesRejected() {
        QVERIFY(!MaterialIconCatalog::contains(QStringLiteral("a")));
        QVERIFY(!MaterialIconCatalog::contains(QStringLiteral("A")));
        QVERIFY(!MaterialIconCatalog::contains(QStringLiteral("playarrow")));
        QVERIFY(!MaterialIconCatalog::contains(QStringLiteral("play_arrow_x")));
        QVERIFY(!MaterialIconCatalog::contains(QString()));
    }

    void familyMatchesOwnLoad() {
        const QString family = MaterialIconCatalog::family();
        QVERIFY(!family.isEmpty());
        const int id = QFontDatabase::addApplicationFont(
            QStringLiteral(":/fonts/MaterialSymbolsRounded_Filled-Regular.ttf"));
        QVERIFY(id >= 0);
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        QVERIFY(!families.isEmpty());
        QCOMPARE(family, families.first());
    }

    void repeatedQueriesConsistent() {
        QCOMPARE(MaterialIconCatalog::names(), MaterialIconCatalog::names());
        QVERIFY(MaterialIconCatalog::contains(App::iconPlayArrow()));
        QVERIFY(MaterialIconCatalog::contains(App::iconPlayArrow()));
    }

    void parseRejectsGarbage() {
        QVERIFY(MaterialIconCatalog::parseFontLigatures(QByteArray()).isEmpty());
        QFile font(QStringLiteral(":/fonts/MaterialSymbolsRounded_Filled-Regular.ttf"));
        QVERIFY(font.open(QIODevice::ReadOnly));
        const QByteArray data = font.readAll();
        QVERIFY(data.size() > 48);
        QVERIFY(MaterialIconCatalog::parseFontLigatures(data.left(48)).isEmpty());
        QByteArray blanked = data;
        memset(blanked.data(), 0, 16); // 表数与首表记录全零 → 无 cmap / GSUB
        QVERIFY(MaterialIconCatalog::parseFontLigatures(blanked).isEmpty());
    }
};

QTEST_MAIN(TestMaterialIconCatalog)

#include "tst_MaterialIconCatalog.moc"

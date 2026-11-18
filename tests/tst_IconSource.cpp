// 字体资源在静态库内，不被引用不入链接——initTestCase 显式 Q_INIT_RESOURCE 兜底。

#include "pch.h"

#include <QtTest/QtTest>

#include "Constants.h"
#include "ui/MaterialIconCatalog.h"
#include "ui/ThemedIconEngine.h"

namespace {

// 图内最不透明像素的 alpha；染色断言只看实心内部，抗锯齿边缘不参与
int maxAlpha(const QPixmap &pixmap) {
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    int result = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *scan = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            result = qMax(result, qAlpha(scan[x]));
    }
    return result;
}

// 最不透明像素的颜色（并列取首个命中）
QColor colorAtMaxAlpha(const QPixmap &pixmap) {
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    int best = 0;
    QColor result;
    for (int y = 0; y < image.height(); ++y) {
        const auto *scan = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (const int a = qAlpha(scan[x]); a > best) {
                best = a;
                result = QColor(scan[x]);
            }
        }
    }
    return result;
}

} // namespace

class TestIconSource : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { Q_INIT_RESOURCE(fonts); }

    void exportReturnsLigatureName() {
        struct Row {
            QString (*fn)();
            const char *expected;
        };
        const QVector<Row> rows = {
            {App::iconMyLocation, "my_location"},
            {App::iconSearch, "search"},
            {App::iconFormatListBulleted, "format_list_bulleted"},
            {App::iconSettings, "settings"},
            {App::iconAdd, "add"},
            {App::iconDeleteForever, "delete_forever"},
            {App::iconPlayArrow, "play_arrow"},
            {App::iconPause, "pause"},
            {App::iconStop, "stop"},
            {App::iconSkipPrevious, "skip_previous"},
            {App::iconSkipNext, "skip_next"},
            {App::iconFastForward, "fast_forward"},
            {App::iconFastRewind, "fast_rewind"},
            {App::iconStream, "stream"},
            {App::iconVolumeUp, "volume_up"},
            {App::iconVolumeOff, "volume_off"},
            {App::iconKeyboardArrowUp, "keyboard_arrow_up"},
            {App::iconKeyboardArrowDown, "keyboard_arrow_down"},
            {App::iconRefresh, "refresh"},
            {App::iconSettingsBackupRestore, "settings_backup_restore"},
        };
        for (const Row &row : rows) {
            const QString name = row.fn();
            QCOMPARE(name, QString(row.expected));
            QVERIFY(!name.startsWith(u":/"));
            QVERIFY(!name.endsWith(u".svg"));
        }
    }

    void ligatureResolvesToSingleGlyph() {
        const int id = QFontDatabase::addApplicationFont(
            QStringLiteral(":/fonts/MaterialSymbolsRounded_Filled-Regular.ttf"));
        QVERIFY(id >= 0);
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        QVERIFY(!families.isEmpty());
        const QString family = families.first();

        QFont font(family);
        font.setPixelSize(32);
        font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoFontMerging));

        const QStringList names = {
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
        for (const QString &name : names) {
            QTextOption option;
            option.setUseDesignMetrics(true);
            QTextLayout layout(name, font);
            layout.setTextOption(option);
            layout.beginLayout();
            QTextLine line = layout.createLine();
            line.setLineWidth(std::numeric_limits<qreal>::max() / 1024);
            layout.endLayout();

            const QList<QGlyphRun> runs = layout.glyphRuns();
            QCOMPARE(runs.size(), 1);
            QCOMPARE(runs.first().glyphIndexes().size(), 1);
        }
    }

    void renderSizesAndDpr() {
        const QIcon icon = Ui::themedIcon(App::iconVolumeUp(), QColor(31, 111, 214));
        const QList<int> sizes = {16, 24, 32};
        const QList<qreal> dprs = {1.0, 1.5, 2.0};
        for (const int size : sizes) {
            for (const qreal dpr : dprs) {
                const QPixmap pixmap = icon.pixmap(QSize(size, size), dpr);
                QVERIFY2(!pixmap.isNull(),
                         qPrintable(QStringLiteral("size=%1 dpr=%2").arg(size).arg(dpr)));
                QCOMPARE(pixmap.size(), QSize(qRound(size * dpr), qRound(size * dpr)));
                QVERIFY2(
                    maxAlpha(pixmap) > 0,
                    qPrintable(
                        QStringLiteral("no opaque pixel at size=%1 dpr=%2").arg(size).arg(dpr)));
            }
        }
    }

    void tintAndDisabledFactor() {
        const QColor red(210, 40, 40);
        const QColor blue(30, 80, 200);
        for (const QColor &tint : {red, blue}) {
            const QIcon icon = Ui::themedIcon(App::iconVolumeUp(), tint);
            const QPixmap pixmap = icon.pixmap(QSize(32, 32));
            QCOMPARE(maxAlpha(pixmap), 255);
            const QColor rendered = colorAtMaxAlpha(pixmap);
            QCOMPARE(rendered.red(), tint.red());
            QCOMPARE(rendered.green(), tint.green());
            QCOMPARE(rendered.blue(), tint.blue());
        }

        // Disabled：tint 的 A 通道恒乘 0.4 系数
        const QIcon icon = Ui::themedIcon(App::iconVolumeUp(), red);
        const QPixmap disabled = icon.pixmap(QSize(32, 32), 1.0, QIcon::Disabled);
        QCOMPARE(maxAlpha(disabled), qRound(255 * 0.4));
    }

    void invalidNameRendersErrorGlyph() {
        const QColor tint(31, 111, 214);
        for (const QString &bad : {QStringLiteral("a"), QStringLiteral("A"),
                                   QStringLiteral("definitely_not_a_ligature")}) {
            const QIcon icon = Ui::themedIcon(bad, tint);
            const QPixmap pixmap = icon.pixmap(QSize(32, 32));
            QVERIFY2(!pixmap.isNull(), qPrintable(bad));
            QVERIFY2(maxAlpha(pixmap) > 0, qPrintable(bad));
            QCOMPARE(colorAtMaxAlpha(pixmap), tint);
        }
        const QPixmap empty = Ui::themedIcon(QString(), tint).pixmap(QSize(32, 32));
        QVERIFY(!empty.isNull());
        QCOMPARE(maxAlpha(empty), 0);
    }

    // 清单全员满足字形判定——清单查表与渲染侧判定的等价护栏
    void catalogNamesResolveToSingleGlyph() {
        const QString family = MaterialIconCatalog::family();
        QVERIFY(!family.isEmpty());
        QFont font(family);
        font.setPixelSize(32);
        font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoFontMerging));
        const QStringList &names = MaterialIconCatalog::names();
        QCOMPARE(names.size(), 4319);
        for (const QString &name : names) {
            QVERIFY2(name.size() >= 2, qPrintable(name));
            QTextOption option;
            option.setUseDesignMetrics(true);
            QTextLayout layout(name, font);
            layout.setTextOption(option);
            layout.beginLayout();
            QTextLine line = layout.createLine();
            line.setLineWidth(std::numeric_limits<qreal>::max() / 1024);
            layout.endLayout();
            const QList<QGlyphRun> runs = layout.glyphRuns();
            QVERIFY2(runs.size() == 1, qPrintable(name));
            QVERIFY2(runs.first().glyphIndexes().size() == 1, qPrintable(name));
        }
    }
};

QTEST_MAIN(TestIconSource)

#include "tst_IconSource.moc"

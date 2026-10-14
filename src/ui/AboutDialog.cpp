#include "ui/AboutDialog.h"

#include "Constants.h"
#include "Version.h" // N84：版本身份口（本文件只许 include 此口，不得 include revision.h）

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent) {
    constexpr int kIconSize = 48; // 上部图标边长，图标列宽同步锁死于此

    setWindowTitle(tr("About %1").arg(App::name()));

    // 全仓无 setWindowIcon，标题栏图标即宿主 exe 的资源图标；内容区取同源图标
    // （QFileIconProvider 对 exe 取文件图标）。图标项必须锁宽：QLabel 水平策略是
    // Preferred，不锁的话上部的多余宽度会按 sizeHint 比例分给它，文字列整体右移
    auto *iconLabel = new QLabel(this);
    const QIcon hostIcon =
        QFileIconProvider().icon(QFileInfo(QCoreApplication::applicationFilePath()));
    iconLabel->setPixmap(hostIcon.pixmap(QSize(kIconSize, kIconSize)));
    iconLabel->setFixedSize(kIconSize, kIconSize);

    // 标题加粗放大两号；两行都不设 alignment——QLabel 缺省即左对齐
    auto *titleLabel = new QLabel(App::name(), this);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleLabel->setFont(titleFont);
    auto *copyrightLabel = new QLabel(tr("Copyright © 2026 wzv5"), this);

    // 行距收口（样式缺省约 6px，叠上各自行高留白会把两行撑散），尾部拉伸让文字块
    // 贴列顶而非在图标高度里居中
    auto *titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(2);
    titleColumn->addWidget(titleLabel);
    titleColumn->addWidget(copyrightLabel);
    titleColumn->addStretch();

    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(24);
    topRow->addWidget(iconLabel, 0, Qt::AlignTop);
    topRow->addLayout(titleColumn, 1); // 图标列定宽，文字列吃满剩余宽度

    // 下部：构建信息，只读可选中复制。无框且视口底色取 Window 色，视觉与标签一致；
    // 尺寸按内容写死，滚动条一律关（否则抢视口宽度）
    auto *infoView = new QTextEdit(this);
    infoView->setReadOnly(true);
    infoView->setLineWrapMode(QTextEdit::NoWrap);
    infoView->setFrameShape(QFrame::NoFrame);
    infoView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    infoView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QPalette infoPalette = infoView->palette();
    infoPalette.setColor(QPalette::Base, infoPalette.color(QPalette::Window));
    infoView->setPalette(infoPalette);

    const QString sep = QStringLiteral(": ");
    const QString nameVersion = tr("Version") + sep;
    const QString nameBuildTime = tr("Build time") + sep;
    const QString nameMpvVersion = tr("mpv version") + sep;
    const QString nameQtVersion = tr("Qt version") + sep;

    // 制表位取"最宽名称 + 四空格间隙"：固定档位会撞停位点——名称恰好压在停位线前时
    // tab 只前进一两个空格宽，值列就错位；动态步进下各行值列起点恒在同一停位
    const QFontMetrics infoMetrics = infoView->fontMetrics();
    qreal widestName = 0;
    for (const QString &n : {nameVersion, nameBuildTime, nameMpvVersion, nameQtVersion})
        widestName = qMax(widestName, qreal(infoMetrics.horizontalAdvance(n)));
    infoView->setTabStopDistance(widestName +
                                 infoMetrics.horizontalAdvance(QStringLiteral("    ")));

    // Build time 展示为本地时间（Version 返回 UTC 指定的 QDateTime）；中间空一行
    // 分隔插件自身与宿主运行库两组
    const QString buildTimeText =
        App::buildDateTime().toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    infoView->setPlainText(QStringList{nameVersion + QLatin1Char('\t') + App::versionInfo(),
                                       nameBuildTime + QLatin1Char('\t') + buildTimeText, QString(),
                                       nameMpvVersion + QLatin1Char('\t') + App::mpvVersion(),
                                       nameQtVersion + QLatin1Char('\t') + App::qtVersion()}
                               .join(QLatin1Char('\n')));

    // 文档尺寸已含 documentMargin 与 tab 展开，照它写死即可保证版本串不折行
    const QSizeF docSize = infoView->document()->size();
    infoView->setFixedSize(int(docSize.width()) + 4, int(docSize.height()) + 4);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(topRow);
    mainLayout->addWidget(infoView);
    mainLayout->addWidget(buttonBox);

    setFixedSize(sizeHint()); // 固定尺寸：信息区已精确适配，缩放无益
}

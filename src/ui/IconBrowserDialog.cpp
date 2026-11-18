#include "pch.h"

#include "ui/IconBrowserDialog.h"

#include "MaterialIconCatalog.h"

namespace {

constexpr int kPadding = 6;

// 网格委托：连字现画到格子图标区（居中口径同 basePixmap），名字画下方。不走
// themedIcon——其底图缓存无淘汰，数千名 × 多尺寸会永久驻留；这里只画可见格子
class LigatureGridDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // 视图 gridSize 与委托 sizeHint 同源：条目矩形取自 sizeHint，不覆写会把图标区压扁
    static QSize cellSize() { return QSize(92, 108); }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return cellSize();
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        // 面板（选中 / 悬停底色）交样式画：内容位清空，防默认文本与图标绘制
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        const QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const QString name = index.data().toString();
        const QRect cell = option.rect;
        // 名字条两行自动换行，放不下的以格子底裁剪（tooltip 有全名）
        const int nameH = 2 * option.fontMetrics.height() + 4;
        const QRect nameRect(cell.x() + 2, cell.bottom() - nameH, cell.width() - 4, nameH - 2);
        QRect iconRect(cell.x() + kPadding, cell.y() + kPadding, cell.width() - 2 * kPadding,
                       nameRect.top() - kPadding - cell.y());
        if (iconRect.width() > iconRect.height()) {
            const int dx = (iconRect.width() - iconRect.height()) / 2;
            iconRect.adjust(dx, 0, -dx, 0);
        }

        // 内容色用 Text 角色：windows11 选中底是近白，换 HighlightedText 会白字白底
        const QColor color = option.palette.text().color();
        const QString family = MaterialIconCatalog::family();
        if (!family.isEmpty()) {
            QFont font(family);
            font.setPixelSize(qMin(iconRect.width(), iconRect.height()));
            // 禁字体合并：缺字时宁可空，不落到系统字体把连字名画成一串字母
            font.setStyleStrategy(
                QFont::StyleStrategy(font.styleStrategy() | QFont::NoFontMerging));
            QTextOption textOption;
            textOption.setUseDesignMetrics(true);
            QTextLayout layout(name, font);
            layout.setTextOption(textOption);
            layout.beginLayout();
            QTextLine line = layout.createLine();
            line.setLineWidth(std::numeric_limits<qreal>::max() / 1024);
            layout.endLayout();
            painter->save();
            painter->setRenderHint(QPainter::TextAntialiasing);
            painter->setPen(color);
            // 行宽横向、行盒纵向居中
            const QPointF origin(iconRect.x() + (iconRect.width() - line.naturalTextWidth()) / 2.0,
                                 iconRect.y() +
                                     (iconRect.height() - (line.ascent() + line.descent())) / 2.0);
            line.draw(painter, origin);
            painter->restore();
        }
        // 名字两行自动换行画在下方（连字名无空格、下划线不是词边界，WordWrap 断不开
        // 整词名——用 WrapAnywhere 任意点断行）
        painter->setPen(color);
        painter->drawText(nameRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWrapAnywhere, name);
    }
};

} // namespace

IconBrowserDialog::IconBrowserDialog(const QString &current, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Icon Browser"));

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter"));
    m_filter->setClearButtonEnabled(true);

    m_grid = new QListWidget(this);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setFlow(QListView::LeftToRight);
    m_grid->setWrapping(true);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setGridSize(LigatureGridDelegate::cellSize());
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setItemDelegate(new LigatureGridDelegate(m_grid));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *main = new QVBoxLayout(this);
    main->setContentsMargins(10, 10, 10, 10);
    main->addWidget(m_filter);
    main->addWidget(m_grid, 1);
    main->addWidget(buttons);

    connect(m_filter, &QLineEdit::textChanged, this, [this] { rebuild(m_filter->text()); });
    connect(m_grid, &QListWidget::currentItemChanged, this, [this] { updateOkState(); });
    connect(m_grid, &QListWidget::itemActivated, this, &QDialog::accept);

    // 初始宽度恰容 6 列：6×cell + 滚动条 + 边框，再垫余量防换行判定挤掉第 6 列
    const int sbExtent = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_grid);
    const int gridOuterW =
        6 * LigatureGridDelegate::cellSize().width() + sbExtent + 2 * m_grid->frameWidth() + 8;
    resize(gridOuterW + 20, 420);
    rebuild(m_filter->text());
    // 当前值在清单内才预选；setCurrentItem 自带滚动到位
    const QList<QListWidgetItem *> found = m_grid->findItems(current, Qt::MatchExactly);
    if (!found.isEmpty())
        m_grid->setCurrentItem(found.first());
    updateOkState();
    m_filter->setFocus();
}

QString IconBrowserDialog::selectedName() const {
    return m_grid->currentItem() ? m_grid->currentItem()->text() : QString();
}

void IconBrowserDialog::rebuild(const QString &filter) {
    m_grid->clear();
    for (const QString &name : MaterialIconCatalog::names()) {
        if (!name.contains(filter, Qt::CaseInsensitive))
            continue;
        QListWidgetItem *item = new QListWidgetItem(name, m_grid);
        item->setToolTip(name);
    }
    updateOkState();
}

// 确定按钮随网格有无选中项启停
void IconBrowserDialog::updateOkState() {
    m_okButton->setEnabled(m_grid->currentItem() != nullptr);
}

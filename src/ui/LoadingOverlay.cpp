#include "pch.h"

#include "LoadingOverlay.h"

namespace {

// 信息面板：自绘不透明 Window 底 + Mid 1px 直角边框（与 NavigatorPanel 边框风格同源）。
// 颜色在绘制期现取调色板，故运行期换配色即时跟随——样式表的 palette() 由
// QStyleSheetStyle 在 polish 期解析一次，换配色不会重解析，遂弃用
class InfoPanel : public QWidget {
  public:
    using QWidget::QWidget;

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const QPalette pal = palette();
        painter.fillRect(rect(), pal.color(QPalette::Window));
        painter.setPen(pal.color(QPalette::Mid));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};

} // namespace

LoadingOverlay::LoadingOverlay(QWidget *parent) : QWidget(parent) {
    setVisible(false);

    auto *outer = new QVBoxLayout(this);
    outer->addStretch();

    auto *panel = new InfoPanel(this);
    auto *inner = new QVBoxLayout(panel);
    // 24 / 16 之外再让出描边那 1px——原样式表的 border 由 QStyleSheetStyle 计入
    // box-model 内边距，自绘边框不吃布局，不让就比改前面大一圈
    inner->setContentsMargins(25, 17, 25, 17);
    inner->setSpacing(8);

    m_title = new QLabel(panel);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_title->setAlignment(Qt::AlignCenter);
    inner->addWidget(m_title);

    // 进度行：居中 + 等宽字体防跳动，仅元数据模式可见
    m_progress = new QLabel(panel);
    m_progress->setAlignment(Qt::AlignCenter);
    m_progress->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    inner->addWidget(m_progress);

    // 取消按钮：仅元数据模式可见；点击后立即禁用（单文件最坏 ~5s 才生效，防重复点击）
    m_cancel = new QPushButton(tr("Cancel"), panel);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_cancel->setEnabled(false);
        emit cancelRequested();
    });
    inner->addWidget(m_cancel, 0, Qt::AlignCenter);

    outer->addWidget(panel, 0, Qt::AlignCenter);
    outer->addStretch();
}

void LoadingOverlay::begin(Mode mode, int total) {
    const bool meta = (mode == Mode::MetaScan);
    m_title->setText(meta ? tr("Loading metadata") : tr("Loading"));
    m_progress->setVisible(meta);
    m_cancel->setVisible(meta);
    m_cancel->setEnabled(true);
    if (meta)
        setProgress(0, total);
    raise();
    setVisible(true);
}

void LoadingOverlay::setProgress(int done, int total) {
    m_progress->setText(QStringLiteral("%1 / %2").arg(done).arg(total));
}

void LoadingOverlay::finish() { setVisible(false); }

void LoadingOverlay::paintEvent(QPaintEvent *) {
    // 遮罩底 = palette Window @ alpha 160/255（≈63%）；paint 期实时取色，主题切换跟随
    QPainter painter(this);
    QColor bg = palette().color(QPalette::Window);
    bg.setAlpha(160);
    painter.fillRect(rect(), bg);
}

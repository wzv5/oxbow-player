#include "pch.h"

#include "LoadingOverlay.h"

LoadingOverlay::LoadingOverlay(QWidget *parent) : QWidget(parent) {
    setVisible(false);

    auto *outer = new QVBoxLayout(this);
    outer->addStretch();

    // 信息区：不透明 Window 底 + Mid 1px 直角边框（与 NavigatorPanel 边框风格一致）；
    // 颜色一律取 palette，不写死，浅/深主题跟随
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("N19InfoPanel"));
    panel->setStyleSheet(QStringLiteral("QFrame#N19InfoPanel { background-color: palette(window); "
                                        "border: 1px solid palette(mid); }"));
    auto *inner = new QVBoxLayout(panel);
    inner->setContentsMargins(24, 16, 24, 16);
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

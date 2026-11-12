#include "pch.h"

#include "ui/WindowTitleBar.h"

#include "Constants.h"
#include "ui/AppearanceManager.h"

// ——N156 文件内常量——
namespace {

constexpr int kNormalHeight = 32;    // Normal / Floating 标题栏高（逻辑 px）
constexpr int kWideButtonWidth = 46; // 三控制按钮与菜单按钮宽
constexpr int kMiniCloseWidth = 34;  // 迷你关闭钮宽
// 关闭钮 hover / 按下底色与 glyph 白（两主题同值，Windows 惯例）
const QColor kCloseHoverBackground(0xC4, 0x2B, 0x1C);
const QColor kCloseGlyphWhite(Qt::white);
// Segoe 字体 glyph 视觉边长（逻辑 px），与自绘 glyph 足迹（约 10px）对齐
constexpr int kSystemGlyphPx = 10;
// 主题 symbolic 渲染边长（逻辑 px）：symbolic 为 16 网格带内边距，按 16 渲染其
// 墨迹才约 10px；按 10 渲染会缩成极小
constexpr int kThemeGlyphPx = 16;

} // namespace

// ——按钮：QAbstractButton 子类，无 Q_OBJECT（不进 moc）；glyph 矢量自绘，颜色
// 绘制时现取调色板。NoFocus（N117）——控制行整排无焦点的同一口径
class WindowTitleBar::TitleBarButton : public QAbstractButton {
  public:
    enum class Kind { Minimize, Maximize, Close, Menu, ExitFullscreen };

    TitleBarButton(Kind kind, WindowTitleBar *bar)
        : QAbstractButton(bar), m_kind(kind), m_bar(bar) {
        setFocusPolicy(Qt::NoFocus);
    }

    QSize sizeHint() const override {
        return m_bar->m_mode == Mode::Mini ? QSize(kMiniCloseWidth, App::miniTitleBarHeight())
                                           : QSize(kWideButtonWidth, kNormalHeight);
    }

    // N156：原生 NC 路径的悬停态（该区域答 HTMAXBUTTON 后 Qt 收不到鼠标事件，
    // 悬停由主窗从 NC 消息回灌；绘制判据并入下）
    void setNativeHovered(bool hovered) {
        if (m_nativeHovered == hovered)
            return;
        m_nativeHovered = hovered;
        update();
    }
    bool nativeHovered() const { return m_nativeHovered; }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const QPalette pal = palette();
        // hover 现读指针下的控件：菜单弹出 / 关闭后 WA_UnderMouse 可能残留脏值，
        // 直接用 underMouse() 会让按钮停在悬停底色；原生 NC 路径另经回灌标志
        const bool hover =
            isDown() || m_nativeHovered || QApplication::widgetAt(QCursor::pos()) == this;
        QColor glyphColor = m_bar->titleForeground(pal);
        if (m_kind == Kind::Close) {
            // 关闭钮 hover / 按下走系统红底白叉（两主题同值）
            if (hover) {
                p.fillRect(rect(), kCloseHoverBackground);
                glyphColor = kCloseGlyphWhite;
            }
        } else if (hover) {
            p.fillRect(rect(), AppearanceManager::titleBarButtonHoverBackground(pal));
        }
        const QPointF c = QPointF(width(), height()) / 2.0;
        drawGlyph(p, c, glyphColor);
    }

    // 双击 / 右键就地消费、不向标题栏传播：否则双击按钮等同双击标题栏（最大化·
    // 还原）、按钮右键弹系统菜单（标题栏的 dblclick / contextMenu 处理器会接到）
    void mouseDoubleClickEvent(QMouseEvent *event) override { event->accept(); }
    void contextMenuEvent(QContextMenuEvent *event) override { event->accept(); }

  private:
    // glyph 来源（维护者裁决）：Windows 非 PUREQT 用 Segoe 标题字体（系统正宗、
    // 矢量）；Linux 非 PUREQT 用 freedesktop 主题 window-*-symbolic（着色）；其余
    // （PUREQT 全平台、取不到的回退）自绘。Fusion 的 SP_TitleBar* 是 MDI 子窗钮
    // （带外框 / 实心按钮），不能当裸 glyph，故不用
    void drawGlyph(QPainter &p, const QPointF &c, const QColor &color) {
#ifndef OXBOW_PUREQT
#ifdef Q_OS_WIN
        if (drawSegoeGlyph(p, c, color))
            return;
#endif
        if (drawThemeGlyph(p, c, color))
            return;
#endif
        drawSelfGlyph(p, c, color);
    }

    void drawSelfGlyph(QPainter &p, const QPointF &c, const QColor &color) {
        QPen pen(color, 1);
        pen.setCosmetic(true); // 1 设备像素线：高缩放下 glyph 仍 crisp
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        switch (m_kind) {
        case Kind::Minimize:
            p.drawLine(QPointF(c.x() - 5.0, c.y() + 3.5), QPointF(c.x() + 5.0, c.y() + 3.5));
            break;
        case Kind::Maximize:
        case Kind::ExitFullscreen:
            // 还原叠框：前框整框偏左下 + 后框右上两段可见边（避免与前框叠粗）。
            // 最大化钮随宿主态分档；退出全屏钮恒叠框——Windows 全屏期 Qt 记普通态，
            // 分档会显成最大化方框，与"退出全屏"语义相反
            if (m_kind == Kind::ExitFullscreen || m_bar->maximizeGlyphRestore()) {
                p.drawRect(QRectF(c.x() - 4.5, c.y() - 2.5, 8.0, 8.0));
                p.drawLine(QPointF(c.x() - 1.5, c.y() - 3.5), QPointF(c.x() + 4.5, c.y() - 3.5));
                p.drawLine(QPointF(c.x() + 4.5, c.y() - 3.5), QPointF(c.x() + 4.5, c.y() + 1.5));
            } else {
                p.drawRect(QRectF(c.x() - 5.0, c.y() - 5.0, 10.0, 10.0));
            }
            break;
        case Kind::Close:
            // 整数坐标 + 无 AA：1px 对角线光栅化对称（半像素致左长右短），与其余
            // glyph 同粗细；AA 全软边发虚、加粗又比邻钮重，均不取
            p.drawLine(QPointF(c.x() - 4.0, c.y() - 4.0), QPointF(c.x() + 4.0, c.y() + 4.0));
            p.drawLine(QPointF(c.x() + 4.0, c.y() - 4.0), QPointF(c.x() - 4.0, c.y() + 4.0));
            break;
        case Kind::Menu:
            for (int i = -1; i <= 1; ++i)
                p.drawLine(QPointF(c.x() - 6.0, c.y() + i * 4.0 + 0.5),
                           QPointF(c.x() + 6.0, c.y() + i * 4.0 + 0.5));
            break;
        }
    }

    // Linux 非 PUREQT：freedesktop 图标主题的 window-*-symbolic（桌面正宗单色
    // SVG、裸 glyph）。Qt 不自动着色（QIconLoaderEngine 原样渲染 SVG 填色），按
    // glyphColor 手动着色以跟主题 / 失活 / 关闭红底白叉；主题缺该名字或 Windows
    // 无系统主题时 null，回退自绘
    bool drawThemeGlyph(QPainter &p, const QPointF &c, const QColor &color) {
        const QString name = themeIconName();
        if (name.isEmpty())
            return false; // Menu 无主题名
        const QIcon icon = QIcon::fromTheme(name);
        if (icon.isNull())
            return false;
        const qreal dpr = p.device()->devicePixelRatio();
        const int px = qRound(kThemeGlyphPx * dpr);
        const QPixmap pm = icon.pixmap(px, px);
        QPixmap tinted(px, px);
        tinted.setDevicePixelRatio(dpr);
        tinted.fill(Qt::transparent);
        QPainter tp(&tinted);
        tp.setCompositionMode(QPainter::CompositionMode_Source);
        tp.fillRect(tinted.rect(), color);
        tp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        tp.drawPixmap(0, 0, pm);
        tp.end();
        p.drawPixmap(QPointF(c.x() - kThemeGlyphPx / 2.0, c.y() - kThemeGlyphPx / 2.0), tinted);
        return true;
    }
    QString themeIconName() const {
        switch (m_kind) {
        case Kind::Minimize:
            return QStringLiteral("window-minimize-symbolic");
        case Kind::Maximize:
            return m_bar->maximizeGlyphRestore() ? QStringLiteral("window-restore-symbolic")
                                                 : QStringLiteral("window-maximize-symbolic");
        case Kind::ExitFullscreen:
            return QStringLiteral("window-restore-symbolic");
        case Kind::Close:
            return QStringLiteral("window-close-symbolic");
        case Kind::Menu:
            return QString();
        }
        return QString();
    }

#if !defined(OXBOW_PUREQT) && defined(Q_OS_WIN)
    // Windows 系统标题字体 glyph（系统自绘标题钮同款）：Segoe Fluent Icons（Win11）
    // 回退 Segoe MDL2 Assets（Win10）。矢量、各 DPI 清晰、几何正宗
    bool drawSegoeGlyph(QPainter &p, const QPointF &c, const QColor &color) {
        const char32_t cp = systemCodepoint();
        if (!cp)
            return false;
        static const bool available = QFontDatabase::families().contains("Segoe Fluent Icons") ||
                                      QFontDatabase::families().contains("Segoe MDL2 Assets");
        if (!available)
            return false;
        QFont font;
        font.setFamilies({"Segoe Fluent Icons", "Segoe MDL2 Assets"});
        font.setPixelSize(kSystemGlyphPx);
        p.save();
        p.setFont(font);
        p.setPen(color);
        p.drawText(QRectF(c.x() - kSystemGlyphPx, c.y() - kSystemGlyphPx, 2.0 * kSystemGlyphPx,
                          2.0 * kSystemGlyphPx),
                   Qt::AlignCenter, QString::fromUcs4(&cp, 1));
        p.restore();
        return true;
    }
    char32_t systemCodepoint() const {
        switch (m_kind) {
        case Kind::Minimize:
            return 0xE921;
        case Kind::Maximize:
            return m_bar->maximizeGlyphRestore() ? 0xE923 : 0xE922;
        case Kind::ExitFullscreen:
            return 0xE923;
        case Kind::Close:
            return 0xE8BB;
        case Kind::Menu:
            return 0;
        }
        return 0;
    }
#endif

    Kind m_kind;
    WindowTitleBar *m_bar;
    bool m_nativeHovered = false;
};

WindowTitleBar::WindowTitleBar(QWidget *hostWindow, QWidget *parent)
    : QWidget(parent), m_hostWindow(hostWindow) {
    setFocusPolicy(Qt::NoFocus); // 不入 Tab 链（N117 基线）
    // 图标与 AboutDialog 同源：QFileIconProvider 对宿主 exe 取文件图标（构造期
    // 取一次；本组件构造一次、寿命随主窗，无需重取）
    m_icon = QFileIconProvider().icon(QFileInfo(QCoreApplication::applicationFilePath()));

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addStretch(1);
    // 顺序：菜单按钮在三控制按钮左侧；Floating 的退出全屏钮在关闭钮
    // 左侧——右起为关闭、退出全屏（关闭恒最右，Windows 标题栏惯例）
    m_menuButton = new TitleBarButton(TitleBarButton::Kind::Menu, this);
    m_menuButton->setToolTip(tr("Menu"));
    m_minButton = new TitleBarButton(TitleBarButton::Kind::Minimize, this);
    m_minButton->setToolTip(tr("Minimize"));
    m_maxButton = new TitleBarButton(TitleBarButton::Kind::Maximize, this);
    m_maxButton->setToolTip(tr("Maximize"));
    m_exitFsButton = new TitleBarButton(TitleBarButton::Kind::ExitFullscreen, this);
    m_exitFsButton->setToolTip(tr("Exit Full Screen"));
    m_closeButton = new TitleBarButton(TitleBarButton::Kind::Close, this);
    m_closeButton->setToolTip(tr("Close"));
    lay->addWidget(m_menuButton);
    lay->addWidget(m_minButton);
    lay->addWidget(m_maxButton);
    lay->addWidget(m_exitFsButton);
    lay->addWidget(m_closeButton);

    connect(m_minButton, &QAbstractButton::clicked, this, &WindowTitleBar::minimizeRequested);
    connect(m_maxButton, &QAbstractButton::clicked, this,
            &WindowTitleBar::maximizeRestoreRequested);
    connect(m_closeButton, &QAbstractButton::clicked, this, &WindowTitleBar::closeRequested);
    connect(m_menuButton, &QAbstractButton::clicked, this, [this] {
        emit menuRequested(QCursor::pos());
        // 菜单 exec 期间按钮收不到 enter/leave，关闭后主动重判一次悬停
        m_menuButton->update();
    });
    connect(m_exitFsButton, &QAbstractButton::clicked, this,
            &WindowTitleBar::maximizeRestoreRequested);

    // 标题与最大化 glyph 都在绘制时现读宿主——宿主侧状态 / 标题变化只会发事件，
    // 不发信号，过滤器装在显式宿主指针上（不装 window()，浮窗重挂后 window() 变）
    if (m_hostWindow)
        m_hostWindow->installEventFilter(this);
    applyModeLayout();
}

WindowTitleBar::~WindowTitleBar() {
    if (m_hostWindow)
        m_hostWindow->removeEventFilter(this);
}

void WindowTitleBar::setMode(Mode mode) {
    if (m_mode == mode)
        return;
    m_mode = mode;
    // 原生 NC 回灌态随形态切换作废：Mini / Floating 没有最大化钮，Normal 回来时
    // 也不该继承上一形态的悬停 / 按下
    m_maxButton->setNativeHovered(false);
    m_maxButton->setDown(false);
    applyModeLayout();
}

void WindowTitleBar::setMenuButtonVisible(bool visible) {
    if (m_menuButtonVisible == visible)
        return;
    m_menuButtonVisible = visible;
    applyModeLayout();
}

bool WindowTitleBar::maximizeGlyphRestore() const {
    return m_hostWindow && m_hostWindow->isMaximized();
}

QRect WindowTitleBar::maximizeButtonRect() const {
    if (m_mode != Mode::Normal || !m_maxButton->isVisible())
        return QRect();
    return m_maxButton->geometry();
}

QRect WindowTitleBar::minimizeButtonRect() const {
    if (m_mode != Mode::Normal || !m_minButton->isVisible())
        return QRect();
    return m_minButton->geometry();
}

QRect WindowTitleBar::closeButtonRect() const {
    return m_closeButton->isVisible() ? m_closeButton->geometry() : QRect();
}

void WindowTitleBar::setMaximizeButtonHovered(bool hovered) {
    m_maxButton->setNativeHovered(hovered);
}

void WindowTitleBar::setMaximizeButtonDown(bool down) { m_maxButton->setDown(down); }

bool WindowTitleBar::maximizeButtonDown() const { return m_maxButton->isDown(); }

void WindowTitleBar::applyModeLayout() {
    setFixedHeight(m_mode == Mode::Mini ? App::miniTitleBarHeight() : kNormalHeight);
    const bool normal = m_mode == Mode::Normal;
    m_minButton->setVisible(normal);
    m_maxButton->setVisible(normal);
    m_menuButton->setVisible(normal && m_menuButtonVisible);
    m_closeButton->setVisible(true);
    m_exitFsButton->setVisible(m_mode == Mode::Floating);
    updateMaximizeButtonTooltip();
    updateGeometry(); // 高度变化逐层传导主窗布局
    update();
}

void WindowTitleBar::updateMaximizeButtonTooltip() {
    if (m_mode == Mode::Normal)
        m_maxButton->setToolTip(maximizeGlyphRestore() ? tr("Restore") : tr("Maximize"));
}

QRect WindowTitleBar::titleRect() const {
    int buttons = 0;
    for (const TitleBarButton *b :
         {m_menuButton, m_minButton, m_maxButton, m_closeButton, m_exitFsButton}) {
        if (b->isVisible())
            buttons += b->width();
    }
    return QRect(0, 0, qMax(0, width() - buttons), height());
}

QRect WindowTitleBar::iconRect() const { return QRect(8, (height() - 16) / 2, 16, 16); }

QColor WindowTitleBar::titleForeground(const QPalette &p) const {
    const bool active = m_hostWindow && m_hostWindow->isActiveWindow();
    return active ? AppearanceManager::titleBarForeground(p)
                  : AppearanceManager::titleBarInactiveForeground(p);
}

void WindowTitleBar::paintEvent(QPaintEvent *) {
    // 背景不画——容器透明，透出主窗 paintEvent 的整窗底色
    QPainter p(this);
    const int left = m_mode == Mode::Normal ? 8 + 16 + 8 : 8; // Normal 先画图标
    if (m_mode == Mode::Normal && !m_icon.isNull()) {
        const QRect ir = iconRect();
        m_icon.paint(&p, ir.x(), ir.y(), ir.width(), ir.height());
    }
    const QString title = m_hostWindow ? m_hostWindow->windowTitle() : QString();
    if (title.isEmpty())
        return;
    const QRect tr = titleRect().adjusted(left, 0, -8, 0);
    if (tr.width() <= 0)
        return;
    const QFontMetrics fm(font());
    p.setPen(titleForeground(palette()));
    p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft,
               fm.elidedText(title, Qt::ElideRight, tr.width()));
}

void WindowTitleBar::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(event);
    // 同视频区 N117 口径：按下即清焦点（方向键回到全局音量模式）
    if (auto *fw = QApplication::focusWidget())
        fw->clearFocus();
    // 宿主图标：按下即弹系统菜单（Windows 标题栏惯例），不参与拖动
    if (m_mode == Mode::Normal && iconRect().contains(event->position().toPoint())) {
        event->accept();
        emit systemMenuRequested(mapToGlobal(iconRect().bottomLeft()));
        return;
    }
    m_dragArmed = true;
    m_dragSent = false;
    m_pressGlobalPos = event->globalPosition().toPoint();
    event->accept();
}

void WindowTitleBar::mouseMoveEvent(QMouseEvent *event) {
    // 只认标题栏自己按下的拖动：子按钮按下后把 move 让出来时不拖窗
    if (!m_dragArmed || !(event->buttons() & Qt::LeftButton))
        return QWidget::mouseMoveEvent(event);
    const QPoint gp = event->globalPosition().toPoint();
    if (!m_dragSent) {
        if ((gp - m_pressGlobalPos).manhattanLength() <= QApplication::startDragDistance())
            return;
        m_dragSent = true;
        emit dragMoveRequested(m_pressGlobalPos); // 首发带按下点（主窗起点锚定用它）
    }
    emit dragMoveUpdated(gp);
}

void WindowTitleBar::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return QWidget::mouseReleaseEvent(event);
    if (m_dragSent)
        emit dragMoveFinished();
    m_dragArmed = false;
    m_dragSent = false;
    m_pressGlobalPos = QPoint();
}

void WindowTitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
    // 仅 Normal 态双击最大化·还原；Mini 无动作、Floating 不接双击（退出走按钮）
    if (event->button() == Qt::LeftButton && m_mode == Mode::Normal) {
        if (iconRect().contains(event->position().toPoint())) {
            emit closeRequested();
            return;
        }
        emit maximizeRestoreRequested();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void WindowTitleBar::contextMenuEvent(QContextMenuEvent *event) {
    emit systemMenuRequested(event->globalPos());
}

bool WindowTitleBar::eventFilter(QObject *watched, QEvent *event) {
    // 宿主窗标题 / 窗口态变化 → 整条重画（标题与 glyph 都是绘制时现读）；
    // 最大化按钮 tooltip 顺带按窗口态刷新（tooltip 是缓存文案，无法现取）
    if (watched == m_hostWindow) {
        const QEvent::Type type = event->type();
        if (type == QEvent::WindowActivate || type == QEvent::WindowDeactivate ||
            type == QEvent::ActivationChange)
            update(); // 取色在 paintEvent 现读宿主 isActiveWindow()，此处只催重画
        if (type == QEvent::WindowTitleChange || type == QEvent::WindowStateChange) {
            if (type == QEvent::WindowStateChange)
                updateMaximizeButtonTooltip();
            update();
        }
    }
    return QWidget::eventFilter(watched, event);
}

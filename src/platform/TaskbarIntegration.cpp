#include "pch.h"

#include "TaskbarIntegration.h"

#include "Constants.h"
#include "Logging.h"
#include "TaskbarState.h"
#include "mpvbridge/MpvController.h"
#include "ui/ThemedIconEngine.h"

namespace {

// 工具条按钮 ID：只在本窗口内唯一，点击时与 THBN_CLICKED 一起回来
enum ThumbButtonId : UINT {
    ThumbPrev = 1,
    ThumbPlayPause = 2,
    ThumbNext = 3,
};

constexpr int kBaseIconPx = 16;          // 100% DPI 下的系统小图标边长
constexpr int kProgressThrottleMs = 200; // time-pos 约 99 Hz，进度按此节流

HWND hostHwnd(QWidget *window) {
    return window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
}

int iconPixels(QWidget *window) {
    return qMax(kBaseIconPx, qRound(kBaseIconPx * (window ? window->devicePixelRatio() : 1.0)));
}

// 系统深浅色：读注册表 AppsUseLightTheme（0 = 深色）。不用 QStyleHints::colorScheme()——
// 本仓 AppearanceManager 会按应用配色改写它，而任务栏预览由系统按系统主题绘制
bool systemPrefersDark() {
    DWORD appsUseLight = 1;
    DWORD size = sizeof(appsUseLight);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsUseLight, &size);
    return status == ERROR_SUCCESS && appsUseLight == 0;
}

QColor glyphTint(bool dark) { return dark ? QColor(Qt::white) : QColor(32, 32, 32); }

// 覆盖徽标：圆角方底 + glyph，画在透明底上；底色与字色都随系统深浅色
QImage renderBadge(bool paused, int px, bool dark) {
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(dark ? QColor(18, 18, 18, 230) : QColor(248, 248, 248, 240));
    painter.setPen(dark ? QPen(Qt::NoPen) : QPen(QColor(0, 0, 0, 48), 1.0));
    const qreal radius = px * 0.22;
    painter.drawRoundedRect(QRectF(0.5, 0.5, px - 1.0, px - 1.0), radius, radius);

    const int glyph = qRound(px * 0.70);
    const QRect target((px - glyph) / 2, (px - glyph) / 2, glyph, glyph);
    Ui::themedIcon(App::iconPath(paused ? App::iconPause() : App::iconPlay()), glyphTint(dark))
        .paint(&painter, target, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
    return image;
}

// 工具条图标：透明底 + 按系统深浅色染色的 glyph
QImage renderGlyph(const QString &iconKey, int px, const QColor &tint) {
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    Ui::themedIcon(App::iconPath(iconKey), tint)
        .paint(&painter, image.rect(), Qt::AlignCenter, QIcon::Normal, QIcon::Off);
    return image;
}

// QImage → HICON：32bpp 顶朝下 DIB 存颜色，1bpp 底朝上 DIB 存 AND 掩码
// （掩码按 alpha 阈值生成，兼容不吃 alpha 的旧路径）
HICON createHIcon(const QImage &source) {
    if (source.isNull())
        return nullptr;
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const int w = image.width();
    const int h = image.height();
    if (w <= 0 || h <= 0)
        return nullptr;

    HDC screen = GetDC(nullptr);

    BITMAPV5HEADER colorHeader{};
    colorHeader.bV5Size = sizeof(colorHeader);
    colorHeader.bV5Width = w;
    colorHeader.bV5Height = -h; // 负高 = 顶朝下，与 QImage 行序一致
    colorHeader.bV5Planes = 1;
    colorHeader.bV5BitCount = 32;
    colorHeader.bV5Compression = BI_BITFIELDS;
    colorHeader.bV5RedMask = 0x00FF0000;
    colorHeader.bV5GreenMask = 0x0000FF00;
    colorHeader.bV5BlueMask = 0x000000FF;
    colorHeader.bV5AlphaMask = 0xFF000000;
    void *colorBits = nullptr;
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO *>(&colorHeader),
                                     DIB_RGB_COLORS, &colorBits, nullptr, 0);

    BITMAPINFO maskHeader{};
    maskHeader.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    maskHeader.bmiHeader.biWidth = w;
    maskHeader.bmiHeader.biHeight = h; // 正高 = 底朝上，下面按反序填行
    maskHeader.bmiHeader.biPlanes = 1;
    maskHeader.bmiHeader.biBitCount = 1;
    maskHeader.bmiHeader.biCompression = BI_RGB;
    void *maskBits = nullptr;
    HBITMAP mask = CreateDIBSection(screen, &maskHeader, DIB_RGB_COLORS, &maskBits, nullptr, 0);

    ReleaseDC(nullptr, screen);

    if (!color || !colorBits || !mask || !maskBits) {
        if (color)
            DeleteObject(color);
        if (mask)
            DeleteObject(mask);
        return nullptr;
    }

    memcpy(colorBits, image.constBits(), size_t(image.bytesPerLine()) * size_t(h));

    const int maskStride = ((w + 31) / 32) * 4; // 1bpp 行按 4 字节对齐
    memset(maskBits, 0, size_t(maskStride) * size_t(h));
    for (int y = 0; y < h; ++y) {
        const uchar *row = image.constScanLine(y);
        auto *out = static_cast<uchar *>(maskBits) + size_t(h - 1 - y) * size_t(maskStride);
        for (int x = 0; x < w; ++x) {
            if (row[x * 4 + 3] < 128) // AND 掩码：1 = 透明
                out[x / 8] |= uchar(0x80 >> (x % 8));
        }
    }

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmMask = mask;
    info.hbmColor = color;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void fillButton(THUMBBUTTON &button, UINT id, HICON icon, const QString &tip, bool enabled) {
    button = THUMBBUTTON{};
    button.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    button.iId = id;
    button.hIcon = icon;
    button.dwFlags = enabled ? THBF_ENABLED : THBF_DISABLED;
    const std::wstring text = tip.toStdWString();
    wcsncpy_s(button.szTip, text.c_str(), _TRUNCATE);
}

void destroyOverlayIcons(TaskbarIntegrationImpl &d);
void destroyThumbIcons(TaskbarIntegrationImpl &d);

} // namespace

struct TaskbarIntegrationImpl {
    MpvController *controller = nullptr;
    // QPointer：宿主可能先于本组件销毁，析构时不能摸悬空指针
    QPointer<QWidget> host;

    ITaskbarList3 *taskbar = nullptr;
    bool comInitialized = false;
    bool disabled = false;
    // 收到 TaskbarButtonCreated 之前不碰任何 ITaskbarList3 方法（MSDN 硬要求）
    UINT taskbarCreatedMsg = 0;
    bool taskbarReady = false;

    // 四项播放事实（信号推送 + 构造期现读的初值）
    bool idle = true;
    bool paused = false;
    double timePos = 0.0;
    double duration = 0.0;
    QString durationPath;

    // 下发去重与节流
    int appliedProgress = -1; // TBPF_*；-1 表示尚未下发
    bool overlayShown = false;
    bool overlayPaused = false;
    qint64 lastProgressMs = -1000000;
    QElapsedTimer clock;

    // 工具条归属（按 HWND 记账：重建后新窗口必须重加）
    HWND buttonsHwnd = nullptr;
    HWND buttonsFailedHwnd = nullptr;

    HICON overlayIcons[2] = {nullptr, nullptr}; // 0 = 播放，1 = 暂停
    int overlayPx = 0;
    bool overlayDark = false;

    HICON thumbPrev = nullptr;
    HICON thumbNext = nullptr;
    HICON thumbPlay = nullptr;
    HICON thumbPause = nullptr;
    int thumbPx = 0;
    bool thumbDark = false;
};

namespace {

void destroyOverlayIcons(TaskbarIntegrationImpl &d) {
    for (HICON &icon : d.overlayIcons) {
        if (icon)
            DestroyIcon(icon);
        icon = nullptr;
    }
    d.overlayPx = 0;
}

void destroyThumbIcons(TaskbarIntegrationImpl &d) {
    if (d.thumbPrev)
        DestroyIcon(d.thumbPrev);
    if (d.thumbNext)
        DestroyIcon(d.thumbNext);
    if (d.thumbPlay)
        DestroyIcon(d.thumbPlay);
    if (d.thumbPause)
        DestroyIcon(d.thumbPause);
    d.thumbPrev = d.thumbNext = d.thumbPlay = d.thumbPause = nullptr;
    d.thumbPx = 0;
}

} // namespace

TaskbarIntegration::TaskbarIntegration(MpvController *controller, QWidget *hostWindow)
    : QObject(hostWindow), m_impl(std::make_unique<TaskbarIntegrationImpl>()) {
    auto &d = *m_impl;
    d.controller = controller;
    d.host = hostWindow;
    d.clock.start();

    // 任务栏按钮由系统异步建立；收到 TaskbarButtonCreated 之前调 ITaskbarList3 会被
    // 静默丢弃，故先注册消息、装过滤器，收到后再落首个快照
    d.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarButtonCreated");

    hostWindow->installEventFilter(this);
    qApp->installNativeEventFilter(this);

    // COM：Qt 的 Windows 平台插件通常已按 STA 初始化（此时返回 S_FALSE）；若宿主已按
    // 别的套间初始化，RPC_E_CHANGED_MODE 仍可正常使用，只是本类不配对反初始化
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(co))
        d.comInitialized = true;
    else if (co != RPC_E_CHANGED_MODE)
        d.disabled = true;

    if (!d.disabled) {
        const HRESULT created = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(&d.taskbar));
        if (FAILED(created) || !d.taskbar) {
            d.taskbar = nullptr;
            d.disabled = true;
        } else if (FAILED(d.taskbar->HrInit())) {
            d.taskbar->Release();
            d.taskbar = nullptr;
            d.disabled = true;
        }
    }
    if (d.disabled)
        OXBOW_LOG_WARNING() << "任务栏接口不可用，任务栏集成停用";

    // 初值：构造早于事件线程启动，推送未必到过，四项事实各现读一次
    d.idle = controller->propertyFlag(QStringLiteral("idle-active"), true);
    d.paused = controller->propertyFlag(QStringLiteral("pause"), false);
    d.duration = controller->propertyDouble(QStringLiteral("duration"), 0.0);
    d.timePos = controller->propertyDouble(QStringLiteral("time-pos"), 0.0);
    d.durationPath = controller->currentPlayingPath();

    connect(controller, &MpvController::idleActive, this, [this](bool active) {
        m_impl->idle = active;
        reapplyAll();
    });
    connect(controller, &MpvController::pauseChanged, this, [this](bool paused) {
        m_impl->paused = paused;
        reapplyAll();
    });
    connect(controller, &MpvController::durationChanged, this,
            [this](const QString &path, double seconds) {
                m_impl->durationPath = path;
                m_impl->duration = seconds;
                applyProgress(true);
            });
    connect(controller, &MpvController::timePosChanged, this, [this](double seconds) {
        m_impl->timePos = seconds;
        applyProgress(false);
    });
    connect(controller, &MpvController::fileLoaded, this, [this](const QString &path) {
        // 新文件从 0 起算；时长只在不属于该文件时清掉——时长推送早于 FILE_LOADED 时
        // 不能把刚拿到的值抹掉，晚到的上一文件时长也不该串进新文件
        m_impl->timePos = 0.0;
        if (m_impl->durationPath != path)
            m_impl->duration = 0.0;
        applyProgress(true);
    });
}

TaskbarIntegration::~TaskbarIntegration() {
    if (qApp)
        qApp->removeNativeEventFilter(this);

    auto &d = *m_impl;
    if (d.host)
        d.host->removeEventFilter(this);

    if (d.taskbar)
        d.taskbar->Release();
    for (HICON &icon : d.overlayIcons) {
        if (icon)
            DestroyIcon(icon);
        icon = nullptr;
    }
    destroyThumbIcons(d);
    if (d.comInitialized)
        CoUninitialize();
}

void TaskbarIntegration::reapplyAll() {
    applyProgress(true);
    applyOverlay(true);
    updateButtons();
}

void TaskbarIntegration::applyProgress(bool force) {
    auto &d = *m_impl;
    if (d.disabled || !d.taskbar || !d.taskbarReady)
        return;

    const TaskbarState state = deriveTaskbarState(d.idle, d.paused, d.timePos, d.duration);
    const int target = state.progress == TaskbarProgress::Paused   ? TBPF_PAUSED
                       : state.progress == TaskbarProgress::Normal ? TBPF_NORMAL
                                                                   : TBPF_NOPROGRESS;
    const bool stateChanged = target != d.appliedProgress;
    if (!stateChanged && !force && d.clock.elapsed() - d.lastProgressMs < kProgressThrottleMs)
        return;

    HWND hwnd = hostHwnd(d.host);
    if (!hwnd)
        return;

    if (stateChanged) {
        d.taskbar->SetProgressState(hwnd, static_cast<TBPFLAG>(target));
        d.appliedProgress = target;
    }
    if (state.progress == TaskbarProgress::None)
        return;

    d.lastProgressMs = d.clock.elapsed();
    // 比例只在 deriveTaskbarState 一处夹到 [0,1]（含 timePos 的负值、越界与非有限值），
    // 这里只换算成毫秒；total 下限 1，防亚毫秒时长把值域压成 0
    const qint64 total = qMax<qint64>(1, qRound64(d.duration * 1000.0));
    const qint64 pos = qRound64(state.fraction * double(total));
    d.taskbar->SetProgressValue(hwnd, ULONGLONG(pos), ULONGLONG(total));
}

void TaskbarIntegration::applyOverlay(bool force) {
    auto &d = *m_impl;
    if (d.disabled || !d.taskbar || !d.taskbarReady)
        return;

    HWND hwnd = hostHwnd(d.host);
    if (!hwnd)
        return;

    const TaskbarState state = deriveTaskbarState(d.idle, d.paused, d.timePos, d.duration);
    if (!state.showBadge) {
        if (d.overlayShown) {
            d.taskbar->SetOverlayIcon(hwnd, nullptr, nullptr);
            d.overlayShown = false;
        }
        return;
    }

    const int px = iconPixels(d.host);
    const bool dark = systemPrefersDark();
    const bool metricsChanged = px != d.overlayPx || dark != d.overlayDark;
    const int index = state.badgePaused ? 1 : 0;
    if (metricsChanged || !d.overlayIcons[index]) {
        // 先建新、成功再毁旧：建不出新徽标时上一批原样留着，不让 shell 握着已销毁的句柄
        HICON fresh = createHIcon(renderBadge(state.badgePaused, px, dark));
        if (!fresh)
            return;
        if (metricsChanged)
            destroyOverlayIcons(d); // DPI 或系统深浅色变了，整批过期
        d.overlayIcons[index] = fresh;
        d.overlayPx = px;
        d.overlayDark = dark;
        // 句柄活到下一次替换为止，不依赖"系统是否拷贝图标"的实现细节；换过即重发
        d.overlayShown = false;
    }

    if (!force && d.overlayShown && d.overlayPaused == state.badgePaused)
        return;

    const std::wstring description =
        (state.badgePaused ? tr("Paused") : tr("Playing")).toStdWString();
    d.taskbar->SetOverlayIcon(hwnd, d.overlayIcons[index], description.c_str());
    d.overlayShown = true;
    d.overlayPaused = state.badgePaused;
}

void TaskbarIntegration::buildButtonIcons() {
    auto &d = *m_impl;
    const int px = iconPixels(d.host);
    const bool dark = systemPrefersDark();
    if (d.thumbPx == px && d.thumbDark == dark && d.thumbPrev && d.thumbNext && d.thumbPlay &&
        d.thumbPause)
        return;

    const QColor tint = glyphTint(dark);
    HICON prev = createHIcon(renderGlyph(App::iconSkipBackward(), px, tint));
    HICON next = createHIcon(renderGlyph(App::iconSkipForward(), px, tint));
    HICON play = createHIcon(renderGlyph(App::iconPlay(), px, tint));
    HICON pause = createHIcon(renderGlyph(App::iconPause(), px, tint));
    if (!prev || !next || !play || !pause) {
        // 建不出新的：旧图标与旧尺寸记账原样留着（下一次再试），不让工具条握着已销毁句柄
        if (prev)
            DestroyIcon(prev);
        if (next)
            DestroyIcon(next);
        if (play)
            DestroyIcon(play);
        if (pause)
            DestroyIcon(pause);
        return;
    }

    // 旧图标同步换下：本函数与随后的 ThumbBarUpdateButtons 之间不跑事件循环，
    // shell 拿不到已销毁的句柄
    destroyThumbIcons(d);
    d.thumbPx = px;
    d.thumbDark = dark;
    d.thumbPrev = prev;
    d.thumbNext = next;
    d.thumbPlay = play;
    d.thumbPause = pause;
}

void TaskbarIntegration::ensureButtons() {
    auto &d = *m_impl;
    if (d.disabled || !d.taskbar || !d.taskbarReady || !d.host || !d.host->isVisible())
        return;

    HWND hwnd = hostHwnd(d.host);
    if (!hwnd || hwnd == d.buttonsHwnd || hwnd == d.buttonsFailedHwnd)
        return;

    buildButtonIcons();
    if (!d.thumbPrev || !d.thumbNext || !d.thumbPlay || !d.thumbPause) {
        d.buttonsFailedHwnd = hwnd;
        OXBOW_LOG_DEBUG() << "任务栏工具条图标不可用，工具条停用";
        return;
    }

    const TaskbarState state = deriveTaskbarState(d.idle, d.paused, d.timePos, d.duration);
    const bool playIcon = d.idle || d.paused;
    THUMBBUTTON buttons[3];
    fillButton(buttons[0], ThumbPrev, d.thumbPrev, tr("Previous"), state.buttonsEnabled);
    fillButton(buttons[1], ThumbPlayPause, playIcon ? d.thumbPlay : d.thumbPause,
               playIcon ? tr("Play") : tr("Pause"), state.buttonsEnabled);
    fillButton(buttons[2], ThumbNext, d.thumbNext, tr("Next"), state.buttonsEnabled);

    const HRESULT hr = d.taskbar->ThumbBarAddButtons(hwnd, 3, buttons);
    if (SUCCEEDED(hr)) {
        d.buttonsHwnd = hwnd;
    } else {
        d.buttonsFailedHwnd = hwnd;
        OXBOW_LOG_DEBUG() << "任务栏工具条添加失败，本窗口不显示工具条";
    }
}

void TaskbarIntegration::updateButtons() {
    auto &d = *m_impl;
    if (d.disabled || !d.taskbar || !d.taskbarReady)
        return;

    HWND hwnd = hostHwnd(d.host);
    if (!hwnd || hwnd != d.buttonsHwnd)
        return;

    buildButtonIcons();
    if (!d.thumbPrev || !d.thumbNext || !d.thumbPlay || !d.thumbPause)
        return;

    const TaskbarState state = deriveTaskbarState(d.idle, d.paused, d.timePos, d.duration);
    const bool playIcon = d.idle || d.paused;
    THUMBBUTTON buttons[3];
    fillButton(buttons[0], ThumbPrev, d.thumbPrev, tr("Previous"), state.buttonsEnabled);
    fillButton(buttons[1], ThumbPlayPause, playIcon ? d.thumbPlay : d.thumbPause,
               playIcon ? tr("Play") : tr("Pause"), state.buttonsEnabled);
    fillButton(buttons[2], ThumbNext, d.thumbNext, tr("Next"), state.buttonsEnabled);
    d.taskbar->ThumbBarUpdateButtons(hwnd, 3, buttons);
}

void TaskbarIntegration::dropIcons() {
    auto &d = *m_impl;
    if (d.taskbar && d.overlayShown) {
        if (HWND hwnd = hostHwnd(d.host))
            d.taskbar->SetOverlayIcon(hwnd, nullptr, nullptr);
        d.overlayShown = false;
    }
    destroyOverlayIcons(d);
    destroyThumbIcons(d);
}

bool TaskbarIntegration::eventFilter(QObject *watched, QEvent *event) {
    auto &d = *m_impl;
    if (watched == d.host) {
        switch (event->type()) {
        case QEvent::WinIdChange:
            // 平台窗口重建：旧按钮随旧窗口消失，等新窗口的 TaskbarButtonCreated 再整体落
            OXBOW_LOG_DEBUG() << "窗口句柄重建，等新任务栏按钮";
            d.taskbarReady = false;
            d.appliedProgress = -1;
            d.overlayShown = false;
            d.buttonsHwnd = nullptr;
            d.buttonsFailedHwnd = nullptr;
            break;
        case QEvent::Show:
            // 补加工具条并整体重放：WinIdChange 落在窗口隐藏期，任务栏按钮尚未建立，
            // 覆盖与进度可能被 shell 丢弃，可见后补一次才稳（幂等，代价几次 COM 调用）
            ensureButtons();
            reapplyAll();
            break;
        case QEvent::DevicePixelRatioChange:
            dropIcons();
            applyOverlay(true);
            updateButtons();
            break;
        default:
            break;
        }
    }
    return false;
}

bool TaskbarIntegration::nativeEventFilter(const QByteArray &eventType, void *message,
                                           qintptr *result) {
    Q_UNUSED(result);
    auto &d = *m_impl;
    if (d.disabled || !d.controller || eventType != "windows_generic_MSG" || !message)
        return false;

    auto *msg = static_cast<MSG *>(message);
    // 先按消息号筛掉无关消息：本过滤器对进程内每条原生消息都跑（视频区指针事件尤密），
    // 不能每条都先去解宿主句柄
    const bool buttonClicked = msg->message == WM_COMMAND && HIWORD(msg->wParam) == THBN_CLICKED;
    const bool themeChanged = msg->message == WM_SETTINGCHANGE && msg->lParam != 0;
    const bool buttonReady = d.taskbarCreatedMsg != 0 && msg->message == d.taskbarCreatedMsg;
    if (!buttonClicked && !themeChanged && !buttonReady)
        return false;

    HWND hwnd = hostHwnd(d.host);
    if (!hwnd || msg->hwnd != hwnd)
        return false;

    // 任务栏按钮就绪：整体落一次当前状态；在此之前调 ITaskbarList3 会被 shell 丢弃
    if (buttonReady) {
        OXBOW_LOG_DEBUG().noquote() << QStringLiteral("任务栏按钮就绪 hwnd=0x%1")
                                           .arg(static_cast<qulonglong>(quintptr(hwnd)), 0, 16);
        d.taskbarReady = true;
        d.appliedProgress = -1;
        d.overlayShown = false;
        d.buttonsHwnd = nullptr;
        d.buttonsFailedHwnd = nullptr;
        reapplyAll();
        ensureButtons();
        return false;
    }

    // 系统深浅色切换：预览工具条与覆盖徽标都要换色
    if (themeChanged) {
        if (QString::compare(QString::fromWCharArray(reinterpret_cast<LPCWSTR>(msg->lParam)),
                             QStringLiteral("ImmersiveColorSet"), Qt::CaseInsensitive) == 0) {
            dropIcons();
            applyOverlay(true);
            updateButtons();
        }
        return false;
    }

    // 直发 mpv 异步命令：不经主窗、不读列表；主窗自己的推送连接会跟随状态
    switch (LOWORD(msg->wParam)) {
    case ThumbPrev:
        d.controller->commandAsync({QStringLiteral("playlist-prev")});
        d.controller->commandAsync(
            {QStringLiteral("set"), QStringLiteral("pause"), QStringLiteral("no")});
        break;
    case ThumbPlayPause:
        d.controller->commandAsync({QStringLiteral("cycle"), QStringLiteral("pause")});
        break;
    case ThumbNext:
        d.controller->commandAsync({QStringLiteral("playlist-next")});
        d.controller->commandAsync(
            {QStringLiteral("set"), QStringLiteral("pause"), QStringLiteral("no")});
        break;
    default:
        break;
    }
    return false;
}

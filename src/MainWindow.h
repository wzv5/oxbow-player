#pragma once

#include "pch.h"

#include "mpvbridge/MpvController.h"

#include "ui/AppearanceManager.h" // N115
#include "ui/MenuSpec.h"          // N119：菜单描述表

#include "Constants.h" // ini 默认值单点（成员初值引用访问器）

// N54：两处接线要用到嵌套类型（PlaylistWidget::LevelJump / JumpFailure、
// BookmarkManager::Origin），嵌套类型必须有完整类型可见
#include "BookmarkManager.h"
#include "playlist/PlaylistWidget.h"

#ifdef Q_OS_WIN
#include "platform/WinFramelessMessages.h" // N156：Windows 原生消息层（Host 接口基类）
#endif

class NavigatorPanel;
class SubtitleBrowserDialog;
class SubtitleExtractor;
class AiSubtitleJob;           // N146：AI 字幕生成任务（主窗持有）
class AiSubtitleDialog;        // N146：生成对话框，按需创建、关闭即销毁
struct AiSubtitleStartContext; // N146：发车复核结果，定义在 ai/AiSubtitleJob.h
class BookmarkEditorDialog;
class ShortcutsDialog;
class CustomToolManager; // N134：自定义工具单表（定义于 CustomToolManager.h）
class ChapterSlider;
class MarkAggregator;            // N15：章节+书签标记聚合器（定义于 MainWindow.cpp）
class PlaylistRegistry;          // N57：列表注册表（装配顶点创建并持有，注入 PlaylistWidget）
class PopupCloseWatch;           // N66：弹层关闭观察器（定义于 MainWindow.cpp）
class CursorAutoHider;           // N67：播放时视频区光标自动隐藏（定义于 MainWindow.cpp）
class StreamRelay;               // N52：mpv 自定义协议适配层（定义于 StreamRelay.h）
class PlaybackReporterRegistry;  // N74：报告者注册表（定义于 PlaybackReporterRegistry.h）
class PlaybackReportDriver;      // N74：上报驱动器（定义于 PlaybackReportDriver.h）
class ThumbEngine;               // N107：缩略图第二 core 引擎（定义于 thumb/ThumbEngine.h）
class ThumbWindow;               // N107：缩略图宿主窗（定义于 ui/ThumbWindow.h）
class WindowTitleBar;            // N156：主窗自绘标题栏（定义于 ui/WindowTitleBar.h）
class ThumbHoverWatch;           // N107：进度条悬停缩略图会话机（定义于 MainWindow.cpp）
class OsdController;             // N113：三形态 OSD 控制器（定义于 ui/OsdController.h）
class CacheStatusOsd;            // N153：缓冲状态 OSD 控制器（定义于 ui/CacheStatusOsd.h）
class DependencyInstallJob;      // N150：可选依赖安装作业（主窗持有，应用级寿命）
class DependencyInstallerWizard; // N150：安装向导（按需创建，QPointer 兜底）
class UpdateChecker;             // N151：检查更新后端（主窗子对象）
class UpdateDialog;              // N151：检查更新对话框（按需创建，QPointer 兜底）

class MainWindow : public QMainWindow
#ifdef Q_OS_WIN
    ,
                   public WinFramelessMessages::Host
#endif
{
    Q_OBJECT
    // N156：子控件边缘的缩放热区与光标转发（qApp 级过滤器；Windows 边缘由系统
    // 命中测试先行接管，此过滤器只在 Linux / PUREQT 形态实际生效）
    class EdgeGestureWatch;

  public:
    // N52：relay 由 plugin.cpp 构造并 install 后传入，构造期收下（不在 show()
    // 之后才装配——列表构造期就要能从注册表取到 relay）；本类仅持有与转交
    // PlaylistWidget（provide 进注册表），不做其他使用。
    // appearance 由 plugin.cpp 构造并完成初始 apply 后注入（非拥有），本类只做
    // 运行期 ini 变更的套用
    explicit MainWindow(MpvController *controller, AppearanceManager *appearance,
                        std::shared_ptr<StreamRelay> relay = nullptr, QWidget *parent = nullptr);
    // N107：退出序兜底——先阻塞拆缩略图 core（N103 销毁顺序）再停引擎线程；
    // 常规退出走 onMpvShutdown 的同款收口，本处覆盖不经它的销毁路径
    ~MainWindow() override;

    // 视频显示区域（mpv 通过 wid 嵌入的目标窗口）
    QWidget *videoWidget() const { return m_videoWidget; }

  protected:
    // N156：整窗底色与 1px 边框（来源分流见 2.4：圆角生效走 DWM 组合器描边，
    // 未生效 / X11 自绘直角描边）
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // N40 变更：还原最大化时按当前视频重新适配（跟随全屏退出重适配逻辑）
    void changeEvent(QEvent *event) override;
    // A20：启动几何收口的一次性触发点（首次 show 时外框几何与所在屏才可靠）
    void showEvent(QShowEvent *event) override;
    // N3：占位符几何跟随 + 视频区单击/双击分流
    bool eventFilter(QObject *watched, QEvent *event) override;
    // N90：ShortcutOverride 否决的控件链侧兜底（常态由 eventFilter 的窗口层先收下，
    // 本覆写只补过滤器随窗口重建而缺位的瞬间）
    bool event(QEvent *event) override;
    // N5：空白区域点击清焦（未被交互控件消费的按下事件冒泡至此）
    void mousePressEvent(QMouseEvent *event) override;
    // N87：插件未认领按键的兜底出口（翻译得出 mpv 键名即异步注入 keypress）
    void keyPressEvent(QKeyEvent *event) override;
    // N95：让位期输入法承接——组字串维护守卫标志、提交串逐码点转发。事件只到达
    // focusObject（让位期焦点归空即主窗本体），插件自己的输入框不受影响
    void inputMethodEvent(QInputMethodEvent *event) override;
    // N95：候选窗 / 组字窗位置单一应答点（视频区左上角，按当前变换折回逆变）与启用态
    // 应答，其余查询回落基类
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    // N156：Windows 原生消息的薄转发——NC / 命中 / 光标 / mini 原生循环的回答收在
    // WinFramelessMessages（平台胶合层），本覆写只留 WM_APPCOMMAND（N137 媒体键）
    // 与转发；非 Windows 形态整体落基线
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

  private slots:
    void openFiles();
    void openUrls(); // N131
    void togglePause();
    void stop();
    void playlistPrev();
    void playlistNext();
    void seekReleased();
    void setPlaylistVisible(bool visible);
    void setNavigatorVisible(bool visible);
    void ensureSettingsDialog();   // R3：按需创建/唤起设置对话框（非模态）
    void ensureShortcutsDialog();  // N86：快捷键编辑对话框（非模态、按需创建）
    void ensureMpvBindingDialog(); // N89：mpv 键位编辑对话框（非模态、按需创建）
    void customizeTools();         // N134
    void applyShortcutsLive();     // N86：落盘成功后的即时生效（从 ini 重载键位）
    // N90：console 开关驱动的键盘让位。开：一次性焦点收口到主窗并装窗口级过滤器；
    // 关：拆过滤器即完。键位表全程不动，让位期内键盘归属由焦点位置决定
    void onConsoleOpenChanged(bool open);
    void copyScreenshotToClipboard(); // N78：截图 → 剪贴板（Ctrl+C 与文件菜单共用）
    void saveScreenshotToFile();      // N78：截图另存为（Ctrl+E 与文件菜单共用）
    // N6：速度控制（读改写 speed 属性，钳制 0.25~8）
    void changeSpeed(double delta);
    void resetSpeed();
    // N38：静音状态联动单一出口（菜单勾选 + 图标按钮同一处刷新，不播报 OSD）
    void applyMuteState(bool muted);
    // 统一播放入口：整表载入 → 跳转 index → seekTo>0 时经挂起机制恢复进度。
    // N56 §2.7：播放链路传实体表——下发的 loadfile 取 playTargets(tab, entities)，即喂
    // mpv 前最后一刻映射的播放形态（与回显同源同形；N65 起 PlayTarget 整体含逐文件
    // 选项）。
    // N60 §2.4：本入口是四个喂入点之一，整批由 PlaylistWidget::PushSession 守卫
    // 开闭推送会话（会话期内 playlist 回显吞掉，终点由屏障 reply 对账）。
    // N30：keepPaused 为真时载入后保持暂停（四步式在 stop 前读到的 pause 状态），
    // 使暂停态下的刷新 / 排序 / 移除无效项不误起播
    void onPlayRequested(const PlaylistEntity::List &entities, int tab, int index, double seekTo,
                         bool keepPaused = false);
    void onFilesAdded(const PlaylistEntity::List &entities);
    void onRemoveRequested(QList<int> rows);
    void onClearRequested();
    void onMpvShutdown();
    // N54：书签层级定位的两种结局（PlaylistWidget 只切层与定位，播放与报错在此决定）。
    // entityIndex 是目标层的实体序（即 mpv playlist-pos 语义）
    // N57 §2.8：tabIndex 是视图在发射时刻由列表身份现场换算的坐标（待办状态里只有身份）
    void onLevelItemReady(const PlaylistWidget::LevelJump &req, int tabIndex, int entityIndex);
    void onLevelItemFailed(const PlaylistWidget::LevelJump &req, PlaylistWidget::JumpFailure why,
                           const QString &detail);

  private:
    // R3：音量上限推送回调（由 MpvController::volumeMaxChanged 驱动）
    void applyVolumeMax(int v);
    // N108：滑条量程重算单点（volume-max 推送与曲线开关切换共用）——勾选曲线时
    // 顶端取 mpv 上限的等价百分比（Util::Volume::mpvToMpc），关态即 m_volumeMax
    void updateVolumeSliderRange();
    // N108：显示百分比 → mpv volume 的下发单点（按当前曲线换算，不含 OSD）——
    // 用户动作、曲线切换与启动恢复三条路径共用同一换算
    void pushVolume(int displayValue);
    // N56：注册表维护（自 PlaylistWidget 上移装配顶点）——初装配（构造期）与
    // 重装配（设置变更）共用，读 ini 构建 HttpClient 缺省参数 provide 进注册表；
    // 覆盖式更新（注册表身份恒定——实体持有的 registry 指针不变，下一次求值
    // resolve 即得新 client）
    void provideHttpServices();
    // N36 遍历策略同路：读 ini 构建 TraversalPolicy 快照 provide 进同一注册表；
    // 取值侧在 DirProvider::enumerate 起始处 resolve
    void provideTraversalPolicy();
    // N56 §2.7：正播实体——凡"以当前内容为对象"的书签 / 聚合器 / OSD / duration
    // 读点统一经此取实体，不再读 mpv 属性。零参：playlist-pos 的真相源是本类的
    // m_playlistPos 镜像，取实体的坐标对在此收口；无正播（停止态 / 镜像未就位）
    // 返回空 Ptr。返回 Ptr 而非 optional<Ptr>：shared_ptr 本身就是可空句柄，
    // 再包一层 optional 只会引入语义上不存在区分的第二重空态
    PlaylistEntity::Ptr playingEntity() const;
    // 现读版：行号与 tab 都当场问 mpv / 列表，不取镜像。只给"载荷与行号必须同拍"的
    // 读点用——事件随 queued 信号进 GUI 线程，其间 mpv 可能已换行，取晚了即认错条目
    // 归属（宁漏不串，N104 / N107 口径）。坐标对的取法仍在此收口，两处只差行号来源；
    // 调用方拿到条目后仍须自证它就是要的那一条（matches 键级判等）
    PlaylistEntity::Ptr livePlayingEntity() const;
    // N77 §2.3：正播 tab 的列表身份——凡书签读点（聚合器三查询 / 取号 / 编辑器聚焦）
    // 与 key 一并传给消费方；停止态或无列表返回 nil（nil 键组照常参与严格口径）
    QUuid playingListUuid() const;
    // N115：style / colorScheme 两键的运行期读取点（空配置路径跳过；初始 apply 在入口）
    void applyAppearanceFromIni();
    // 播放 / 暂停图标的唯一赋值点（paused 为真出"播放"三角），现态记入 m_playIconPaused
    void applyPlayIcon(bool paused);
    // 控制行与静音 / 开关 / 齿轮图标按当前调色板重灌：构造期灌初值与 changeEvent 收到
    // 外观事件共用此出口（幂等，首行判空兜住控件树建成前的样式事件）
    void retintIcons();
    // N56：注册表维护的设置变更入口（自 PlaylistWidget 上移）——命中相关键时
    // 重读 ini 覆盖注册表内部实例，下一次求值即生效
    void onPluginIniKeysChanged(const QStringList &keys);
    // N99：上报开关的变更通路与启动期初值共用——遍历 ReporterSwitchCatalog，命中
    // changedKeys 的行（空表即全读，启动期那一路）后 queued 改驱动器启用表、切闸门；
    // 行的当前值一律经 readReporterSwitchValues 取（读法不住本类）。这条通路一律不
    // 判断据、不起线程——判据只在起播那一处重查；线程未起时这次调用只是把表改掉
    // （queued 事件堆在 beginCollecting 之前处理），启动后按新表运作
    void syncReporterSwitches(const QStringList &changedKeys = {});
    // N74：起播段的转发——本类是"在播哪一册"的唯一知情者（模型侧现算正播实体的
    // 归属），故由这里把 (播放串, 归属列表身份) 一次性投给驱动器，而不是让驱动器
    // 从播放串反推（直发形态的串里没有列表身份）。连在 fileLoaded 上、须早于
    // maybeBootReportDriver 的 connect（保证 beginSegment 先于 beginCollecting 入队）
    void forwardReportSegment(const QString &path);
    // N74：线程启动判据的唯一重查点（fileLoaded 到达即查）——注册表里有在册报告者
    // 且账目聚合判"有任一启用"（ReporterSwitchCatalog::anyEnabled，与驱动器同口径），
    // 且线程从未起过，则起线程并投一次 beginCollecting。启动一次即不再销毁：开关
    // 切换走驱动器内闸门，不走这里
    void maybeBootReportDriver();
    // N119：菜单栏装配——遍历描述表建五个顶级菜单，内容挂 aboutToShow 整体重建
    void createMenuBar();
    // N119：动作连接唯一出生处，启动调一次
    void connectActions();
    // N119：按描述表装配一个菜单段，不连任何信号（菜单栏与右键菜单共用）
    void renderMenuGroup(QMenu &menu, const MenuSpec::Group &group);
    // N123：条目循环本体，子菜单经它递归渲染（两个渲染面与子菜单共用同一份装配）
    void renderMenuItems(QMenu &menu, std::span<const MenuSpec::Item> items);
    // N134：Tools 菜单的插件工具段（N135 的 mpv menu-data 段按 N155 开关决定是否前置），名称 >
    // 分段自动 建子菜单（两段皆空时只剩编辑入口、无前导分隔线）
    void buildToolSection(QMenu &menu);
    // N138：常驻工具动作组重建——与 tools() 等长，装不装键由 planKeys 单点判定
    void rebuildToolActions();
    // N134：工具命令执行唯一收口——失败 OSD，日志不记命令原文；N135 的 mpv
    // menu-data 条目同走这里
    void runToolCommand(const QString &label, const QString &command);
    // N135：mpv menu-data 段的递归渲染——hidden 整条跳过、disabled / checked 施加到
    // 条目与其子菜单、空子菜单摘除、分隔线走 appendMenuSeparator；命令条目经
    // runToolCommand 执行
    void buildMpvMenuItems(QMenu &menu, const QList<MpvController::MenuDataItem> &items);
    // N119：视频区右键菜单入口，弹出前按 N5 清焦
    void showVideoContextMenu(const QPoint &globalPos);
    QWidget *createLeftArea();
    // A15：无状态按钮统一接线——事件来源走 action trigger，启停跟随 action enabled
    void bindButton(QAbstractButton *btn, QAction *action);
    void connectController();
    // 导航器浮窗定位到主窗口右下角（N1）
    void repositionNavigator();
    void updateTimeDisplay();
    void playAt(int index);
    // 停止态的上一个/下一个步进（A9/A16）：以当前选中列表为基准并开始播放；
    // 列表循环开启时首尾互绕，否则越界无动作
    void stepStopped(bool next);
    // A16：上一/下一动作三路联动启停（矩阵见 specs/A16.md）
    void updateNavigationActions();
    // 进入停止状态：停止播放（保留列表）、复位播放按钮图标
    void enterStoppedState();
    // N3：覆盖式全屏、视频区几何同步与单击/双击分流
    void toggleFullscreen();
    // N68：小窗模式进出唯一收口（I/Esc/关闭钮三路共用）；全屏与小窗互斥
    void toggleMini();
    void syncVideoGeometry();
    // ---- N156：手势路由与自驱动环路 ----
    enum class GestureKind { Move, Resize };
    enum class GesturePhase { Begin, Update, End };
    // 手势路由单点：标题栏三信号与两容器热区事件都先落这里，按 GesturePolicy
    // 分流（Native → startSystemMove / tryStartSystemResize；SelfDriven →
    // begin / update / finishGesture）；环路不活跃时多余信号空转吸收
    void dispatchGesture(GesturePhase phase, GestureKind kind, Qt::Edges edges,
                         const QPoint &globalPos);
    // 热区缩放的原生循环入口（仅 Native 后端调用）；m_fullscreen / 最大化早退
    bool tryStartSystemResize(Qt::Edges edges);
    void beginGesture(GestureKind kind, Qt::Edges edges, const QPoint &globalPos);
    void updateGesture(const QPoint &globalPos);
    void finishGesture();
    // N156：最大化·还原统一动作（标题栏 Qt 点击与原生 NC 释放共用）；Windows 下
    // 兼作 WinFramelessMessages::Host 的动作出口
    void toggleMaximizeRestore();
    // ---- N156：WinFramelessMessages::Host 实现（窗口态真相与视频比例；基类接口
    // 仅 Windows 门禁，方法本身全平台编译——videoRatio 亦供自驱动环路与迷你几何收口）----
    bool fullscreen() const { return m_fullscreen; }
    bool mini() const { return m_mini; }
    // 视频宽高比（无流 16:9，与进入路径同源）——自驱动缩放 / WM_SIZING / 迷你几何
    // 收口共用这一份，杜绝多处各写一式
    double videoRatio() const {
        return (m_pendingVideoW > 0 && m_pendingVideoH > 0)
                   ? double(m_pendingVideoW) / double(m_pendingVideoH)
                   : 16.0 / 9.0;
    }
    // ---- N66：全屏浮窗装配与显隐收口（规格 §三-3）----
    enum class Overlay { ControlBar, Playlist, TitleBar }; // N156：加标题栏浮窗宿主
    void ensureFullscreenOverlays();
    void reparentIntoOverlay(QWidget *what, QWidget *host);
    void restoreOriginalParent(QWidget *what);
    void setOverlayShown(Overlay which, bool shown);
    void evaluateOverlayVisibility(const QPoint &clientPos);
    void reevaluateOverlays();
    void repositionFullscreenOverlays();
    // N119：菜单栏可见性唯一收口——偏好真相 ∧ 非覆盖态。N156 起降为
    // applyWindowChrome 的内部步骤，不再有外部调用点
    void applyMenuBarVisibility();
    // N156：窗口 chrome 唯一收口——按判序定标题栏三态 / 容器边距 / 菜单与菜单
    // 按钮显隐 / Windows 圆角，再整窗重画
    void applyWindowChrome();
    // N121：尺寸事实的唯一入口——权威 videoSizeChanged 与 on_preloaded 预估值共用
    void applyVideoSize(int videoW, int videoH);
    // N10：普通态窗口尺寸自适应视频（videoSizeChanged 直连触发，收口见 resizeVideoAreaTo）
    void adaptWindowSizeToVideo(int videoW, int videoH);
    // N122：普通态视频区尺寸收口，N10 自适应与倍率命令共用（细节见定义处）
    void resizeVideoAreaTo(const QSize &wantLogical);
    // N122：窗口尺寸倍率命令（一次性、不记忆），最大化先还原再应用显式尺寸
    void applyWindowScale(double factor);
    // N123：五档倍率的启用态跟随有效视频尺寸——快捷键与两个菜单渲染面同状态
    void updateWindowScaleActions();
    // N122：倍率的小窗路径——定宽保比，下限钳宽、上限缩回
    void applyMiniWindowScale(double factor);
    // N68：小窗期换片自适应——保持当前客户区宽不变、高按新比例重算
    // （尺寸与位置不回默认），保比在 refitMiniToAspect、越界收回在 clampMiniIntoWorkArea
    void refitMiniToVideo();
    // N156 小窗几何收口（保视频区宽调高）——宽语义为视频区宽，高按视频比例
    // （无流按 16:9）重算后经 miniClientSize 落客户区；幂等。只由换片自适应触发
    // （定框由 WM_SIZING / 自驱动环路在各自域保比与钳位）。越界钳回不在本函数，
    // 见 clampMiniIntoWorkArea
    void refitMiniToAspect();
    // N156 外框越出工作区即按最小移动原则收回（未越界空操作）。只由程序性场景
    // 调用：换片自适应与倍率命令；用户拖动/松手不钳（N68 第 8 条裁决）
    void clampMiniIntoWorkArea();
    // A20：播放列表显隐几何联动——左 pane 宽度不变、主窗口向右扩展，
    // 空间不足先压视频区触底再压播放列表宽度（实测值写回记忆）
    void applyPlaylistGeometry(bool expanding, int leftAnchor);
    // A20：启动几何收口（定宽 → 定高 → 居中），仅首次 show 执行
    void applyStartupGeometry();
    // A20：主窗口居中到所在屏工作区（外框口径，居中后钳回兜底）
    void centerOnScreen();
    // A20：最小移动钳回——外框矩形按两轴独立、越界多少移多少收进区域，
    // 返回外框左上角（未越界原样返回）；MainWindow 内多处复用
    QPoint minimalMoveTopLeft(const QRect &frame, const QRect &area) const;
    void onVideoClickTimeout();
    // N5：统一音量调节入口——钳制到 [0, 上限] 后下发 mpv，UI 经观察回流刷新
    void changeVolume(int delta);
    // B14：用户音量动作统一收口——钳制、下发并直显音量 OSD；
    // 启动恢复路径刻意绕行（restoreSettings 直写，无 OSD）
    void applyVolume(int value);
    // N6：相对 seek（秒）；停止态（无播放位置）无动作
    void seekRelative(int deltaSeconds);
    // N3：导航器按需创建生命周期收口（关闭即销毁，null=未打开）
    void ensureNavigator();
    void destroyNavigator();
    // N143：字幕浏览器面板按需创建生命周期收口（关闭即销毁，null=未打开）
    void ensureSubtitleBrowser();
    void destroySubtitleBrowser();
    // N143：ffmpeg 路径解析单点；N149 起显式值与自动档同判据（解析得到真实文件才出串）
    QString resolvedFfmpegPath() const;
    // N146：AI 字幕生成——对话框按需创建（关闭即销毁），任务归主窗持有
    void ensureAiSubtitleDialog();
    AiSubtitleStartContext aiSubtitleStartContext() const; // 点开始的现读复核
    QString aiSubtitleDisplayName() const;                 // 当前文件行的显示名现读
    void refreshAiSubtitleFile();                          // 在播变化时推一次当前文件行
    // 持久化：关闭时保存 / 启动时恢复
    void saveSettings();
    void restoreSettings();
    void applyShortcutOverrides(); // N86：键位收口（幂等，按 ini 现状重算；两路径同码）
    // N90：让位期键盘归属判据（合取式，单点）——eventFilter 与 event() 共用
    bool yieldOwnsKeyboard() const;
    // N90：让位转发单点——按 mpv 键名异步注入 keypress，eventFilter 的截断分支与
    // keyPressEvent 兜底共用；全仓只允许这一种转发形态
    void forwardKeyToMpv(const QKeyEvent *event);
    // N95：文本转发单点——提交串按 UCS-4 码点逐个注入 keypress，仅让位期生效
    void forwardTextToMpv(const QString &text);
    // N90：让位期把主窗装成 QWindow 过滤器（幂等，窗口重建后须补装）
    void attachYieldFilter();
    void detachYieldFilter();
    // 循环模式："none" / "file" / "playlist"
    void applyLoopMode(const QString &mode);
    QString currentLoopMode() const;
    static QString formatTime(double seconds);
    // N13：轨道文案与切换
    QString formatTrackText(const MpvController::TrackInfo &t) const;
    qint64 trackBitrate(const MpvController::TrackInfo &t) const;
    // 按类型取轨道快照并按 id 升序——三组流子菜单与流选择按钮的分组共用这一份取表
    QList<MpvController::TrackInfo> tracksOfType(const QString &type) const;
    // N119：轨道动态段——四个子菜单现读现填
    void buildTrackSection(QMenu &parent);
    // B18：流选择按钮的显隐与启用由 m_tracks 派生
    void updateStreamButton();
    void showStreamMenu();
    // N13：按 type 把 vid / aid / sid 排出。N94：目标是第二字幕时先关第二字幕再设
    // sid（mpv 不允许主 / 第二同为一条，切换即抢占）
    void switchToTrack(const MpvController::TrackInfo &t);
    void disableSubtitles();
    // N94：第二字幕（secondary-sid）。与上面两个出口对称：切换同理抢占（目标已是
    // 主字幕时先关 sid），关闭只动 secondary-sid、不碰 sid
    void switchToSecondarySubtitle(const MpvController::TrackInfo &t);
    void disableSecondarySubtitles();
    // 往 menu 追加一组该类型轨道的选择条目（视频 / 音频：勾选按 selected）
    void addTrackSelectionGroup(QMenu *menu, const QString &type);
    // 往 menu 追加一组字幕选择条目：有轨时首项"None"（N96：0 轨时 None 无所指不添加），
    // 其后全量字幕轨（两组都列全量、
    // 不排除已被对方占用的那条——"同一条只能落在一组"由点击时的抢占下发体现）。
    // secondary 选槽位——勾选一律按 main-selection（主字幕 0、第二字幕 1）：
    // selected 的语义是"在解码"，两条字幕同时解码时双双为真，判不出归属。
    // 两个 add* 都不清空菜单、也不加标题与分隔线：播放菜单的子菜单日后要在字幕流下
    // 追加"添加字幕"之类的动作，流选择按钮的分组则只提供选择，两处形态各异，故外层一律
    // 留给调用方
    void addSubtitleSelectionGroup(QMenu *menu, bool secondary);

    // ——N96 手动添加音频流 / 字幕流——
    // "添加"项对话框过滤器：扩展名现读 mpv 的 --sub-auto-exts / --audio-exts（跟随用户
    // mpv.conf 自定义），读失败或读空才回退 Constants.h 的内置清单。主字幕与第二字幕同表
    QString trackFileFilter(bool subtitle) const;
    // 清单取值单点（N97）：过滤器拼接与视频区拖放的字幕判据共用——读取形态一字不动
    // （每次现读 mpv 属性，读空回退内置表），故两处的判据永不漂移
    QStringList trackExtensions(bool subtitle) const;
    // 流子菜单"添加"项装配单点：条件分隔线（菜单非空才加，0 轨时添加项顶格）+ 动作。
    // 动作不进任何分组函数，与"选择条目"分属两件事。音频档只看 subtitle，
    // secondary 取其默认值即可（传出的第二档只给字幕菜单用）
    void appendTrackAddAction(QMenu *menu, bool subtitle, bool secondary = false);
    // 选文件 → audio-add / sub-add 下发（同步，返回码判成败）→ 成败 OSD。
    // 第二字幕只加进来不选中（sub-add auto），选中由用户点条目走 switchToSecondarySubtitle
    void addAudioTrack();
    void addSubtitleTrack(bool secondary);
    // 下发 + 成败 OSD 收口（N97）：对话框入口与视频区拖放共用，命令与文案一字不变
    void applySubtitleFile(const QString &path, bool secondary);

    // ——N97 视频区文件拖放——
    // "打开文件"的尾部四步收口（对话框入口与拖放共用）：整表替换默认列表并从头开播。
    // 集合为空即一步都不进——四步里切 tab 的两步排在 onPlayRequested 之前，不先判空
    // 会"什么都没播"却把前台 tab 切走
    void playFilesFromPaths(const QStringList &files);
    // 视频区拖放三类事件（DragEnter / DragMove / Drop）的统一处理：一律消费（返回
    // true）；受理与否体现在事件对象上，判据与执行只在 Drop 做一次
    bool handleVideoDropEvent(QDropEvent *event);

    // N43：滤镜动态段——两个子菜单按 af / vf 现读重建；点击条目翻转启用位并整串下发
    void buildFilterSection(QMenu &parent);
    void toggleFilterEntry(const QString &property, int index);

    // N110：比例动态段——勾选按属性现读回填，故装配与回填同处；条目是 ActionManager
    // 主实例（N123），不再当场新建
    void buildAspectSection(QMenu &parent);
    void onCustomAspectRatio();

    // N15：章节与书签
    // N119：章节动态段——填进本菜单本身（另三个填充函数往 parent 里建子菜单），
    // 并登记为活体章节菜单
    void buildChapterSection(QMenu &target);
    void updateChapterMenuState();
    void updateChapterMenuChecked();
    void updateChapterSlider();
    // N54：跳转拆成两条语义不共用的路。章节语义是"当前文件的这一时刻"，只能在手上
    // 现有的列表里解决（jumpToChapter 即现 jumpToFileTime 函数体逐字改名沿用）；
    // 书签语义是"回到它所属的列表与所在层"，只认书签自带的身份，不再扫描别的列表
    void jumpToChapter(const QString &path, double time);
    void jumpToBookmark(const QString &path, double time, const BookmarkManager::Origin &origin);
    // N54 §2.2 第 4 步的回退档：把这一个文件作为会话镜像推入默认 tab（仅本地项走到这里）
    void fallbackPlayInDefaultTab(const QString &path, double time);
    // N54：书签跳转的报错出口——正文一句人话，路径与提供者回传原文进 informativeText
    void warnBookmarkJump(const QString &reason, const QString &path, const QString &detail);
    void ensureBookmarkEditor();
    void onAddBookmark();

    MpvController *m_controller;
    // N147：配置路径不再由本类持有，消费者一律取 App::paths().iniFile() / iniDir()
    // N52：转接协议适配层（寿命归 plugin.cpp 的 shared_ptr，覆盖 exec 全程）
    std::shared_ptr<StreamRelay> m_relay;
    // N115：外观套用点——入口构造并注入的非拥有指针，本类只做运行期套用
    // （平台默认样式的构造期捕获因此发生在入口，先于任何 setStyle）
    AppearanceManager *m_appearance = nullptr;
    ActionManager *m_actions = nullptr; // A15：全部 UI 动作出生与登记处，先于容器构建
    // N56 §2.7：服务注册表由装配顶点（本类）创建并持有，注入 PlaylistWidget；
    // relay 的 provide<StreamRelayIf> 亦在此完成（早于任何提供者出生）
    std::shared_ptr<ServiceRegistry> m_registry;
    // N57 §2.3：播放列表注册表（列表模型的唯一所有者）由本类创建并持有，注入
    // PlaylistWidget；后备提供者在本类构造期经 createDefaultProvider 造好交入。
    // 不入服务注册表（可变模型对象与"resolve 任意线程"的线程契约不符）
    std::shared_ptr<PlaylistRegistry> m_listRegistry;
    // N74：报告者注册表——同一份句柄三处用：provide 进服务注册表、注入驱动器构造、
    // 下传 PlaylistRegistry 供列表侧注册；本类这份只用于线程启动判据现查 hasEntry
    std::shared_ptr<PlaybackReporterRegistry> m_reporters;
    // N74：上报驱动器（构造期创建并连好全部信号，不启动线程；进程生命周期存活，
    // 不 delete——退出序靠闸门与事件循环收线，见 PlaybackReportDriver 注释）
    PlaybackReportDriver *m_reportDriver = nullptr;
    QSplitter *m_splitter = nullptr;
    // N156：中央容器（包 splitter，承担 band 左右下边距环）；标题栏容器承载自绘
    // 标题栏与菜单栏，经 setMenuWidget 顶替 QMainWindow 菜单区。此后任何代码
    // 不得再调 QMainWindow::menuBar()——它会懒建第二个菜单栏并顶掉容器
    QWidget *m_centralFrame = nullptr;
    QWidget *m_titleBarContainer = nullptr;
    QMenuBar *m_menuBar = nullptr;
    WindowTitleBar *m_titleBar = nullptr;
    // N66：左 pane 容器（createLeftArea 产物）。重挂收口 restoreOriginalParent
    // 据此把 m_bottomBar 挂回布局末项——左 pane 的布局指针不另存，经此取回
    QWidget *m_leftPane = nullptr;
    // N66：进度行 + 控制行的底部条带容器（左 pane 布局末项）。全屏期整体重挂进
    // 控制栏浮窗——"整条控制栏"成为一次 setParent 的单元；容器不携带任何逻辑
    QWidget *m_bottomBar = nullptr;

    // ---- N66：全屏浮窗控制栏与播放列表 ----
    QWidget *m_controlOverlay = nullptr;  // 控制栏浮窗宿主（按需创建，首次进全屏才构造）
    QWidget *m_playlistOverlay = nullptr; // 播放列表浮窗宿主（同上）
    QWidget *m_titleBarOverlay = nullptr; // N156：标题栏浮窗宿主（同上；X11 侧 bypass 同控制栏）
    QWidget *m_fsResizeStrip = nullptr;   // 列表浮窗左缘拖宽热区（宿主内手动几何透明子条）
    // 视频区 QWindow（WA_NativeWindow 置位即有）：安装与比对同一个指针。
    // N156 起主窗句柄终身稳定，该 QWindow 全程存活；QPointer 仅作悬空兜底
    // （构造期断言违约显影）
    QPointer<QWindow> m_videoWindow = nullptr;
    int m_bottomBarHeight = 0; // 重挂时记录的 sizeHint 高度（Hidden 态读不到实测高度）
    int m_titleBarHeight = 0;  // N156：标题栏浮窗高度（同法重挂时记录）
    int m_fsPlaylistWidth = 0; // 全屏期浮窗宽度（进程内记忆，与窗口态 m_playlistWidth 互不写入）
    // 窗口态面板可见性真相：唯一写点 setPlaylistVisible；saveSettings 读此而非活体
    // isVisible()——全屏期面板被重挂隐藏、A11 二次保存在窗口隐藏后触发，活体不可信。
    // N119 §3.1：全屏期同一开关也写此真相，右缘门控同读
    bool m_playlistVisibleTruth = false;
    // N119：菜单栏偏好真相——写点只有 Menu Bar 动作与 restoreSettings；saveSettings
    // 读此而非 m_menuBar->isVisible()（覆盖态活体恒假，A11 同坑）
    bool m_menuBarVisibleTruth = true;
    int m_preFsLeftWidth = 0;     // 进全屏前的左 pane 宽度（退出恢复锚点；须在普通几何下采集）
    bool m_fsResizing = false;    // 列表浮窗左缘拖宽进行中（挂起条件之一）
    int m_fsResizeStartX = 0;     // 拖宽起点光标全局 x
    int m_fsResizeStartWidth = 0; // 拖宽起点浮窗宽度
    // 视频显示区域（mpv wid 嵌入目标）：主窗直接子控件（N3），
    // 脱离布局管理由 syncVideoGeometry 手动同步，父链构造期定死后全程不动
    QWidget *m_videoWidget = nullptr;
    // N3：布局槽位占位符，普通态视频区几何的锚点；最小尺寸约束在此
    QWidget *m_videoPlaceholder = nullptr;
    // N67：播放时视频区光标自动隐藏（构造期创建，事件过滤由类自身接管）
    CursorAutoHider *m_cursorHider = nullptr;
    // N40：最近一次收到的视频尺寸发布值——去重收口后每次都是一致对、
    // 只在真实变化时到达，不再需要合并缓冲定时器；仅退出全屏的重适配路径取用
    int m_pendingVideoW = 0;
    int m_pendingVideoH = 0;
    // B16：全屏期间若切换了视频（videoSizeChanged 被守卫跳过），退出普通态时
    // 需按当前视频尺寸重新适配；同一视频则维持 N10"全屏退出不重新适配"语义
    bool m_refitAfterFullscreen = false;
    // N40 变更：最大化期间若切换了视频（适配被 isMaximized 守卫拦下），还原
    // 最大化时按当前视频尺寸重新适配——跟随全屏退出重适配的同款语义
    bool m_refitAfterMaximize = false;
    ChapterSlider *m_progressSlider = nullptr; // N15：进度条（带章节/书签标记层）
    // 悬停 tooltip 合并节流：MouseMove 只记最后位置，单次定时器到期算一次
    QTimer *m_seekTipTimer = nullptr;
    QPoint m_seekTipGlobal; // 最后一条 move 的全局坐标（换算来源 + tooltip 落点）
    QSlider *m_volumeSlider = nullptr;
    // N5：音量调节配置（INI 键 volumeMax / volumeWheelStep / volumeKeyStep）
    int m_volumeMax = 130; // R3：音量上限，mpv 默认 130；运行时由 applyVolumeMax 同步
    int m_volumeWheelStep = App::volumeWheelStepDefault(); // 滚轮每格步长（缺键 2）
    int m_volumeKeyStep = App::volumeKeyStepDefault();     // 方向键每格步长
    // N108：音量曲线开关（INI 键 quadraticVolumeCurve）——勾选时滑条显示百分比按
    // MPC-HC 二次方听感标尺经 Util::Volume 换算为 mpv volume，量程随之取等价百分比
    bool m_quadraticVolumeCurve = App::quadraticVolumeCurveDefault();
    // N155：Tools 菜单是否前置 mpv menu-data 段（INI 键 showMpvMenuData，缺键关）
    bool m_showMpvMenuData = App::showMpvMenuDataDefault();
    // N38：静音——菜单项与音量行图标按钮共用同一 QAction（勾选态由 mpv 推送驱动）
    QAction *m_muteAction = nullptr;
    QToolButton *m_muteButton = nullptr;
    QPushButton *m_playButton = nullptr;
    bool m_playIconPaused = true; // 播放钮当前图标对应的态（true = 显示"播放"三角）
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_prevButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QLabel *m_timeLabel = nullptr;
    QPushButton *m_playlistToggle = nullptr;
    QAction *m_playlistAction = nullptr;
    PlaylistWidget *m_playlist = nullptr;
    // ---- A20：播放列表显隐几何联动 ----
    int m_playlistWidth = 0; // 进程内记忆的播放列表宽度（客户区像素）；0 = 未定，用默认值
    bool m_playlistGeometryPending = false; // 构造期展开，几何调整延后到首次 show
    bool m_firstShowDone = false;           // showEvent 一次性守卫（最小化恢复/全屏退出也会触发）
    QAction *m_navigatorAction = nullptr;   // 视图菜单"导航器"勾选项（N1）
    NavigatorPanel *m_navigator = nullptr;  // 导航器浮窗（N3 按需创建）：null=未打开
    QAction *m_subtitleBrowserAction = nullptr;         // 视图菜单"字幕浏览器"勾选项（N143）
    SubtitleBrowserDialog *m_subtitleBrowser = nullptr; // N143：按需创建，null=未打开
    SubtitleExtractor *m_subtitleExtractor = nullptr;   // N143：取数模块，寿命不随面板开关
    AiSubtitleJob *m_aiSubtitleJob = nullptr;           // N146：任务归主窗持有，首次使用时创建
    AiSubtitleDialog *m_aiSubtitleDialog = nullptr;     // N146：按需创建，关闭即销毁
    QAction *m_fullscreenAction = nullptr;              // 视图菜单"全屏"勾选项（N3）
    QAction *m_miniAction = nullptr;                    // 视图菜单"小窗"勾选项（N68）
    QAction *m_aboutAction = nullptr;        // About 动作（不进登记表，两个菜单共用一份）
    QAction *m_checkUpdatesAction = nullptr; // N151：帮助菜单入口（自建，不进登记表）

    // ---- N151：检查更新 ----
    UpdateChecker *m_updateChecker = nullptr; // m_registry 建好即造，寿命随子对象链
    QPointer<UpdateDialog> m_updateDialog;    // 按需创建，WA_DeleteOnClose
    void showUpdateDialog(bool startCheck);   // 自动与手动共用的单一打开函数

#ifdef OXBOW_WITH_OPTIONAL_DEPENDENCY_INSTALLER
    // ---- N150：可选依赖安装器 ----
    QAction *m_installDependenciesAction = nullptr;         // 帮助菜单入口（自建，不进登记表）
    DependencyInstallJob *m_dependencyJob = nullptr;        // 作业构造即无父，主窗只持指针
    QPointer<DependencyInstallerWizard> m_dependencyWizard; // 按需创建，关闭即销毁
    void openDependencyInstaller();
    void refreshInstallerResults(); // 执行收口：重解析 ffmpeg 与重预载 MediaInfo
#endif

    void reloadMediaInfoLibrary(); // 按 ini 现值重预载 MediaInfo 库（N142）

    double m_duration = 0.0;
    double m_timePos = 0.0;
    bool m_seeking = false;
    int m_playlistPos = -1; // mpv 当前播放项，-1 表示停止状态
    bool m_startupPlaybackCheckDone = false;
    int m_pendingSeekRow = -1;       // >=0：有挂起的会话恢复 seek，值为目标行；-1 无
    double m_pendingSeekTime = -1.0; // 挂起 seek 的目标进度（秒），配合 m_pendingSeekRow 使用
    // 会话数据每次进程只落盘一次（A11）：二次保存在窗口隐藏后触发，
    // isVisible() 等活体状态会被读错（playlistExpanded 曾被覆写为 false）
    bool m_settingsSaved = false;
    // 本次进程是否发生过播放（A13）：未播放就关闭属于会话延续，
    // 不得清除上次保存的 lastListUuid（N57 §2.7）
    bool m_playedThisSession = false;
    // ---- OSD 三形态控制器（N113）：本类定「说什么」，它定「怎么显示」----
    OsdController *m_osd = nullptr; // 构造期创建：视频区装配之后、ThumbWindow 之前
    // N153：缓冲状态 OSD——持续态自持状态机，与 m_osd 的一次性通道分立；装配序同上
    CacheStatusOsd *m_cacheOsd = nullptr;
    // 进度条悬停 tooltip 的唯一出口（合并节流的落点，见 eventFilter 同名分支）
    void flushSeekToolTip();

    // N78：截图两槽的公共前置——取图失败（无正播 / 载荷异常）即 OSD 报错并返空图
    QImage grabScreenshotOrNotify();

    // ---- N8：主窗口显示优化 ----
    QString playingDisplayName(); // 正播项显示名（与列表行同一字段，标题与 OSD 同源）
    // N25：标题与 OSD #2 同源刷新（B11 镜像取名）。playlist-pos 值不变不再推送，
    // 回刷收编后一律经此刷新一次（正播名错位与总数变化都在内）
    void updateNowPlayingUi();

    // ---- N3：单击/双击分流与覆盖式全屏 ----
    QTimer *m_clickTimer = nullptr; // 单次定时器，间隔=双击间隔
    // N113：单击超时的动作意图——press 只按缓存 m_playlistPos 记账（< 0 记 StoppedStart，
    // 否则 InTrackToggle），不碰 mpv；超时按实时态分流收口
    enum class ClickAction { None, InTrackToggle, StoppedStart };
    ClickAction m_clickAction = ClickAction::None;
    // N91：退出区间标志。点过关闭钮即置位并保持——窗口此后须一直 mapped 到进程结束
    // （提前 unmap 会让 vo 等不到 Present 完成事件而卡死），故重复关闭钮与标题回刷都由它挡
    bool m_closing = false;
    bool m_fullscreen = false; // 覆盖式全屏态标志（syncVideoGeometry 分支依据）
    // N68：小窗态标志（与 m_fullscreen 互斥，syncVideoGeometry 同走"铺满客户区"分支）
    bool m_mini = false;
    // ---- N156：自驱动环路状态（全平台无条件编译，是否启用由 GesturePolicy 判据决定）----
    enum class GestureMode { None, Move, Resize };
    GestureMode m_gestureMode = GestureMode::None;
    Qt::Edges m_gestureEdges;  // 当前 Resize 手势的拖边组合
    QRect m_gestureOriginRect; // 手势起点窗口矩形（几何命令一律从它推）
    QPoint m_gesturePressPos;  // 手势起点全局鼠标位
#ifdef Q_OS_WIN
    // N156：DWM 圆角是否生效；paintEvent 据此选边框来源。门禁与三处读写点一致
    bool m_dwmRounded = false;
    // N156：Windows 原生消息层（NC 命中 / 光标 / 最大化钮 / mini 原生循环）。构造
    // 装配末创建并自装应用级过滤器，寿命随主窗子对象链；mini 拖动未吸附锚点归它持有
    WinFramelessMessages *m_framelessMessages = nullptr;
#endif
    // N156：进小窗前快照（仿 m_preFs* 一套，独立不混用）；面板可见性复用
    // m_playlistVisibleTruth。flags 快照退役——主窗 flags 构造期定死，运行期不切换
    bool m_preMiniMaximized = false; // 进小窗前是否最大化（退出分流依据）
    QRect m_preMiniRect;             // 进小窗前完整几何（最大化时取 normalGeometry）
    double m_savedZoom = 0.0;        // 进全屏前 zoom/pan 快照（先存后写零）
    double m_savedPanX = 0.0;
    double m_savedPanY = 0.0;
    bool m_preFsMaximized = false; // 进全屏前是否最大化（Win 退出分流 / 非 Win 走不走直发）
    // A18 无框全屏：进全屏前完整几何快照（含位置，普通态恢复用）；N156 起 flags
    // 快照退役（主窗 flags 构造期定死），该矩形同时是 setPlacementAtomic 的
    // 平台侧还原位来源（2.8）
    QRect m_preFsRect;
    // N141：置真 ⟹ 本轮全屏走的是 EWMH 直发（不进 Qt 的 WindowFullScreen 状态机），
    // Qt 侧窗口态全程记作最大化、全屏的真相只在 m_fullscreen；退出侧据此对称收口
    bool m_fsStateDirect = false;

    // ---- N13：轨道选择 ----
    QList<MpvController::TrackInfo> m_tracks;
    QPushButton *m_streamButton = nullptr; // 流选择按钮

    // ---- N15：章节与书签 ----
    BookmarkManager *m_bookmarkMgr = nullptr;
    BookmarkEditorDialog *m_bookmarkEditor = nullptr;
    // N134：自定义工具单表
    CustomToolManager *m_toolMgr = nullptr;
    // N138：常驻工具动作组，与 tools() 等长；parent 是主窗，菜单 clear() 只摘除不销毁
    QList<QAction *> m_toolActions;
    // R3：设置对话框（非模态，按需创建）
    class SettingsDialog *m_settingsDialog = nullptr;
    // N86：快捷键编辑对话框（非模态，按需创建）
    class ShortcutsDialog *m_shortcutsDialog = nullptr;
    class MpvBindingDialog *m_bindingDialog = nullptr; // N89：mpv 键位编辑（input.conf）
    // N90：console 让位态（真 = 让位生效：窗口级过滤器已装且焦点判据生效）
    bool m_inputYielded = false;
    // N95：组字中标志（由 inputMethodEvent 按 preeditString 维护；非空时键名转发只吞不转）
    bool m_imeComposing = false;
    // N119：活体章节菜单——最近一次构建的章节段所在菜单，updateChapterMenuChecked
    // 仅在其可见时逐 tick 回写；右键菜单即建即毁，QPointer 兜底
    QPointer<QMenu> m_liveChapterMenu;
    // 章节+书签标记聚合器（MainWindow.cpp 内实现）：合并规则 + 缓存
    // 收口于此，主窗口不再持有抽象成员；进程生命周期随主窗口
    MarkAggregator *m_marks = nullptr;
    // N77 §2.3：最近一次 requestLevelItemJump 的源组——jumpToBookmark 每次发射前
    // 记账（单槽覆盖，与 widget 侧单待办同形），ready / failed 两槽据此按组取展示串
    BookmarkManager::Group m_jumpSourceGroup;

    // ---- N107：时间轴悬停缩略图 ----
    QThread *m_thumbThread = nullptr;        // 引擎专属线程（引擎对象随 finished 销毁）
    ThumbEngine *m_thumbEngine = nullptr;    // 第二 mpv core 引擎（affine 引擎线程）
    ThumbWindow *m_thumbWindow = nullptr;    // 缩略图宿主窗（一套复用，hide/show 切换）
    ThumbHoverWatch *m_thumbWatch = nullptr; // 会话机（事件域 = 进度条控件）
    bool m_hoverThumbnailEnabled = App::hoverThumbnailDefault(); // INI 键 hoverThumbnail（缺键开）
};

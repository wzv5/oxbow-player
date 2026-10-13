#pragma once

#include "pch.h"

#include "Constants.h" // N74：reportJellyfin 缺省值单点（App::reportJellyfinDefault）
#include "MpvController.h"

// N54：两处接线要用到嵌套类型（PlaylistWidget::LevelJump / JumpFailure、
// BookmarkManager::Origin），嵌套类型必须有完整类型可见
#include "BookmarkManager.h"
#include "PlaylistWidget.h"

class NavigatorPanel;
class ActionManager;
class BookmarkEditorDialog;
class ShortcutsDialog;
class ChapterSlider;
class MarkAggregator;           // N15：章节+书签标记聚合器（定义于 MainWindow.cpp）
class PlaylistRegistry;         // N57：列表注册表（装配顶点创建并持有，注入 PlaylistWidget）
class PopupCloseWatch;          // N66：弹层关闭观察器（定义于 MainWindow.cpp）
class CursorAutoHider;          // N67：播放时视频区光标自动隐藏（定义于 MainWindow.cpp）
class StreamRelay;              // N52：mpv 自定义协议适配层（定义于 StreamRelay.h）
class PlaybackReporterRegistry; // N74：报告者注册表（定义于 PlaybackReporterRegistry.h）
class PlaybackReportDriver;     // N74：上报驱动器（定义于 PlaybackReportDriver.h）

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    // N52：relay 由 plugin.cpp 构造并 install 后传入，构造期收下（不在 show()
    // 之后才装配——列表构造期就要能从注册表取到 relay）；本类仅持有与转交
    // PlaylistWidget（provide 进注册表），不做其他使用
    explicit MainWindow(MpvController *controller, const QString &configFile = QString(),
                        std::shared_ptr<StreamRelay> relay = nullptr, QWidget *parent = nullptr);

    // 视频显示区域（mpv 通过 wid 嵌入的目标窗口）
    QWidget *videoWidget() const { return m_videoWidget; }

  protected:
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
    // N68：小窗态等比缩放（WM_SIZING）与边缘吸附（WM_MOVING）——函数体整体经
    // Windows 且未定义 OXBOW_PUREQT 门禁。降级形态两项都不做（循环外改窗位会被原生
    // 模态循环覆写，实测乱跳；§一 原裁决），只留定态保比（scheduleMiniRefit →
    // refitMiniToAspect）与进窗贴角
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

  private slots:
    void openFiles();
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
    void applyShortcutsLive();     // N86：落盘成功后的即时生效（从 ini 重载键位）
    // N90：console 开关驱动的键盘让位。开：一次性焦点收口到主窗并装窗口级过滤器；
    // 关：拆过滤器即完。键位表全程不动，让位期内键盘归属由焦点位置决定
    void onConsoleOpenChanged(bool open);
    void copyScreenshotToClipboard(); // N78：截图 → 剪贴板（Ctrl+C 与文件菜单共用）
    void saveScreenshotToFile();      // N78：截图另存为（Ctrl+E 与文件菜单共用）
    void showAboutDialog();
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
    // N77 §2.3：正播 tab 的列表身份——凡书签读点（聚合器三查询 / 取号 / 编辑器聚焦）
    // 与 key 一并传给消费方；停止态或无列表返回 nil（nil 键组照常参与严格口径）
    QUuid playingListUuid() const;
    // N56：注册表维护的设置变更入口（自 PlaylistWidget 上移）——命中相关键时
    // 重读 ini 覆盖注册表内部实例，下一次求值即生效
    void onPluginIniKeysChanged(const QStringList &keys);
    // N74：reportJellyfin 变更通路与启动期初值共用——读 ini 刷新成员并 queued 改
    // 驱动器启用表、切闸门。这条通路一律不判断据、不起线程——判据只在起播那一处
    // 重查；线程未起时这次调用只是把表改掉（queued 事件堆在 beginCollecting 之前
    // 处理），启动后按新表运作
    void syncReportKeyEnabled();
    // N74：起播段的转发——本类是"在播哪一册"的唯一知情者（模型侧现算正播实体的
    // 归属），故由这里把 (播放串, 归属列表身份) 一次性投给驱动器，而不是让驱动器
    // 从播放串反推（直发形态的串里没有列表身份）。连在 fileLoaded 上、须早于
    // maybeBootReportDriver 的 connect（保证 beginSegment 先于 beginCollecting 入队）
    void forwardReportSegment(const QString &path);
    // N74：线程启动判据的唯一重查点（fileLoaded 到达即查）——有任一启用键（本类
    // 手上的 ini 值）且注册表里有在册报告者，且线程从未起过，则起线程并投一次
    // beginCollecting。启动一次即不再销毁：开关切换走驱动器内闸门，不走这里
    void maybeBootReportDriver();
    void createMenus();
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
    // ---- N66：全屏浮窗装配与显隐收口（规格 §三-3）----
    enum class Overlay { ControlBar, Playlist };
    void ensureFullscreenOverlays();
    void reparentIntoOverlay(QWidget *what, QWidget *host);
    void restoreOriginalParent(QWidget *what);
    void setOverlayShown(Overlay which, bool shown);
    void evaluateOverlayVisibility(const QPoint &clientPos);
    void reevaluateOverlays();
    void repositionFullscreenOverlays();
    void setFullscreenPlaylistEdgeEnabled(bool enabled);
    // N10：普通态窗口尺寸自适应视频（videoSizeChanged 直连触发，
    // 物理像素 1:1、工作区钳制、最小移动收回；尺寸事实源经 N40 改滤镜后输出尺寸）
    void adaptWindowSizeToVideo(int videoW, int videoH);
    // N68：小窗期换片自适应——保持当前客户区宽不变、高按新比例重算
    // （尺寸与位置不回默认），保比在 refitMiniToAspect、越界收回在 clampMiniIntoWorkArea
    void refitMiniToVideo();
    // N68 小窗几何收口（保宽调高）——客户区宽不变、高按视频比例（无流按 16:9）重算；
    // 幂等。两条路径共用：主路径只经换片自适应（定框由 WM_SIZING 在原生域保比与钳位），
    // 降级路径另加结果驱动的收口轮询（见 scheduleMiniRefit）。越界钳回不在本函数，
    // 见 clampMiniIntoWorkArea
    void refitMiniToAspect();
    // N68 外框越出工作区即按最小移动原则收回（未越界空操作）。只由程序性场景调用：
    // 换片自适应（§6 整窗不越界）与降级路径的进窗贴角；用户拖动/松手不钳（第 8 条裁决）
    void clampMiniIntoWorkArea();
#if !defined(Q_OS_WIN) || defined(OXBOW_PUREQT)
    // N68 降级路径的纯 Qt 定态收口触发器——move/resize 事件去抖（无跨平台松手信号），
    // 静默达 miniRefitDebounceMs 触发一轮；N83 结果驱动：触发时查收敛（±1px），未收敛
    // 调一次并续轮询；收敛即停，重试预算（miniRefitMaxAttempts）耗尽且无外部唤醒时停，
    // 外部唤醒（位置变化 / 与请求不符的尺寸事件）随时重新武装
    void scheduleMiniRefit(bool positionAroused = false);
#endif
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
    bool isPaused();
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
    // 按类型取轨道快照并按 id 升序——三组流子菜单与 ♪ 按钮的分组共用这一份取表
    QList<MpvController::TrackInfo> tracksOfType(const QString &type) const;
    void rebuildTrackMenus();
    // B18：流按钮与流子菜单状态统一由 m_tracks 派生
    void updateStreamControls();
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
    // 追加"添加字幕"之类的动作，♪ 按钮的分组则只提供选择，两处形态各异，故外层一律
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

    // N43：播放菜单的滤镜子菜单（视频在前）——条目与启用态在播放菜单弹出时
    // 按 af / vf 属性实时重建；点击条目翻转其启用位并整串下发（运行期态，
    // 不写 conf）
    void rebuildFilterMenus();
    void toggleFilterEntry(const QString &property, int index);

    // N15：章节与书签
    void rebuildChapterMenu();
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
    QString m_configFile; // 配置文件完整路径（插件 DLL 同目录下的 oxbow.ini）
    // N52：转接协议适配层（寿命归 plugin.cpp 的 shared_ptr，覆盖 exec 全程）
    std::shared_ptr<StreamRelay> m_relay;
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
    // N74：reportJellyfin 的 ini 现值（启动期读、设置变更通路刷新）——线程启动
    // 判据的第一条件，缺省与 ini 读值同源（App::reportJellyfinDefault）
    bool m_reportJellyfinEnabled = App::reportJellyfinDefault();
    QSplitter *m_splitter = nullptr;
    // N66：左 pane 容器（createLeftArea 产物）。重挂收口 restoreOriginalParent
    // 据此把 m_bottomBar 挂回布局末项——左 pane 的布局指针不另存，经此取回
    QWidget *m_leftPane = nullptr;
    // N66：进度行 + 控制行的底部条带容器（左 pane 布局末项）。全屏期整体重挂进
    // 控制栏浮窗——"整条控制栏"成为一次 setParent 的单元；容器不携带任何逻辑
    QWidget *m_bottomBar = nullptr;

    // ---- N66：全屏浮窗控制栏与播放列表 ----
    QWidget *m_controlOverlay = nullptr;  // 控制栏浮窗宿主（按需创建，首次进全屏才构造）
    QWidget *m_playlistOverlay = nullptr; // 播放列表浮窗宿主（同上）
    QWidget *m_fsResizeStrip = nullptr;   // 列表浮窗左缘拖宽热区（宿主内手动几何透明子条）
    // 视频区 QWindow（WA_NativeWindow 置位即有）：安装与比对同一个指针。
    // QPointer 兜底——若 A18 重建主窗平台窗口时该 QWindow 未随子 HWND 存活，
    // 失效模式退化为唤起静默失灵而非比对悬空指针（构造期断言违约显影）
    QPointer<QWindow> m_videoWindow = nullptr;
    int m_bottomBarHeight = 0; // 重挂时记录的 sizeHint 高度（Hidden 态读不到实测高度）
    bool m_fsPlaylistEdgeEnabled =
        false;                 // 右缘自动显示允许位（全屏态真相源；ini 键 fullscreenPlaylistEdge）
    int m_fsPlaylistWidth = 0; // 全屏期浮窗宽度（进程内记忆，与窗口态 m_playlistWidth 互不写入）
    // 窗口态面板可见性真相：唯一写点 setPlaylistVisible；saveSettings 读此而非活体
    // isVisible()——全屏期面板被重挂隐藏、A11 二次保存在窗口隐藏后触发，活体不可信
    bool m_playlistVisibleTruth = false;
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
    QSlider *m_volumeSlider = nullptr;
    // N5：音量调节配置（INI 键 volumeMax / volumeWheelStep / volumeKeyStep）
    int m_volumeMax = 130;     // R3：音量上限，mpv 默认 130；运行时由 applyVolumeMax 同步
    int m_volumeWheelStep = 1; // 滚轮每格步长
    int m_volumeKeyStep = 5;   // 方向键每格步长
    // N38：静音——菜单项与音量行图标按钮共用同一 QAction（勾选态由 mpv 推送驱动）
    QAction *m_muteAction = nullptr;
    QToolButton *m_muteButton = nullptr;
    QPushButton *m_playButton = nullptr;
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
    QAction *m_fullscreenAction = nullptr;  // 视图菜单"全屏"勾选项（N3）
    QAction *m_miniAction = nullptr;        // 视图菜单"小窗"勾选项（N68）

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
    // ---- N7：VideoOverlay 视频叠加层（A3 顶级 Tool 浮窗，仅 OSD/字幕/弹幕，不含控制栏）----
    QWidget *m_videoOverlay = nullptr; // 顶级浮窗容器（VideoOverlay），几何始终跟随 m_videoWidget
    QLabel *m_osdLabel = nullptr;      // OSD 文本（左上角 12px 内边距）
    QTimer *m_osdTimer = nullptr;      // OSD 2s 自动隐藏
    // 帧合并去重：同事件循环内多信号折叠为一次显示（#2 > #3 > #1）
    bool m_osdCoalescePending = false;
    QString m_pendingNowPlaying;
    QString m_pendingStop;
    QString m_pendingPlayPause;
    void showOsd(const QString &text);
    void repositionVideoOverlay();
    void flushOsd();
    void ensureVideoOverlay();

    // N78：截图两槽的公共前置——取图失败（无正播 / 载荷异常）即 OSD 报错并返空图
    QImage grabScreenshotOrNotify();

    // ---- N8：主窗口显示优化 ----
    QString playingDisplayName(); // 正播项显示名（与列表行同一字段，标题与 OSD 同源）
    // N25：标题与 OSD #2 同源刷新（B11 镜像取名）。playlist-pos 值不变不再推送，
    // 回刷收编后的正播名错位须经此主动刷新
    void updateNowPlayingUi();
    double
    progressPercentAt(const QPoint &pos) const; // 进度条鼠标 x → 百分比（0~100），无效返回 -1

    // ---- N3：单击/双击分流与覆盖式全屏 ----
    QTimer *m_clickTimer = nullptr;     // 单次定时器，间隔=双击间隔
    bool m_pendingPauseRestore = false; // 播放中 press 已暂停，双击需恢复播放
    bool m_clickActionPending = false;  // 暂停/停止态 press 待超时执行
    // B14：一次性吞咽标志——视频区单击引发的暂停/恢复属性回声不入 OSD 队列；
    // 暂停 OSD 由单击超时补显，双击场景全程静默
    bool m_suppressPlayPauseOsd = false;
    // N91：退出区间标志。点过关闭钮即置位并保持——窗口此后须一直 mapped 到进程结束
    // （提前 unmap 会让 vo 等不到 Present 完成事件而卡死），故重复关闭钮与标题回刷都由它挡
    bool m_closing = false;
    bool m_fullscreen = false; // 覆盖式全屏态标志（syncVideoGeometry 分支依据）
    // N68：小窗态标志（与 m_fullscreen 互斥，syncVideoGeometry 同走"铺满客户区"分支）
    bool m_mini = false;
    // N68：进小窗前快照（仿 m_preFs* 一套，独立不混用）；面板可见性复用 m_playlistVisibleTruth
    bool m_preMiniMaximized = false; // 进小窗前是否最大化（退出分流依据）
    QRect m_preMiniRect;             // 进小窗前完整几何（最大化时取 normalGeometry）
    Qt::WindowFlags m_preMiniFlags;  // 进小窗前窗口样式（退出恢复触发重建）
#if defined(Q_OS_WIN) && !defined(OXBOW_PUREQT)
    // N68：小窗拖动期的**未吸附基准**（物理像素，仅 GUI 线程在拖动模态循环内读写）。
    // 吸附结果不得作为下一条 WM_MOVING 的判定基准——系统给的下条矩形建立在上一次
    // 已吸附的位置上，据此判距离会把每轮位移又拉回贴齐位，慢速拖动永远脱不开（实测）。
    // 故在 WM_ENTERSIZEMOVE 记下起点矩形与光标，整轮拖动只用"起点 + 鼠标总位移"作原始位
    QRect m_miniDragOriginRect;
    QPoint m_miniDragCursorOrigin;
    // N68：可见框 inset 缓存（DWM 可见外框相对原生 RECT 的差值，物理像素）——
    // 原生外框含不可见缩放边框（贴边留缝）、客户区不含发丝边框（贴边裁边），
    // 都不是人眼可见框。模态循环公共起点测一次，EXITSIZEMOVE 作废
    QMargins m_miniFrameInsets;
    bool m_miniFrameInsetsValid = false;
#endif
#if !defined(Q_OS_WIN) || defined(OXBOW_PUREQT)
    // N68 降级路径收口去抖定时器（惰性创建；主路径无此需要）
    QTimer *m_miniRefitTimer = nullptr;
    // N83 结果驱动的重试预算：自最近一次外部几何唤醒起，未收敛的收口轮数。
    // 耗尽即停，等下一次外部唤醒重新武装（清零规则见 scheduleMiniRefit）
    int m_miniRefitAttempts = 0;
    // N83：最近一次收口下发的客户区尺寸——区分「自家修正的回声」与「外部几何变化」。
    // 与请求一致的尺寸事件不清预算（否则自家 resize 的回声把预算清零，上限形同虚设）；
    // QSize() 为「从未下发」哨兵
    QSize m_miniRefitRequested;
    // 进窗贴角待办：置位期间那次收口改做贴角（装饰尺寸要 show 之后才落定，即时读
    // frameGeometry 会偏）；首条去抖收口消费并清零
    bool m_miniEntryGluePending = false;
#endif
    double m_savedZoom = 0.0; // 进全屏前 zoom/pan 快照（先存后写零）
    double m_savedPanX = 0.0;
    double m_savedPanY = 0.0;
    bool m_preFsMaximized = false; // 进全屏前是否最大化（退出分流依据）
    // A18 无框全屏：进全屏前完整几何快照（含位置，普通态恢复用）+ 窗口样式快照；
    // 落盘守卫与退出恢复共用
    QRect m_preFsRect;
    Qt::WindowFlags m_preFsFlags;

    // ---- N13：轨道选择 ----
    QList<MpvController::TrackInfo> m_tracks;
    QMenu *m_videoStreamMenu = nullptr;
    QMenu *m_audioStreamMenu = nullptr;
    QMenu *m_subStreamMenu = nullptr;
    QMenu *m_secondarySubStreamMenu = nullptr; // N94：第二字幕流（播放菜单，位于字幕流之下）
    QPushButton *m_streamButton = nullptr;     // ♪ 流选择按钮

    // ---- N43：滤镜子菜单（条目弹出时按属性实时重建，不作缓存）----
    QMenu *m_videoFilterMenu = nullptr;
    QMenu *m_audioFilterMenu = nullptr;

    // ---- N15：章节与书签 ----
    QMenu *m_chapterMenu = nullptr;
    BookmarkManager *m_bookmarkMgr = nullptr;
    BookmarkEditorDialog *m_bookmarkEditor = nullptr;
    // R3：设置对话框（非模态，按需创建）
    class SettingsDialog *m_settingsDialog = nullptr;
    // N86：快捷键编辑对话框（非模态，按需创建）
    class ShortcutsDialog *m_shortcutsDialog = nullptr;
    class MpvBindingDialog *m_bindingDialog = nullptr; // N89：mpv 键位编辑（input.conf）
    // N90：console 让位态（真 = 让位生效：窗口级过滤器已装且焦点判据生效）
    bool m_inputYielded = false;
    // N95：组字中标志（由 inputMethodEvent 按 preeditString 维护；非空时键名转发只吞不转）
    bool m_imeComposing = false;
    QActionGroup *m_chapterGroup = nullptr;
    // 章节+书签标记聚合器（MainWindow.cpp 内实现）：合并规则 + 缓存
    // 收口于此，主窗口不再持有抽象成员；进程生命周期随主窗口
    MarkAggregator *m_marks = nullptr;
    // N77 §2.3：最近一次 requestLevelItemJump 的源组——jumpToBookmark 每次发射前
    // 记账（单槽覆盖，与 widget 侧单待办同形），ready / failed 两槽据此按组取展示串
    BookmarkManager::Group m_jumpSourceGroup;
};

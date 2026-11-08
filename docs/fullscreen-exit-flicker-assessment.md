# 全屏退出闪烁调查：问题成因、技术细节、取舍与最终放弃

- 日期：2026-11-08
- 范围：Windows 主路径；对照 Linux/X11 分支与 Qt 6.12 源码
- 依据：`qtbase-everywhere-src-6.12.0` 中 `qwidget.cpp` / `qwidget.h` / `qwidget_p.h` / `qwidgetwindow.cpp` / `qwindow.cpp` / `qwindowswindow.cpp` / `qxcbwindow.cpp`；本仓《AGENTS.md》「八、持久陷阱」与《docs/pitfalls.md》；归档规格 A18 / A19 / A20 / N141 / N66 / N68
- 性质：调查结论与技术评估，不是方案规格。结论为**暂不修复、维持 A18 现状**，问题登记为主仓账本已知缺陷；若重开，按主仓规格流程另立草案。

## 一、结论

「全屏退出时窗口短暂消失后恢复、露出下层桌面或其他窗口」不是偶发、不是系统动画、也不是 mpv 嵌入问题，而是 `QWidget::setWindowFlags` 的 Qt 契约性 withdraw：A18 的全屏机制靠它切换有框/无框样式，退出侧的一次 `setWindowFlags(m_preFsFlags)` 会让 Qt 把窗口 `ShowWindow(SW_HIDE)` 掉约 15~40ms，再由 `show()` 重新映射。

关键在于：**Windows 平台层其实不需要这次 withdraw**。`QWindowsWindow::setWindowFlags` 只是原地改 `GWL_STYLE` / `GWL_EXSTYLE` 并保留 `WS_VISIBLE`，HWND 全程不变。hide 是 `QWidget` 通用路径的保守副作用；但它同时绑定着 `WA_WState_Created` / `WA_WState_Visible` 的清零与整套记账重建，绕开它就会失去 QWidget 层的几何与状态一致性。

结论是：任何「不 withdraw」的写法都会把代价转移到别处——Qt 记账分家、客户区/外框换算出错导致逐轮累计漂移、最大化状态机回放动画、可见布局重排。而「遮蔽 withdraw」需要覆盖层、抓屏与额外顶层窗口，维护者明确否决。当前 Qt 6.12 的 QWidget 架构下，**「零 withdraw + 记账一致 + 无可见中间帧 + 无额外对象」四者不可兼得**，故放弃修复。

## 二、问题成因

### 2.1 触发链

A18 的「无框手动几何全屏」在进入侧调 `setWindowFlags(Qt::Window | Qt::FramelessWindowHint)` 去掉边框，退出侧调 `setWindowFlags(m_preFsFlags)` 恢复有框样式；两次都是 `QWidget` 层 API。Qt 6.12 的实际链路：

1. `QWidget::setWindowFlags` → `QWidgetPrivate::setWindowFlags`：只要 flags 变化且涉及 `Qt::Window`，一律调 `QWidget::setParent(parentWidget(), flags)`，不区分本次变化在平台上是否真需要重建。
2. `QWidget::setParent(QWidget *, Qt::WindowFlags)`：`if (wasCreated && !WA_WState_Hidden) hide();` → `QWidgetPrivate::hide_helper()` → `hide_sys()` → 平台层 `ShowWindow(SW_HIDE)`，窗口被 unmap。
3. `QWidgetPrivate::setParent_sys` 随后无条件 `setAttribute(WA_WState_Created, false)` 与 `setAttribute(WA_WState_Visible, false)`；即使 `destroyWindow` 判定为假、HWND 没被销毁，Qt 记账也把窗口当成「未创建、不可见」。
4. 因此 `show()` / `showMaximized()` 不是可选的。`QWidget::setWindowFlags` 的文档把这条写成公开契约：对窗口修改窗口标志时会调用 `setParent()` 并导致控件被隐藏，必须再调用 `show()` 才能重新显示。

Qt 这样设计的原因：窗口 flags 是平台窗口的**创建期参数**（窗口类型、有无边框、父子关系、transient parent、任务栏属性，在 `CreateWindowEx` 那刻定死）。Qt 用统一的 `setParent` 路径处理所有 flags 变化，对可见窗口先 hide，以免销毁/重建/改样式期间屏幕出现半状态窗口，并让 `WA_WState_*` 记账与平台状态一致地清零，再由 `show()` 走完整的创建、映射、布局与子窗口树流程。

### 2.2 Windows 平台层的实情

`qwindowswindow.cpp` 的 `QWindowsWindow::setWindowFlags` → `setWindowFlags_sys` → `WindowCreationData::applyWindowFlags`：

- 只做 `SetWindowLongPtr(GWL_STYLE / GWL_EXSTYLE)`，且 `newStyle = style | (oldStyle & (WS_DISABLED | WS_VISIBLE))` **保留 `WS_VISIBLE`**；
- `WindowCreationData::initialize` 以 `SetWindowPos(SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)` 收口；
- 不销毁、不重建 HWND，主窗句柄全程不变，不产生 `WM_DESTROY` / `WM_NCDESTROY`，只有 `WM_STYLECHANGED`。

也就是说，切换 `FramelessWindowHint` 在平台层完全不需要 unmap。`QWindow::setFlags` 走的正是这条路（只更新 `QWindowPrivate::windowFlags` 并调 `QPlatformWindow::setWindowFlags`），不经 `setParent`、不 hide。这是全部后续尝试的起点，也是全部麻烦的起点。

### 2.3 A18 为什么把 withdraw 写进设计

《docs/specs/archive/A18.md》「技术事实 1 / 6」把这次隐式隐藏当作可利用的机制：

- 趁隐藏期 `setGeometry(m_preFsRect)` 植入进全屏前的快照几何，它随即成为最大化的「还原位置」，取消最大化时能精确回到原尺寸；
- 控件重挂、菜单/播放列表显隐、`applyPlaylistGeometry` 的迭代收敛、`singleShot` 重适配等恢复动作也都在隐藏期跑完，`show()` 只画最终帧。

所以 hide → 几何植入 → show 实际承担了一次**隐式批处理**。闪一下是它的可见代价；反过来，拿掉 hide 之后「视频区跳 1、2 次」也不是新缺陷，而是原本被隐藏期遮住的重排显形。

### 2.4 空档为什么有 15~40ms

`setWindowFlags` 内部先 `ShowWindow(SW_HIDE)`，`show()` 内部才重新映射；两者之间除了隐藏期 `setGeometry`，还有 `show()` 内部的样式重算（`WM_STYLECHANGED` / `WM_NCCALCSIZE` / `WM_WINDOWPOSCHANGED`）、子窗口树重新映射与布局，`show()` 本身约占 30ms。窗口在这段时间里不在合成列表中，屏幕露出的就是下层桌面或其他应用。表现必现，与系统动画、刷新率、Qt 动态或静态形态无关。

## 三、技术细节

### 3.1 记账分家

`qwidget.h` 的 `QWidget::windowFlags()` / `windowType()` 读 `QWidget::data->window_flags`；`qwidgetwindow.cpp` 的 `QWidgetWindow` 不监听 `QWindow::flagsChanged`。因此 `QWindow::setFlags` **不会回写 QWidget 记账**：QWidget 侧 flags 恒定，QWindow 侧 flags 随形态切换。

`qwidget.cpp` 的 `QWidgetPrivate::frameStrut()` 只在 `data.fstrut_dirty` 为真且窗口已创建、可见时才 `updateFrameStrut()`；而 `fstrut_dirty` 只由 `QWidget::setParent` 置真（同一函数内 `if (f & Qt::Window) d->data.fstrut_dirty = true;`）。`QWindow::setFlags` 不经过它，装饰缓存不会在切换当下刷新——这正是《docs/pitfalls.md》「窗口态转移后主窗几何的恢复分两拍」在 Windows 的形态。

### 3.2 几何累计漂移

进全屏前客户区 `313,199 2247x1193`，外框 `313,168 2247x1224`（上边 31px 装饰）。绕开 withdraw 后第一次退出，客户区变成 `321,230 2231x1154`：宽 `-16`（左右各 8）、高 `-39`（上 31 + 下 8）、位置 `+8 / +31`，正好是一圈装饰。成因是装饰读数仍为 0 时 `QWidget::setGeometry` 把客户区矩形当外框下发给系统，系统按有框样式解释，客户区被内缩一圈；下一轮快照又取活体 `geometry()`，于是逐轮累计：

```
2247x1193 → 2231x1154 → 2215x1115 → 2199x1076
```

这正是《docs/pitfalls.md》「不要在转移中间按活体几何下发」「判已落定不得用尺寸不等于上一尺度」所禁止的形态。

### 3.3 装饰收敛必须等下一拍

样式切换后，装饰读数不会在同步调用链内归位，必须等事件循环下一拍才回到真实值（`frameGeometry()` 才比 `geometry()` 多出约 31px）。而窗口此时是可见的，等这一拍本身就构成一个可见中间帧；在读数归位之前下发任何几何，都会重复 3.2 的误差。

### 3.4 可见布局重排的入口

恢复序列会反复改写视频区几何：`MainWindow::resizeEvent` 与 `MainWindow::eventFilter` 中 `m_videoPlaceholder` 的 Resize / Move 分支都会触发 `syncVideoGeometry()`；`restoreOriginalParent`、`m_playlist->setVisible`、`applyPlaylistGeometry` 的迭代 resize、`m_refitAfterFullscreen` 的 `singleShot(0)` 是主要来源。布局事件异步到达，即便在恢复序列末尾收口一次，之后仍会有一次 `Resize` / `Move` 把窗口几何改掉，视频区随之再跳一次。

### 3.5 最大化状态机

最大化态进入时，`QWindow::setFlags(Frameless)` 或进入侧既有的 `setWindowState(Qt::WindowNoState)` 会撤掉 MAXIMIZED，回放系统动画；退出侧 `showMaximized()` 把 Qt 记账置真，但真实几何滞后两拍才落定，出现「最大化按钮显示最大化、实际窗口很小」。即便在进入侧保留 MAXIMIZED 记账，样式与几何操作仍会把它撤掉，退出侧依旧触发 `WindowStateChange` 与两拍几何。

### 3.6 Linux 侧对照

`qxcbwindow.cpp` 的 `QXcbWindow::setWindowFlags` 不 withdraw，只写 `_NET_WM_WINDOW_TYPE` / `_NET_WM_STATE` / Motif hints；但仅 `WindowStaysOnTopHint` / `WindowStaysOnBottomHint` 变化会置 `m_recreationReasons` 触发重建——这正是 N141 中「Qt 通道改置顶会销毁重建」的成因。因此 Linux 侧的正解仍是 `platform/X11Util` 直发 EWMH、几何交窗口管理器分两拍恢复、全程不碰 flags（N141 口径）。Windows 与 Linux 的差异不是「改 flag 的方式不同」，而是两套不同的窗口状态机。

## 四、取舍

| 方案 | 做法 | 结果 | 代价 / 否决理由 |
|---|---|---|---|
| 现状（A18） | `QWidget::setWindowFlags` + 隐藏期植入几何 + `show()` | 记账、几何、最大化全部正确 | 一次 15~40ms withdraw，露出下层窗口；视频区重排被隐藏期遮住 |
| 退出侧不恢复样式 | 跳过退出侧的 flags 恢复 | 不闪、无 withdraw | 窗口保持无框、边框不回来；恢复序列在可见态跑，视频区跳 1~2 次 |
| Win32 原地改样式 | `SetWindowLongPtr(GWL_STYLE)` + `SetWindowPos(SWP_FRAMECHANGED)` | 不闪、边框恢复 | 绕过 Qt 通道，QWidget 记账仍认无框，`frameGeometry()` 与真实装饰分家 |
| QWindow 通道 + 门控 | `QWindow::setFlags` + 门控 `resizeEvent` + 跳过迭代 resize | 不闪、QWindow 记账同步 | 边框不回来；门控覆盖不全（漏 `eventFilter` 入口）且收口过早，视频区仍被反复写 |
| 两侧统一 QWindow 通道 | 进入与退出都走 `QWindow::setFlags`，QWidget 记账恒定为有框 | 不闪、无快照污染 | 装饰缓存不刷新，逐轮累计漂移；最大化动画回放、按钮与几何不一致 |
| 等装饰收敛后提交 | 切换样式后同步等待装饰读数归位，再按客户区口径下发静态快照；最大化态保留 MAXIMIZED 记账 | 同步拿不到装饰读数 | 漂移与动画照旧；异步等一拍本身就是可见中间帧 |
| 覆盖层 | 保留 `QWidget::setWindowFlags`，用同屏抓屏覆盖层遮住 withdraw | 理论上记账正确且视觉连续 | 维护者否决：新增独立顶层窗、抓屏、置顶无框与 DPI/多屏处理，复杂度与维护成本过高，等于把问题搬家 |
| 完全平台化 | Win32 后端自持样式、几何、最大化状态，QWidget 永不改 flags | 单一真相、零 withdraw | 等于重写 Qt 窗口几何层；任务栏、弹层 transient parent、焦点链、`frameStrut`、`normalGeometry` 全部要自持，且异步装饰收敛的可见中间帧仍无解 |
| 缩短空档 | 恢复序列挪到 hide 之前，让 hide→show 只剩样式切换 | 无需新对象 | 空档主要耗在 `show()` 内部的样式与子窗口树重算，不在恢复序列里，成功无保证 |

## 五、为什么最终放弃

1. **hide 不是孤立的一句 `ShowWindow(SW_HIDE)`**。它绑定 `setParent_sys` 对 `WA_WState_Created` / `WA_WState_Visible` 的清零、`show()` 的重建映射，以及 `fstrut_dirty` 的置真。拿掉它，`QWidget` 层的装饰读数、客户区/外框换算、`normalGeometry`、最大化状态全部失去同步点；由此出现的漂移、边框丢失、动画回放都源于此，而非实现瑕疵。
2. **装饰收敛必须异步一拍**。窗口可见时等这一拍，就必然出现「有框全屏」或「几何未落定」的中间帧；要遮住它就得回到覆盖层，而覆盖层已被否决。
3. **完全平台化是结构上的单一真相，但代价与风险远超收益**：它要求自持窗口状态机、几何换算与任务栏/弹层/焦点链接线，并且仍要解决异步收敛的可见性。为一个 15~40ms 的观感问题重写窗口层，与「优先可维护性」的前提相悖。
4. **A18 的隐藏期虽然造成闪烁，却同时免费提供了几何收敛与布局批处理**。去掉它必须自己造一个等价的批处理机制，而所有已试的等价物都更重。

结论：**维持 A18 现状，把闪烁登记为已知缺陷，本轮不做修复**。重开的合理前提是：Qt 升级后 `QWidgetPrivate::setWindowFlags` 不再无条件走 `setParent` / hide；或主窗架构改为以 `QWindow` 为顶层窗、`QWidget` 只承载内容，从根上消除两套记账；或出现不引入额外对象即可遮蔽一次 withdraw 的平台能力。在此之前，任何「删掉 hide」的局部改动都会退化成第四节某一类代价。

## 六、重开时的起点

- 先复核当前 Qt 版本里 `QWidgetPrivate::setWindowFlags` 与 `QWidgetPrivate::setParent_sys` 的契约是否变化，以及 `QWidgetWindow` 是否开始监听 `QWindow::flagsChanged`；这两点决定绕开 QWidget 通道是否还需要自持记账。
- 若走平台化，先解决「装饰读数何时可信」：以 `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` 现读装饰，判据不得用「尺寸已不等于上一尺度」（《docs/pitfalls.md》同条），并把最大化状态与还原位置收成单点真相。
- 行为面回归清单至少覆盖：普通态/最大化态各一次进出、连续十次进出零累计漂移、多屏非主屏、高 DPI、Win11 圆角、任务栏覆盖、N66 浮窗与焦点基线、N90 console 让位、N137 媒体键、N68 等比缩放与吸附、N88 fullscreen 属性镜像、INI 落盘守卫。

# 字幕浏览器 / 编辑器可行性评估

- 日期：2026-10-27
- 范围：Windows 主路径；讨论对象以**文本字幕**（subrip/srt、ass/ssa、webvtt 等）为主，PGS / DVD 图形字幕不在内
- 依据：本机 mpv 源码 0.41.0（`player/lua/select.lua`、`osdep/subprocess-win.c`）与本地 mpv 手册；本机 ffmpeg 实测
- 性质：技术评估，不是方案规格。若后续决定实施，按主仓规格流程另立草案。

## 一、结论

1. **mpv 客户端 API 只暴露“当前这一行”，不暴露“全部行”，也没有任何写入字幕内容的通道。** 浏览与编辑都不能直接建立在现有 API 上，必须自己取数据（读文件或解包轨道）。这是全部障碍的根。
2. **内置菜单的 Subtitle lines 不是内核能力**，而是 `select.lua` 用 `subprocess` 命令拉 ffmpeg，把字幕轨导出成 LRC 文本再逐行解析。它证明了“API 之外有路”，同时把代价摆明了：**硬依赖外部 ffmpeg、丢全部样式与定位、时间只到秒**。
3. **只读浏览的最小可行面是“外部字幕文件”**：Qt 直接读文件自己解析，零外部依赖、可保真（原始文件里的样式块与全部字段都在手），覆盖多数真实使用场景。只有**内嵌轨**才需要 ffmpeg 或自己链 libavformat。
4. **编辑器的成本不在 UI，而在写回**：外部文件改完用 `sub-reload` 生效；内嵌轨 mpv 没有任何写通道，改完必须整轨重封装（mkvmerge / ffmpeg），是一次**文件级**操作，语义完全不同于“改一行”。
5. **建议：先做只读浏览，且不要把 ffmpeg 变成硬依赖。** ffmpeg 做成可选增强——有 ffmpeg 才解锁内嵌轨，没有也能对外部文件提供完整能力。不要一上来做编辑器。

## 二、需求切片

字幕功能不是一件事，先切开再谈难度：

| 切片 | 外部字幕文件 | 内嵌字幕轨 |
| --- | --- | --- |
| 只读浏览（列出全部行 + 点击跳转） | 直接读文件自行解析，无外部依赖 | 需 ffmpeg / libavformat |
| 跟随播放高亮当前行 | 监听 `time-pos` + 自有时间轴即可 | 同左 |
| 编辑文本并即时生效 | 改文件 + `sub-reload` | 无通道，需重封装 |
| 改时间轴（平移 / 重定时） | 同上 | 同上 |
| 编辑样式（ASS 特效、位置） | 写回文件即可 | 同上 |
| 图形字幕（PGS / DVD） | 读不出文本，先排除 | 同左 |

结论：**“外部 + 只读”是唯一低门槛切片**，其余每一格都额外付一笔账。

## 三、mpv 侧的事实边界

### 3.1 只有当前行

手册中与字幕内容相关的属性全部是“当前事件”语义：

- `sub-text`：当前字幕文本（去格式；非文本字幕返回空）
- `sub-text/ass`：当前行的 ASS 形式，只含 Text 部分
- `sub-text/ass-full`：当前 ASS 事件的**完整一行**（含全部字段），配合 `sub-ass-extradata` 可拿到样式头
- `sub-start` / `sub-end`：当前事件的首末时间（多个事件重合时取第一个）
- `secondary-sub-text`：同上，第二字幕
- `sub-ass-extradata`：当前 ASS 轨的 extradata 原文（非 ASS 轨不可用）

时间轴相关的只有全局项：`sub-delay` 整体平移，`sub-speed` 把事件时间戳乘一个系数（0.1–10.0，只影响文本文字幕）。**没有逐事件偏移**。

### 3.2 轨道元数据是够用的

`track-list` 已经给出做这件事需要的定位信息：

- `id`：用于 `sid` / `aid` / `vid` 的编号，**只在同类型轨道内唯一**（手册原文：unique within tracks of the same type），从 1 起
- `codec`、`type`、`selected`、`main-selection`（0 表示 sid，1 表示 secondary-sid）
- `external` + `external-filename`：外部字幕的来源串
- `ff-index`：ffmpeg 工具眼里的流下标（走 ffmpeg 路线时应当用这个，而不是 `id - 1`）

本仓 `MpvController` 的 `parseTrackList` 已经解析出 `id / type / codec / external / externalFilename / mainSelection`，**缺少 `ff-index`**——若走 ffmpeg 路线需要补字段。

### 3.3 命令面

- `sub-seek <skip>`：把播放位置移到相隔 `<skip>` 个事件的位置（适合按事件逐条导航）
- `sub-step <skip>`：改 `sub-delay` 使下一个事件显示（不动播放位置）
- `sub-add` / `sub-remove` / `sub-reload`：加载、卸载、重载字幕轨；**`sub-remove` 与 `sub-reload` 只对外部字幕文件有效**
- `seek`：真正的跳转仍由它落地

### 3.4 明确没有的

- **没有任何属性或命令列出全部字幕行**
- **没有任何写入字幕文本的接口**（`sub-text` 只能读不能设值，没有事件级增删）
- 没有“事件总数”之类的元信息
- 没有让客户端拿到解码后字幕包的通道（内容类能力一律终止在“当前 ASS 行”这一层）

## 四、mpv 内置菜单的做法

### 4.1 定位

内置脚本 `player/lua/select.lua`：菜单条目在 `menu` 绑定里列出来，动作是 `select-subtitle-line` 绑定。

### 4.2 完整流程

```lua
local sub = mp.get_property_native("current-tracks/sub")
-- 外部：ffmpeg -loglevel error -i <external-filename> -f lrc -map_metadata -1 -fflags +bitexact -
-- 内嵌：ffmpeg -loglevel error -i <path> -map s:<id-1> -f lrc -map_metadata -1 -fflags +bitexact -
local r = mp.command_native({name = "subprocess", capture_stdout = true, args = {...}})
```

拿到 LRC 文本后：逐行剥标签（HTML、ASS 花括号、`\h`、时间戳前缀，纯图形行丢掉）→ 以「秒」为键去重 → 按时间排序 → 拼成「时间 + 文本」的显示串 → 交给 `mp.input.select` 列出 → 选中后 `seek <time + sub-delay> absolute`。

几个怪癖（照抄实现时它们会跟着一起被复制过来）：

- `subrip` 单独放宽一条：**允许空行进列表**，其他 codec 的空行一律丢弃。脚本没写理由，属既有行为
- 时间用 `math.floor` 取**整秒**当键和 seek 目标：同一秒内的多行仍分别列出，但都只能跳到这一秒的起点，亚秒精度整体丢失
- 无视频轨、或视频轨是图片时（`current-tracks/video/image` 不等于 false），seek 额外 +0.1 秒；正常视频轨不加。脚本注释说是为了修暂停态下落不到目标事件
- 用 `-map s:<id-1>` 推 ffmpeg 流下标，本质是拿 mpv 的 `id` 当 ffmpeg 下标用；严格起见应该用 `ff-index`

### 4.3 可用性判据与失败表现

菜单项是否置灰只看“当前是否选中了文本字幕轨”：遍历 `track-list`，存在 `main-selection == 0` 且 codec 不是 `dvd_subtitle` / `hdmv_pgs_subtitle` 的字幕轨即可用。**不检测 ffmpeg 是否存在**，所以：

- ffmpeg 缺失 → `error_string == "init"` → OSD 报 `Failed to extract subtitles: ffmpeg not found.`
- ffmpeg 在但执行失败（打不开 URL、需要 header/cookie、容器不支持）→ `status ~= 0` → OSD 报 `Failed to extract subtitles.`

**没有 ffmpeg 时该项就是废的**——外部字幕也一样废：ffmpeg 是两分支的共同前置，三元表达式只决定喂给 ffmpeg 哪个输入。

### 4.4 为什么这么写

LRC 是唯一既带时间戳又是纯文本的中间格式，选它就不必在 Lua 里实现解复用与字幕解码。代价是这条链路天然**有损**：样式、位置、绘图、SSA 特效全都拿不回来，只能当“文本清单 + 跳转”用。

### 4.5 本机实测状态

- Windows 的 `subprocess` 底层是 `CreateProcessW(NULL, cmdline, ...)`（`osdep/subprocess-win.c`）：`lpApplicationName` 为 NULL，走 Win32 标准搜索序，**mpv.exe 所在目录排在 PATH 之前**——不想污染 PATH 就把 `ffmpeg.exe` 丢到 mpv.exe 同目录。
- 本机 PATH 内有 `ffmpeg 9.0.2-full_build-www.gyan.dev`，且 `-muxers` 含 `lrc`：内置该项当前可用。

## 五、实现“字幕浏览器”的障碍

### 5.1 数据源本身不可靠

- 不能裸读 `path` 属性。按主仓 `docs/pitfalls.md` 中“mpv 的 `path` 属性不是可信正播路径”那条，`path` 是 open 阶段处理出来的展示值，可能拼出不存在的路径；一律走 `MpvController::currentPlayingPath()`。
- `external-filename` 是**回显串**：可能是 URL，可能是 `edl://` 复合串（`select.lua` 自己就在处理这个前缀），也可能指向加密后端；能否直接读盘要用既有回显整形设施判定，不能假设它是本地可读路径。
- 本仓的 rclone crypt 与本地转接：交给外部程序的那个串若为虚拟 URL，ffmpeg 与 Qt 的 `QFile` 都打不开；只有本地转接地址才有戏。**这会直接决定功能要不要对某些来源禁用**。
- `external-filename` 指向的网络或临时文件可能被清理，缓存策略要想清楚。

### 5.2 解析必须自己实现

取到数据之后必须有一层解析器：

- srt / ass / webvtt 各自的方言；CRLF、BOM、无末尾换行、非法行、编码探测（GB18030 / UTF-8 / UTF-16 都不稀奇）
- ASS 还要处理 `[Script Info]`、`[V4+ Styles]`、`Dialogue:` 字段转义、`Comment:` 行、绘图行（纯绘图事件可能没有可读文本）
- 多事件时间重叠、时间戳格式（`0:00:01.23` 与 `0:00:01,23`）、事件乱序
- 这一层应独立成解析模块，按主仓测试约定覆盖（边界行 / CRLF / BOM / 非法行 / 同名重复），不要写在 UI 里

### 5.3 时间轴语义

- mpv 的实际显示时间 = 事件时间 + `sub-delay`（`sub-speed` 还会把时间戳整体乘系数），列表上的时间与跳转时间必须按同一套换算，否则看着总是错一行。
- “当前行”不能靠 mpv 直接给：需要监听 `time-pos` 后自己在事件表里二分查找。
- seek 之后立刻读 `sub-text` 不保证已经是目标行的值（属性更新滞后），跳转后的高亮要允许一帧级别的延迟。

### 5.4 UI 与性能

- 长片字幕是数千条量级：列表必须走 model/view 虚拟化，不能一次堆满控件。
- 跟随播放要不要自动滚：自动滚会把用户正在看的行推走，通常是“播放中跟随、手动滚动后暂停跟随一段时间”。
- 行内容带 HTML / ASS 标签时，是显示原串还是清洗后的串要先定，别让 UI 同时吃两种。
- 用户文案一律可翻译串（`tr(...)`），翻译事项不由方案承担。

### 5.5 进程与线程

- 若复用 ffmpeg：**不要在主插件 handle 上同步跑 `subprocess`**（会阻塞到进程结束；`playback_only` 默认 true，还会随当前播放条目结束被杀）。Qt 侧直接用 `QProcess` 异步更简单，也不用动 `plugin.cpp` 的符号 `RESOLVE` 清单；必须用 mpv 命令时走 `mpv_command_node_async` 配 `MPV_EVENT_COMMAND_REPLY`，并注意 `stdout` 是 `MPV_FORMAT_BYTE_ARRAY`（字节数组，不是字符串）。
- 结果要在“文件 + 轨道 + 字幕文件修改时间”粒度上缓存，避免每次开面板都重跑一次 ffmpeg。

## 六、实现“编辑器”的额外障碍

前面所有障碍之外，编辑器还要付：

1. **写回外部文件**：改完用 `sub-reload` 生效。但它是“卸载 + 重新添加”，**轨道 `id` 与选中状态都会变**，UI 必须重新定位当前轨；同名、多轨场景要防止加错。
2. **写回内嵌轨：mpv 没有通道。** 只能在磁盘上整轨重封装（替换字幕流），等待时间与失败风险都不是播放插件该承担的；而且这与本仓“只读地对待播放源”的立场冲突。
3. **要编辑的东西比想象的多**：文本之外还有样式、位置、层、ASS 特效；改了时间要重排事件顺序；保存要处理编码与 BOM（写坏就是整轨加载不出来）。
4. **与磁盘文件的竞态**：外部文件可能被别的进程改写，`sub-reload` 期间也可能正碰上用户在切轨，重载后要重新确认“当前轨还是不是原来那条”。
5. **保存语义要先定模型**：立即写磁盘、临时导出、还是“另存为新字幕文件”。多数播放器的做法是导出新文件，而不是改原文件。

结论：**编辑器是另一个量级的项目**（那是一整个字幕编辑器的工程量，不是播放器里的一个面板），不应与浏览器放在同一个方案里做。

## 七、路线对比

| 路线 | 覆盖范围 | 外部依赖 | 保真度 | 相对成本 | 备注 |
| --- | --- | --- | --- | --- | --- |
| A 只解析外部字幕文件 | 外部轨 | 无 | 高（含样式） | 低 | 推荐第一步；覆盖不到内嵌轨 |
| B ffmpeg subprocess（对齐内置做法） | 外部 + 内嵌 | ffmpeg | 低（LRC 有损） | 中 | 需处理存在性探测、异步、缓存 |
| C 自己链 libavformat | 外部 + 内嵌 | 构建期依赖 | 高 | 高 | 引入分发与许可证评估，与本仓轻依赖取向冲突 |
| D 第二个 core + `sub-seek` 逐行扫描 | 外部 + 内嵌 | 无 | 中（只能拿当前行文本） | 中，且慢 | 思路：用第二个 core 关掉音视频，反复 `sub-seek 1` 并读 `sub-text`。**未实测**：暂停态下 seek 后 `sub-text` 是否及时更新需要预研验证；逐行 seek 的耗时可能不可接受。仅在明确排斥 ffmpeg 时才评估 |
| E 不做 | — | — | — | 0 | 保持现状，用户用内置菜单 |

推荐组合：**A 为基线，B 为可选增强**（检测到 ffmpeg 才对内嵌轨开放）。

## 八、若本仓要做的前置纪律

- 取当前播放入口一律走 `MpvController::currentPlayingPath()`，不裸读 `path`（pitfalls 那条）。
- 外部字幕串一律经既有回显整形设施处理，不假设是本地可读路径；对不可处理的来源**显式禁用并给用户可读提示**，不要静默失败。
- 走 ffmpeg 路线要在 `TrackInfo` 补 `ff-index`，不再用 `id - 1` 推流下标。
- 外部进程必须异步化（`QProcess` 或 `mpv_command_node_async`），禁止在主 handle 上同步阻塞。
- 解析器独立成模块，按主仓测试约定覆盖边界情形。
- 新增用户文案用 `tr(...)`；方案不列翻译事项。
- 实施按主仓规格流程：先登记草案 → 澄清 → 冻结 → 用编号方案分支实施；不得越过。

## 九、附录

### 9.1 关键接口

- 属性：`sub-text` / `sub-text/ass` / `sub-text/ass-full` / `sub-ass-extradata` / `sub-start` / `sub-end` / `secondary-sub-text` / `sub-delay` / `sub-speed` / `current-tracks/sub` / `track-list`
- 命令：`sub-add` / `sub-remove` / `sub-reload` / `sub-seek` / `sub-step` / `seek` / `subprocess`
- 客户端符号：`mpv_get_property` / `mpv_observe_property` / `mpv_command` / `mpv_command_node` / `mpv_command_node_async`
- 节点格式：`MPV_FORMAT_NODE_MAP`（命名参数）、`MPV_FORMAT_BYTE_ARRAY`（子进程 stdout）

### 9.2 mpv 源码与手册位置

- `player/lua/select.lua`：`menu` 绑定的可用性判据，`select-subtitle-line` 的 ffmpeg 调用与 LRC 解析
- `osdep/subprocess-win.c`：`CreateProcessW(NULL, cmdline, ...)` 与其搜索序行为
- `DOCS/man/input.rst`：`subprocess` 命令、`sub-text` 属性族、`track-list/N/*`（含 `ff-index` 与 `main-selection` 的语义）、各 `sub-*` 命令
- `DOCS/man/options.rst`：`--stream-dump`（另一种“让外部工具也能读到源”的思路，供后续评估）

### 9.3 预研验证清单（走 B / D 之前先回答）

1. 外部 srt / ass 自解析器对真实素材的通过率（含 GB18030、BOM、非法行）
2. ffmpeg 是否安装、在 PATH 还是 mpv.exe 同目录、该构建是否带 `lrc` muxer
3. 对一条网络源：交给 ffmpeg 的那个串能否打开（含需要 header/cookie 的情形）
4. rclone crypt 场景下实际拿到的路径形态（虚拟 URL 还是本地转接地址）
5. `sub-reload` 之后轨道 `id` 与选中状态的实际变化（决定编辑器路线是否可行）
6. 路线 D：暂停态 `sub-seek` 后 `sub-text` 的更新延迟，以及逐行扫描的总耗时

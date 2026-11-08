# 自定义 mpv 渲染器评估

- 日期：2026-10-25（2026-11-08 按 mpv 0.42.0 复核 D3D11 render API）
- 范围：Windows 主路径；Wayland / macOS 只作移植评估
- 依据：本机 mpv 源码 0.41.0 / 0.42.0；0.42 `RELEASE_NOTES`、`include/mpv/render_d3d11.h`、`video/out/d3d11/libmpv_d3d11.c`
- 性质：技术评估，不是方案规格。若后续决定实施，按主仓规格流程另立草案。

> **0.42 复核摘要（2026-11-08）**：0.42 新增官方 **D3D11 render API 后端**（`MPV_RENDER_API_TYPE_D3D11`，`render_d3d11.h`）。调用方传入自己的 `ID3D11Device`，每帧把 `ID3D11Texture2D` 交给 `mpv_render_context_render`，纹理与交换链归调用方；支持 `R10G10B10A2` / `RGBA16F`，`d3d11va` 可在该 device 上零拷贝。它推翻了 2026-10-25 版“官方只有 OpenGL/SW、D3D11 只能靠第三方 fork”的事实前提；但该后端内部仍走 `render_backend_gpu` → `gl_video_init`，即 `vo=gpu`（`gl_video`），不是 `gpu-next`。**Windows 继续 `wid` 的结论不变**；变化是“自定义 D3D11 不再需要维护 mpv fork”，门槛降到 Qt RHI/DComp/交换链、HDR 信令与生命周期回归。

## 一、结论

1. **Windows 不做自定义渲染器，继续用 `wid` 嵌入。** 默认配置下当前路径已经是 D3D11 渲染（`vo=gpu-next` 默认走 d3d11 context），零拷贝硬解与 HDR 输出都保留；自建渲染器改变的是“谁持有交换链、谁做合成”，不是解码或缩放本身。
2. **mpv 0.42 起官方 render API 有 OpenGL、D3D11、软件三种后端**；仍没有 Vulkan 后端。D3D11 后端接受外部 `ID3D11Device` 并渲染到调用方提供的 `ID3D11Texture2D`，不再需要第三方 fork 才能用 D3D11；但它的渲染器仍是 `vo=gpu`（`gl_video`），不是 `gpu-next`。
3. **render API 的 GPU 后端仍是 `vo=gpu`（`gl_video`），不是当前的 `gpu-next`（libplacebo）；D3D11 后端也一样。** 换过去等于主动退回上一代渲染器；D3D11 后端解决了硬解拷贝与 10-bit 纹理格式，但 Qt Widgets 的最终输出/HDR 信令与合成路径仍是另一笔代价，画质与性能不能按“有 D3D11 就等价”来假设。
4. **自定义渲染器在 Windows 上的独特收益基本只剩 Wayland / macOS 移植**：`--wid` 只支持 X11、win32、Android。Qt 合成 / 叠层是另一个收益，但本仓已用 N3、N66、N68 等大量工作绕过去了，边际价值不足以支付画质与性能代价。
5. **如果未来要做，按平台分呈现路径**：Windows 保持 `wid`；Wayland / macOS 走官方 OpenGL render API。Windows 若未来做自定义合成，走官方 D3D11 render API + Qt RHI/DComp/自有交换链，不再需要 mpv fork；但仍是 2–4 周量级，不改变当前“不做”的结论。

## 二、当前基线

- 本仓是 mpv 的 C 插件：mpv 是宿主，插件产物是动态库，入口 `mpv_open_cplugin` 内建 Qt Widgets 窗口；插件全程通过 handle 与 mpv 通信，不拥有 mpv 生命周期。
- 主 core 的视频输出：插件把视频区原生子控件 `winId()` 写入 `wid`，mpv 自建子 HWND 并渲染进去。主 core 不覆盖 `vo`；在默认配置下使用 `gpu-next` + d3d11 context，硬解可走 `d3d11va` 零拷贝。
- 视频区是原生窗口（`Qt::WA_NativeWindow` + `Qt::WA_DontCreateNativeAncestors`）；控制栏、播放列表等叠层是 alien 控件，与原生窗口重叠时的合成次序未定义；全屏、小窗、焦点、光标、输入法都围绕这一事实做了专门处理。
- 缩略图用第二个 mpv core，自己 `mpv_create`，也用 `wid` 嵌进缩略图宿主窗；`hwdec=no`，与主 core 独立。
- 已积累的既有工作（N3 全屏、N66 右缘唤起、N68 小窗、N5/N85/N87/N117 焦点与键盘等）都是围绕原生子窗口建立的。评估自定义渲染器时，这些语义必须整体重验，不能只算“把画面接过来”的成本。
- 需要守住的基线指标：4K/60 播放时的 CPU 与 GPU 占用、硬解是否零拷贝、HDR/10-bit 是否保留、帧节奏与延迟、全屏/小窗切换稳定性、截图与 OSD 是否可用。

## 三、官方 render API 的事实边界

### 3.1 API 表面

`mpv/render.h` 公开的 API 类型（0.42）：

- `MPV_RENDER_API_TYPE_OPENGL`（`"opengl"`）
- `MPV_RENDER_API_TYPE_D3D11`（`"d3d11"`）——0.42 新增，详见 `render_d3d11.h`
- `MPV_RENDER_API_TYPE_SW`（`"sw"`）

仍没有 Vulkan 后端。D3D11 后端不再需要“外部传入 swapchain 门面”：调用方传入 `ID3D11Device`（`MPV_RENDER_PARAM_D3D11_INIT_PARAMS`），每帧传入一个 `ID3D11Texture2D`（`MPV_RENDER_PARAM_D3D11_FBO`，必须与传入的 device 相同，mpv 不持有引用）；纹理由调用方创建、持有和呈现。FBO 支持 `B8G8R8A8` / `R8G8B8A8` / `R10G10B10A2` / `R16G16B16A16_FLOAT`，不支持 sRGB 格式；纹理需 `D3D11_BIND_RENDER_TARGET` + `D3D11_USAGE_DEFAULT`，非 MSAA / 数组 / mipmap。Vulkan 仍只有 mpv 内部 `ra_ctx`，没有公开 render API。

### 3.2 渲染器代际

`vo=libmpv` 的 `render_backends` 仍只有 `render_backend_gpu` 与 `render_backend_sw`（见 `video/out/vo_libmpv.c` 的 `render_backends`）。D3D11 不是新的 render backend，而是 `render_backend_gpu` 下的一个 `libmpv_gpu_context`（`libmpv_gpu_context_d3d11`，见 `video/out/d3d11/libmpv_d3d11.c`）；GPU 后端的 `init` 最终仍调用 `gl_video_init`（见 `video/out/gpu/libmpv_gpu.c` 的 `init`），也就是 `vo=gpu` 的渲染器。

`gpu-next` 使用 libplacebo 的 `pl_renderer`，**没有 render API 后端**；D3D11 后端也不会改变这一点。因此：

- 选择官方 render API，渲染器从 `gpu-next` 退到 `gpu`；
- 受影响的不只是“GL 还是 D3D11”，还包括色调映射、Dolby Vision、缩放与色彩管理的实现路径；
- 对 SDR 1080p，两者差距可能不明显；对 HDR、4K、动画/渐变内容，差异需要实测，不能假设等价。

### 3.3 线程与生命周期契约

关键入口：

- `mpv_render_context_create`
- `mpv_render_context_set_update_callback`
- `mpv_render_context_update`
- `mpv_render_context_render`
- `mpv_render_context_report_swap`
- `mpv_render_context_free`

约束：

- update callback 由 mpv 的 VO 线程触发，**回调内不得调用 mpv API**；应转到 Qt 线程再处理。
- `mpv_render_*` 可以在任意线程调用，但同一 context 的调用必须串行；OpenGL 后端要求调用时 GL context 已 current。
- D3D11 后端：任意线程可调用，但同一 `ID3D11Device` 的 immediate context 不得与其他线程并发使用；mpv 不恢复 D3D11 状态，调用方在 `mpv_render_*` 之后要自行重设；几何/曲面细分/域着色阶段未设置时须保持 `ClearState()` 默认值。
- D3D11 硬解：`d3d11va` 在传入 device 上解码并零拷贝；若 device 以 `D3D11_CREATE_DEVICE_SINGLETHREADED` 创建，只支持 `d3d11va-copy`。
- 渲染线程不得等待“非 safe”的 libmpv 调用，否则会死锁或降级；mpv 的 `flip_page` 最多等约 200ms，超时会丢帧并打印 `mpv_render_context_render() not being called or stuck`。
- `MPV_RENDER_PARAM_ADVANCED_CONTROL` 会启用 Direct Rendering 与 GPU 截图，但同时把“渲染线程不得等 core”从建议变成硬约束；违反会真死锁。默认关闭，确认渲染循环不会阻塞后再开。
- `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME` 默认开启，`mpv_render_context_render` 会阻塞到目标显示时刻。GUI 线程渲染时应关闭，或配合 `video-timing-offset=0`，否则界面会被视频节奏拖住。
- `report_swap` 是可选的；一旦开始调用就要保持一致。开启 advanced control 后，每次 update callback 之后必须调用的是 `mpv_render_context_update`（不是 `report_swap`），并检查返回的 `MPV_RENDER_UPDATE_FRAME`。
- `MPV_RENDER_PARAM_SKIP_RENDERING` 可用于控件不可见时跳过实际绘制，但仍按正常帧时序处理。

### 3.4 从 cplugin 使用 render API

技术上可行，但属于非文档化用法：

- 插件入口拿到的是 `mp_new_client` 创建的普通脚本 client，与核心共享同一个 client API；`vo=libmpv` 的 `preinit` 从 client API 获取 render context（见 `video/out/vo_libmpv.c` 的 `preinit`），代码上没有“脚本 client 不允许”的检查。
- 本机 0.42.0 Windows 构建在 `--vo=help` 中列出 `libmpv`，并导出 `mpv_render_context_*` 全部符号；本仓插件本来就用 `mpv_create` 建第二 core，说明宿主是静态链接 libmpv、API 可达。
- D3D11 后端没有新增 `mpv_render_context_*` 符号；实现时把 `render_d3d11.h` 纳入内置头，并在 Windows 侧链接 `d3d11.lib` / `dxgi.lib`。
- `mpv/render.h` 在 `MPV_CPLUGIN_DYNAMIC_SYM` 下也定义了 `pfn_mpv_render_context_*` 指针；插件现有 `resolveMpvSymbols` 只需增加对应的 `RESOLVE(...)` 项，不必手写函数指针表。
- 但 mpv 手册明确说 `libmpv` 这个 VO 是“供 libmpv 直接嵌入使用，在其他场景无用”（`DOCS/man/vo.rst` 的 `libmpv` 一节）。从 cplugin 里设置 `vo=libmpv` 不是上游承诺的用法，必须运行时探测，并承担上游行为变化的风险。
- 不是所有 mpv 构建都保证包含 `vo=libmpv` 或导出 render API；插件启动时要检查 `vo` 列表与符号，失败就回退到 `wid`，不能把启动路径写死。
- `vo=libmpv` 与 `wid` 互斥；如果用户已经配置了 `wid`，应沿用现有的“让位”策略，不强行接管。

### 3.5 硬件解码

- `hwdec=d3d11va` 的零拷贝前提是渲染后端为 D3D11：`video/out/d3d11/hwdec_d3d11va.c` 的 `init` 会检查 `ra_is_d3d11`，GL 后端直接返回失败。
- **官方 D3D11 render API**：外部 device 上 `d3d11va` 可零拷贝（device 不能是 `SINGLETHREADED`）；这是 0.42 新增的能力，不再依赖第三方 fork。
- **官方 OpenGL render API**：Windows 上零拷贝仍要 ANGLE；否则只能走 `dxva2-dxinterop`（WGL-NV，主要是 NVIDIA）或 `nvdec`（CUDA-GL，NVIDIA）。Intel / AMD 在 WGL 下通常落到 `d3d11va-copy`，每帧 GPU→CPU，再上传给 GL；4K/60 开销明显，8K 基本不可接受。
- `hwdec=auto-safe` 会自动避开不可用的 interop，不会崩，但“能不能零拷贝”取决于后端、显卡与驱动；D3D11 后端也不是所有构建/设备都可用。

### 3.6 输出、色彩与合成

- QOpenGLWidget 的默认 FBO 是 8-bit RGBA；Qt Widgets 的最终合成与窗口缓冲也没有 HDR/10-bit 输出通路。即使把内部 FBO 提到更高精度，最终仍落到 8-bit，HDR 与 10-bit 只能靠 mpv 先 tone-map。
- QOpenGLWidget 的画面还要经过 Qt 的 backing store 合成，比 `wid` 直出多一次全屏绘制；4K 下这是可测量的 GPU 带宽与合成开销。
- `MPV_RENDER_PARAM_OPENGL_FBO` 的宽高必须按设备像素给（`widget 尺寸 × devicePixelRatio`），否则高 DPI 下会糊。
- `MPV_RENDER_PARAM_FLIP_Y` 需要按实际方向验证；OpenGL FBO 的原点与 mpv 输出约定不一致时画面会上下颠倒。
- D3D11 FBO 支持 `R10G10B10A2` / `RGBA16F`，纹理层面可 10-bit/HDR；但 render API 没有 HDR 元数据参数，DXGI 色彩空间、HDR10 metadata、交换链呈现都要调用方自己设置。
- D3D11 的最终合成取决于调用方：Qt Widgets 的 raster 最终缓冲仍没有 HDR/10-bit 通路；QRhiWidget/自有交换链是另一笔工程。纹理跨 device 共享还要 keyed mutex/fence。

### 3.7 其他功能面

- OSD：mpv 的 `osd-overlay` / `show-text` 仍然由 mpv 渲染进画面，理论上不受影响；独立的 Qt OSD 窗也不受影响，但叠层关系从“原生/alien”变成普通 Qt 合成，需要回归。
- 截图：`screenshot-raw` 在 `vo=libmpv` 下走 VO 当前帧与软件转换路径，是否可用、硬解帧是否能正确下载，必须在 spike 中实测；若不可用，需要 advanced control 的 GPU 截图或软件回退。
- 字幕、滤镜、panscan、旋转、keepaspect 等由 mpv 内部处理，理论上与 VO 无关。
- D3D11 后端下 mpv 仍把 OSD/字幕渲染进调用方纹理；截图、硬解帧下载、GPU 截图需按 D3D11 路径实测。
- 任务栏缩略图/进度依赖主窗 HWND，视频区换成 QOpenGLWidget 或 RHI 控件后主窗仍在，通常不受影响，但要回归。
- 第二个 core（缩略图）继续用 `wid` 即可，不需要跟着改。

## 四、为什么 Windows 不做自定义渲染器

### 4.1 目标不匹配

本项目当前只保证 Windows 编译运行。Windows 上 `wid` 路径已经由 mpv 自己完成 D3D11 渲染、零拷贝硬解与 HDR 输出；自定义渲染器不会让解码更快，也不会让缩放更好，只会把“呈现”从 mpv 搬到插件。

### 4.2 画质倒退

- render API 的 GPU 后端仍是 `vo=gpu`，不是 `gpu-next`；D3D11 后端也一样。色调映射、Dolby Vision、缩放与色彩管理都会换一条实现路径。
- D3D11 FBO 可给 10-bit/HDR 纹理，但 HDR 元数据与最终合成归调用方；QOpenGLWidget 路径仍 8-bit。不能把“D3D11 纹理格式”当成“画质等价”。
- 这两项叠加，对以画质为卖点的播放器仍是实质性退步；D3D11 只消除了硬解拷贝和 fork 成本，没有把渲染器代际补回来。

### 4.3 性能倒退

- 官方 D3D11 render API 解决了 Intel/AMD 的 `d3d11va` 零拷贝（不再必须 `d3d11va-copy`）；OpenGL 路径的拷贝问题仍在。
- Qt 侧仍多一次合成/呈现（QOpenGLWidget 或 QRhi/自有交换链）；GUI 线程渲染与界面事件争抢时间。
- 4K/60 与高码率 HEVC/AV1 仍是最容易暴露问题的场景。

### 4.4 复杂度与风险

- QOpenGLWidget 的上下文可能被销毁重建；本仓全屏与小窗都通过 `setWindowFlags` 触发平台窗口重建，必须先解决 render context 的重建或改用不重建窗口的全屏路径。
- D3D11 路径不需要 GL context，但要自己管 device/swapchain、immediate context 单线程、共享纹理/fence、resize 与 HDR 元数据；Qt RHI 集成与 device 共享是主要不确定项。
- D3D11 device 不能是 `SINGLETHREADED`（否则硬解只能 copy）；混合显卡/多 GPU 要选 adapter。
- 视频区从原生 HWND 变成普通 widget，N5/N85/N87/N117 的焦点、光标、输入法、覆盖态键盘让位等语义都要重验；这部分工作量往往被低估。
- `vo=libmpv` 从 cplugin 使用不是上游文档化用法，宿主构建差异、脚本 client 权限、退出顺序都可能成为故障点。
- 渲染节奏与 GUI 事件循环耦合：GUI 线程渲染简单，但界面卡顿会直接掉帧；独立渲染线程又要处理共享 GL context、离屏 FBO、纹理合成与生命周期，复杂度显著上升。

### 4.5 收益边际很小

- Qt 合成/叠层确实是自定义渲染器的好处，但本仓已经为原生子窗口付出了大量工程：全屏隐藏菜单栏、叠层宿主、焦点收口、光标处理、任务栏集成等。
- D3D11 render API 把“自定义合成”的门槛从维护 mpv fork 降到 Qt RHI/DComp/交换链，但画质代际（gpu vs gpu-next）、HDR 信令与 Qt 最终合成都还在；为了叠层而换渲染器仍不划算。若叠层/z-order 是痛点，先修合成层（DirectComposition、原生叠层窗、z-order 管理）。

### 4.6 第三方 D3D11 fork：现在只服务于 gpu-next/DXGI 私有能力

0.42 官方 D3D11 render API 已覆盖“外部 device + 外部纹理 + d3d11va 零拷贝”，**只为 D3D11 已不需要 fork**。仍可能需要 fork 的情形只剩：要把 `gpu-next`（`pl_renderer`）接到 D3D11 render API 上（上游没有这个后端），或需要私有的 DXGI swapchain 门面/跨进程共享/其他上游未提供的语义。

如果仍走第三方 fork，常见做法是在 libmpv render API 上扩展 `MPV_RENDER_API_TYPE_DXGI`，通过 `mpv_dxgi_init_params{device, swapchain}` 把外部 D3D11 设备和一个自实现的 `IDXGISwapChain` 门面交给 mpv；门面内部通常是多张共享纹理 + fence 的 mailbox。这个方案要成立，前提是：

- mpv 侧有对应的私有 render backend（上游没有），必须维护一份定制 mpv 构建；
- 宿主侧有外部纹理消费机制（例如注册纹理、拿到共享句柄）；Qt Widgets 没有等价的 texture registrar；
- 插件侧要自己管理 D3D11 device、adapter 选择、共享纹理、fence 与呈现线程。

更关键的是：如果只看第三方 fork 的公开接口，只能看到“注入 device/swapchain”，**无法确认它的渲染器是 `gl_video`（`vo=gpu`）还是 `pl_renderer`（`gpu-next`）**（官方 0.42 D3D11 后端已明确是 `gl_video`）。最省力的实现通常是复用 `gl_video`，那样即使换成 D3D11，也仍然回不到 `gpu-next` 的画质；要做到“D3D11 + gpu-next”，等于把 `pl_renderer` 移植到外部交换链接口，工程量在 mpv fork 侧，且每次 mpv 升级都要 rebase。

这与本仓“不分发 mpv 库、不随 mpv 版本重发”的原则直接冲突。**只做 D3D11 已不需要它**；只有当“Windows 自定义合成 + HDR/10-bit + 零拷贝且必须 gpu-next”成为硬需求，并且团队接受长期维护 mpv fork 时，才按方案 B 单独立项。

### 4.7 对比表

| 维度 | 当前 `wid` | 官方 GL render API | 官方 D3D11 render API（0.42） | 第三方 D3D11 fork |
| --- | --- | --- | --- | --- |
| 渲染器 | `gpu-next`（libplacebo） | `vo=gpu`（`gl_video`） | `vo=gpu`（`gl_video`） | 通常仍是 `vo=gpu`，除非把 `pl_renderer` 接进来 |
| 硬解 | `d3d11va` 零拷贝 | Intel/AMD 多为 `d3d11va-copy`；NVIDIA 可 `nvdec` | `d3d11va` 零拷贝（非 `SINGLETHREADED`） | 可 `d3d11va` 零拷贝 |
| HDR/10-bit | 可保留 | 最终 8-bit，HDR 丢失 | 纹理可 10-bit/HDR；HDR 元数据与最终合成归调用方 | 取决于交换链与渲染器，可做 10-bit |
| 合成 | 原生子 HWND + alien 叠层 | Qt 原生合成 | 调用方 swapchain / QRhi / DComp，复杂 | 需 Qt 侧外部纹理导入，复杂 |
| 移植 | X11 / win32 / Android | Wayland / macOS 可用 | Windows only | 同 GL，但需定制 mpv |
| 插件侧成本 | 0 | 约 2–4 周 | 约 2–4 周起，另加 Qt/HDR 集成 | 1.5–3 个月起，加长期维护 |
| 主要风险 | 原生窗口/叠层 | 画质、性能、上下文重建 | HDR 信令、Qt 纹理/设备共享、生命周期 | mpv fork、分发、ABI |

## 五、如果要做：方案 A（官方 render API）

> Windows 自定义合成优先用 0.42 的 D3D11 后端；Wayland / macOS 用 OpenGL 后端。下面 §5.2–5.4 以实现 OpenGL 后端为主，D3D11 的差异见 §5.5。

### 5.1 适用条件

只建议在以下条件之一成立时做：

- Wayland / macOS 进入实际路线图，需要一条可移植的呈现路径；
- （OpenGL 路径）明确接受 Windows 上 `gpu` 替代 `gpu-next`、8-bit 输出与可能的硬解拷贝；D3D11 路径的取舍见 §5.5；
- 或者只是做一次性 spike，用来量化代价，不进入主线。

如果只是 Windows 叠层/z-order 痛点，不要走这条。

### 5.2 总体结构

建议把“视频呈现”收成一个窄接口，而不是把 `wid` 语义散落在主窗：

- `WidPresenter`：现有路径，Windows / X11 使用；
- `RenderApiPresenter`：QOpenGLWidget + `vo=libmpv`，Wayland / macOS 使用；
- `D3D11Presenter`：Windows 自定义合成时用官方 D3D11 render API + 自有交换链/QRhi（见 §5.5），不编译 OpenGL 路径；
- 启动时按平台与能力探测选择，失败回退到 `WidPresenter`。

若只在 Wayland / macOS 编译 `RenderApiPresenter`，Windows 二进制可以完全不含 GL 路径，避免无谓的回归面。

### 5.3 实现步骤

#### 5.3.1 构建与符号

- CMake 增加 `Qt6::OpenGLWidgets`（静态 Qt 包已提供 `Qt6OpenGLWidgets`）。
- 包含 `<mpv/render.h>`；在 `plugin.cpp` 的 `resolveMpvSymbols()` 里增加：

  - `mpv_render_context_create`
  - `mpv_render_context_set_update_callback`
  - `mpv_render_context_update`
  - `mpv_render_context_render`
  - `mpv_render_context_report_swap`
  - `mpv_render_context_free`

  `mpv/render.h` 在 `MPV_CPLUGIN_DYNAMIC_SYM` 下已经定义了对应的 `pfn_*` 变量，沿用现有 `RESOLVE` 宏即可。
- D3D11 版本另需 `<mpv/render_d3d11.h>`、`d3d11.lib` / `dxgi.lib`，以及 `MPV_RENDER_PARAM_D3D11_*`；render context 符号不变。
- 入口处做能力探测：宿主是否导出上述符号、`vo` 列表中是否有 `libmpv`；任一不满足就回退 `wid`。

#### 5.3.2 启动与 render context 创建

- 不再设置 `wid`，改为 `mpv_set_property_string(handle, "vo", "libmpv")`；两者不能同时设置。
- render context 必须在视频 VO 创建前建立。插件通常在启动时加载，此时尚未起播；若插件是后加载的，创建 context 会打断当前 VO，需要接受一次黑闪或拒绝接管。
- 创建 context 时使用 Qt 当前的 GL 上下文：

```cpp
mpv_opengl_init_params init{};
init.get_proc_address = [](void *, const char *name) -> void * {
    auto *ctx = QOpenGLContext::currentContext();
    return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(name)) : nullptr;
};
mpv_render_param params[] = {
    {MPV_RENDER_PARAM_API_TYPE,
     const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init},
    {MPV_RENDER_PARAM_INVALID, nullptr},
};
mpv_render_context_create(&m_renderCtx, m_mpv, params);
```

- 设置 update callback，回调只做线程切换，不碰 mpv：

```cpp
mpv_render_context_set_update_callback(
    m_renderCtx,
    [](void *opaque) {
        auto *w = static_cast<VideoGlWidget *>(opaque);
        QMetaObject::invokeMethod(w, "update", Qt::QueuedConnection);
    },
    m_widget);
```

- 若启用 `MPV_RENDER_PARAM_ADVANCED_CONTROL`，渲染线程还要调用 `mpv_render_context_update()`，并根据 `MPV_RENDER_UPDATE_FRAME` 决定是否 `render`；第一版不要开，先证明渲染循环不会阻塞 core。

#### 5.3.3 绘制

在 `QOpenGLWidget::paintGL` 里，Qt 已经让该 widget 的 GL context current，直接把 mpv 画进默认 FBO：

```cpp
mpv_opengl_fbo fbo{
    defaultFramebufferObject(),
    int(width() * devicePixelRatioF()),
    int(height() * devicePixelRatioF()),
    0, // internal_format：0 表示未知/默认
};
int flip = 1;       // 按实际方向验证
int block = 0;      // GUI 线程渲染应关闭阻塞
mpv_render_param params[] = {
    {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
    {MPV_RENDER_PARAM_FLIP_Y, &flip},
    {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
    {MPV_RENDER_PARAM_INVALID, nullptr},
};
mpv_render_context_render(m_renderCtx, params);
mpv_render_context_report_swap(m_renderCtx);
```

- `width/height` 用设备像素；`resizeGL` 只负责让 widget 重新 paint，mpv 会按 FBO 尺寸重新配置。
- 关闭 `BLOCK_FOR_TARGET_TIME` 后建议同时把 `video-timing-offset` 设为 0，否则 A/V 同步可能有偏移；这两项要一起实测。
- 若开启 advanced control，每次 update callback 之后必须调用 `mpv_render_context_update` 并检查 `MPV_RENDER_UPDATE_FRAME`；`report_swap` 仍按“一旦使用就保持一致”的规则调用。

#### 5.3.4 生命周期与上下文重建

- 连接 `QOpenGLWidget::aboutToBeDestroyed`：先清 update callback，再 `makeCurrent()`，调用 `mpv_render_context_free`，`doneCurrent()`，最后置空。
- 如果 GL context 被重建（全屏/小窗 `setWindowFlags`、reparent、显式重建），必须重新创建 render context；旧 context 的 GL 对象全部失效。
- 启动时如果 render context 创建失败，恢复 `vo` 与 `wid`，走旧路径；不要留下“vo=libmpv 但没有 context”的半死状态。
- 退出顺序：先停止起播/清回调，再释放 render context，最后销毁 QOpenGLWidget；避免 mpv 的 VO 线程访问已销毁的 FBO。

#### 5.3.5 线程模型

- **第一版：GUI 线程渲染。** 简单、贴近 QOpenGLWidget 的常规用法；风险是界面卡顿会直接掉帧，且 `paintGL` 的节奏受 Qt 重绘调度影响。
- **第二版（仅在 GUI 线程不可接受时）：独立渲染线程。** 需要独立的 GL context、离屏 FBO、与 Qt 共享纹理或回读合成；还要处理上下文共享、fence、生命周期与报告 swap。工作量远大于第一版，不要一上来就做。
- 无论哪种，update callback 都只负责唤醒，不在 mpv 线程做 GL 或 Qt 控件操作。

#### 5.3.6 全屏、小窗与焦点

- 优先改用不重建平台窗口的全屏路径；若必须保留现有 `setWindowFlags` 方案，则把“render context 重建”纳入全屏/小窗切换的固定步骤，并做反复进出测试。
- QOpenGLWidget 是普通 widget：去掉 `WA_NativeWindow` / `WA_DontCreateNativeAncestors`，重新审视频区的焦点策略、`QWindow` 级过滤器、光标自动隐藏、输入法提交路径、覆盖态键盘让位。
- N5/N85/N87/N117 的既有行为要逐项回归；焦点链的先后由控件被 parent 进顶层的顺序决定，视频区从原生窗口变成普通 widget 后，这个顺序可能变化。

#### 5.3.7 硬解、色彩与截图

- 硬解：默认 `auto-safe`；在日志里记录实际选中的 hwdec，确认 Intel/AMD 是否落到 `d3d11va-copy`；如有 NVIDIA，验证 `nvdec` 零拷贝路径。
- 色彩：明确接受 8-bit 输出；如果业务要求 HDR/10-bit，方案 A 直接不成立，不要试图用更高精度 FBO 绕，最终合成仍会丢。
- 截图：先验证 `screenshot-raw`；若在 `vo=libmpv` 下失败，再评估 advanced control 的 GPU 截图或软件回退，不能默默缺失功能。
- OSD：沿用 mpv 的 `osd-overlay` / `show-text`；Qt 叠层改为普通合成后，回归叠层 z-order 与全屏菜单栏策略。
- 第二个 core：继续 `wid`，不改。

#### 5.3.8 主要风险与缓解

| 风险 | 表现 | 缓解 |
| --- | --- | --- |
| GL context 重建 | 全屏/小窗后黑屏、崩溃、图像冻结 | 监听 `aboutToBeDestroyed`，重建 render context；优先改掉 flags 重建路径 |
| 硬解拷贝 | 4K/60 CPU 飙高、掉帧 | 记录实际 hwdec；必要时给用户明确提示；HDR/高码率场景回退 `wid` |
| 8-bit 输出 | HDR 发灰、色带 | 明确不支持；有 HDR 需求时不做 A |
| GUI 线程阻塞 | 菜单/拖动卡顿，mpv 打印 stuck | 关闭 `BLOCK_FOR_TARGET_TIME`；必要时改独立渲染线程 |
| `vo=libmpv` 灰区 | 某些 mpv 构建不导出/不含该 VO | 启动探测，失败回退 `wid`；日志明确原因 |
| 截图/OSD 回归 | 截图失败、OSD 位置异常 | spike 阶段逐项验证，失败即回退 |
| 焦点/光标回归 | 键盘被吞、光标不隐藏、IME 失效 | 按 N5/N85/N87/N117 清单回归；不要只测鼠标点击 |

### 5.4 Spike 与验收指标

建议先做 2–4 天的 spike，只回答“值不值得继续”：

1. 从 cplugin 成功创建 render context，起播一个 1080p 文件；
2. 4K/60 HEVC 播放 10 分钟，记录 CPU、GPU、实际 hwdec、丢帧数；
3. 全屏进出、小窗切换、DPI 切换各 10 次，确认无黑屏/崩溃；
4. 截图、OSD、字幕、panscan、旋转各验证一次；
5. 记录 `gpu` 与当前 `gpu-next` 的截图/画面对比（HDR 与 SDR 各一组）；
6. D3D11 路径另测：device 共享、纹理格式/HDR 元数据、swapchain resize、immediate context 串行。

Go 的条件：4K 硬解可接受、全屏/小窗稳定、截图与 OSD 可用、画质差异在可接受范围。任何一项不达标，就不进入主线。

若进入主线，按本仓标准估算约 2–4 周（含规格、实现、回归与文档），最大不确定项是上下文重建和焦点/光标回归。

### 5.5 Windows 自定义合成：官方 D3D11 render API（0.42）

- 用 `MPV_RENDER_API_TYPE_D3D11` + `mpv_d3d11_init_params{device}` 创建 render context；device 由插件创建或取自 Qt RHI（`QRhiD3D11NativeHandles`），不要用 `SINGLETHREADED`。
- 每帧创建/复用一个 `ID3D11Texture2D`（`D3D11_BIND_RENDER_TARGET` + `D3D11_USAGE_DEFAULT`，格式 `R10G10B10A2` / `RGBA16F` / `BGRA8`），通过 `mpv_d3d11_fbo{tex}` 交给 `mpv_render_context_render`。
- 呈现：自有 DXGI swapchain 或 QRhiWidget + 原生纹理包装；纹理跨 device 共享用 keyed mutex/fence；resize 重建纹理与交换链。
- HDR：render API 没有 HDR 元数据参数；DXGI 色彩空间、HDR10 metadata、SDR/HDR 切换由插件设置。Qt Widgets 的最终缓冲仍无 HDR 通路，必须评估 QRhi/DComp 或自有交换链。
- 线程：D3D11 immediate context 单线程，`mpv_render_*` 调用期间禁止其他线程使用；mpv 不恢复 D3D11 状态。
- 生命周期：render context 必须在 VO 创建前建立；全屏/小窗若重建窗口，要重建 swapchain/纹理；退出先停渲染、清回调、释放 context，再销毁纹理。
- 画质：后端仍是 `vo=gpu`；若必须 `gpu-next`，回到方案 B（fork/port）。

## 六、如果要做：方案 B（第三方 fork；现在只服务于 gpu-next/DXGI 私有能力）

0.42 官方 D3D11 render API 已覆盖“外部 device + 外部纹理 + d3d11va 零拷贝”，**只为 D3D11 已不需要 fork**。只有在“Windows 必须自定义合成 + HDR/10-bit + 零拷贝硬解 **且要求 gpu-next**”同时成立，或需要上游没有的 DXGI swapchain 门面/私有语义时，才考虑维护 mpv fork。

### 6.1 mpv 侧

- 上游 0.42 已有 D3D11 backend；fork 的增量只剩 `gpu-next`/DXGI 私有部分。
- 新增一个 render API 类型（例如 `dxgi`）与对应的 init params，携带外部 `ID3D11Device` 与 `IDXGISwapChain`。
- 新增 render backend：
  - 用外部 device 创建 `ra_ctx` / `ra`（D3D11）；
  - 选择渲染器：复用 `gl_video` 最省力，但仍是 `vo=gpu`；若要求 `gpu-next`，需要把 `pl_renderer` 接进 render backend，工作量与风险都显著上升；
  - 接入 hwdec：`d3d11va` 的 mapper 依赖 `ra_is_d3d11`，外部 D3D11 context 才能零拷贝；
  - 处理 `wrap_fbo`、`done_frame`、screenshot、resize、色彩空间与 HDR 元数据。
- 交换链契约：mpv 每帧会 `GetBuffer(0)` 取渲染目标，`Present` 可能被调用；门面必须正确实现 `GetBuffer` / `GetDesc` / `Present`，其余方法即使 stub 也要返回合理值。门面内部通常用多张共享纹理加 fence 做 mailbox，避免生产/消费互相覆盖。
- 同步与 resize：换尺寸要重建纹理与共享句柄；跨进程/跨 API 共享要处理 fence 与 adapter 一致性。
- 头文件与 ABI：新增头文件、API 类型字符串、版本约束；定制 mpv 与插件必须成对发布。

### 6.2 插件侧

- 创建 D3D11 device，尽量与 Qt 的 RHI/adapter 同卡；混合显卡/多 GPU 场景要显式选择 adapter，否则共享纹理可能黑屏。
- 呈现到 Qt 有两种方式：
  - **QRhiWidget + 原生纹理包装**：由 Qt RHI 的 D3D11 后端消费共享纹理；需要处理 device 共享、immediate context 单线程、纹理生命周期与 resize。
  - **原生子 HWND + DXGI 交换链**：技术上最直接，但又回到 `wid` 的原生窗口模型，叠层/合成问题原样存在，基本失去做自定义渲染器的意义。
- 帧同步：生产者提交后 signal fence，消费者拿到最近完成的共享句柄；要处理“最新帧覆盖旧帧”、resize 期间的句柄失效、退出时的异步释放。
- 线程：D3D11 immediate context 必须单线程使用；渲染线程与 Qt 线程的边界要固定，不能随手在 UI 线程碰 device context。

### 6.3 构建、分发与维护

- 必须自建并分发定制 mpv（或定制 libmpv），与本仓“不分发 mpv、不随 mpv 版本重发”的原则冲突；还要承担 GPL/LGPL 分发义务的评估。
- 每次 mpv 升级都要 rebase 私有 backend，回归 hwdec、HDR、色彩、截图、OSD、全屏。
- 插件只能与定制 mpv 配套使用，失去“任意 mpv 构建都能加载”的部署优势。
- 粗估：mpv 后端 3–6 周，Qt 侧纹理接入 2–4 周，合计 1.5–3 个月起；这还是顺利情况。

### 6.4 采用前的审计项

如果仍要评估方案 B，先拿到 fork 的 mpv 侧补丁并确认：

1. render backend 用的是 `gl_video` 还是 `pl_renderer`；
2. 是否支持 `gpu-next` 的色彩管理/HDR；
3. `d3d11va` 是否真正零拷贝，还是内部仍走 copy；
4. 交换链门面的 `GetBuffer`/`Present`/resize/fence 语义；
5. 对 mpv 版本的依赖与 rebase 成本；
6. 许可证与分发方式。

在这些问题没有明确答案前，不要把“D3D11 渲染器”当成画质/性能的解决方案。

## 七、平台矩阵与决策

| 平台 | 当前 | 若未来需要移植 / 自定义呈现 |
| --- | --- | --- |
| Windows | `wid`（gpu-next + d3d11 + 零拷贝 + HDR） | 默认保持 `wid`；若做自定义合成，用官方 D3D11 render API + Qt RHI/DComp/自有交换链（2–4 周起，无需 fork） |
| Linux X11 | `wid` | 保持 `wid` |
| Wayland | 无 `wid` 支持 | 官方 GL render API（接受 `gpu`/8-bit/硬解拷贝） |
| macOS | 无 `wid` 支持 | 官方 GL render API（同上） |

决策规则：

- **Windows 叠层/z-order/全屏痛点** → 先修合成层（DirectComposition、原生叠层窗、z-order、隐藏策略），不要为了叠层换渲染器。
- **Wayland/macOS 进入路线图** → 只为这些平台实现 OpenGL render API，Windows 继续 `wid`；不要做全局切换。
- **HDR/10-bit/零拷贝是硬指标** → 保留 `wid`；D3D11 render API 虽能出 10-bit 纹理，但 HDR 元数据与 Qt 最终合成仍要自建，不是开箱等价。
- **Windows 自定义 D3D11 但不要求 gpu-next** → 用官方 D3D11 render API，无需 fork。
- **Windows 自定义 D3D11 且必须 gpu-next** → 这是 mpv fork 项目，不是插件项目；按方案 B 单独立项评估。

## 八、附录

### 8.1 关键符号

- render context：`mpv_render_context_create` / `set_update_callback` / `update` / `render` / `report_swap` / `free`
- 参数：`MPV_RENDER_PARAM_API_TYPE`、`OPENGL_INIT_PARAMS`、`OPENGL_FBO`、`D3D11_INIT_PARAMS`、`D3D11_FBO`、`FLIP_Y`、`BLOCK_FOR_TARGET_TIME`、`ADVANCED_CONTROL`、`NEXT_FRAME_INFO`、`SKIP_RENDERING`
- 更新标志：`MPV_RENDER_UPDATE_FRAME`
- 硬解：`d3d11va`、`d3d11va-copy`、`nvdec`、`dxva2-dxinterop`、`d3d11-egl`

### 8.2 mpv 源码位置（相对 mpv 源码根）

- `include/mpv/render.h`：API 类型、参数、线程契约
- `include/mpv/render_gl.h`：OpenGL 后端与 Windows/ANGLE 说明
- `include/mpv/render_d3d11.h`：D3D11 后端、init params、FBO 格式/线程/硬解约束
- `video/out/vo_libmpv.c`：`render_backends`、`preinit`、update callback、200ms 超时
- `video/out/gpu/libmpv_gpu.c`：GPU 后端 `init`、`gl_video_init`、`libmpv_gpu_context_d3d11` 分派
- `video/out/d3d11/libmpv_d3d11.c`：D3D11 context 的 `ra_d3d11`、`wrap_fbo` / `done_frame`
- `video/out/d3d11/ra_d3d11.c`：外部 device 的 `ra` 与纹理包装
- `video/out/opengl/libmpv_gl.c`：OpenGL 后端 `wrap_fbo` / `done_frame`
- `video/out/d3d11/hwdec_d3d11va.c`：`ra_is_d3d11` 检查
- `video/out/opengl/hwdec_d3d11egl.c`、`video/out/opengl/hwdec_dxva2gldx.c`：GL 下的硬解 interop
- `video/decode/vd_lavc.c`：`hwdec_autoprobe_info`
- `DOCS/man/vo.rst`：`libmpv` VO 的定位
- `DOCS/man/options.rst`：`--wid` 的平台支持

### 8.3 验证清单

- 宿主：`--vo=help` 是否含 `libmpv`；是否导出 render API 符号
- 起播：render context 创建、首帧、seek、暂停/恢复
- 硬解：实际 hwdec 名称、是否零拷贝、4K/60 CPU/GPU
- D3D11：device 非 `SINGLETHREADED`、纹理格式/绑定、immediate context 串行、resize/keyed mutex、HDR 元数据
- 画面：方向（FLIP_Y）、DPI、panscan、旋转、keepaspect
- 交互：全屏进出、小窗切换、叠层 z-order、菜单栏、光标、键盘、IME
- 功能：截图、OSD、字幕、滤镜、播放列表、任务栏缩略图
- 稳定性：上下文重建、反复 resize、退出/重载、异常路径回退
- 画质：HDR/SDR 各一组与当前 `gpu-next` 的对比

### 8.4 术语

- `wid`：mpv 的现有窗口嵌入方式；Windows 上由 mpv 创建子 HWND 并自建交换链。
- render context：libmpv render API 的渲染状态，绑定一个 mpv core 的 client API。
- advanced control：启用 Direct Rendering 与 GPU 截图，同时要求渲染线程绝不阻塞 core。
- `gpu` / `gpu-next`：mpv 的两代 GPU 渲染器；后者基于 libplacebo，当前默认。
- D3D11 render API：libmpv 0.42 新增的 render API 后端；调用方传入 `ID3D11Device` 与每帧 `ID3D11Texture2D`，自行呈现。

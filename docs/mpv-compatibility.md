# mpv compatibility

Oxbow runs as a C plugin inside mpv, and the video area is embedded through mpv's `wid`. mpv stays the playback engine and the process owner; the plugin supplies the window and the interface. This page lists what of mpv works through that arrangement, what does not, and how safe mode changes whether the plugin loads at all.

**Requirements**: mpv 0.39 or later; Windows 10 (1809+) 64-bit; Linux with an X11 session or a Wayland session with XWayland. Pure Wayland and macOS/Android are not supported — `wid` only exists on Windows, X11 and Android.

## Supported mpv features

- **Playback**: everything mpv does — hardware decoding, filters, subtitle and audio tracks, caching, playlists, seeking — is left to mpv and works as usual.
- **Configuration**: `mpv.conf`, profiles and command-line options apply normally, except for the window options and the plugin-set values listed below.
- **Keyboard bindings**: shortcuts in Oxbow's own table are handled by the plugin; every other key is handed back to mpv as a `keypress`, so `input.conf` and mpv's default key map keep working. While mpv's built-in console is open, the plugin yields the keyboard entirely and forwards input, including IME text.
- **Mouse bindings**: with **Forward mouse events to mpv** enabled (off by default), mouse bindings in `input.conf` take effect in the video area.
- **Scripts**: other scripts keep running normally. With **Show mpv menu-data in the Tools menu** enabled (off by default), a script's `menu-data` menu is rendered into the Tools menu.
- **Observed properties**: `fullscreen`, `ontop`, `pause`, `time-pos`, `duration`, `volume`, `volume-max`, `mute`, `playlist-pos`, `playlist`, `track-list`, `sub-delay`, `video-zoom` / `video-pan-x` / `video-pan-y` and cache state drive the interface; values written by scripts or IPC arrive through the same path. `fullscreen`, `ontop`, `volume` and `mute` are also written back when changed in the UI.
- **External control**: scripts and IPC clients can read and write `user-data/oxbow/mini-window` to query or toggle the mini-window state. Any mpv property that controls playback or one of the observed properties above works through the same channels.
- **OSD**: the plugin's own messages go through mpv's `osd-overlay` on dedicated channel ids, so they do not replace overlays owned by scripts.
- **Subtitles**: mpv's `reset-on-next-file` is extended with `sub-delay`, so a subtitle offset does not leak into the next file; the generated-subtitle cache directory is appended to `sub-file-paths`.

## Unsupported mpv properties

The window belongs to the plugin, not to mpv, so mpv's window handling cannot take effect.

| Area | Properties | What happens |
| --- | --- | --- |
| Window state and geometry | `border`, `title-bar`, `window-maximized`, `window-minimized`, `window-scale`, `geometry`, `autofit`, `autofit-larger`, `autofit-smaller`, `screen`, `screen-name`, `fs-screen`, `fs-screen-name`, `keepaspect-window`, `snap-window`, `force-window-position`, `auto-window-resize`, `hidpi-window-scale`, `title`, `show-in-taskbar`, `window-dragging`, `focus-on`, `on-all-workspaces` | Writes succeed and read back, but the Qt window does not change. `title` is set by the plugin (it follows the playlist); mpv-side title changes do not touch it. |
| Video output backends | `vo`, `gpu-context` | Only backends that can render into a `wid` window work; anything else cannot embed (see the whitelist under [Safe mode](#safe-mode)). |
| Encoder output | `o` | Encoding to a file conflicts with the GUI host and is not supported. |
| External window ownership | `wid` | If `wid` is already positive when the plugin loads, the plugin steps aside and leaves the output to that owner. |
| Run-mode defaults | `idle`, `keep-open`, `force-window` | The plugin writes `yes` / `no` / `yes` at startup: it stays alive when the playlist ends, advances on end of file, and keeps a video output for the idle canvas. |
| External automation | scripts and IPC that drive the window | Playback control works; window position, size and state expectations do not, because those properties have no effect. |

`fullscreen` and `ontop` are the exceptions among window options: both are observed bidirectionally, so writing them from a script or IPC really does enter fullscreen or change the window's topmost state. Safe mode still treats a command-line `--fs` or `--ontop` as an external-automation signal, because the launch is asking for window behaviour to be driven from outside.

## Safe mode

Safe mode is for the unsupported side above: instead of loading and presenting window values that do not reflect the real window, the plugin can step aside at startup and leave the whole instance to mpv.

- **Default**: off. Enable it with the **Safe Mode** checkbox at the end of **Settings → Basic**, or set `safeMode=true` in `oxbow.ini`.
- **Environment**: `OXBOW_SAFE_MODE=1` / `true` / `yes` / `on` enables it, `0` / `false` / `no` / `off` disables it for one launch (overriding the ini file), and `2` always keeps the plugin disabled. Empty or unrecognized values fall back to the ini setting.
- **When it yields**: the plugin returns without creating a window, writing options, registering protocols or starting any thread. mpv then runs natively. The only trace is one line in the plugin log — no dialog, no OSD, no terminal output.
- **Recovery**: start once with `OXBOW_SAFE_MODE=0` to load the plugin and clear the checkbox, or edit `oxbow.ini`.

With safe mode on, the plugin steps aside if any of these is true:

1. The run is not a pseudo-GUI launch (`player-operation-mode` is not `pseudo-gui`), which covers a plain console launch.
2. A control option from the list below was given **on the command line** (judged by `option-info/<name>/set-from-commandline`; `mpv.conf` and profiles do not count).
3. `input-ipc-server` or `input-ipc-client` has a non-empty value, whatever its source.

**Options that make it yield**:

| Class | Options |
| --- | --- |
| Window state and geometry | `border`, `title-bar`, `fullscreen`, `ontop`, `window-maximized`, `window-minimized`, `window-scale`, `geometry`, `autofit`, `autofit-larger`, `autofit-smaller`, `screen`, `screen-name`, `fs-screen`, `fs-screen-name`, `keepaspect-window`, `snap-window`, `force-window-position`, `auto-window-resize`, `hidpi-window-scale`, `title`, `show-in-taskbar`, `window-dragging`, `focus-on`, `on-all-workspaces` |
| Control entrances and run modes | `input-ipc-server`, `input-ipc-client`, `scripts`, `idle`, `keep-open`, `force-window`, `o`, `vo` (whitelisted values only), `gpu-context` (whitelisted values only), `input-vo-keyboard` |

`fs` is a command-line alias of `fullscreen` and is not listed separately; `wid` is not listed because an externally assigned `wid` already makes the plugin step aside.

**`vo` / `gpu-context` whitelist** — these two values are accepted only for backends that can render into `wid`. A comma-separated list must be entirely inside the whitelist; mixed lists, trailing commas (meaning "fall back outside the list"), decorations such as `!`, `@` or `=`, and unknown values still make the plugin yield.

| Option | Accepted values |
| --- | --- |
| `vo` | `gpu`, `gpu-next`, `direct3d` (Windows), `x11`, `xv`, `vdpau`, `vaapi` (X11) |
| `gpu-context` | `auto` (only as a single value), `win`, `winvk`, `angle`, `dxinterop`, `d3d11` (Windows), `x11`, `x11vk`, `x11egl` (X11) |

Presentation-only options such as `cursor-autohide`, `taskbar-progress`, `osc`, `osd-level`, `osd-duration`, `screenshot-format` and `drag-and-drop` do not trigger safe mode: they only change how things look or behave locally, so the plugin never avoids them.

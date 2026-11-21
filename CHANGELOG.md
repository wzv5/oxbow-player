# Changelog

## [0.1.5] - 2026-11-21

### Added

- **Script interface**: scripts can put an action on the control bar, with command, callback, icon, tooltip and checked / disabled / hidden state kept up to date at runtime (`script-action`, bound from a tool via `@<id>`), and can ask the plugin to pop up a menu at the cursor with submenus, separators and per-entry commands or callbacks (`context-menu`). See [script interface](docs/script-interface.md).
- **Custom tools on the control bar**, with side and icon settings in the tool editor.
- **Icon browser** for custom tool icons, with ligature-name completion and validation.

### Changed

- Built-in icons now use Material Symbols Rounded ligatures; the bundled SVG assets and the Qt SVG dependency are gone.
- Time formatting is unified across the interface, and the control bar clock keeps a fixed width instead of squeezing the buttons.
- Playlist and provider settings pages are left-aligned across both columns; rclone crypt options follow the filename-encryption mode.
- The custom tool editor drops its unused note field.

### Fixed

- Safe mode no longer skips loading on mpv versions before 0.42.0.
- Switching to another item in the same playlist no longer rebuilds the queue: no flicker, and playback starts sooner.

## [0.1.4] - 2026-11-16

### Added

- **Safe mode**: a new option under **Settings → Basic**. When enabled, the plugin stays out of the way if mpv is started in a shape the plugin cannot take over. The `OXBOW_SAFE_MODE` environment variable overrides the setting at startup; a value of `2` always keeps the plugin disabled. See [mpv compatibility](docs/mpv-compatibility.md#safe-mode) for the trigger conditions and the full avoid list.
- **Always on top** in the View menu, mirrored with mpv's `ontop` property; entering fullscreen or mini mode keeps its own stacking and the change is echoed in the OSD.
- **Mini-window state as a public property**: the plugin publishes `user-data/oxbow/mini-window`, so scripts and IPC clients can read the mini state and toggle it; the published value always follows the real window.

### Fixed

- The video area no longer jumps several times when entering or leaving fullscreen or mini mode, or when showing or hiding the playlist.
- `Ctrl+-` and `Alt+-` are no longer rejected as invalid key names, and no longer break conflict detection in the shortcut dialog.

## [0.1.3] - 2026-11-13

### Added

- **Forward mouse events to mpv**: a new option under **Settings → Basic**; when enabled, mouse bindings in `input.conf` take effect in the video area.

### Changed

- **Custom-drawn frameless window**: appearance is now consistent across platforms, and a menu button appears to the left of the minimize button when the menu bar is hidden.
- **mpv key bindings**: bindings can now be assigned to mouse buttons, and the key field supports autocompletion.

### Fixed

- The window no longer flickers when entering or leaving fullscreen.
- Left-clicking the video area to dismiss the plugin's own context menu no longer toggles playback.

## [0.1.2] - 2026-11-08

### Added

- **Buffering status overlay** — during initial buffering and after seeks, the video area now shows buffering progress and read speed in the lower-left corner; the overlay clears when playback resumes.
- **Show mpv menu-data in the Tools menu** — a new option under **Settings → Basic** that controls whether mpv's `menu-data` section is shown in the Tools menu.

### Changed

- The mpv `menu-data` section is now hidden by default, so mpv 0.42's built-in menu no longer fills the Tools menu. Enable the new option to show mpv's built-in menu or menus supplied by scripts; the change applies the next time the menu opens.

## [0.1.1] - 2026-11-06

### Added

- **Check for updates** — compares the running version with the latest GitHub release. The automatic check runs at startup according to the configured interval and only prompts when a newer version exists; the manual check always opens the result dialog.
- **Project homepage link** in the About dialog.

### Changed

- Left-clicking a slider's track now jumps to that position, regardless of the active interface style.
- The default remote request timeout was raised from 5 to 10 seconds to reduce GitHub API timeouts on slow networks.

### Fixed

- Subtitle browser table headers are now translated.

## [0.1.0] - 2026-11-03

- Initial release.

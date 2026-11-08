# Changelog

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

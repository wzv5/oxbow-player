# Oxbow

**English** · [简体中文](README.zh-CN.md)

> A desktop video player powered by mpv — minimal interface, focused on playback itself.

## Introduction

Oxbow is a desktop video player for Windows: the playback engine is [mpv](https://mpv.io/), the interface is built with Qt. It installs as an mpv plugin — drop it into mpv's scripts directory, and from then on, starting mpv greets you with a full-fledged player interface.

Oxbow's philosophy is simple: **when watching a video, any extra interface is a distraction**. It spends its effort where it matters, adding the conveniences of everyday use: playlist management, bookmarks, mini window, and navigator.

![Oxbow main window: settings dialog and the multi-tab playlist](docs/screenshot-en.png)

## Features

- **Multi-tab playlists**: each tab picks its own list source:
  - Static (hand-edited lists);
  - Local directory (bound to a folder, refreshed on demand to show the latest contents);
  - WebDAV (browse directories on a server);
  - External (m3u / txt live sources);
  - Jellyfin (browse media libraries);
- **Resume memory**: each tab remembers the last played item and playback position, so you pick up where you left off next time;
- **Bookmarks & chapters**: bookmarks are one click away and, together with chapters, marked right on the seek bar — visible at a glance, click to jump;
- **Mini window mode**: an always-on-top mini window with aspect-locked resizing and automatic edge snapping;
- **Navigator**: a Photoshop-style whole-frame thumbnail — the viewport rectangle shows what is currently visible; click to jump, drag to pan, scroll to zoom;
- **Filter editor**: add or remove audio / video filters one by one, taking effect immediately;
- **Screenshots**: copy to clipboard or save to file with one action.

## Installation

### Prerequisites

- Windows 10 (1809+) or later (64-bit) — this floor comes from Qt 6
- [mpv](https://mpv.io/) — any recent version

### Install the plugin

Download the latest prebuilt package: <https://github.com/wzv5/oxbow-player/releases/latest>; or [build from source](#building-from-source).

The plugin is a folder placed under mpv's script directory:

```text
<mpv>/portable_config/scripts/oxbow/     # portable mpv
%APPDATA%/mpv/scripts/oxbow/             # installed mpv
├── main.dll        # plugin entry
├── oxbowcore.dll   # the plugin itself
└── MediaInfo.dll   # optional, enables media metadata enrichment
```

Start mpv — the Oxbow window opens automatically.

## Uninstall

**Oxbow is portable**: delete the plugin folder (`scripts/oxbow/`) and it is gone — nothing written to the registry, nothing touched system-wide. All settings and bookmarks live in the single `oxbow.ini` inside that folder and are removed along with it; mpv itself is unaffected.

## Building from source

### Requirements

- Any toolchain with C++23 support should work in theory; only the Windows target has been verified so far, other combinations are untested
- CMake
- Ninja
- Qt 6.x
- No mpv dev package needed — the repo bundles `mpv/client.h`

### Configure and build

```bash
cd public
cmake -B build -G "Ninja Multi-Config" -DQt6_ROOT="D:/Qt/6.11.2/msvc2022_64" -DMPV_BIN_DIR="D:/path/to/mpv"
cmake --build build --config Release
```

- `Qt6_ROOT` points at your Qt installation.
- `MPV_BIN_DIR` (optional) points at the directory containing `mpv.exe`. When set, every build automatically deploys the plugin into `portable_config/scripts/oxbow/` next to mpv — ready to run right after building.
- Without `MPV_BIN_DIR`, the artifacts land in `build/Release/` and must be copied manually (see [Installation](#installation)).

### Build options

| Option | Default | Description |
| --- | --- | --- |
| `OXBOW_STATICQT` | `OFF` | Link Qt statically. Smaller footprint, faster startup; recommended — requires a static Qt build. |
| `OXBOW_BUILD_TESTS` | `OFF` | Build the unit-test executables; run with `ctest --test-dir build -C <config>`. |
| `OXBOW_PUREQT` | `OFF` | Force some features (e.g. the mini window) onto the pure-Qt fallback path (development aid). |
| `MPV_BIN_DIR` | *(empty)* | Directory containing `mpv.exe`; enables automatic post-build deployment. |

## Why the name

This project was started purely to test Ox Alpha's capabilities — the test went so well that it simply grew into a player. The name keeps the ox: an *oxbow* is the U-shaped bow of an ox yoke, the part that fits around the ox's neck and connects its pulling power to the plow — mpv is the ox pulling the cart, and Oxbow is that yoke.

## Acknowledgements

- [mpv](https://mpv.io/) — the playback engine
- [Qt](https://www.qt.io/) — the application framework
- [MediaInfo](https://mediaarea.net/en/MediaInfo) — optional media metadata
- [Icons8](https://icons8.com/) — the default icon pack

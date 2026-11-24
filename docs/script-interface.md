# Script interface

Scripts talk to Oxbow over mpv's `script-message` channel. Target client: `oxbow`. Send with `mp.commandv("script-message-to", "oxbow", ...)`. mpv scripting is assumed; this page lists only what the plugin adds.

## Table of Contents

- [Script interface](#script-interface)
  - [Table of Contents](#table-of-contents)
  - [Handshake](#handshake)
  - [Conventions](#conventions)
  - [script-action: control bar actions](#script-action-control-bar-actions)
  - [context-menu: script menus](#context-menu-script-menus)
  - [script-playlist: lists driven by a script](#script-playlist-lists-driven-by-a-script)
    - [Layer payload](#layer-payload)
    - [Messages the plugin shows](#messages-the-plugin-shows)
  - [Example scripts](#example-scripts)

## Handshake

```lua
local initialized = false

local function init(version)
    if initialized then
        return
    end
    initialized = true
    -- The plugin can take messages now. Send the first one here.
end

mp.register_script_message("oxbow-version", init)          -- register first
mp.commandv("script-message-to", "oxbow", "get-version", mp.get_script_name())
```

| Direction | Message | Args |
| --- | --- | --- |
| plugin → all scripts | `oxbow-version` | version |
| script → plugin | `get-version` | client name (your `mp.get_script_name()`) |
| plugin → that client | `oxbow-version` | version |

The broadcast covers scripts loaded before the plugin, the reply covers those loaded after. A script that loads before the plugin gets both — the broadcast when the plugin comes up, and the answer to its `get-version` — so `init` runs twice, and the guard above is what keeps the body to one run. Do not drop it when copying this snippet: without it the `create` is sent twice and everything else the body sets up (observers, fetches, timers) runs twice as well. `get-version` without a client name is answered with nothing. The version is `0.1.0` form, not negotiated. `user-data/oxbow/version` mirrors it for `mp.observe_property`, but the message is the authoritative copy.

## Conventions

- Inbound name is the protocol alone (`script-action`, `context-menu`, `script-playlist`); what follows is the protocol's own business — the protocols below put a verb (`create`, `set`, `show`, `layer`) right after it.
- Messages the plugin sends are named `oxbow-<protocol>:<event>` (`oxbow-script-action:created`, `oxbow-script-playlist:need-layer`) and go to the client that asked. These are the plugin's replies and notifications, listed per protocol below. A `callback` field is a name of your own and comes back unchanged.
- The sender names itself in the `owner` argument; the plugin echoes it verbatim and authenticates nothing.
- Arguments are UTF-8 strings; the JSON payload is one argument, never split. Use `mp.commandv`, not an assembled command string.
- Unhandled message names are ignored silently; nothing is queued, so a message sent before the target exists is lost, and callbacks or replies to a gone client fail into the log.

## script-action: control bar actions

A custom tool whose command is `@<id>` binds to the action of that id: the script supplies the click command and callback, the button icon and tooltip, and the checked / disabled / hidden state. The tool keeps its name, shortcut and control bar position.

```lua
local utils = require "mp.utils"

mp.commandv("script-message-to", "oxbow", "script-action", "create",
            mp.get_script_name(), "my-action", utils.format_json({
                command = "screenshot",
                icon = "photo_camera",
                tooltip = "Take a screenshot",
            }))
```

The script sends:

| Message | Args |
| --- | --- |
| `script-action` `create` | `owner id json` |
| `script-action` `set` | `owner id json` |

`create` registers; `set` updates and is accepted only from the creating client. Ids are `[A-Za-z0-9_.-]{1,64}`.

| Field | Type | Absent | Meaning |
| --- | --- | --- | --- |
| `command` | string | empty | mpv command string, run on click |
| `callback` | string | empty | message sent to `owner`, with the action id. May be combined with `command`, which runs first |
| `icon` | string | empty | Material Symbols name for the button; overrides the tool's own icon |
| `tooltip` | string | empty | button tooltip; empty falls back to the tool name's last segment |
| `checked` | boolean | absent | present at all makes it checkable; `true` checks it / shows the button pressed |
| `disabled` | boolean | `false` | greyed out, does not respond |
| `hidden` | boolean | `false` | not rendered; its shortcut does nothing |

`icon` and `tooltip` affect the control bar button only; menu items keep the tool's name and no icon.

`set` is a partial update: given fields merge, absent fields keep their value, so `{"disabled":false}` clears disabling, `{"tooltip":""}` clears a tooltip, and `{}` changes nothing. Unknown fields are ignored and a wrongly-typed field counts as absent; one bad field never drops the action.

An `@id` with no live action renders nothing — no menu item, no button, shortcut does nothing — silently. Actions are memory-only, so after a restart every bound tool is in that state until its script creates the action again. To hide an action, `set` `hidden: true`; there is no delete message.

To the client that sent `create` / `set`, the plugin sends:

| Message | Args | When |
| --- | --- | --- |
| `oxbow-script-action:created` | `id` | `create` succeeded |
| `oxbow-script-action:rejected` | `id reason` | `create` / `set` dropped |

| Reason | Cause |
| --- | --- |
| `bad-args` | fewer than three arguments after `create` / `set` |
| `bad-id` | empty, over 64 characters, or outside `[A-Za-z0-9_.-]` |
| `bad-json` | payload is not a JSON object |
| `exists` | `create` for a live id |
| `not-found` | `set` for an unknown id |
| `owner-mismatch` | `set` from another client |

`set` has no success receipt; the interface redraws on the next event loop. Both receipts are optional — `exists` is the normal answer after reloading a script, and `set` still updates the surviving action.

A click runs `command` through mpv, then sends `callback` to `owner` with the action id. A failing command gets the usual tool OSD. If the script exits, the action stays at its last state: callbacks fail into the log, `command` still runs.

## context-menu: script menus

`show` pops up a menu at the cursor. The plugin owns the popup and the closing notification; the script owns the entries and what a click does.

```lua
mp.commandv("script-message-to", "oxbow", "context-menu", "show",
            mp.get_script_name(), "my-menu", utils.format_json({ items = items }))
```

The script sends:

| Message | Args |
| --- | --- |
| `context-menu` `show` | `owner menu-id json` |

The payload is `{"items": [...]}`, nested through submenus; other fields are ignored. Menu ids are `[A-Za-z0-9_.-]{1,64}`.

```json
{
  "items": [
    { "title": "Repeat", "checked": true, "callback": "menu-picked", "id": "repeat" },
    { "title": "Speed", "items": [
      { "title": "1.0x", "callback": "menu-picked", "id": "speed-1.0" },
      { "title": "1.5x", "checked": false, "callback": "menu-picked", "id": "speed-1.5" },
      { "title": "-" },
      { "title": "Custom…", "disabled": true }
    ]},
    { "title": "-" },
    { "title": "Take Screenshot", "command": "screenshot" }
  ]
}
```

| Kind | Test | Fields |
| --- | --- | --- |
| separator | `title` is exactly `-` | only `title` |
| submenu | title not `-`, has `items` | `title`, `items`; action fields ignored |
| item | otherwise | `title` required, plus below |

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `command` | string | empty | mpv command string, run on click |
| `callback` | string | empty | message sent to `owner` on click |
| `id` | string | generated | item identity reported with the callback |
| `checked` | boolean | absent | present at all makes it checkable; `true` checks it |
| `disabled` | boolean | `false` | greyed out, not pickable |

- Titles are verbatim: leading spaces render (the only way to indent), `&` may act as a mnemonic, `-` is reserved.
- A malformed entry is dropped alone (empty or non-string `title`, non-array `items`, wrongly-typed field); the rest still shows. Leading, trailing and repeated separators are trimmed per level, and empty submenus are removed.
- Item ids: an explicit `id` is kept as given; otherwise the position path (`0`, `1`, `1/0`), where separators take no number and a submenu takes one. No uniqueness check.

The plugin sends back to `owner`:

| Message | Args | When |
| --- | --- | --- |
| `oxbow-context-menu:rejected` | `menu-id reason` | `show` opened nothing |
| `oxbow-context-menu:closed` | `menu-id dismissed` | every close, item clicks included |

Reasons: `bad-args` (missing arguments), `bad-id`, `bad-json` (not an object, or `items` missing or not an array), `empty` (nothing left after trimming), `busy`.

`dismissed` is `"true"` when nothing was picked, `"false"` when an entry was; item commands and callbacks arrive first, so `closed` is the place to drop bookkeeping. Not sent on plugin exit.

- One menu at a time, opened at the cursor. A valid `show` closes the previous menu first — its `closed` goes out before the new menu appears — and this works across owners.
- `show` is rejected while the main window is inactive, a modal dialog is open, another popup is open, or the plugin is shutting down. The script menu itself does not count as another popup. Refusals leave the current menu alone.
- There is no message to close a menu: it closes on a pick, Escape, a click elsewhere, a replacing `show`, or exit.

Click: `command` runs through mpv, then `callback` is sent to `owner` as `menu-id item-id`. A failing command is only logged. An entry with neither is a label and just closes the menu; disabled entries do not respond and do not close it. `checked` is a snapshot — persist it yourself and pass it again next time.

## script-playlist: lists driven by a script

A `Script` list is bound to a data source id and asks the script that registered it for content, layer by layer. Everything else is an ordinary list: persists, resumes, bookmarks, tab order; the user renames, moves, duplicates, sorts and deletes it. Scripts never create, delete or rename lists.

```lua
mp.commandv("script-message-to", "oxbow", "script-playlist", "create",
            mp.get_script_name(), "my-source")
```

| Direction | Message | Args |
| --- | --- | --- |
| script → plugin | `script-playlist` `create` | `owner id` |
| script → plugin | `script-playlist` `layer` | `owner reqid json` |
| plugin → script | `oxbow-script-playlist:created` | `id` |
| plugin → script | `oxbow-script-playlist:rejected` | `id reason` |
| plugin → script | `oxbow-script-playlist:need-layer` | `id reqid parent-path` |
| plugin → script | `oxbow-script-playlist:cancel-layer` | `id reqid` |

- Ids `[A-Za-z0-9_.-]{1,64}`. Same owner re-`create` is a no-op and still answers `created`.
- Reasons: `bad-args` (no id), `bad-id`, `exists` (id held by another owner). No owner: logged only, nothing to answer.
- `layer` echoes `owner` and `reqid`; the JSON is one argument. Unknown or expired `reqid`, or another owner: dropped and logged.
- `parent-path` is container ids joined with `/`; the root layer is empty.
- Requests happen on first activation, on switching to an unloaded tab, and on manual refresh. `created` triggers nothing; there is no invalidate message — after updating its data a script waits for the next `need-layer`.
- First load: up to 3 s for `create`, once per list; each layer gets 10 s. Timeout → `cancel-layer` plus a placeholder; failures are not cached.

### Layer payload

```json
{
  "containers": [
    { "id": "s1", "title": "Season 1", "path": "Show/Season 1" }
  ],
  "items": [
    { "url": "C:\\media\\a.mkv",
      "displayName": "Episode 1",
      "filename": "a.mkv",
      "options": { "start": "42", "sub-file": "C:\\media\\a.ass" },
      "file": { "size": 1234567890, "created": 1763664000, "modified": 1763700000 },
      "metadata": { "format": "MPEG-4", "duration": 1260, "recordedDate": 1763664000,
                    "title": "Media Title", "album": "Album", "performer": "Uploader" } }
  ]
}
```

| Layer field | Type | Absent | Meaning |
| --- | --- | --- | --- |
| `containers` | array | empty | rows shown above the items |
| `items` | array | empty | playable entries |
| `error` | string | normal layer | **fails the whole layer**: non-empty after trim is shown verbatim in place of the list and `containers` / `items` are dropped. Empty or `null` keeps the layer normal |

| Container field | Type | Absent | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | layer id, `[A-Za-z0-9_.-]{1,64}`. Addressing only: it is the segment sent back in `parent-path` when the user opens the container, and is never shown. Duplicates on one layer keep the first |
| `title` | string | `id` | the level's display name: the row text, the Name row of its properties dialog, and one breadcrumb segment — the breadcrumb is built from titles only, never from `path`. Unrelated to `metadata.title` |
| `path` | string | empty | display address only — a directory path or a URL — and is never joined with `title`. Non-empty: the Path row shows it verbatim and Copy Path is enabled; empty: the Path row falls back to a breadcrumb built from `title` and Copy Path is disabled. Reveal in File Explorer needs it non-empty, non-network and existing on disk. Not used for addressing, sorting or persistence |

| Item field | Type | Absent | Meaning |
| --- | --- | --- | --- |
| `url` | string | required | absolute path or a URL with `://`; anything else drops the item |
| `displayName` | string | url's file name | row text and the Display Name row |
| `filename` | string | url's last segment | File Name row and the file-name sort keys |
| `options` | object | none | mpv option name → scalar value, applied when the item plays |
| `file` | object | none | `size` (bytes), `created` / `modified` (unix seconds) |
| `metadata` | object | none | `format`, `duration` (seconds), `recordedDate` (unix seconds), `title`, `album`, `performer` |

- Names: `displayName` = row text and Display Name row; `filename` = File Name row and the file-name sort keys; `metadata.title` = Tags title. Independent.
- `file`: positive only — `size` whole bytes, `created` / `modified` whole seconds (fractions truncated). Wrong type, non-positive or out of range = absent.
- `metadata`: exact keys, no `date` / `author` aliases; empty strings, non-positive numbers and wrong types = absent; `recordedDate` lands as local time. Reported values win over a file scan.
- `options`: scalars only — strings (empty dropped), numbers and booleans as text; arrays, objects and `null` ignored.
- Non-object entries dropped (items count as skipped); items without a usable `url` skipped; the warning appears on the root layer only.
- Invalid layer: payload not a JSON object; `containers` / `items` present but not an array; `error` present with any other type.

### Messages the plugin shows

| Situation | Text |
| --- | --- |
| id unbound, or no script registered it | `No script is providing this list.` |
| the owning client is gone | `The script providing this list is no longer running.` |
| no answer in time | `The script list did not respond in time.` |
| invalid layer | `The script list returned invalid data.` |
| skipped items, root layer only | `Some items were skipped because they do not have a valid absolute path or URL.` |
| script's own `error` | shown verbatim |

- Registration lasts one process; after a restart the list waits for `create` again.
- Ids are never released: a reload under another client name gets `exists`, and the bound lists keep failing until the plugin restarts. Same name plus `create` is harmless.
- Many lists may bind one id; each keeps its own tab, view state, sort rule and position.
- No item editing, import, clear or dedupe; sorting works.

## Example scripts

- [track-picker.lua](../example/scripts/track-picker.lua) — `script-action`, `context-menu`. The action is bound by a tool's `@track-picker`; `set` sends only `hidden`, driven by `mp.observe_property("track-list")`. Its callback opens a video / audio / subtitle menu: disabled headers, `checked` on the active track, `set vid` / `set aid` / `set sid` commands. Setup: a tool with command `@track-picker`, shown on the control bar.
- [bilibili-popular.lua](../example/scripts/bilibili-popular.lua) — `script-playlist`. The root layer is a static table of four containers; three answer with playable items, while `Weekly` answers with the issue index and an issue answers with that week's entries, so `parent-path` grows to `weekly/400`. One fetch runs at a time: a request arriving mid-fetch answers with `error`, and so does a failed fetch. `cancel-layer` aborts the fetch it started; `ytdl` off answers `error` on item layers. Title goes to `displayName`, duration / publish date / uploader to `metadata`. Bilibili takes a `buvid` cookie once and one `curl`; both sit in their own block. Setup: a Script list bound to `bilibili-popular` with `yt-dlp` in `PATH`.

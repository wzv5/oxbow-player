# Script interface

Scripts talk to Oxbow over mpv's `script-message` channel. Target client: `oxbow`. Send with `mp.commandv("script-message-to", "oxbow", ...)`. mpv scripting is assumed; this page lists only what the plugin adds.

## Handshake

```lua
local function init(version)
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

The broadcast covers scripts loaded before the plugin, the reply covers those loaded after; both can arrive, so `init` may run twice and needs a guard. `get-version` without a client name is answered with nothing. The version is `0.1.0` form, not negotiated. `user-data/oxbow/version` mirrors it for `mp.observe_property`, but the message is the authoritative copy.

## Conventions

- Inbound name is the protocol alone (`script-action`, `context-menu`); what follows is the protocol's own business — the two below put a verb (`create`, `set`, `show`) right after it.
- Messages the plugin sends are named `oxbow-<protocol>:<event>` (`oxbow-script-action:created`, `oxbow-context-menu:closed`) and go to the client that asked. These are the plugin's replies and notifications, listed per protocol below. A `callback` field is a name of your own and comes back unchanged.
- The sender names itself in the `owner` argument; the plugin echoes it verbatim and authenticates nothing.
- Arguments are UTF-8 strings; the JSON payload is one argument, never split. Use `mp.commandv`, not an assembled command string.
- Unhandled message names are ignored silently.

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

## Notes

- No authentication, rate limiting, splitting, buffering or replay: a message sent before the target exists is gone, which is what the handshake is for.
- No script liveness probing; callbacks to a gone client fail into the log.
- Nothing is persisted; actions and menus live for the process only. Action changes coalesce into one redraw per event loop.

See [`example/scripts/track-picker.lua`](../example/scripts/track-picker.lua) for both protocols in use, handshake included.

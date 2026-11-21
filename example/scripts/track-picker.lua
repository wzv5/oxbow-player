--[[
  track-picker.lua - oxbow-player example

  Two plugin interfaces are used here:

    * script-action - a control-bar action the script creates and keeps in
      sync with the track-list; hidden while there is nothing to switch to.
    * context-menu  - a menu the plugin pops up when the button is clicked.

  Setup: copy this file into mpv's scripts directory next to the oxbow plugin,
  then create a custom tool with command "@track-picker" (Tools -> Customize
  Tool List...) and turn on "Show as control bar button". "@track-picker" is
  what binds the tool to the action created below.

  Receipts (created / rejected / closed) are logged only: the script never
  waits for them. mpv keeps commands in order, so a "set" after "create" is
  enough.
]]

local utils = require "mp.utils"

local PLUGIN = "oxbow"
local SCRIPT = mp.get_script_name()
local ACTION_ID = "track-picker"
local MENU_ID = "track-picker"
local CLICK_MESSAGE = "track-picker-clicked"

local function log(message)
    mp.commandv("print-text", "[track-picker] " .. message)
end

-- Video / audio / subtitle tracks of the current file, by group.
local function collect_tracks()
    local tracks = { video = {}, audio = {}, subs = {} }
    for _, track in ipairs(mp.get_property_native("track-list") or {}) do
        if track.type == "video" and not track.image then
            tracks.video[#tracks.video + 1] = track
        elseif track.type == "audio" then
            tracks.audio[#tracks.audio + 1] = track
        elseif track.type == "sub" then
            tracks.subs[#tracks.subs + 1] = track
        end
    end
    return tracks
end

-- Hidden when there is nothing to switch to: a second video or audio track,
-- or any subtitle.
local function hidden_now()
    local tracks = collect_tracks()
    return not (#tracks.video > 1 or #tracks.audio > 1 or #tracks.subs > 0)
end

-- "set" is a partial update, so only "hidden" is sent; sending an unchanged
-- value again is harmless.
local function sync_hidden()
    mp.commandv("script-message-to", PLUGIN, "script-action", "set",
                SCRIPT, ACTION_ID, utils.format_json({ hidden = hidden_now() }))
end

-- Title, else language, else codec, else id.
local function track_label(track)
    if track.title and track.title ~= "" then
        if track.lang and track.lang ~= "" then
            return track.title .. " [" .. track.lang .. "]"
        end
        return track.title
    end
    return track.lang or track.codec or ("Track " .. tostring(track.id))
end

-- The plugin generates item ids from item positions, so no explicit id is
-- needed; disabled entries without a command are plain labels, used here as
-- section headers. The whole menu is a single JSON argument.
local function show_menu()
    local tracks = collect_tracks()
    local items = {}

    local function header(title)
        items[#items + 1] = { title = title, disabled = true }
    end

    local function entry(title, command, checked)
        items[#items + 1] = { title = title, command = command, checked = checked }
    end

    if #tracks.video > 1 then
        header("Video")
        for _, track in ipairs(tracks.video) do
            entry(track_label(track), "set vid " .. tostring(track.id), track.selected or false)
        end
    end
    if #tracks.audio > 1 then
        header("Audio")
        for _, track in ipairs(tracks.audio) do
            entry(track_label(track), "set aid " .. tostring(track.id), track.selected or false)
        end
    end
    if #tracks.subs > 0 then
        header("Subtitle")
        local any_selected = false
        for _, track in ipairs(tracks.subs) do
            any_selected = any_selected or track.selected == true
        end
        entry("None", "set sid no", not any_selected)
        for _, track in ipairs(tracks.subs) do
            entry(track_label(track), "set sid " .. tostring(track.id), track.selected or false)
        end
    end

    if #items == 0 then
        return -- an empty menu would be rejected
    end

    mp.commandv("script-message-to", PLUGIN, "context-menu", "show",
                SCRIPT, MENU_ID, utils.format_json({ items = items }))
end

-- The plugin sends the action's callback message with the action id.
mp.register_script_message(CLICK_MESSAGE, function(id)
    if id ~= ACTION_ID then
        return
    end
    show_menu()
end)

-- The plugin's answers. All of them are optional: registering them only makes
-- the protocol observable, and everything above works without them.

-- Optional. The action now exists; "set" sent after "create" works anyway.
mp.register_script_message("oxbow-script-action:created", function(id)
    log("action created: " .. tostring(id))
end)

-- Optional. Why a create / set was dropped: bad-args / bad-id / bad-json /
-- exists / not-found / owner-mismatch. "exists" just means the action outlived
-- a script reload.
mp.register_script_message("oxbow-script-action:rejected", function(id, reason)
    if id == ACTION_ID and reason == "exists" then
        return
    end
    log("action rejected: " .. tostring(id) .. " (" .. tostring(reason) .. ")")
end)

-- Optional. Why the menu was not shown: bad-args / bad-id / bad-json / empty /
-- busy.
mp.register_script_message("oxbow-context-menu:rejected", function(menu_id, reason)
    log("menu rejected: " .. tostring(menu_id) .. " (" .. tostring(reason) .. ")")
end)

-- Optional. Sent on every close, item clicks included; dismissed is "true" when
-- nothing was picked.
mp.register_script_message("oxbow-context-menu:closed", function(menu_id, dismissed)
    if menu_id == MENU_ID then
        log("menu closed (dismissed=" .. tostring(dismissed) .. ")")
    end
end)

-- Handshake: the plugin broadcasts "oxbow-version" once it is ready, and the
-- get-version request covers scripts loaded after it. init can run twice, hence
-- the guard.
local initialized = false

local function init()
    if initialized then
        return
    end
    initialized = true

    mp.commandv("script-message-to", PLUGIN, "script-action", "create",
                SCRIPT, ACTION_ID, utils.format_json({
                    callback = CLICK_MESSAGE,
                    icon = "airplay",
                    tooltip = "Tracks (script example)",
                    hidden = hidden_now(),
                }))

    -- After create: the observer fires once right away and a "set" sent
    -- earlier would be rejected.
    mp.observe_property("track-list", "native", sync_hidden)
end

mp.register_script_message("oxbow-version", init)
mp.commandv("script-message-to", PLUGIN, "get-version", SCRIPT)

--[[
  bilibili-popular.lua - oxbow-player example

  Demonstrates the script-playlist protocol: register a data source id, then
  answer need-layer requests. The root layer is two containers; each fetches
  its Bilibili ranking list when opened. Only one fetch runs at a time - a
  second request while one is running is answered with an error. cancel-layer
  is registered for completeness: curl's short timeout means the plugin
  normally never gives up first.

  Setup: copy this file into mpv's scripts directory next to the oxbow plugin,
  create a "Script" list in the plugin and bind it to "bilibili-popular".
  Entries go to mpv's builtin ytdl hook, so yt-dlp or youtube-dl must be in
  PATH.

  Entries are Bilibili pages that only resolve through the ytdl hook, so with
  ytdl off a container answers {"error": "..."} instead of fetching. Title goes
  to "displayName"; duration / publish date / uploader go into the item's
  "metadata" under "duration" / "recordedDate" / "performer".
]]

local utils = require "mp.utils"

local PLUGIN = "oxbow"
local SCRIPT = mp.get_script_name()
local LIST_ID = "bilibili-popular"
local USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36"
-- Stays inside the plugin's 10 s reply window, so a slow fetch still answers.
local CURL_MAX_TIME = "8"

local LAYERS = {
    { id = "popular", title = "综合热门", url = "https://api.bilibili.com/x/web-interface/popular?ps=20&pn=1" },
    { id = "precious", title = "排行榜", url = "https://api.bilibili.com/x/web-interface/popular/precious?page_size=100&page=1" },
}

local YTDL_OFF_ERROR = "yt-dlp is not enabled: set ytdl=yes so this list can resolve Bilibili pages."
local BUSY_ERROR = "Another layer is still loading; retry in a moment."

local function log(message)
    mp.commandv("print-text", "[bilibili-popular] " .. message)
end

-- An unknown ytdl read counts as on.
local function ytdl_enabled()
    return mp.get_property_native("options/ytdl") ~= false
end

-- One JSON payload per reply; the plugin pairs replies by owner + reqid.
local function answer(reqid, payload)
    mp.commandv("script-message-to", PLUGIN, "script-playlist", "layer", SCRIPT, reqid, payload)
end

local function root_layer()
    local containers = {}
    for _, layer in ipairs(LAYERS) do
        containers[#containers + 1] = { id = layer.id, title = layer.title }
    end
    return { containers = containers, items = {} }
end

-- Skip entries without a bvid; they cannot become a playable URL.
local function item_from_entry(entry)
    if type(entry) ~= "table" or type(entry.bvid) ~= "string" or entry.bvid == "" then
        return nil
    end
    local item = {
        url = "https://www.bilibili.com/video/" .. entry.bvid,
        displayName = (type(entry.title) == "string" and entry.title) or "",
    }
    local meta = {}
    if type(entry.duration) == "number" and entry.duration > 0 then
        meta.duration = entry.duration
    end
    if type(entry.pubdate) == "number" and entry.pubdate > 0 then
        meta.recordedDate = entry.pubdate
    end
    if type(entry.owner) == "table" and type(entry.owner.name) == "string" and entry.owner.name ~= "" then
        meta.performer = entry.owner.name
    end
    if next(meta) ~= nil then
        item.metadata = meta
    end
    return item
end

-- The running fetch, or nil. cancel-layer aborts it only for its starter.
local pending = nil

local function start_fetch(reqid, url)
    pending = { reqid = reqid }
    pending.handle = mp.command_native_async({
        name = "subprocess",
        playback_only = false, -- a data fetch outlives track switches
        capture_stdout = true,
        args = { "curl", "-sS", "--fail", "--max-time", CURL_MAX_TIME, "-A", USER_AGENT, url },
    }, function(success, result, err)
        if pending and pending.reqid == reqid then
            pending = nil
        end
        local root = nil
        if success and type(result) == "table" and result.status == 0 then
            root = utils.parse_json(result.stdout)
        end
        local items = {}
        if type(root) == "table" and type(root.data) == "table" and type(root.data.list) == "table" then
            for _, entry in ipairs(root.data.list) do
                local item = item_from_entry(entry)
                if item then
                    items[#items + 1] = item
                end
            end
            log("fetched " .. #items .. " entries")
        else
            log("fetch failed" .. (err and (": " .. err) or ""))
        end
        answer(reqid, utils.format_json({ items = items }))
    end)
end

mp.register_script_message("oxbow-script-playlist:need-layer", function(list_id, reqid, parent_path)
    if list_id ~= LIST_ID then
        return
    end
    if parent_path == "" then
        answer(reqid, utils.format_json(root_layer())) -- structure only, no I/O
        return
    end
    for _, layer in ipairs(LAYERS) do
        if layer.id == parent_path then
            if pending then
                answer(reqid, utils.format_json({ error = BUSY_ERROR }))
            elseif not ytdl_enabled() then
                answer(reqid, utils.format_json({ error = YTDL_OFF_ERROR }))
            else
                start_fetch(reqid, layer.url)
            end
            return
        end
    end
    answer(reqid, utils.format_json({ error = "Unknown layer: " .. parent_path }))
end)

-- The plugin gave up waiting (10 s timeout). Abort only the request that
-- started the running fetch; its completion still answers, but by then the
-- plugin has dropped that reqid, so the reply is ignored.
mp.register_script_message("oxbow-script-playlist:cancel-layer", function(list_id, reqid)
    if list_id ~= LIST_ID then
        return
    end
    if pending and pending.reqid == reqid then
        mp.abort_async_command(pending.handle)
        pending = nil
    end
end)

-- Receipts are optional observability.
mp.register_script_message("oxbow-script-playlist:created", function(list_id)
    if list_id == LIST_ID then
        log("data source registered")
    end
end)

mp.register_script_message("oxbow-script-playlist:rejected", function(list_id, reason)
    if list_id == LIST_ID then
        log("create rejected: " .. reason)
    end
end)

-- Handshake: the plugin broadcasts "oxbow-version" once ready and get-version
-- covers scripts loaded after it; init may run twice, hence the guard.
local initialized = false

local function init()
    if initialized then
        return
    end
    initialized = true
    mp.commandv("script-message-to", PLUGIN, "script-playlist", "create", SCRIPT, LIST_ID)
end

mp.register_script_message("oxbow-version", init)
mp.commandv("script-message-to", PLUGIN, "get-version", SCRIPT)

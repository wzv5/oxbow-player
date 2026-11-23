--[[
  bilibili-popular.lua - oxbow-player example

  Exercises the script-playlist protocol: the script registers a data source id
  and answers need-layer requests on demand. The root layer is pure structure -
  two containers, "综合热门" and "排行榜" - and each container fetches its own
  Bilibili ranking API when opened, so the list is two levels deep.

  Setup: copy this file into mpv's scripts directory next to the oxbow plugin,
  create a "Script" list in the plugin and bind it to "bilibili-popular". Entries
  are handed to mpv's builtin ytdl hook (ytdl=yes is the default), so yt-dlp or
  youtube-dl must be installed and in PATH.

  Flow: the plugin broadcasts "oxbow-version" once ready and the script also
  asks via get-version, so init runs once whichever comes first. init sends
  create; a same-owner re-create is answered with created again. need-layer
  arrives on first open or manual refresh only - the plugin never invalidates a
  layer, so each request fetches fresh data, and container layers are fetched
  only when the user opens them. cancel-layer arrives when the plugin gave up
  waiting (10 s timeout); it aborts the fetch only if that request started it.

  Entries are Bilibili pages, which only resolve through the ytdl hook, so a
  container layer asked for while ytdl is off answers {"error": "..."} instead
  of fetching; the panel shows that text as the layer's failure placeholder.

  Title goes to "displayName"; duration / publish date / uploader go into the
  item's "metadata" under "duration" / "recordedDate" / "performer" and are
  kept as the entry's media metadata, injected values winning over a
  MediaInfo scan.
]]

local utils = require "mp.utils"

local PLUGIN = "oxbow"
local SCRIPT = mp.get_script_name()
local LIST_ID = "bilibili-popular"
local USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " ..
    "(KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36"
-- Keep curl inside the plugin's 10 s reply window, so a slow fetch still
-- answers (with an empty layer) before the timeout placeholder takes over.
local CURL_MAX_TIME = "8"

-- Root containers in display order; the ids are what the plugin sends back as
-- parent-path when a container is opened. Both endpoints answer with the same
-- shape (data.list of video entries, see item_from_entry), so one parser serves
-- both.
local LAYERS = {
    {
        id = "popular",
        title = "综合热门",
        url = "https://api.bilibili.com/x/web-interface/popular?ps=20&pn=1",
    },
    {
        id = "precious",
        title = "排行榜",
        url = "https://api.bilibili.com/x/web-interface/popular/precious?page_size=100&page=1",
    },
}

-- A container layer without ytdl would hand out entries mpv cannot resolve,
-- so it answers with an error instead of a list.
local YTDL_OFF_ERROR = "yt-dlp is not enabled: set ytdl=yes so this list can " ..
    "resolve Bilibili pages."

-- options/ytdl is the option's live value; plain "ytdl" is the fallback for
-- builds that do not expose the options/ namespace. Unknown reads count as on.
local function ytdl_enabled()
    local value = mp.get_property_native("options/ytdl")
    if value == nil then
        value = mp.get_property_native("ytdl")
    end
    return value ~= false
end

local function log(message)
    mp.commandv("print-text", "[bilibili-popular] " .. message)
end

-- One API entry into one protocol item; entries without a bvid are skipped.
-- Title becomes the display name; duration / publish date / uploader ride in
-- "metadata" under their in-register keys.
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
    if type(entry.owner) == "table" and type(entry.owner.name) == "string"
            and entry.owner.name ~= "" then
        meta.performer = entry.owner.name
    end
    if next(meta) ~= nil then
        item.metadata = meta
    end
    return item
end

-- One JSON payload per reply, the whole layer in a single argument. Owner
-- comes first after the verb; the plugin pairs replies by owner + reqid.
local function answer(reqid, payload)
    mp.commandv("script-message-to", PLUGIN, "script-playlist", "layer",
                SCRIPT, reqid, payload)
end

local function root_layer()
    local containers = {}
    for _, layer in ipairs(LAYERS) do
        containers[#containers + 1] = { id = layer.id, title = layer.title }
    end
    return { containers = containers, items = {} }
end

-- Per-container fetch state: one curl per container, requests queue behind it
local fetches = {}
for _, layer in ipairs(LAYERS) do
    fetches[layer.id] = { layer = layer, handle = nil, starter = nil, waiters = {} }
end

local function finish_fetch(key, success, result, err)
    local state = fetches[key]
    state.handle = nil
    state.starter = nil
    local items = nil
    local body = nil
    if success and type(result) == "table" and result.status == 0 then
        body = result.stdout
    end
    local root = body and utils.parse_json(body) or nil
    if type(root) == "table" and type(root.data) == "table"
            and type(root.data.list) == "table" then
        items = {}
        for _, entry in ipairs(root.data.list) do
            local item = item_from_entry(entry)
            if item then
                items[#items + 1] = item
            end
        end
        log(key .. ": fetched " .. #items .. " entries")
    else
        -- An empty layer beats hanging the panel until the plugin's timeout;
        -- the next need-layer (manual refresh included) retries the fetch.
        log(key .. ": fetch failed" .. (err and (": " .. tostring(err)) or ""))
    end
    local payload = utils.format_json({ items = items or {} })
    for _, reqid in ipairs(state.waiters) do
        answer(reqid, payload)
    end
    state.waiters = {}
end

local function start_fetch(key)
    local state = fetches[key]
    if state.handle then
        return -- one fetch at a time per container; new requests queue up
    end
    -- Pure map form: mpv's node command parser rejects a mixed table.
    state.handle = mp.command_native_async({
        name = "subprocess",
        playback_only = false, -- a data fetch outlives track switches
        capture_stdout = true,
        args = { "curl", "-sS", "--fail",
                 "--max-time", CURL_MAX_TIME,
                 "-A", USER_AGENT, state.layer.url },
    }, function(success, result, err)
        finish_fetch(key, success, result, err)
    end)
end

mp.register_script_message("oxbow-script-playlist:need-layer",
        function(list_id, reqid, parent_path)
    if list_id ~= LIST_ID then
        return
    end
    reqid = tostring(reqid)
    parent_path = parent_path or ""
    if parent_path == "" then
        answer(reqid, utils.format_json(root_layer())) -- structure only, no I/O
        return
    end
    -- Single segment: the container id the plugin got from this root layer
    local state = fetches[parent_path]
    if not state then
        log("unknown layer: " .. parent_path)
        answer(reqid, utils.format_json({ items = {} }))
        return
    end
    if not ytdl_enabled() then
        log(parent_path .. ": ytdl is off, answering with an error")
        answer(reqid, utils.format_json({ error = YTDL_OFF_ERROR }))
        return
    end
    state.waiters[#state.waiters + 1] = reqid
    if not state.handle then
        state.starter = reqid -- this request owns the fetch
        start_fetch(parent_path)
    end
end)

mp.register_script_message("oxbow-script-playlist:cancel-layer",
        function(list_id, reqid)
    if list_id ~= LIST_ID then
        return
    end
    reqid = tostring(reqid)
    for key, state in pairs(fetches) do
        local kept = {}
        for _, id in ipairs(state.waiters) do
            if id ~= reqid then
                kept[#kept + 1] = id
            end
        end
        state.waiters = kept
        if state.handle and state.starter == reqid then
            mp.abort_async_command(state.handle) -- completion answers the rest
        end
    end
end)

-- Receipts: created means the id is ours (nothing to prefetch - the root layer
-- is static and containers fetch only when opened); rejected(exists) means
-- another owner holds the id (the id -> owner table only grows), so it only logs.
mp.register_script_message("oxbow-script-playlist:created", function(list_id)
    if list_id == LIST_ID then
        log("data source registered")
    end
end)

mp.register_script_message("oxbow-script-playlist:rejected", function(list_id, reason)
    if list_id == LIST_ID then
        log("create rejected: " .. tostring(reason))
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

    mp.commandv("script-message-to", PLUGIN, "script-playlist", "create",
                SCRIPT, LIST_ID)
end

mp.register_script_message("oxbow-version", init)
mp.commandv("script-message-to", PLUGIN, "get-version", SCRIPT)

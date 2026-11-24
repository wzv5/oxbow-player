--[[
  bilibili-popular.lua - oxbow-player example

  Demonstrates the script-playlist protocol: register a data source id, then
  answer need-layer requests with one JSON payload. The root layer is a static
  table of containers. Opening Popular, Precious or Ranking answers with
  playable items. Weekly is two levels deeper: opening it answers with the
  issue index, opening an issue answers with that week's items - so the plugin
  asks for "weekly/400", and each part of parent-path names one container.

  Only one fetch runs at a time: a request arriving mid-fetch answers with an
  error, and so does a fetch that fails. cancel-layer aborts the fetch its
  request started.

  Bilibili specifics stay in one block - the cookie the issue endpoint wants,
  one curl, and two mappings from data.list. Title goes to "displayName";
  duration / publish date / uploader go to "metadata" as "duration" /
  "recordedDate" / "performer".

  Setup: copy this file into mpv's scripts directory next to the oxbow plugin,
  create a "Script" list in the plugin and bind it to "bilibili-popular".
  Entries resolve through mpv's builtin ytdl hook, so yt-dlp or youtube-dl
  must be in PATH.
]]

local utils = require "mp.utils"

local PLUGIN = "oxbow"
local SCRIPT = mp.get_script_name()
local LIST_ID = "bilibili-popular"

local USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36"
local FETCH_MAX_TIME = "8"
local COOKIE_MAX_TIME = "3"

-- The issue endpoint answers -352 without a buvid3 cookie, so the first
-- fetch asks finger/spi for one and keeps it.
local COOKIE_URL = "https://api.bilibili.com/x/frontend/finger/spi"

local WEEKLY_ID = "weekly"
local WEEKLY_LIST_URL = "https://api.bilibili.com/x/web-interface/popular/series/list"
local WEEKLY_ISSUE_URL = "https://api.bilibili.com/x/web-interface/popular/series/one?number="

-- The root layer. A layer with a url fetches its items on open; Weekly has
-- none, because it expands into issues first.
local LAYERS = {
    { id = "popular", title = "综合热门", url = "https://api.bilibili.com/x/web-interface/popular?ps=20&pn=1" },
    { id = "precious", title = "入站必刷", url = "https://api.bilibili.com/x/web-interface/popular/precious?page_size=100&page=1" },
    { id = "ranking", title = "排行榜", url = "https://api.bilibili.com/x/web-interface/ranking/v2?rid=0&type=all" },
    { id = WEEKLY_ID, title = "每周必看" },
}

local BUSY_ERROR = "Another layer is still loading; retry in a moment."
local FETCH_ERROR = "Bilibili request failed; retry in a moment."
local YTDL_OFF_ERROR = "yt-dlp is not enabled: set ytdl=yes so this list can resolve Bilibili pages."

local function log(message)
    mp.commandv("print-text", "[bilibili-popular] " .. message)
end

-- One JSON payload per reply; the plugin pairs replies by owner + reqid.
local function answer(reqid, payload)
    mp.commandv("script-message-to", PLUGIN, "script-playlist", "layer", SCRIPT, reqid, utils.format_json(payload))
end

-- The root layer is structure only, so it never touches the network.
local function root_layer()
    local containers = {}
    for _, layer in ipairs(LAYERS) do
        containers[#containers + 1] = { id = layer.id, title = layer.title }
    end
    return { containers = containers }
end

-- An unknown ytdl read counts as on.
local function ytdl_enabled()
    return mp.get_property_native("options/ytdl") ~= false
end

-- Bilibili endpoints all keep their array under data.list.
local function list_of(root)
    if type(root) == "table" and type(root.data) == "table" and type(root.data.list) == "table" then
        return root.data.list
    end
end

-- One data.list entry -> a playable item. Only bvid is required; values the
-- plugin cannot use are dropped on its side.
local function to_item(entry)
    if type(entry) ~= "table" or type(entry.bvid) ~= "string" or entry.bvid == "" then
        return nil
    end
    local owner = type(entry.owner) == "table" and entry.owner.name or nil
    return {
        url = "https://www.bilibili.com/video/" .. entry.bvid,
        displayName = entry.title or "",
        metadata = { duration = entry.duration, recordedDate = entry.pubdate, performer = owner },
    }
end

local function to_items(list)
    local items = {}
    for _, entry in ipairs(list) do
        local item = to_item(entry)
        if item then
            items[#items + 1] = item
        end
    end
    return { items = items }
end

-- One data.list entry -> a container. Ids take no path separator, so a
-- numeric issue has to be formatted into one.
local function to_containers(list)
    local containers = {}
    for _, entry in ipairs(list) do
        local id = type(entry) == "table" and entry.number or nil
        if type(id) == "number" then
            id = string.format("%.0f", id)
        end
        if type(id) == "string" and id:match("^%d+$") then
            containers[#containers + 1] = { id = id, title = entry.name or id }
        end
    end
    return { containers = containers }
end

-- parent-path -> the url to fetch and how to turn data.list into a reply.
-- The first segment names a layer; a second segment exists only under Weekly,
-- where it names an issue.
local function resolve(path)
    local id, rest = path:match("^([^/]+)/(.*)$")
    if not id then
        id = path
    end
    if id == WEEKLY_ID then
        if rest and rest ~= "" then
            return WEEKLY_ISSUE_URL .. rest, to_items
        end
        return WEEKLY_LIST_URL, to_containers
    end
    if rest then
        return nil
    end
    for _, layer in ipairs(LAYERS) do
        if layer.id == id then
            return layer.url, to_items
        end
    end
end

-- The running fetch, or nil. cancel-layer aborts it only for its starter.
local pending = nil

-- nil until taken, then the cookie, or "" when the attempt failed - and a
-- failed attempt is never retried, so later requests go without one.
local cookie = nil

local function curl_args(url, max_time)
    local args = { "curl", "-sS", "--fail", "--max-time", max_time, "-A", USER_AGENT }
    if cookie and cookie ~= "" then
        args[#args + 1] = "-b"
        args[#args + 1] = cookie
    end
    args[#args + 1] = url
    return args
end

-- Only the issue endpoint checks the cookie; the others ignore it. Blocking,
-- but it runs once and is bounded by COOKIE_MAX_TIME.
local function ensure_cookie()
    if cookie then
        return
    end
    cookie = ""
    local result = mp.command_native({
        name = "subprocess",
        playback_only = false,
        capture_stdout = true,
        args = curl_args(COOKIE_URL, COOKIE_MAX_TIME),
    })
    local root = type(result) == "table" and result.status == 0 and utils.parse_json(result.stdout) or nil
    local data = type(root) == "table" and root.data or nil
    if type(data) == "table" and data.b_3 and data.b_4 then
        cookie = "buvid3=" .. data.b_3 .. "; buvid4=" .. data.b_4
    end
end

local function start_fetch(reqid, url, to_payload)
    pending = { reqid = reqid }
    ensure_cookie()
    pending.handle = mp.command_native_async({
        name = "subprocess",
        playback_only = false, -- a data fetch outlives track switches
        capture_stdout = true,
        args = curl_args(url, FETCH_MAX_TIME),
    }, function(success, result, err)
        if pending and pending.reqid == reqid then
            pending = nil
        end
        local root = nil
        if success and type(result) == "table" and result.status == 0 then
            root = utils.parse_json(result.stdout)
        end
        local list = list_of(root)
        if not list then
            log("fetch failed" .. (err and (": " .. err) or ""))
            answer(reqid, { error = FETCH_ERROR })
            return
        end
        answer(reqid, to_payload(list))
    end)
end

mp.register_script_message("oxbow-script-playlist:need-layer", function(list_id, reqid, path)
    if list_id ~= LIST_ID then
        return
    end
    if path == "" then
        answer(reqid, root_layer())
        return
    end
    local url, to_payload = resolve(path)
    local video = to_payload == to_items -- item layers resolve through mpv's ytdl hook
    if not url then
        answer(reqid, { error = "Unknown layer: " .. path })
    elseif pending then
        answer(reqid, { error = BUSY_ERROR })
    elseif video and not ytdl_enabled() then
        answer(reqid, { error = YTDL_OFF_ERROR })
    else
        start_fetch(reqid, url, to_payload)
    end
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

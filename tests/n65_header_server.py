# -*- coding: utf-8 -*-
# ============================================================================
# N65 条目级请求头人工验收服务器（不参与单元测试，仅供维护者手工验收）
#
# 起一个只监听 127.0.0.1 的 HTTP 服务，供给两份外部清单：
#   /list.m3u   干净清单——首载【不应】出现任何警告（白名单两键已落地，
#               旧的 "headers … ignored" 串已作废）
#   /warn.m3u   白名单外键 + 末尾悬空指令行——应只弹【一条】新警告
#               （串里不见条数、不见键名），且不报 malformed
# 清单里每条目各带各自的 #EXTVLCOPT / EXTINF 属性区请求头（UA / Referer），
# 另含一份 HLS 媒体清单（验头随文件级选项传播到清单与分片）。
#
# 每收到一个被登记路径的请求，逐头比对"客户端实际带来的头"与"该条目在清单里
# 写明的头"，在控制台打印 PASS / FAIL（含 got / want），并给累计计数。
# 未登记路径只打印原始头（不判）。清单取源由插件 HttpClient 发出、本就不带头，
# 打印为 INFO 不判（N65 §五：不给清单取源请求设头）。
#
# .ts 不产媒体数据：判完头一律回空白 404（验收只看头，媒体内容无关）；HLS 媒体
# 清单照常供（验头随文件级选项传播到清单与分片）。播放时 mpv 请带 `--ytdl=no`
# （否则 404 会触发 ytdl_hook 取网页再问一次，多出一条非插件下发的请求）。
# 端口随机，运行后打印两份 .m3u 的 http 链接。
#
# 用法：python public/tests/n65_header_server.py    （Ctrl+C 退出）
# ============================================================================

import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"

# ——判据取值域——
ABSENT = None            # 该头必须不出现（出现即 FAIL：污染）
DEFAULT_UA = "<default>"  # mpv 默认 UA（--user-agent 默认 libmpv）；未带头的条目落此

# 含逗号与空白的完整 Chrome UA：钉"逐字节原样"（A4 / A4'）——被当 string list
# 分列就会切成两段，被 trim 内部就会丢空白
CHROME_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.6498.127 Safari/537.36"
)
REF_PLAIN = "http://ref.example/watch?from=n65"
REF_COMMA = "http://h/,q=1&r=2"          # 逗号 + 等号 + &：不得被分列 / 转义
REF_ATTR = "http://attr.example/,q=1"    # 属性区引号值内含逗号（引号感知分界）

# 分片路径专用提示：mpv 拿不到分片时可能退回单独重开该 .ts，此时头按 A2 记明
# 还原为默认——真出现这一形态时提示跟着 FAIL 打
SEG_HINT = "mpv 若退回单独重开该 .ts，头会还原为默认（A2 记明的形态）"

# ——判据表：路径 → (期望头, FAIL 时追加的提示)——
EXPECT = {
    "/both.ts": ({"User-Agent": CHROME_UA, "Referer": REF_PLAIN}, None),
    "/comma.ts": ({"User-Agent": CHROME_UA, "Referer": REF_COMMA}, None),
    "/refonly.ts": ({"User-Agent": DEFAULT_UA, "Referer": REF_PLAIN}, None),
    "/none.ts": ({"User-Agent": DEFAULT_UA, "Referer": ABSENT}, None),
    "/emptyua.ts": ({"User-Agent": DEFAULT_UA, "Referer": REF_PLAIN}, None),
    "/emptyref.ts": ({"User-Agent": DEFAULT_UA, "Referer": ABSENT}, None),
    "/attr.ts": ({"User-Agent": "AttrUA/1.0 (build 7)", "Referer": REF_ATTR}, None),
    "/override.ts": ({"User-Agent": "OverrideUA/2.0", "Referer": REF_PLAIN}, None),
    "/bare.ts": ({"User-Agent": "BareUA/3.0", "Referer": ABSENT}, None),
    "/hls/media.m3u8": ({"User-Agent": CHROME_UA, "Referer": REF_PLAIN}, None),
    "/hls/seg0.ts": ({"User-Agent": CHROME_UA, "Referer": REF_PLAIN}, SEG_HINT),
    "/warn1.ts": ({"User-Agent": "WarnUA/1.0", "Referer": ABSENT}, None),
    "/warn2.ts": ({"User-Agent": DEFAULT_UA, "Referer": REF_PLAIN}, None),
}


def match(want, got):
    """单头判定：ABSENT 允许真缺席或空值——mpv 逐文件选项播完还原时把 referrer
    留成空串（--referrer 默认值），此后不带 referrer 的条目会发空值 Referer 头
    （N65 §1.2 实测记明的"播完自动还原"形态，mpv/ffmpeg 层行为，非插件链路）；
    DEFAULT_UA 允许缺席或 libmpv 开头；其余逐字节相等。"""
    if want is ABSENT:
        return got is None or got == ""
    if want is DEFAULT_UA:
        return got is None or got.startswith("libmpv")
    return got == want


def fmt(want):
    if want is ABSENT:
        return "(不出现；空值等价——mpv 还原语义)"
    if want is DEFAULT_UA:
        return "(默认 libmpv)"
    return want


# ——清单素材——

def build_list_m3u(base):
    """干净清单：六种带头形态 + HLS 条目。EXTINF 一律先于其 EXTVLCOPT 行
    （先于 EXTINF 的未消费选项会被新 EXTINF 顶替，口径见 N65 解析侧）。"""
    return "\n".join([
        "#EXTM3U",
        "#EXTINF:-1,UA + Referer (A1 / A4)",
        "#EXTVLCOPT:http-user-agent=" + CHROME_UA,
        "#EXTVLCOPT:http-referrer=" + REF_PLAIN,
        base + "/both.ts",
        "#EXTINF:-1,UA + Referer 含逗号 (A4')",
        "#EXTVLCOPT:http-user-agent=" + CHROME_UA,
        "#EXTVLCOPT:http-referrer=" + REF_COMMA,
        base + "/comma.ts",
        "#EXTINF:-1,Referer only (A1)",
        "#EXTVLCOPT:http-referrer=" + REF_PLAIN,
        base + "/refonly.ts",
        "#EXTINF:-1,no per-entry headers (A1)",
        base + "/none.ts",
        "#EXTINF:-1,empty UA value (A7)",
        "#EXTVLCOPT:http-user-agent=",
        "#EXTVLCOPT:http-referrer=" + REF_PLAIN,
        base + "/emptyua.ts",
        "#EXTINF:-1,empty referrer value (A7)",
        "#EXTVLCOPT:http-referrer=",
        base + "/emptyref.ts",
        '#EXTINF:-1 http-user-agent="AttrUA/1.0 (build 7)" '
        'http-referrer="' + REF_ATTR + '",quoted attrs (A1 属性区支)',
        base + "/attr.ts",
        '#EXTINF:-1 http-user-agent="AttrUA/1.0" http-referrer="' + REF_PLAIN
        + '",后写覆盖 (A2)',
        "#EXTVLCOPT:http-user-agent=OverrideUA/2.0",
        base + "/override.ts",
        "#EXTVLCOPT:http-user-agent=BareUA/3.0",
        base + "/bare.ts",
        "#EXTINF:-1,HLS 清单与分片都带头 (A2)",
        "#EXTVLCOPT:http-user-agent=" + CHROME_UA,
        "#EXTVLCOPT:http-referrer=" + REF_PLAIN,
        base + "/hls/media.m3u8",
        "",
    ])


def build_warn_m3u(base):
    """带白名单外键的清单：该弹一条新警告（不带条数 / 键名），条目照常播；
    末尾留一行无目标行消费的 #EXTVLCOPT（A7 后半：不报 malformed）。"""
    return "\n".join([
        "#EXTM3U",
        "#EXTINF:-1,whitelisted-out key: network-caching",
        "#EXTVLCOPT:network-caching=1000",
        "#EXTVLCOPT:http-user-agent=WarnUA/1.0",
        base + "/warn1.ts",
        "#EXTINF:-1,whitelisted-out key: http-cookie",
        "#EXTVLCOPT:http-cookie=a=b",
        "#EXTVLCOPT:http-referrer=" + REF_PLAIN,
        base + "/warn2.ts",
        "#EXTVLCOPT:http-user-agent=DanglingUA/9.0",
        "",
    ])


def build_hls_m3u(base):
    return "\n".join([
        "#EXTM3U",
        "#EXT-X-VERSION:3",
        "#EXT-X-TARGETDURATION:2",
        "#EXT-X-MEDIA-SEQUENCE:0",
        "#EXTINF:2.0,",
        base + "/hls/seg0.ts",
        "#EXT-X-ENDLIST",
        "",
    ])


CONTENT_TYPE = {
    ".m3u": "application/vnd.apple.mpegurl",
    ".m3u8": "application/vnd.apple.mpegurl",
}


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr):
        self.lock = threading.Lock()
        self.counts = {"pass": 0, "fail": 0}
        self.seen = {}  # 路径 → 已判定次数（第 2 次起若头已还原默认，按 A2 形态计）
        self.playlists = {}
        super().__init__(addr, Handler)

    def emit(self, text):
        """整块一次写出：多线程下不交错。"""
        with self.lock:
            sys.stdout.write(text + "\n")
            sys.stdout.flush()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 静默默认 stderr 日志
        pass

    def _headers_block(self, head_lines):
        return "\n".join(head_lines)

    def _serve(self, body, content_type):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD" and body:
            try:
                self.wfile.write(body)
            except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
                pass  # mpv 中途断开属预期

    def do_GET(self):
        self._handle()

    def do_HEAD(self):
        self._handle()

    def _handle(self):
        path = urllib.parse.urlparse(self.path).path
        srv = self.server
        ua = self.headers.get("User-Agent")
        ref = self.headers.get("Referer")

        if path in srv.playlists:
            # 清单取源：插件 HttpClient 发出、本就不带头（N65 §五）——只记不判
            srv.emit("[INFO] %s %s\n      User-Agent=%s  Referer=%s\n"
                     "        （清单取源不判：N65 §五 不给清单请求设头）"
                     % (self.command, path, ua, ref))
            self._serve(srv.playlists[path].encode("utf-8"),
                        CONTENT_TYPE[".m3u"])
            return

        if path in EXPECT:
            want, hint = EXPECT[path][0], EXPECT[path][1]
            seen_n = srv.seen.get(path, 0)
            srv.seen[path] = seen_n + 1
            ua_got = self.headers.get("User-Agent")
            ref_got = self.headers.get("Referer")
            # 已判定过的路径再次以"全默认头"出现 = mpv 退回重开 / 展开条目的
            # 还原形态（A2 记明），单独标注、不计 PASS/FAIL
            if seen_n > 0 and (ua_got is None or ua_got.startswith("libmpv")) \
                    and (ref_got is None or ref_got == ""):
                srv.emit("[还原] GET %s（第 %d 次）\n"
                         "      头已还原为默认——A2 记明的退回重开 / 收编展开形态，不计判定\n"
                         % (path, seen_n + 1))
                if path.endswith(".ts"):
                    self._send_empty_404()
                else:
                    self._serve(srv.playlists["/__hls__.m3u8"].encode("utf-8"),
                                CONTENT_TYPE[".m3u8"])
                return
            note = "（HEAD，仅判头）" if self.command == "HEAD" else ""
            lines = [("[%s] %s %s" % (self.command, path, note)).rstrip()]
            fails = 0
            for name, exp in want.items():
                got = self.headers.get(name)
                ok = match(exp, got)
                if not ok:
                    fails += 1
                row = "      %-11s %s  got=%r" % (name, "PASS" if ok else "FAIL", got)
                if not ok:
                    row += "  want=%s" % fmt(exp)
                lines.append(row)
            if fails and hint:
                lines.append("      提示：" + hint)
            with srv.lock:
                if fails:
                    srv.counts["fail"] += 1
                else:
                    srv.counts["pass"] += 1
                total = srv.counts["pass"] + srv.counts["fail"]
                counts = (srv.counts["pass"], srv.counts["fail"], total)
            lines.append("      >>> %s（累计 %d PASS / %d FAIL，共 %d 次判定）"
                         % ("FAIL" if fails else "PASS", counts[0], counts[1], counts[2]))
            srv.emit(self._headers_block(lines))
            if path.endswith(".ts"):
                self._send_empty_404()  # 判完头即回空白 404，不产媒体数据
            else:
                body = srv.playlists["/__hls__.m3u8"].encode("utf-8")
                self._serve(body, CONTENT_TYPE[".m3u8"])
            return

        if path.endswith(".ts"):
            srv.emit("[RAW] %s %s\n      User-Agent=%s  Referer=%s\n"
                     "        （未登记路径，只记不判）" % (self.command, path, ua, ref))
            self._send_empty_404()
            return

        srv.emit("[404] %s %s" % (self.command, path))
        self._send_empty_404()

    def _send_empty_404(self):
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()


def print_table():
    print("判据表（这些路径的请求逐头比对，其余只记不判）：", flush=True)
    for path in EXPECT:
        want = EXPECT[path][0]
        parts = ["%s=%s" % (k, fmt(v)) for k, v in want.items()]
        print("  %-20s %s" % (path, "  ".join(parts)), flush=True)


def main():
    httpd = Server((HOST, 0))
    port = httpd.server_address[1]
    base = "http://%s:%d" % (HOST, port)

    httpd.playlists = {
        "/list.m3u": build_list_m3u(base),
        "/warn.m3u": build_warn_m3u(base),
        "/__hls__.m3u8": build_hls_m3u(base),
    }

    print("", flush=True)
    print("N65 条目级请求头验收服务器已绑定 %s" % base, flush=True)
    print("", flush=True)
    print("测试清单（干净，首载不应出现任何警告）：", flush=True)
    print("  %s/list.m3u" % base, flush=True)
    print("测试清单（白名单外键 + 末尾悬空行：应只出一条新警告、不报 malformed）：",
          flush=True)
    print("  %s/warn.m3u" % base, flush=True)
    print("", flush=True)
    print_table()
    print("", flush=True)
    print(".ts 一律回空白 404（只判头，不产媒体数据；播放请给 mpv 加 --ytdl=no）",
          flush=True)
    print("等待请求……（Ctrl+C 退出）", flush=True)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n退出。累计 %d PASS / %d FAIL"
              % (httpd.counts["pass"], httpd.counts["fail"]), flush=True)


if __name__ == "__main__":
    main()

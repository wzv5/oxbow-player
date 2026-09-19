# ============================================================================
# N34 HttpClient 单元测试桩服务器（纯 stdlib + 外部 openssl 命令行）
#
# 由 tst_HttpClient.cpp 以 QProcess 启动；要求 PATH 上有 python 与 openssl。
# 就绪后向 stdout 打一行：
#   STUB-READY {"http":A,"defect":B,"flow":F,"https":C,"ca":"-----BEGIN..."}
# 其中 A/B/F/C 为 127.0.0.1 上随机绑定的端口，D 为 Digest 专用 origin 端口，
# ca 为 openssl 现场生成的自签证书
# PEM（SAN 含 IP:127.0.0.1，测试按是否加入进程内可信根区分信任两态）。
# TLS 握手在每个连接的 handler 线程完成（不占 accept 循环）。
#
# 端点：
#   主端口(A)
#     /basic/*        401 Basic（WWW-Authenticate），凭据 u1:p1 通过 → 200 "basic ok"
#     /digest/*       只认 Digest（401 + Digest 挑战；Basic 头一律再挑战），
#                     凭据 u2:p2 通过 → 200 "digest ok"（authenticationRequired 回退用）
#     /redir-rel/a    301 → Location: /b
#     /redir-303/a    303 → Location: /b
#     /redir-loop/a   301 → Location: 自身（循环跳转）
#     /redir-slow/a   301 → Location: /slow（abort 落在重定向后一跳用）
#     /redir-abs/a    301 → Location: http://127.0.0.1:<缺陷端口>/b（跨 origin 重定向：
#                     端口不同即不同 origin；N50 流式用它验「跨 origin 照常跟随」
#                     与「凭据不越界」——缺陷端口独立记日志，可直接查该跳有没有带
#                     Authorization）
#     /redir-cross-basic/a
#                     301 → Location: http://127.0.0.1:<专用认证端口>/basic/x（跨 origin
#                     且落点【需要认证】）。N50 流式的「凭据不越界」判据：落点索要认证
#                     时不得回填原 origin 的凭据，请求须以 AuthenticationRequired 落终态，
#                     且落点日志中该跳不得带 Authorization。
#                     落点必须是【缓存干净的专用 origin】，不能复用认证端口(D)：QNAM 按
#                     host:port 缓存已成功的 authenticator，认证端口被 Digest 用例认证过，
#                     同 origin 的新请求会先回放那份缓存凭据，日志上的 auth 标志就不再
#                     只反映我们的行为（详见 D 端口那段注释与 N50 用例注释）
#     /slow           sleep 8s 后 200（总时限 / 切片取消用）
#     /trickle        头立即回、body 50 字节每 100ms 吐 1 字节（总时长 ~5s；
#                     总时限 vs 闲置超时的区分用：慢速但持续传输也须在总时限处断开）
#     /trickle-long   同上但更长：body 80 字节每 100ms 吐 1 字节（~8s）。N50 流式用：
#                     流式作业无总时限、须读满；同端点上的非流式作业会在 5000ms
#                     总时限处断开，构成计时器分支的对照判据
#     /notfound       404（N50 流式错误终态用：非 2xx 该落 kReadError）
#     /truncated      声明 Content-Length: 100000 却只发 4096 字节就关连接（N50 流式
#                     「缓冲先于终态」用：错误终态下已到的字节仍须先交付再报错）
#     /hold           sleep 1.5s 后 200（并发 / 排队撤销用）
#     /big            200 + 2MiB body（headersOnly 截断 / 体上限）；HEAD 正确只回头
#     PROPFIND 任意路径 207，body = "MS:" + 请求体（验证同方法同 body 跟随重定向）
#     其余            200 "ok <path>"
#   缺陷端口(B) —— HEAD 一律 405（缺陷登记判据：不支持 HEAD）
#   涌body端口(F) —— HEAD /flow 把 HEAD 当 GET 直传 1MiB 不断开（实测干净 200；
#                     不断开即不可区分、不得登记，用例断言不污染，见 _handle_defect 注释）；
#                     与 405 端口隔离：两用例各自登记、互不污染；其余同缺陷端口
#   认证端口(D) —— /digest/* 专用 origin（kind 与主端口同为 main）。
#                     必须独立 origin：QNAM 按连接缓存已成功的 authenticator，
#                     同 origin 换凭据后首次请求会先以旧凭据试一次（401 后经
#                     authenticationRequired 换新凭据自愈），挑战往返计数断言
#                     会与用例执行顺序耦合，故隔离。
#   https端口(C)
#     /tls            200 "tls ok"
#   各端口通用
#     GET  /__log     返回 JSON {"requests":[...],"connections":{"total","peak"}}
#                     （自身不记日志）
#     POST /__reset   清空日志与连接计数（自身不记日志）
# ============================================================================

import base64
import hashlib
import json
import os
import ssl
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CN = "N34 Stub Test"


def generate_cert_and_key(cert_dir, cert_path, key_path):
    """openssl 现场生成自签证书（SAN 含 127.0.0.1，30 天有效期）。
    部分发行版编译期内置的 openssl.cnf 路径不存在（如 scoop 版），故自带最小配置。"""
    cnf_path = os.path.join(cert_dir, "openssl.cnf")
    with open(cnf_path, "w") as f:
        f.write("[req]\ndistinguished_name = req_dn\n[req_dn]\n")
    subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", key_path, "-out", cert_path,
            "-days", "30", "-subj", "/CN=" + CN,
            "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
            "-config", cnf_path,
        ],
        check=True, capture_output=True,
    )


class StubServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, kind, ssl_ctx=None):
        self.kind = kind          # "main" / "defect"
        self.ssl_ctx = ssl_ctx    # 非 None 时在 handler 线程内做 TLS 握手
        self.log = []
        self.log_lock = threading.Lock()
        # 连接计数：total 累计连接数，active 当前在连，peak 峰值并发（重置归零）
        self.conn_count = 0
        self.conn_active = 0
        self.conn_peak = 0
        self.defect_port = 0
        self.auth2_port = 0  # 专用认证 origin（跨 origin 重定向的落点用，见文件头注释）
        super().__init__(("127.0.0.1", 0), StubHandler)


class StubHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 静默默认 stderr 日志
        pass

    # TLS 握手下放到 handler 线程：accept 循环不被握手阻塞/污染，
    # 单个握手失败只影响该连接
    def setup(self):
        self._counted = False
        if self.server.ssl_ctx is not None:
            # TLS 包装必须作用在 self.request 上：self.connection 由
            # super().setup() 从 self.request 赋值，此时还不存在
            self.request = self.server.ssl_ctx.wrap_socket(
                self.request, server_side=True)
        super().setup()
        with self.server.log_lock:
            self.server.conn_count += 1
            self.server.conn_active += 1
            self.server.conn_peak = max(self.server.conn_peak, self.server.conn_active)
            self._counted = True

    def finish(self):
        if self._counted:
            with self.server.log_lock:
                self.server.conn_active = max(0, self.server.conn_active - 1)
        if hasattr(self, "rfile"):
            super().finish()
        else:
            # 握手失败：流未建立，直接关闭连接（socketserver 会收尾 request）
            try:
                self.request.close()
            except OSError:
                pass

    # ——工具——
    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        return self.rfile.read(length) if length else b""

    def _respond(self, status, body=b"", headers=()):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _record(self):
        with self.server.log_lock:
            auth = self.headers.get("Authorization", "")
            self.server.log.append({
                "method": self.command,
                "path": self.path.split("?")[0],
                "auth": bool(auth),
                # 认证方案（"Basic" / "Digest" / ""），供测试诊断挑战往返用
                "scheme": auth.split(" ", 1)[0] if auth else "",
                # 完整 Authorization（测试固件凭据，无保密要求），供深查摘要计算用
                "authv": auth,
            })

    def _handle_common(self):
        body = self._read_body()

        if self.command == "GET" and self.path == "/__log":
            with self.server.log_lock:
                payload = json.dumps({
                    "requests": self.server.log,
                    "connections": {
                        "total": self.server.conn_count,
                        "peak": self.server.conn_peak,
                    },
                }).encode()
            self._respond(200, payload, [("Content-Type", "application/json")])
            return
        if self.command == "POST" and self.path == "/__reset":
            with self.server.log_lock:
                self.server.log.clear()
                self.server.conn_count = 0
                self.server.conn_active = 0
                self.server.conn_peak = 0
            self._respond(200, b"reset")
            return

        self._record()

        if self.server.kind == "defect":
            self._handle_defect(body)
        else:
            self._handle_main(body)

    def _handle_main(self, body):
        # 缺省 Basic 认证：u1:p1 → 200，其余 401
        if self.path.startswith("/basic"):
            expected = "Basic " + base64.b64encode(b"u1:p1").decode()
            if self.headers.get("Authorization") == expected:
                self._respond(200, b"basic ok")
            else:
                self._respond(401, b"need auth", [("WWW-Authenticate", 'Basic realm="stub"')])
            return
        # Digest-only 认证：只认 Digest 响应（含预置 Basic 的首打一律再挑战，
        # 走 authenticationRequired 回退）；凭据 u2:p2 → 200，其余 401
        if self.path.startswith("/digest"):
            self._handle_digest()
            return
        # 慢响应：总时限 / 切片取消
        if self.path == "/slow":
            time.sleep(8)
            self._respond(200, b"slow ok")
            return
        # 细流响应：头立即回，body 每 100ms 吐 1 字节（共 ~5s）——慢速但持续
        # 传输须在总时限处断开（区分总时限与闲置超时）；客户端中途断开属预期
        if self.path == "/trickle":
            self.send_response(200)
            self.send_header("Content-Length", "50")
            self.end_headers()
            try:
                for _ in range(50):
                    time.sleep(0.1)
                    self.wfile.write(b"x")
                    self.wfile.flush()
            except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
                pass
            return
        # 长细流：同上但总时长 ~8s，明显超过 Config::timeoutMs 的 5000ms 默认
        # （N50 流式：无总时限须读满；非流式在同一端点必被总时限断开）
        if self.path == "/trickle-long":
            self.send_response(200)
            self.send_header("Content-Length", "80")
            self.end_headers()
            try:
                for _ in range(80):
                    time.sleep(0.1)
                    self.wfile.write(b"y")
                    self.wfile.flush()
            except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
                pass
            return
        # 404：流式错误终态（非 2xx）
        if self.path == "/notfound":
            self._respond(404, b"nope")
            return
        # 声明 100000 字节却只发 4096 就关连接：流式「缓冲先于终态」用——错误终态下
        # 已到的字节仍须先交付完再报错，不得截断丢弃
        if self.path == "/truncated":
            self.send_response(200)
            self.send_header("Content-Length", "100000")
            self.end_headers()
            try:
                self.wfile.write(b"z" * 4096)
                self.wfile.flush()
            except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
                pass
            self.close_connection = True
            return
        # 短挂起：并发 / 排队撤销用（1.5s，快于 /slow）
        if self.path == "/hold":
            time.sleep(1.5)
            self._respond(200, b"hold ok")
            return
        # 大响应体：headersOnly 截断 / 体上限；HEAD 正确处理（只回头）
        if self.path == "/big":
            data = b"x" * (2 * 1024 * 1024)
            self._respond(200, data if self.command != "HEAD" else b"")
            return
        # 重定向族：301/303 保方法保 body 跟随；/b 为落点
        redirects = {
            "/redir-rel/a": (301, "/b"),
            "/redir-303/a": (303, "/b"),
            "/redir-loop/a": (301, "/redir-loop/a"),
            "/redir-slow/a": (301, "/slow"),
            "/redir-abs/a": (301, "http://127.0.0.1:%d/b" % self.server.defect_port),
            # N50：跨 origin 且落点【需要认证】——凭据不越界的判据。首跳 origin 无凭据可带
            # （流式不预置），落点索要认证时兜底也不得回填原 origin 的凭据，故请求须以
            # AuthenticationRequired 落错误终态
            "/redir-cross-basic/a": (301,
                                     "http://127.0.0.1:%d/basic/x" % self.server.auth2_port),
        }
        if self.path in redirects:
            status, loc = redirects[self.path]
            self._respond(status, b"", [("Location", loc)])
            return
        # PROPFIND：响应体回显请求体（验证同方法同 body 跟随重定向）
        if self.command == "PROPFIND":
            self._respond(207, b"MS:" + body)
            return
        self._respond(200, b"ok " + self.path.encode())

    DIGEST_REALM = "stub"
    DIGEST_NONCE = "n34nonce"
    DIGEST_USER = "u2"
    DIGEST_PASS = "p2"

    def _md5(self, s):
        return hashlib.md5(s.encode()).hexdigest()

    def _handle_digest(self):
        """Digest-only 认证（RFC 7616 qop=auth 简化实现，无重放保护，测试够用）。
        非 Digest 头（含预置 Basic 的首打）一律 401 + Digest 挑战，客户端经
        authenticationRequired 带 Digest 重试；凭据 u2:p2 通过 → 200。"""
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Digest "):
            fields = {}
            for part in auth[7:].split(","):
                if "=" in part:
                    k, v = part.split("=", 1)
                    fields[k.strip()] = v.strip().strip('"')
            try:
                ha1 = self._md5("%s:%s:%s" % (self.DIGEST_USER, self.DIGEST_REALM,
                                              self.DIGEST_PASS))
                ha2 = self._md5("%s:%s" % (self.command, fields["uri"]))
                # 两种算法都接受：Qt 首试可能不带 qop（RFC 2069 老式），
                # 重试才带 nc/cnonce/qop；缺字段即 401，不抛异常
                if fields.get("qop"):
                    expected = self._md5("%s:%s:%s:%s:%s:%s" % (
                        ha1, fields["nonce"], fields["nc"], fields["cnonce"],
                        fields["qop"], ha2))
                else:
                    expected = self._md5("%s:%s:%s" % (ha1, fields["nonce"], ha2))
                if (fields.get("username") == self.DIGEST_USER
                        and fields.get("realm") == self.DIGEST_REALM
                        and fields.get("nonce") == self.DIGEST_NONCE
                        and fields.get("response") == expected):
                    self._respond(200, b"digest ok")
                    return
            except KeyError:
                pass
        self._respond(401, b"need digest", [(
            "WWW-Authenticate",
            'Digest realm="%s", nonce="%s", algorithm=MD5, qop="auth"'
            % (self.DIGEST_REALM, self.DIGEST_NONCE))])

    def _handle_defect(self, body):
        # 缺陷服务器模拟：把 HEAD 当 GET 直传 1MiB 后不断开（keep-alive）。
        # 可观测边界（实测结论）：QNAM 在响应头到达即完成 HEAD——不派发响应体、
        # 不等 body、不理会后续断开；故该形状干净 200、不得登记，用例以此断言
        # "不断开的涌 body 不污染缺陷表"。缺陷登记仅来自 405 / 501 明示，
        # 以及将来某 Qt 版本若派发 HEAD 体时命中的 64KB 阈值（防御性保留）。
        if self.command == "HEAD" and self.path == "/flow":
            self.send_response(200)
            self.send_header("Content-Length", str(4 * 1024 * 1024))
            self.end_headers()
            try:
                for _ in range(4):  # 共 1MiB；客户端不读（流控下服务端可能阻塞，属预期）
                    self.wfile.write(b"x" * (256 * 1024))
                    self.wfile.flush()
            except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
                pass
            return
        if self.command == "HEAD":
            self._respond(405, b"head not allowed")
            return
        # PROPFIND 与主端口同形：跨 origin 重定向落点同为 207，方法保持才可验证
        if self.command == "PROPFIND":
            self._respond(207, b"MS:" + body)
            return
        self._respond(200, b"ok " + self.path.encode())

    do_GET = _handle_common
    do_POST = _handle_common
    do_HEAD = _handle_common
    do_PROPFIND = _handle_common
    do_PUT = _handle_common


def main():
    cert_dir = tempfile.mkdtemp(prefix="n34_stub_")
    cert_path = os.path.join(cert_dir, "stub.crt")
    key_path = os.path.join(cert_dir, "stub.key")
    generate_cert_and_key(cert_dir, cert_path, key_path)
    with open(cert_path) as f:
        cert_pem = f.read()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert_path, key_path)

    main_srv = StubServer("main")
    defect_srv = StubServer("defect")
    flow_srv = StubServer("defect")  # 涌 body 专用 origin（与 405 登记隔离）
    auth_srv = StubServer("main")  # Digest 专用 origin（与主端口隔离，见上）
    tls_srv = StubServer("main", ssl_ctx=ctx)
    tls2_srv = StubServer("main", ssl_ctx=ctx)  # 未受信用例专用 origin
    auth2_srv = StubServer("main")  # 跨 origin 重定向落点专用 origin（须缓存干净，见上）

    for srv in (main_srv, defect_srv, flow_srv, auth_srv, tls_srv, tls2_srv, auth2_srv):
        srv.defect_port = defect_srv.server_address[1]
        srv.auth2_port = auth2_srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()

    print(
        "STUB-READY "
        + json.dumps({
            "http": main_srv.server_address[1],
            "defect": defect_srv.server_address[1],
            "flow": flow_srv.server_address[1],
            "auth": auth_srv.server_address[1],
            "auth2": auth2_srv.server_address[1],
            "https": tls_srv.server_address[1],
            "tls2": tls2_srv.server_address[1],
            "ca": cert_pem,
        }),
        flush=True,
    )

    # 由测试进程 kill 结束；此处只等待
    threading.Event().wait()


if __name__ == "__main__":
    main()

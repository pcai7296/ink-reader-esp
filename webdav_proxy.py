# -*- coding: utf-8 -*-
"""ESP8266 进度同步 -> 坚果云 WebDAV 的 HTTP 反向代理桥。

设备(ESP8266) 以明文 http 访问本机:  http://<PC-IP>:8080/dav/...
本脚本转发到坚果云官方:             https://dav.jianguoyun.com/dav/...
(官方 TLS 由 PC 承担, 不受 ESP8266 RAM 限制; Authorization 头原样透传)

用法:  python webdav_proxy.py [端口]     默认 8080
"""
import http.server
import urllib.request
import urllib.error
import sys

TARGET = "https://dav.jianguoyun.com"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

SKIP_REQ = ("host", "content-length", "transfer-encoding",
            "connection", "accept-encoding", "user-agent")
SKIP_RSP = ("transfer-encoding", "connection", "keep-alive", "content-encoding")


class Proxy(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _forward(self, method, body=None):
        url = TARGET + self.path
        req = urllib.request.Request(url, data=body, method=method)
        for k, v in self.headers.items():
            if k.lower() in SKIP_REQ:
                continue
            req.add_header(k, v)
        try:
            resp = urllib.request.urlopen(req, timeout=60)
            data = resp.read()
        except urllib.error.HTTPError as e:
            resp, data = e, e.read()
        except Exception as e:
            self.send_response(502)
            self.send_header("Content-Length", "0")
            self.end_headers()
            print("PROXY_ERR", method, self.path, repr(e), flush=True)
            return
        self.send_response(resp.status)
        sent_len = False
        for k, v in resp.headers.items():
            if k.lower() in SKIP_RSP:
                continue
            if k.lower() == "content-length":
                if method == "HEAD":
                    # HEAD 无 body: 保留上游 Content-Length(资源大小)
                    self.send_header(k, v)
                sent_len = True
                continue
            self.send_header(k, v)
        if method != "HEAD" and not sent_len:
            self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if method != "HEAD":
            self.wfile.write(data)
        print("PROXY", method, self.path, resp.status, flush=True)

    def do_HEAD(self):
        self._forward("HEAD")

    def do_GET(self):
        self._forward("GET")

    def do_PUT(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        self._forward("PUT", body)

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    print("webdav_proxy listening on 0.0.0.0:%d -> %s" % (PORT, TARGET), flush=True)
    http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Proxy).serve_forever()

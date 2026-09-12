#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""phone_sim_server.py — 在电脑上模拟"手机端进度服务器"（LUMI1 协议）

用户 2026-09-12 要求：手机端应答器容易被系统杀掉导致联调反复，先用 **PC 模拟手机**
把设备侧彻底跑通，再回头优化手机体验。

它同时提供手机端的两项服务：
  1. **UDP :8390 发现应答** —— 收到 `LUMIDISC` 就回 `LUMIACK <本机 IP>`（设备靠这个找到"手机"）
  2. **HTTP :8384 进度服务** —— `GET /progress?file=<RFC3986>` 回 LUMI1；`PUT /progress` 收下并打印

用法：
  python phone_sim_server.py                          # 默认返回 offset=0（0%）
  python phone_sim_server.py --pct 86.97              # 按百分比造进度（size 默认 58418739）
  python phone_sim_server.py --offset 50803872 --size 58418739
  python phone_sim_server.py --no-book                # 模拟"手机上没有这本书"(404 no-book)
  python phone_sim_server.py --no-progress            # 模拟"手机侧还没进度"(404 no-progress)
  python phone_sim_server.py --file "《武炼巅峰》作者：莫默.txt"   # 只认这本书，其它回 no-book

设备侧用法：设备会通过 UDP 发现找到本机 IP（同网段），无需手工配置；
若发现失败，可在 SD 根目录放 `/sync.cfg` 写入 `ip=<本机IP>` 直连。
"""

import argparse
import json
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, unquote

UDP_PORT = 8390
HTTP_PORT = 8384
DISCOVERY_QUERY = "LUMIDISC"
DISCOVERY_REPLY = "LUMIACK "

STATE = {
    "file": None,        # None = 任何书名都认
    "mode": "ok",        # ok | no-book | no-progress
    "size": 58418739,
    "offset": 0,
    "pct": None,
    "ts": 0,
}


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def local_ips():
    """本机所有 IPv4（供 UDP 应答选与请求方同网段的地址）"""
    ips = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips:
                ips.append(ip)
    except Exception:
        pass
    # 兜底：用一个不会真的发包的 UDP connect 探测默认出口 IP
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        if ip not in ips:
            ips.insert(0, ip)
    except Exception:
        pass
    return ips


def udp_responder(stop_evt):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", UDP_PORT))
    s.settimeout(0.5)
    log("UDP 发现应答已启动 :%d（收到 LUMIDISC 回 LUMIACK <本机IP>）" % UDP_PORT)
    while not stop_evt.is_set():
        try:
            data, addr = s.recvfrom(256)
        except socket.timeout:
            continue
        except Exception as e:
            log("UDP 收包异常: %r" % (e,))
            continue
        text = data.decode("utf-8", "replace").strip()
        if text == DISCOVERY_QUERY:
            ips = local_ips()
            prefix = addr[0].rsplit(".", 1)[0]
            self_ip = next((ip for ip in ips if ip.rsplit(".", 1)[0] == prefix), None) or (ips[0] if ips else None)
            if self_ip:
                reply = (DISCOVERY_REPLY + self_ip).encode("utf-8")
                s.sendto(reply, addr)
                log("UDP 发现应答 -> %s:%d  [%s]" % (addr[0], addr[1], DISCOVERY_REPLY + self_ip))
        else:
            log("UDP 收到未知包 from %s: %r" % (addr, text[:40]))


def build_lumi():
    size = int(STATE["size"])
    if STATE["pct"] is not None:
        offset = int(size * float(STATE["pct"]) / 100.0)
    else:
        offset = int(STATE["offset"])
    pct = float(STATE["pct"]) if STATE["pct"] is not None else (offset * 100.0 / size if size else 0.0)
    ts = STATE["ts"] or int(time.time() * 1000)
    return "LUMI1\nts=%d\nsize=%d\noffset=%d\npct=%.2f\n" % (ts, size, offset, pct)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"      # 需要显式 Content-Length；见下方每次响应都带

    def log_message(self, fmt, *args):
        pass                            # 用我们自己的日志格式

    def _send(self, code, body):
        raw = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        fname = unquote(q.get("file", [""])[0])
        log("GET %s  file=[%s]" % (u.path, fname))
        if u.path != "/progress":
            self._send(404, "not found")
            return
        if not fname:
            self._send(400, "missing file param")
            return
        if STATE["mode"] == "no-book":
            log("  -> 404 no-book（模拟：手机书架上没有这本书）")
            self._send(404, "no-book")
            return
        if STATE["mode"] == "no-progress":
            log("  -> 404 no-progress（模拟：手机侧还没这本书的进度）")
            self._send(404, "no-progress")
            return
        if STATE["file"] and fname != STATE["file"]:
            log("  -> 404 no-book（只认 [%s]）" % STATE["file"])
            self._send(404, "no-book")
            return
        payload = build_lumi()
        log("  -> 200 LUMI1:\n" + "\n".join("     " + l for l in payload.strip().split("\n")))
        self._send(200, payload)

    def do_PUT(self):
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        log("PUT %s (Content-Length=%d) body:\n" % (u.path, n) +
            "\n".join("     " + l for l in body.strip().split("\n")))
        if u.path != "/progress":
            self._send(404, "not found")
            return
        if STATE["mode"] == "no-book":
            log("  -> 404 no-book（模拟）")
            self._send(404, "no-book")
            return
        # 收下并解析出 file=/offset= 打日志，便于确认设备推了什么
        info = {}
        for line in body.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                info[k.strip()] = v.strip()
        log("  -> 200 ok（收到 file=[%s] offset=%s size=%s pct=%s）" %
            (info.get("file", "?"), info.get("offset", "?"), info.get("size", "?"), info.get("pct", "?")))
        self._send(200, "ok")


def main():
    ap = argparse.ArgumentParser(description="模拟手机端进度服务器（LUMI1）")
    ap.add_argument("--port", type=int, default=HTTP_PORT, help="HTTP 端口（默认 8384）")
    ap.add_argument("--udp-port", type=int, default=UDP_PORT, help="发现端口（默认 8390）")
    ap.add_argument("--file", default=None, help="只认这个书名（默认任何书名都认）")
    ap.add_argument("--size", type=int, default=STATE["size"], help="文件大小（默认 58418739）")
    ap.add_argument("--offset", type=int, default=0, help="字节偏移")
    ap.add_argument("--pct", type=float, default=None, help="百分比（给了就按它算 offset）")
    ap.add_argument("--no-book", action="store_true", help="模拟 404 no-book")
    ap.add_argument("--no-progress", action="store_true", help="模拟 404 no-progress")
    ap.add_argument("--no-udp", action="store_true", help="不开 UDP 应答（只测 HTTP）")
    a = ap.parse_args()

    STATE["file"] = a.file
    STATE["size"] = a.size
    STATE["offset"] = a.offset
    STATE["pct"] = a.pct
    if a.no_book:
        STATE["mode"] = "no-book"
    elif a.no_progress:
        STATE["mode"] = "no-progress"

    log("本机 IP: %s" % ", ".join(local_ips()))
    log("模式=%s file=%s size=%d offset=%d pct=%s" %
        (STATE["mode"], STATE["file"] or "(任意)", STATE["size"], STATE["offset"], STATE["pct"]))

    stop_evt = threading.Event()
    if not a.no_udp:
        threading.Thread(target=udp_responder, args=(stop_evt,), daemon=True).start()

    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    log("HTTP 进度服务已启动 :%d（GET/PUT /progress）按 Ctrl+C 退出" % a.port)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        log("退出")
    finally:
        stop_evt.set()
        srv.server_close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
phone_sim_server.py — 用电脑模拟"手机端 LUMI1 进度服务器"（安卓真机不在身边时的联调工具）。
- HTTP :8384  GET /progress?file=... -> 200 + LUMI1(含 v3 指纹, 模拟手机带水印版《武炼巅峰》)
           PUT  /progress -> 解析 body, 打印是否携带指纹组, 200/400
- UDP  :8390  LUMIDISC -> LUMIACK <本机IP>   (ESP STA 模式下自动发现电脑)
行为刻意模拟"手机文件与 ESP 不一致"(181B 水印): fs/h0/h1 不同, h2 相同。
"""
import http.server
import socketserver
import socket
import threading
import sys

# 手机版《武炼巅峰》(带 181B zxcs 水印) 的指纹与进度 —— 实测抓取自安卓端 GET 响应
PHONE_FP = {
    'fs': 57969941,
    'h0': 'c92074002b4cb80d7c7e3c43868c2877c056c6c0',
    'h1': '7c3859db47585834dbb284f829a15c2f8f239222',
    'h2': '3c786e7c5aabbd42594dfb28769cef13cdfa5502',
}
GET_BODY = (
    "LUMI1\nts=1787672031406\nsize=57969941\noffset=18535674\npct=31.97\n"
    f"fs={PHONE_FP['fs']}\nh0={PHONE_FP['h0']}\nh1={PHONE_FP['h1']}\nh2={PHONE_FP['h2']}\n"
)


def local_ip():
    # 强制选路由到 192.168.0.0/24 (ESP 所在网段) 的接口; UDP connect 只做路由选择不发包
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('192.168.0.10', 80))
        return s.getsockname()[0]
    except Exception:
        return '192.168.0.7'
    finally:
        s.close()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith('/progress'):
            body = GET_BODY.encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)
            print(f'[GET] {self.path[:80]} -> 200 ({len(body)}B, 含指纹)', flush=True)
        else:
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()

    def do_PUT(self):
        ln = int(self.headers.get('Content-Length', 0) or 0)
        body = self.rfile.read(ln).decode('utf-8', 'replace') if ln > 0 else ''
        has_fp = all(k in body for k in ('fs=', 'h0=', 'h1=', 'h2='))
        size = offset = pct = ''
        for line in body.split('\n'):
            if line.startswith('size='):
                size = line[5:]
            elif line.startswith('offset='):
                offset = line[7:]
            elif line.startswith('pct='):
                pct = line[4:]
        print(f'[PUT] body({ln}B) size={size} offset={offset} pct={pct} 指纹组={"有" if has_fp else "无!!!"}', flush=True)
        if has_fp:
            # 模拟手机端三态对比: 与手机文件指纹比较
            mask = 0
            if size and size != str(PHONE_FP['fs']):
                mask |= 1
            for line in body.split('\n'):
                if line.startswith('h0=') and line[3:] != PHONE_FP['h0']:
                    mask |= 2
                elif line.startswith('h1=') and line[3:] != PHONE_FP['h1']:
                    mask |= 4
                elif line.startswith('h2=') and line[3:] != PHONE_FP['h2']:
                    mask |= 8
            parts = []
            if mask & 1:
                parts.append('大小')
            if mask & 2:
                parts.append('头部')
            if mask & 4:
                parts.append('中部')
            if mask & 8:
                parts.append('尾部')
            print(f'[PUT] 三态=MISMATCH mask={mask} 警告="文件不一致：{'/'.join(parts) or ''}"', flush=True)
        self.send_response(200)
        self.send_header('Content-Length', '2')
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(b'ok')


def udp_loop(ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(('0.0.0.0', 8390))
    print(f'[UDP] 8390 监听中 (LUMIDISC -> LUMIACK {ip})', flush=True)
    while True:
        try:
            data, addr = s.recvfrom(256)
            if data.startswith(b'LUMIDISC'):
                # 注意: 不能带 \n —— ESP IPAddress::fromString 遇 \n 返回 false, 发现会失败
                s.sendto(f'LUMIACK {ip}'.encode(), addr)
                print(f'[UDP] 收到 LUMIDISC 来自 {addr[0]}:{addr[1]} -> 回复 {ip}', flush=True)
        except Exception as e:
            print(f'[UDP] err {e}', flush=True)


if __name__ == '__main__':
    ip = local_ip()
    print(f'=== 电脑模拟手机服务器 本机IP={ip} ===', flush=True)
    threading.Thread(target=udp_loop, args=(ip,), daemon=True).start()
    with socketserver.ThreadingTCPServer(('0.0.0.0', 8384), Handler) as srv:
        srv.allow_reuse_address = True
        print('[HTTP] 8384 监听中 (GET/PUT /progress)', flush=True)
        srv.serve_forever()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""serial_listen.py — 可靠版 ESP 串口监听 (pyserial), 写入日志文件。"""
import serial
import sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM20'
log = sys.argv[2] if len(sys.argv) > 2 else r'J:\code\esp8266\ink-reader-esp\build\serial_diag4.log'

ser = serial.Serial(port, 115200, timeout=0.5)
print(f'listening {port} -> {log}', flush=True)
with open(log, 'ab') as f:
    while True:
        data = ser.read(4096)
        if data:
            f.write(data)
            f.flush()

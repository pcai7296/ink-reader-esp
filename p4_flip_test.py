#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P4 连续翻页验收: 串口注入 K3S 翻页 N 次, 统计异常 (零 SD 访问 / 无第 1 页异常 / 无重启)。

前置:
  1) 固件用 `-DSERIAL_REMOTE=1` 编译 (物理 KEY3=RX 让给串口, 按键由本脚本注入):
       arduino-cli compile --fqbn esp8266:esp8266:d1_mini --libraries libraries \
         --build-property "compiler.cpp.extra_flags=-DSERIAL_REMOTE=1" \
         --build-path build_remote ink-reader-esp.ino
  2) 设备已进入 阅读页 (内部介质里的书已打开);
  3) 串口空闲 (本脚本会先尝试释放占用: 见 esp_dev.release_port)。

用法:
  python p4_flip_test.py --port COM20 --count 1000 --interval 0.35
"""
import argparse
import datetime
import os
import re
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("need pyserial: pip install pyserial")
    sys.exit(2)

ANOMALY_PATTERNS = [
    ("PAGE_NEXT_RECFAIL", "页表记录读取失败(旧代码会显示第1页)"),
    ("PAGE_READ_FAIL", "正文读取失败"),
    ("PAGE_REC_OPEN_FAIL", "页表打开失败"),
    ("PAGE_REC_ZERO", "页首偏移为 0(第1页异常的直接原因)"),
    ("PAGE_REC_SHORT", "页表记录残缺"),
    ("PAGE_REC_BAD", "页表记录非法"),
    ("PROGRESS_ZERO_SKIP", "进度写入 0 被拦截"),
    ("SD_REINIT", "阅读期间出现 SD 访问(架构违规)"),
    ("reader_assert", "阅读期间 RF 未关(流程漏关)"),
    ("Fatal exception", "崩溃"),
    ("Soft WDT", "软看门狗复位"),
    ("BOOT reason", "中途重启"),
]

OK_PATTERNS = ["PAGE_NEXT from=", "RF_OFF reason=reader_enter", "SD_OFF reader_enter", "PROG_FLUSH"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM20")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--interval", type=float, default=0.35)
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    logpath = args.log or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "serial_logs",
        "p4_flip_%s.log" % datetime.datetime.now().strftime("%H%M%S"))
    os.makedirs(os.path.dirname(logpath), exist_ok=True)

    try:
        s = serial.Serial(args.port, args.baud, timeout=0.1)
    except Exception as e:
        print("open %s failed: %s" % (args.port, e))
        sys.exit(1)

    counters = {}
    pages = []
    stop = threading.Event()
    fh = open(logpath, "w", encoding="utf-8")
    fh.write("# P4 flip test  %s  count=%d interval=%.2fs port=%s\n"
             % (datetime.datetime.now().isoformat(), args.count, args.interval, args.port))
    fh.flush()

    def reader():
        buf = b""
        while not stop.is_set():
            try:
                chunk = s.read(512)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                if not text:
                    continue
                fh.write(text + "\n")
                fh.flush()
                for pat, _ in ANOMALY_PATTERNS:
                    if pat in text:
                        counters[pat] = counters.get(pat, 0) + 1
                m = re.search(r"PAGE_NEXT from=(\d+)", text)
                if m:
                    pages.append(int(m.group(1)))

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    print("P4: 注入 %d 次 K3S (间隔 %.2fs, 约 %.1f 分钟) ..." % (args.count, args.interval, args.count * args.interval / 60.0))
    t0 = time.time()
    for i in range(args.count):
        if stop.is_set():
            break
        try:
            s.write(b"K3S\n")
            s.flush()
        except Exception as e:
            print("write failed at %d: %s" % (i + 1, e))
            break
        if (i + 1) % 100 == 0:
            print("  sent %d/%d  (%.0fs)" % (i + 1, args.count, time.time() - t0))
        time.sleep(args.interval)
    time.sleep(2.0)
    stop.set()
    time.sleep(0.3)
    try:
        s.close()
    except Exception:
        pass
    fh.close()

    print("\n== 结果 ==")
    print("PAGE_NEXT 次数: %d  (首=%s 末=%s)" % (len(pages), pages[0] if pages else "-", pages[-1] if pages else "-"))
    if len(pages) >= 2:
        bad = sum(1 for a, b in zip(pages, pages[1:]) if b != a + 1)
        print("页码非 +1 递增次数: %d" % bad)
    anomalies = 0
    for pat, desc in ANOMALY_PATTERNS:
        n = counters.get(pat, 0)
        if n:
            anomalies += n
            print("  [异常] %-22s x%d  %s" % (pat, n, desc))
    if anomalies == 0:
        print("  ✅ 无异常: 无第 1 页异常 / 无 SD 访问 / 无崩溃重启")
    print("日志: %s" % logpath)
    return 1 if anomalies else 0


if __name__ == "__main__":
    sys.exit(main())

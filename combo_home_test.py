#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""组合键回首页"阅读会话收尾"验收 (全库审查 #1 修复验证)。

被测行为: 组合键(KEY2 短 → KEY3 短 ≤1s)从"阅读族"界面回首页时必须收尾阅读会话:
  chapterFreeBuffers (4.5KB) / progressFlushForce / 关 txtFile+索引句柄 / statsOnSessionEnd。
修复前只对 APP_READER 收尾 → 从 APP_CHAPTERS / APP_MARKS 回首页 4.5KB 不归还堆。

验收判据 (串口 trace 的 [heap=] 前缀):
  1) 出现 `COMBO_HOME from mode=3`(章节目录) / `mode=10`(历史标记)
  2) 出现 `COMBO_HOME close session mode=<同模式> keepBuild=0`
  3) close session 行的 heap − COMBO_HOME from 行的 heap ≥ 4096 (章节缓冲归还)

前置:
  1) 固件用 `-DSERIAL_REMOTE=1` 编译烧录 (--serial-remote):
       python esp_dev.py --serial-remote --steps build,flash
  2) 设备停在首页, 且"最近阅读"有记录 (homeSel=0 主卡可直接续读);
  3) 串口空闲 (脚本会先尝试释放占用: esp_dev.release_port)。

用法:
  python combo_home_test.py --port COM20 --scenario chapters
  python combo_home_test.py --scenario marks
  python combo_home_test.py --scenario both
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

HERE = os.path.dirname(os.path.abspath(__file__))

# 设备把 traceFmt 的换行写成字面 "\\n"(两字符) → 先还原再切行 (p4_flip_test 同款处理)
LIT_NL = "\\n"


class Monitor:
    """后台读串口: 逐行落盘 + 供 wait_for() 轮询。"""

    def __init__(self, port, baud, logpath):
        self.s = serial.Serial(port, baud, timeout=0.1)
        self.fh = open(logpath, "w", encoding="utf-8")
        self.lines = []
        self.raw = ""
        self.stop = threading.Event()
        self.th = threading.Thread(target=self._run, daemon=True)
        self.th.start()

    def _run(self):
        buf = b""
        while not self.stop.is_set():
            try:
                chunk = self.s.read(512)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                for piece in text.split(LIT_NL):
                    if piece:
                        self._emit(piece)

    def _emit(self, text):
        self.lines.append(text)
        self.fh.write(text + "\n")
        self.fh.flush()

    def wait_for(self, patterns, timeout):
        """等到任一 pattern 出现, 返回命中的 (行, pattern); 超时返回 (None, None)。"""
        t0 = time.time()
        seen = len(self.lines)
        while time.time() - t0 < timeout:
            while seen < len(self.lines):
                line = self.lines[seen]
                seen += 1
                for p in patterns:
                    if p in line:
                        return line, p
            time.sleep(0.05)
        return None, None

    def send(self, cmd, settle=0.45):
        self.s.write((cmd + "\n").encode())
        self.s.flush()
        time.sleep(settle)

    def close(self):
        self.stop.set()
        time.sleep(0.2)
        try:
            self.s.close()
        except Exception:
            pass
        self.fh.close()


HEAP_RE = re.compile(r"\[heap=(\d+)")


def heap_of(line):
    if not line:
        return None
    m = HEAP_RE.search(line)
    return int(m.group(1)) if m else None


def find_last(lines, needle):
    for line in reversed(lines):
        if needle in line:
            return line
    return None


def open_reader(mon):
    """首页主卡(最近阅读) 续读进正文。"""
    mon.send("K3L", settle=1.5)
    line, pat = mon.wait_for(["READER_SRC sd path=", "SD_OFF reader_enter", "LFS_MOUNT ok",
                              "TXT unsupported path=", "READER_SD_BUS_FAIL"], timeout=30)
    return line, pat


def enter_chapters(mon):
    """阅读页 → 菜单 → 章节(第 7 项 sel=6, 右键短按 6 次) → 右长进入。"""
    mon.send("K3L", settle=1.0)                      # 打开阅读菜单 (sel=0)
    mon.wait_for(["EVENT_MENU_OPEN"], timeout=8)
    for _ in range(6):
        mon.send("K3S", settle=0.35)
    mon.send("K3L", settle=1.5)                      # 执行"章节"
    return mon.wait_for(["CHAPTER_ENTER"], timeout=15)


def enter_marks(mon):
    """阅读页 → 菜单 → 标签(第 8 项 sel=7) → 子菜单 → 历史标记(sel=1) → 右长进入。"""
    mon.send("K3L", settle=1.0)
    mon.wait_for(["EVENT_MENU_OPEN"], timeout=8)
    for _ in range(7):
        mon.send("K3S", settle=0.35)
    mon.send("K3L", settle=1.0)                      # 打开"标签"子菜单
    mon.send("K3S", settle=0.5)                      # 光标: 标记本页 → 历史标记
    mon.send("K3L", settle=1.5)                      # 执行"历史标记"
    return mon.wait_for(["MARK_ENTER"], timeout=15)


def run_scenario(mon, name, expected_mode, opener):
    print("\n== 场景: %s ==" % name)
    results = {"ok": False}

    line, pat = open_reader(mon)
    if not line:
        print("  [FAIL] 打不开书 (最近阅读无记录?) —— 请先在设备上读一次/确认主卡有记录")
        return results
    print("  进书: %s" % line.strip()[:110])

    line, pat = opener(mon)
    if not line:
        print("  [FAIL] 未进入目标界面 (期望 trace: %s)"
              % ("CHAPTER_ENTER" if expected_mode == 3 else "MARK_ENTER"))
        return results
    print("  进界面: %s" % line.strip()[:110])
    heap_in_view = heap_of(line)

    time.sleep(1.5)                                  # 让页表/缓冲完成分配
    mon.send("B", settle=3.0)                        # 组合键: 中短 + 右短 (≤1s) → 回首页

    from_line = find_last(mon.lines, "COMBO_HOME from mode=")
    close_line = find_last(mon.lines, "COMBO_HOME close session")
    if not from_line:
        print("  [FAIL] 未捕获 COMBO_HOME from (组合键未生效? SERIAL_REMOTE 版才能注入)")
        return results
    print("  %s" % from_line.strip()[:130])
    if close_line:
        print("  %s" % close_line.strip()[:130])

    m = re.search(r"COMBO_HOME from mode=(\d+)", from_line)
    mode_ok = m and int(m.group(1)) == expected_mode
    close_ok = close_line is not None and ("mode=%d" % expected_mode) in close_line
    h_before, h_after = heap_of(from_line), heap_of(close_line)
    delta = (h_after - h_before) if (h_before and h_after) else None

    print("  mode 正确: %s | 收尾行存在: %s" % (mode_ok, close_ok))
    print("  heap: 收尾前=%s 收尾后=%s Δ=%s" % (h_before, h_after, delta))
    if heap_in_view:
        print("  (参考) 进界面时 heap=%s" % heap_in_view)

    results["ok"] = bool(mode_ok and close_ok and delta is not None and delta >= 4096)
    if results["ok"]:
        print("  ✅ PASS: 从 mode=%d 回首页已收尾, 章节缓冲 4.5KB 归还堆 (Δ=%d B)" % (expected_mode, delta))
    elif delta is not None and delta < 4096:
        print("  ❌ FAIL: 收尾后 heap 未回升 ≥4096B (Δ=%d) → 仍泄漏/未释放" % delta)
    else:
        print("  ❌ FAIL: 关键 trace 缺失")
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM20")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--scenario", default="chapters", choices=["chapters", "marks", "both"])
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    logpath = args.log or os.path.join(
        HERE, "serial_logs",
        "combo_home_%s.log" % datetime.datetime.now().strftime("%H%M%S"))
    os.makedirs(os.path.dirname(logpath), exist_ok=True)

    try:
        sys.path.insert(0, HERE)
        import esp_dev
        esp_dev.release_port(args.port)
        print("已尝试释放 %s 占用" % args.port)
    except Exception as e:
        print("(跳过端口释放: %s)" % e)

    mon = Monitor(args.port, args.baud, logpath)
    print("串口 %s 已打开, 日志: %s" % (args.port, logpath))
    time.sleep(2.0)
    boot = find_last(mon.lines, "WAKE route=") or find_last(mon.lines, "BOOT reason")
    print("启动标记: %s" % (boot.strip()[:110] if boot else "(未捕获, 继续)"))

    scenarios = []
    if args.scenario in ("chapters", "both"):
        scenarios.append(("APP_CHAPTERS (章节目录)", 3, enter_chapters))
    if args.scenario in ("marks", "both"):
        scenarios.append(("APP_MARKS (历史标记)", 10, enter_marks))

    all_ok = True
    for name, mode, opener in scenarios:
        r = run_scenario(mon, name, mode, opener)
        all_ok = all_ok and r["ok"]
        time.sleep(1.5)

    mon.close()
    print("\n==== 汇总: %s ====" % ("✅ 全部 PASS" if all_ok else "❌ 存在 FAIL"))
    print("日志: %s" % logpath)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())

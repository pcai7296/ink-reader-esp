#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""组合键回首页"阅读会话收尾"验收 (全库审查 #1 修复验证)。

被测行为: 组合键(KEY2 短 → KEY3 短 ≤1s)从"阅读族"界面回首页时必须收尾阅读会话:
  chapterFreeBuffers (4.5KB) / progressFlushForce / 关 txtFile+索引句柄 / statsOnSessionEnd。
修复前只对 APP_READER 收尾 → 从 APP_CHAPTERS / APP_MARKS 回首页 4.5KB 不归还堆。

验收判据 (串口 trace 行首的 [heap=] 前缀, 设备打印的字面 "\n" 由本脚本还原):
  1) 出现 `COMBO_HOME from mode=3`(章节目录) / `mode=10`(历史标记)
  2) 出现 `COMBO_HOME close session mode=<同模式> keepBuild=0`
  3) close session 行 heap − COMBO_HOME from 行 heap ≥ 4096 (4.5KB 章节缓冲归还堆)

前置:
  1) 固件用 SERIAL_REMOTE=1 编译烧录(物理 KEY3=RX 让给串口, 按键由本脚本注入):
       python esp_dev.py --serial-remote --build-path build_remote --steps build
       python esp_dev.py --build-path build_remote --steps flash
  2) 设备"最近阅读"有记录(主卡能续读) —— 否则先用普通固件读一次书;
  3) 串口空闲(脚本会先调 esp_dev.release_port 尝试释放占用)。

用法:
  python combo_home_test.py --scenario chapters
  python combo_home_test.py --scenario marks
  python combo_home_test.py --scenario both
"""
import argparse
import datetime
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    print("need pyserial: pip install pyserial")
    sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
LIT_NL = "\\n"   # 设备把 trace 换行写成字面 "\n"(两字符) → 先还原再切行


class Device:
    """单线程串口会话: 每一步"发送"前**重开一次句柄**。
    实测(CH340 + 长驻句柄): 设备长时间只打印不发命令时, 读管道会静默停摆 —— 后续
    设备输出与注入命令都收不到(日志停在某行不再增长), 而每次重新 open 就恢复正常。
    因此 send() 一律 close→open→write→drain, 与 remote_probe 的单次运行模型一致。"""

    def __init__(self, port, baud, logpath):
        self.port, self.baud = port, baud
        self.s = None
        self.fh = open(logpath, "w", encoding="utf-8")
        self.lines = []
        self.buf = b""
        self.reconnects = 0
        self.steps = 0
        self._open()

    def _open(self):
        try:
            self.s = serial.Serial(self.port, self.baud, timeout=0.1)
            self.buf = b""
            return True
        except Exception:
            time.sleep(1.0)
            return False

    def _reopen(self):
        if self.reconnects >= 8:
            return False
        self.reconnects += 1
        try:
            self.s.close()
        except Exception:
            pass
        time.sleep(0.3)
        if self._open():
            self._emit("[probe] serial reopen #%d" % self.reconnects)
            return True
        return False

    def _emit(self, text):
        self.lines.append(text)
        self.fh.write(text + "\n")
        self.fh.flush()

    def drain(self, seconds):
        """读 seconds 秒, 返回本次新收的行。"""
        t0 = time.time()
        new = []
        while time.time() - t0 < seconds:
            chunk = b""
            try:
                chunk = self.s.read(512)
            except Exception:
                if not self._reopen():
                    break
                continue
            if not chunk:
                continue
            self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                for piece in text.split(LIT_NL):
                    if piece:
                        self._emit(piece)
                        new.append(piece)
        return new

    def send(self, cmd, settle=0.6):
        """重开句柄 → 注入一条按键命令 → 读 settle 秒; 返回该窗口内的新行。"""
        self.steps += 1
        try:
            self.s.close()
        except Exception:
            pass
        time.sleep(0.2)
        if not self._open():
            print("  [WARN] 串口重开失败, 跳过 %s" % cmd)
            return []
        self.drain(0.15)   # 丢掉重开瞬间的陈旧残包
        try:
            self.s.write((cmd + "\n").encode())
            self.s.flush()
        except Exception:
            if not self._reopen():
                return []
            try:
                self.s.write((cmd + "\n").encode())
                self.s.flush()
            except Exception:
                return []
        return self.drain(settle)

    def wait_for(self, patterns, timeout):
        """轮询等待任一 pattern 出现; 返回 (行, pattern) 或 (None, None)。"""
        t0 = time.time()
        while time.time() - t0 < timeout:
            for line in self.drain(0.25):
                for p in patterns:
                    if p in line:
                        return line, p
        return None, None

    def close(self):
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


def goto_home(dev):
    """先按组合键强制回首页(任何界面都生效) —— 让脚本不依赖设备的初始界面。"""
    dev.send("B", settle=2.5)
    dev.drain(1.0)
    return find_last(dev.lines, "COMBO_HOME from mode=")


def open_reader(dev):
    """首页主卡(最近阅读) 续读进正文。"""
    dev.send("K3L", settle=1.5)
    return dev.wait_for(["READER_SRC sd path=", "SD_OFF reader_enter", "LFS_MOUNT ok",
                         "TXT unsupported path=", "READER_SD_BUS_FAIL"], timeout=40)


def enter_chapters(dev):
    """阅读页 → 菜单(右长) → 章节(第 7 项 sel=6, 右短 6 次) → 右长进入。"""
    dev.send("K3L", settle=1.0)
    dev.wait_for(["EVENT_MENU_OPEN"], timeout=10)
    for _ in range(6):
        dev.send("K3S", settle=0.35)
    dev.send("K3L", settle=1.5)
    return dev.wait_for(["CHAPTER_ENTER"], timeout=20)


def enter_marks(dev):
    """阅读页 → 菜单 → 标签(第 8 项 sel=7) → 子菜单 → 历史标记(sel=1) → 右长进入。"""
    dev.send("K3L", settle=1.0)
    dev.wait_for(["EVENT_MENU_OPEN"], timeout=10)
    for _ in range(7):
        dev.send("K3S", settle=0.35)
    dev.send("K3L", settle=1.0)   # 打开"标签"子菜单
    dev.send("K3S", settle=0.5)   # 光标: 标记本页 → 历史标记
    dev.send("K3L", settle=1.5)   # 执行"历史标记"
    return dev.wait_for(["MARK_ENTER"], timeout=20)


def run_scenario(dev, name, expected_mode, opener):
    print("\n== 场景: %s ==" % name)
    home_line = goto_home(dev)
    print("  起始归位: %s" % (home_line.strip()[:110] if home_line else "(未捕获, 继续)"))

    line, _ = open_reader(dev)
    if not line:
        print("  [FAIL] 打不开书 (最近阅读无记录?) —— 请先在设备上读一次/确认主卡能续读")
        return False
    print("  进书: %s" % line.strip()[:110])

    line, _ = opener(dev)
    if not line:
        print("  [FAIL] 未进入目标界面 (期望 trace: %s)"
              % ("CHAPTER_ENTER" if expected_mode == 3 else "MARK_ENTER"))
        return False
    print("  进界面: %s" % line.strip()[:110])

    time.sleep(1.5)   # 让页表/章节缓冲完成分配
    dev.send("B", settle=3.0)   # 组合键: 中短 + 右短 (≤1s) → 回首页

    from_line = find_last(dev.lines, "COMBO_HOME from mode=")
    close_line = find_last(dev.lines, "COMBO_HOME close session")
    if not from_line:
        print("  [FAIL] 未捕获 COMBO_HOME from (组合键未生效? 需 SERIAL_REMOTE 版固件)")
        return False
    print("  %s" % from_line.strip()[:130])
    if close_line:
        print("  %s" % close_line.strip()[:130])

    m = re.search(r"COMBO_HOME from mode=(\d+)", from_line)
    mode_ok = bool(m) and int(m.group(1)) == expected_mode
    close_ok = close_line is not None and ("mode=%d" % expected_mode) in close_line
    h_before, h_after = heap_of(from_line), heap_of(close_line)
    delta = (h_after - h_before) if (h_before and h_after) else None

    print("  mode 正确: %s | 收尾行存在: %s" % (mode_ok, close_ok))
    print("  heap: 收尾前=%s 收尾后=%s Δ=%s" % (h_before, h_after, delta))

    ok = bool(mode_ok and close_ok and delta is not None and delta >= 4096)
    if ok:
        print("  ✅ PASS: 从 mode=%d 回首页已收尾, 章节缓冲归还堆 (Δ=%d B ≥ 4096)" % (expected_mode, delta))
    elif delta is not None:
        print("  ❌ FAIL: 收尾后 heap 回升不足 4096B (Δ=%d) → 仍泄漏/未释放" % delta)
    else:
        print("  ❌ FAIL: 关键 trace 缺失")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM20")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--scenario", default="chapters", choices=["chapters", "marks", "both"])
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    logpath = args.log or os.path.join(
        HERE, "serial_logs", "combo_home_%s.log" % datetime.datetime.now().strftime("%H%M%S"))
    os.makedirs(os.path.dirname(logpath), exist_ok=True)

    try:
        sys.path.insert(0, HERE)
        import esp_dev
        esp_dev.release_port(args.port)
        print("已尝试释放 %s 占用" % args.port)
    except Exception as e:
        print("(跳过端口释放: %s)" % e)

    dev = Device(args.port, args.baud, logpath)
    print("串口 %s 已打开, 日志: %s" % (args.port, logpath))
    dev.drain(1.5)

    scenarios = []
    if args.scenario in ("chapters", "both"):
        scenarios.append(("APP_CHAPTERS (章节目录)", 3, enter_chapters))
    if args.scenario in ("marks", "both"):
        scenarios.append(("APP_MARKS (历史标记)", 10, enter_marks))

    all_ok = True
    for name, mode, opener in scenarios:
        all_ok = run_scenario(dev, name, mode, opener) and all_ok
        dev.drain(1.5)

    dev.close()
    print("\n==== 汇总: %s ====" % ("✅ 全部 PASS" if all_ok else "❌ 存在 FAIL"))
    print("日志: %s" % logpath)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())

# serial_listen.py — 常驻串口监听（无超时，除非终止/拔出一直监听）
# 功能：
#   1. 每次启动新建日志 log_1.txt, log_2.txt, ...（递增编号，不覆盖历史）
#   2. 断线/拔出自动重连（等待端口回来，不退出）
#   3. 端口被占用时自动检测并清理（kill 占用该 COM 口的进程）
#   4. 监听期间 DTR/RTS 低电平（不触发 D1 mini 自动复位）
# 用法: python serial_listen.py [COM4] [115200]
import os
import sys
import time
import glob
import signal
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

HERE = os.path.dirname(os.path.abspath(__file__))
LOGDIR = os.path.join(HERE, "serial_logs")
os.makedirs(LOGDIR, exist_ok=True)

def next_log():
    n = 1
    while os.path.exists(os.path.join(LOGDIR, "log_%d.txt" % n)):
        n += 1
    return os.path.join(LOGDIR, "log_%d.txt" % n)

def next_raw():
    n = 1
    while os.path.exists(os.path.join(LOGDIR, "raw_%d.bin" % n)):
        n += 1
    return os.path.join(LOGDIR, "raw_%d.bin" % n)

def kill_port_owners():
    """尝试清理占用端口的进程（Windows: 杀掉 esptool/python/监听实例）。"""
    import subprocess
    killed = []
    # 杀 python/esptool 残留（除自己）
    out = subprocess.run(["wmic", "process", "where", "name like '%python%'",
                          "get", "processid,commandline", "/format:csv"],
                         capture_output=True, text=True, timeout=10).stdout
    for line in out.splitlines():
        if not line.strip() or "CommandLine" in line:
            continue
        parts = line.split('","')
        if len(parts) < 2:
            continue
        cmd, pid = parts[0].lstrip('"'), parts[-1].rstrip('"')
        if pid.isdigit() and int(pid) != os.getpid() and 'serial_listen' not in cmd and 'esptool' in cmd.lower():
            try:
                subprocess.run(["taskkill", "/F", "/PID", pid], capture_output=True, timeout=5)
                killed.append("esptool-python(%s)" % pid)
            except Exception:
                pass
    # 杀其他 serial_listen.py 实例
    out = subprocess.run(["wmic", "process", "where", "name like '%python%'",
                          "get", "processid,commandline", "/format:csv"],
                         capture_output=True, text=True, timeout=10).stdout
    for line in out.splitlines():
        if not line.strip() or "CommandLine" in line:
            continue
        parts = line.split('","')
        if len(parts) < 2:
            continue
        cmd, pid = parts[0].lstrip('"'), parts[-1].rstrip('"')
        if pid.isdigit() and int(pid) != os.getpid() and 'serial_listen' in cmd:
            try:
                subprocess.run(["taskkill", "/F", "/PID", pid], capture_output=True, timeout=5)
                killed.append("listener(%s)" % pid)
            except Exception:
                pass
    return killed

def main():
    logfile = next_log()
    rawfile = next_raw()   # 原始字节备份（崩溃 dump 二进制被 UTF-8 解码会损坏; raw 保留完整）
    print("LISTEN %s @%d -> %s (raw=%s)" % (PORT, BAUD, logfile, rawfile))
    port = None
    clean_attempts = 0
    buf = b""
    raw = open(rawfile, "ab")   # 追加原始字节
    while True:
        try:
            if port is None or not port.is_open:
                try:
                    port = serial.Serial(PORT, BAUD, timeout=0.5)
                    port.dtr = False
                    port.rts = False
                    clean_attempts = 0
                    with open(logfile, "a", encoding="utf-8") as f:
                        f.write("=== %s PORT OPEN %s @%d ===\n" %
                                (time.strftime("%Y-%m-%d %H:%M:%S"), PORT, BAUD))
                except serial.SerialException:
                    if clean_attempts < 3:
                        clean_attempts += 1
                        try:
                            killed = kill_port_owners()
                        except Exception:
                            killed = []
                        with open(logfile, "a", encoding="utf-8") as f:
                            f.write("=== %s PORT BUSY (attempt %d); killed: %s ===\n" %
                                    (time.strftime("%Y-%m-%d %H:%M:%S"), clean_attempts, ",".join(killed)))
                    else:
                        with open(logfile, "a", encoding="utf-8") as f:
                            f.write("=== %s PORT BUSY, waiting... ===\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
                    time.sleep(2)
                    continue
            n = port.in_waiting
            if n > 0:
                data = port.read(n)
                raw.write(data)   # 保原始字节（崩溃 dump 不损坏）
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.rstrip(b"\r").decode("utf-8", errors="replace")
                    if line:
                        with open(logfile, "a", encoding="utf-8") as f:
                            f.write(line + "\n")
            time.sleep(0.05)
        except serial.SerialException:
            try:
                if port:
                    port.close()
            except Exception:
                pass
            port = None
            with open(logfile, "a", encoding="utf-8") as f:
                f.write("=== %s PORT LOST, waiting... ===\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
            time.sleep(2)
        except KeyboardInterrupt:
            break

if __name__ == "__main__":
    main()

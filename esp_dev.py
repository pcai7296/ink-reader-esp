#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
esp_dev.py — ink-reader-esp 一体式开发工具: 条件编译 → 烧录 → 串口监听
=====================================================================
特性:
  1) 自动解除其它进程对串口的占用 (按"进程命令行含端口号 + 可执行名 python/pythonw/esptool"
     过滤, 排除自身与祖先进程, 用 taskkill 强杀后重试)
  2) 条件编译参数: --define KEY=VALUE / --define KEY (经 compiler.cpp.extra_flags=-D… 传入 arduino-cli)
     快捷开关: --boot-ap (=BOOT_AP_MODE=1) / --serial-remote (=SERIAL_REMOTE=1)
  3) 烧录前/监听前自动释放串口; 监听断线自动重连并继续输出(不丢后续日志)
  4) 每次启动监听日志自动递增: serial_logs/log_1.txt, log_2.txt, … (目录内自动取最小空缺序号)
  5) --dry-run 只打印将执行的命令, 不产生副作用

用法示例:
  python esp_dev.py                              # 全流程: 编译(无额外宏)+烧录+监听(前台常驻)
  python esp_dev.py --boot-ap --steps all        # 编译 BOOT_AP_MODE=1 + 烧录 + 监听
  python esp_dev.py --define SERIAL_REMOTE=1 --define FOO --steps build
  python esp_dev.py --steps monitor --seconds 8  # 只监听 8 秒(自测用)
  python esp_dev.py --dry-run --define BOOT_AP_MODE=1   # 打印命令不执行
"""
import argparse
import datetime
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    serial = None

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SKETCH = "ink-reader-esp.ino"
DEFAULT_FQBN = "esp8266:esp8266:d1_mini"
KILL_NAMES = {"python.exe", "pythonw.exe", "python3.exe", "esptool.exe", "esptool"}
CMD_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def log(msg):
    print("[esp_dev] " + msg, flush=True)


def run_cmd(cmd, verbose=True):
    if verbose:
        log("CMD> " + " ".join('"%s"' % c if (" " in c or c == "") else c for c in cmd))
    return subprocess.run(cmd)


def get_processes():
    """Windows: 取全部进程的 PID/PPID/Name/CommandLine (JSON)。"""
    script = (
        "Get-CimInstance Win32_Process | "
        "Select-Object ProcessId,ParentProcessId,Name,CommandLine | "
        "ConvertTo-Json -Compress"
    )
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
            capture_output=True, text=True, timeout=30,
        )
        if r.returncode != 0:
            return []
        data = json.loads(r.stdout or "[]")
        if isinstance(data, dict):
            data = [data]
        return data
    except Exception:
        return []


def ancestor_pids(pid):
    rows = get_processes()
    by_pid = {int(p.get("ProcessId", 0)): int(p.get("ParentProcessId", 0)) for p in rows}
    out = set()
    cur = pid
    for _ in range(32):
        if cur in by_pid and by_pid[cur] and by_pid[cur] != cur:
            out.add(by_pid[cur])
            cur = by_pid[cur]
        else:
            break
    return out


def release_port(port, verbose=True):
    """强杀命令行里带 <port> 的 python/pythonw/esptool 进程(排除自身与祖先)。返回被杀 PID 列表。"""
    token = port.upper()
    me = os.getpid()
    anc = ancestor_pids(me)
    rows = get_processes()
    targets = []
    for p in rows:
        try:
            pid = int(p.get("ProcessId", 0))
        except (TypeError, ValueError):
            continue
        if pid == me or pid in anc:
            continue
        name = (p.get("Name") or "").lower()
        cl = p.get("CommandLine") or ""
        if name in KILL_NAMES and token in cl.upper():
            targets.append((pid, p.get("Name") or "?", cl[:160]))
    killed = []
    for pid, name, cl in targets:
        if verbose:
            log("release: kill %s pid=%d (%s)" % (name, pid, cl))
        try:
            subprocess.run(["taskkill", "/PID", str(pid), "/F"],
                           capture_output=True, timeout=15)
            killed.append(pid)
        except Exception:
            pass
    return killed


def ensure_free(port, verbose=True):
    """尝试探测端口是否空闲(可打开); 忙则杀占用者后重试一次。返回 True=空闲。"""
    if serial is None:
        return True
    for attempt in (1, 2):
        try:
            s = serial.Serial(port, 115200, timeout=0.1)
            s.close()
            return True
        except Exception:
            if attempt == 1:
                if verbose:
                    log("port %s busy -> releasing holders" % port)
                release_port(port, verbose)
                time.sleep(1.0)
    return False


def next_log_file(log_dir):
    os.makedirs(log_dir, exist_ok=True)
    used = set()
    for f in glob.glob(os.path.join(log_dir, "log_*.txt")):
        m = re.search(r"log_(\d+)\.txt$", os.path.basename(f))
        if m:
            used.add(int(m.group(1)))
    n = 1
    while n in used:
        n += 1
    return os.path.join(log_dir, "log_%d.txt" % n)


def monitor(port, baud, logfile, seconds=None, do_release=True, verbose=True, build_note=""):
    if serial is None:
        log("ERROR: pyserial 未安装 (pip install pyserial)")
        return 2
    log("LOG  -> %s" % logfile)
    lines_written = 0
    with open(logfile, "w", encoding="utf-8") as fh:
        fh.write("# ink-reader-esp serial log\n")
        fh.write("# started : %s\n" % datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
        fh.write("# port    : %s @ %d\n" % (port, baud))
        fh.write("# build   : %s\n" % (build_note or "n/a"))
        fh.write("# cmdline : %s\n\n" % " ".join(sys.argv))
        fh.flush()

        start = time.time()
        while True:
            try:
                s = serial.Serial(port, baud, timeout=0.2)
                log("monitor %s @ %d started" % (port, baud))
                break
            except Exception as e:
                if do_release:
                    release_port(port, verbose)
                if seconds is not None and time.time() - start >= seconds:
                    return 0
                marker = "[%s] device offline (%s), retry in 3s..." % (
                    datetime.datetime.now().strftime("%H:%M:%S"), e)
                print(marker, flush=True)
                fh.write(marker + "\n")
                fh.flush()
                time.sleep(3)

        buf = b""
        try:
            while True:
                if seconds is not None and time.time() - start >= seconds:
                    break
                try:
                    chunk = s.read(512)
                except Exception:
                    chunk = None
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        text = line.decode("utf-8", "replace").rstrip("\r")
                        if text:
                            print(text, flush=True)
                            fh.write(text + "\n")
                            fh.flush()
                            lines_written += 1
                else:
                    time.sleep(0.02)
        except KeyboardInterrupt:
            log("user interrupt")
        finally:
            try:
                s.close()
            except Exception:
                pass
            if seconds is not None:
                log("auto-stop after %ss (lines=%d)" % (seconds, lines_written))
    return 0


def build(project, fqbn, libraries, build_path, sketch, defines, verbose=True, dry=False):
    cmd = ["arduino-cli", "compile", "--fqbn", fqbn]
    if libraries:
        cmd += ["--libraries", libraries]
    if defines:
        flags = " ".join(defines)
        cmd += ["--build-property", "compiler.cpp.extra_flags=%s" % flags]
    cmd += ["--build-path", build_path]
    cmd += [os.path.join(project, sketch) if not os.path.isabs(sketch) else sketch]
    if dry:
        log("DRY> " + " ".join('"%s"' % c if " " in c else c for c in cmd))
        return 0, os.path.join(build_path, os.path.splitext(sketch)[0] + ".ino.bin")
    if shutil.which("arduino-cli") is None:
        log("ERROR: arduino-cli not found in PATH")
        return 1, ""
    r = run_cmd(cmd, verbose)
    if r.returncode != 0:
        log("BUILD FAILED (exit=%d)" % r.returncode)
        return r.returncode, ""
    bin_path = os.path.join(build_path, os.path.splitext(os.path.basename(sketch))[0] + ".ino.bin")
    if not os.path.exists(bin_path):
        # 兜底: 在 build 目录内按 *ino.bin 找最近产物
        cands = sorted(glob.glob(os.path.join(build_path, "*.ino.bin")),
                       key=os.path.getmtime, reverse=True)
        bin_path = cands[0] if cands else ""
    if bin_path:
        log("BIN  -> %s (%d B, %s)" % (bin_path, os.path.getsize(bin_path),
                                        datetime.datetime.fromtimestamp(
                                            os.path.getmtime(bin_path)).strftime("%H:%M:%S")))
    return 0, bin_path


def find_esptool():
    for name in ("esptool.exe", "esptool", "esptool.py"):
        p = shutil.which(name)
        if p:
            return p
    return "esptool.exe"


def flash(port, bin_path, fs_items, baud=460800, do_release=True, verbose=True, dry=False):
    if not dry and not os.path.exists(bin_path):
        log("ERROR: bin not found: %s (先 build?)" % bin_path)
        return 1
    if not dry and do_release and not ensure_free(port, verbose):
        log("ERROR: port %s still busy after release" % port)
        return 1
    cmd = [find_esptool(), "--port", port, "--baud", str(baud), "write-flash"]
    cmd += ["0x0", bin_path]
    for addr, f in fs_items:
        cmd += [addr, f]
    if dry:
        log("DRY> " + " ".join('"%s"' % c if " " in c else c for c in cmd))
        return 0
    r = run_cmd(cmd, verbose)
    if r.returncode != 0:
        log("FLASH FAILED (exit=%d)" % r.returncode)
    return r.returncode


def main():
    ap = argparse.ArgumentParser(
        description="ink-reader-esp 一体式: 条件编译+烧录+串口监听",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("--steps", choices=("all", "build", "flash", "monitor"), default="all")
    ap.add_argument("-D", "--define", action="append", default=[], metavar="KEY[=VALUE]",
                    help="条件编译宏(可多次): BOOT_AP_MODE=1 / SERIAL_REMOTE")
    ap.add_argument("--boot-ap", action="store_true", help="快捷: -D BOOT_AP_MODE=1")
    ap.add_argument("--serial-remote", action="store_true", help="快捷: -D SERIAL_REMOTE=1")
    ap.add_argument("--port", default="COM20")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--flash-baud", type=int, default=460800)
    ap.add_argument("--fqbn", default=DEFAULT_FQBN)
    ap.add_argument("--libraries", default=os.path.join(HERE, "libraries"))
    ap.add_argument("--build-path", default=os.path.join(HERE, "build"))
    ap.add_argument("--sketch", default=os.path.join(HERE, DEFAULT_SKETCH))
    ap.add_argument("--log-dir", default=os.path.join(HERE, "serial_logs"))
    ap.add_argument("--seconds", type=float, default=None,
                    help="监听 N 秒后自动结束(测试用); 缺省 = 前台常驻直到 Ctrl+C")
    ap.add_argument("--fs", action="append", default=[], metavar="ADDR=PATH",
                    help="额外烧录文件, 如 0x200000=build/data.littlefs.bin (可多次)")
    ap.add_argument("--no-release", action="store_true", help="不自动解除串口占用")
    ap.add_argument("--dry-run", action="store_true", help="只打印命令不执行")
    args = ap.parse_args()

    defines = list(args.define)
    if args.boot_ap:
        defines.append("BOOT_AP_MODE=1")
    if args.serial_remote:
        defines.append("SERIAL_REMOTE=1")
    # 合法性校验 + 统一格式 -DNAME 或 -DNAME=VALUE
    norm = []
    for d in defines:
        if "=" in d:
            k, v = d.split("=", 1)
            if not CMD_NAME_RE.match(k) or not re.match(r"^[A-Za-z0-9_\-\.]*$", v):
                log("ERROR: 非法宏定义: %s" % d)
                return 2
            norm.append("-D%s=%s" % (k, v))
        else:
            if not CMD_NAME_RE.match(d):
                log("ERROR: 非法宏名: %s" % d)
                return 2
            norm.append("-D%s" % d)

    fs_items = []
    for item in args.fs:
        if "=" not in item:
            log("ERROR: --fs 需 ADDR=PATH 格式: %s" % item)
            return 2
        addr, path = item.split("=", 1)
        fs_items.append((addr, path))

    dry = args.dry_run
    rc = 0
    build_note = ""
    bin_path = ""

    if args.steps in ("all", "build"):
        rc, bin_path = build(HERE, args.fqbn, args.libraries, args.build_path,
                             args.sketch, norm, dry=dry)
        build_note = "fqbn=%s flags=[%s] bin=%s" % (
            args.fqbn, " ".join(norm) if norm else "-",
            os.path.basename(bin_path) if bin_path else "n/a")
        if rc != 0 and not dry:
            return rc

    if args.steps in ("all", "flash"):
        if not dry and not bin_path:
            # 单独 --steps flash: 直接使用 build 目录最新 bin
            cands = sorted(glob.glob(os.path.join(args.build_path, "*.ino.bin")),
                           key=os.path.getmtime, reverse=True)
            bin_path = cands[0] if cands else ""
        if not dry and not bin_path:
            log("ERROR: 找不到 bin (先执行 build)")
            return 1
        rc = flash(args.port, bin_path, fs_items, args.flash_baud,
                   do_release=not args.no_release, dry=dry)
        if rc != 0 and not dry:
            return rc

    if args.steps in ("all", "monitor"):
        if dry:
            log("DRY> monitor %s @ %d -> %s" % (args.port, args.baud,
                                                next_log_file(args.log_dir)))
            return 0
        logfile = next_log_file(args.log_dir)
        return monitor(args.port, args.baud, logfile, seconds=args.seconds,
                       do_release=not args.no_release, build_note=build_note)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rodata_audit.py — ESP8266 (Arduino) 静态内存审计 v2
====================================================
替代已丢失的旧 map-解析版（旧版逐段解析 GNU map 输入行，格式脆、易漏 2 行式条目）。
新口径（更精确、可复现）:
  * ELF 级权威总量:  readelf -S -W 按 VMA 分类 DRAM 区段 (.data/.noinit/.rodata/.bss)
  * 逐文件账本:      对 build/<sketch>/ 下每个 .o 单独 readelf, 统计 rodata/bss/data
  * 大符号榜:        nm -S --size-sort 取具名 BSS/data/rodata 符号
用法:
  python rodata_audit.py <build_dir> [--save <out.json>]
  # build_dir 内含 *.ino.elf 与 sketch/*.o
输出口径与 docs/mem_profile_2026-09.md 一致:
  RAM 总量 80,192 (0x13940, DRAM 基址 0x3FFE8000)
  静态已用 = .data+.noinit+.rodata+.bss (DRAM 全部 ALLOC 区段)
  堆顶 ≈ RAM 总量 − 静态结束偏移(含对齐)   (项目沿用约定: 80,192 − 静态已用 ≈ 可用堆上限)
"""
import glob
import json
import os
import re
import subprocess
import sys

RAM_BASE = 0x3FFE8000
RAM_TOTAL = 0x13940          # 80,192
IRAM_BASE = 0x40100000
IROM_BASE = 0x40200000
DRAM_END = RAM_BASE + RAM_TOTAL

SECTION_RE = re.compile(
    r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+"
    r"[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]*\s*(\S*)", re.IGNORECASE)


def find_toolchain():
    env = os.environ.get("XTENSA_BIN")
    if env and os.path.isdir(env):
        return env
    cands = []
    for pat in (
        r"J:\Arduino15\packages\esp8266\tools\xtensa-lx106-elf-gcc\*\bin",
        r"C:\Users\Administrator\.platformio\packages\toolchain-xtensa\bin",
        r"C:\Users\Administrator\AppData\Local\Arduino15\packages\esp8266\tools\xtensa-lx106-elf-gcc\*\bin",
    ):
        cands += glob.glob(pat)
    for d in cands:
        if os.path.isfile(os.path.join(d, "xtensa-lx106-elf-readelf.exe")):
            return d
    return ""


def run(tool, args):
    p = subprocess.run([tool] + args, capture_output=True, text=True)
    return p.stdout


def read_sections(path, toolbin):
    """返回 {name: (addr, size, typ)} 及内存分类总和。"""
    out = run(os.path.join(toolbin, "xtensa-lx106-elf-readelf.exe"), ["-S", "-W", path])
    secs = {}
    for line in out.splitlines():
        m = SECTION_RE.match(line)
        if not m:
            continue
        name, typ, addr_s, size_s = m.group(1), m.group(2), m.group(3), m.group(4)
        addr = int(addr_s, 16)
        size = int(size_s, 16)
        if typ not in ("PROGBITS", "NOBITS"):
            continue
        secs[name] = (addr, size, typ)
    return secs


def classify(secs):
    dram = {}   # name -> size
    for name, (addr, size, _typ) in secs.items():
        if size == 0:
            continue
        if RAM_BASE <= addr < DRAM_END:
            dram[name] = dram.get(name, 0) + size
    s_data = dram.get(".data", 0)
    s_noinit = dram.get(".noinit", 0)
    s_rodata = dram.get(".rodata", 0)
    s_bss = dram.get(".bss", 0)
    others = {k: v for k, v in dram.items()
              if k not in (".data", ".noinit", ".rodata", ".bss")}
    s_other = sum(others.values())
    s_static = s_data + s_noinit + s_rodata + s_bss + s_other
    # 静态结束偏移(含对齐) = DRAM 内最大 end − 基址
    max_end = max((a + s) for n, (a, s, _) in secs.items()
                  if RAM_BASE <= a < DRAM_END and s > 0)
    heap_ceiling = RAM_TOTAL - (max_end - RAM_BASE)
    return {
        "dram": dram,
        "data": s_data, "noinit": s_noinit, "rodata": s_rodata,
        "bss": s_bss, "other": s_other, "other_detail": others,
        "static_total": s_static,
        "heap_ceiling": heap_ceiling,
        "max_static_end_off": max_end - RAM_BASE,
    }


def audit_elf(elf, toolbin):
    c = classify(read_sections(elf, toolbin))
    return c


def audit_object_files(obj_dir, toolbin):
    """逐 .o: name -> {rodata,bss,data,noinit}
    注意: 可重定位 .o 的所有段地址都是 0, 不能按地址分类 —— 只能按段名计数。"""
    def sum_by_name(secs, prefix):
        return sum(s for n, (_, s, _t) in secs.items() if n == prefix or n.startswith(prefix))
    rows = {}
    for o in sorted(glob.glob(os.path.join(obj_dir, "*.o"))):
        base = os.path.basename(o)
        # 去掉 .cpp.o / .c.o / .ino.cpp.o → 源名
        src = re.sub(r"\.(cpp|ino\.cpp|c)\.o$", "", base)
        if src.endswith(".ino"):
            src = "ink-reader-esp.ino"
        secs = read_sections(o, toolbin)
        r = sum_by_name(secs, ".rodata")
        b = sum_by_name(secs, ".bss")
        d = sum_by_name(secs, ".data")
        ni = sum_by_name(secs, ".noinit")
        rows[src] = {"rodata": r, "bss": b, "data": d, "noinit": ni,
                     "static": r + b + d + ni}
    return rows


def top_symbols(elf, toolbin, kind, limit=40):
    """kind: 'b' BSS / 'd' data / 'r' rodata (具名符号)"""
    nm = os.path.join(toolbin, "xtensa-lx106-elf-nm.exe")
    out = run(nm, ["-S", "--size-sort", elf])
    want = kind.upper()
    rows = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        try:
            size = int(parts[1], 16)
            typ = parts[2]
            sym = " ".join(parts[3:])
        except ValueError:
            continue
        # 'b'=局部(file-scope static) BSS, 'B'=全局; data/rodata 同理, 两种都要
        if typ not in (want, want.lower()):
            continue
        if sym.startswith("__") or sym in ("_end", "_heap_start"):
            continue
        rows.append((size, sym))
    rows.sort(reverse=True)
    return rows[:limit]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    build_dir = sys.argv[1]
    toolbin = find_toolchain()
    if not toolbin:
        print("ERROR: xtensa 工具链未找到 (设 XTENSA_BIN 指向 bin 目录)")
        sys.exit(2)
    elfs = [e for e in glob.glob(os.path.join(build_dir, "*.elf"))
            if not e.endswith(".o")]
    if not elfs:
        print("ERROR: build 目录无 .elf:", build_dir)
        sys.exit(2)
    elf = elfs[0]
    obj_dir = os.path.join(build_dir, "sketch")

    elf_audit = audit_elf(elf, toolbin)
    objs = audit_object_files(obj_dir, toolbin) if os.path.isdir(obj_dir) else {}
    t_bss = top_symbols(elf, toolbin, "b")
    t_data = top_symbols(elf, toolbin, "d")
    t_rodata = top_symbols(elf, toolbin, "r")

    def fmt_table(title, rows, headers):
        print("\n== %s ==" % title)
        if not rows:
            print("  (空)")
            return
        w = [max(len(str(r[i])) for r in rows + [headers]) for i in range(len(headers))]
        print("  " + "  ".join(str(h).ljust(w[i]) for i, h in enumerate(headers)))
        for r in rows:
            print("  " + "  ".join(str(r[i]).ljust(w[i]) for i in range(len(headers))))

    print("ELF   : %s" % elf)
    print("工具链: %s" % toolbin)
    print("\n== RAM 总账 (ELF readelf -S, DRAM 0x3FFE8000..+0x13940) ==")
    c = elf_audit
    print("  RAM 总量        : %d (0x%x)" % (RAM_TOTAL, RAM_TOTAL))
    print("  .data           : %d" % c["data"])
    print("  .noinit         : %d" % c["noinit"])
    print("  .rodata (RAM)   : %d" % c["rodata"])
    print("  .bss            : %d" % c["bss"])
    print("  其他 DRAM 区段  : %d %s" % (c["other"], c["other_detail"] if c["other"] else ""))
    print("  ---- 静态已用(全部) = %d  (%.1f%%) ----" % (c["static_total"],
                                                        100.0 * c["static_total"] / RAM_TOTAL))
    print("  静态结束偏移     : 0x%x (=heap_start−0x3FFE8000)" % c["max_static_end_off"])
    print("  堆顶(可用堆上限) ≈ %d  (RAM总量−静态结束)" % c["heap_ceiling"])
    print("  注: 项目旧口径 80,192−静态 与其一致(差 4B 对齐); 实际 freeHeap 另减栈/lwIP 运行分配")

    objs_rows = sorted(
        ((v["rodata"] + v["bss"] + v["data"] + v["noinit"], k, v) for k, v in objs.items()),
        reverse=True)
    fmt_table("逐源文件静态占用 (sketch 目录 .o)", 
              [(v["rodata"], v["bss"], v["data"], s) for s, k, v in objs_rows],
              ["源文件(rodata/bss/data/合计)"])
    print("  明细: [rodata, bss, data, static] = %s" %
          {k: [v["rodata"], v["bss"], v["data"], v["static"]] for _, k, v in objs_rows})
    fmt_table("BSS 具名符号 Top %d" % len(t_bss),
              [(sz, s) for sz, s in t_bss], ["size", "symbol"])
    fmt_table("DATA 具名符号 Top", [(sz, s) for sz, s in t_data[:12]], ["size", "symbol"])
    fmt_table("RODATA 具名符号 Top", [(sz, s) for sz, s in t_rodata[:12]], ["size", "symbol"])

    if "--save" in sys.argv:
        idx = sys.argv.index("--save")
        with open(sys.argv[idx + 1], "w", encoding="utf-8") as f:
            json.dump({"elf": elf,
                       "ram": {k: c[k] for k in
                               ("data", "noinit", "rodata", "bss", "other",
                                "static_total", "heap_ceiling", "max_static_end_off")},
                       "per_file": {k: v for _, k, v in objs_rows},
                       "top_bss": t_bss}, f, ensure_ascii=False, indent=1)
        print("\nsaved -> %s" % sys.argv[idx + 1])


if __name__ == "__main__":
    main()

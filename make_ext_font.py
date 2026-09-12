#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_ext_font.py — 把电脑上的 TTF/OTF/TTC 字体转成墨水屏用的 .bin 外部字库
（放到 SD 卡根目录，固件"阅读菜单 → 字体选择 → 外部"即可使用）

设计约束（与固件渲染链路对齐，2026-09-12）：
  * 正文渲染走 u8g2 `chinese_gb2312`，其 CJK 字格是 **16×16、步进 16px**；外部字库必须保持
    **同样的字格与步进**，排版/分页索引才不会错位 → 所以 v1 只替换 **CJK 字形**，
    ASCII 仍由内置字体绘制（ASCII 宽度表被排版索引依赖，换掉会错行）。
  * 1bpp（黑/白），每行 MSB-left，行内按字节对齐：字节数 = ceil(w/8) * h。

文件格式（小端；EFNT v1）：
  偏移 0   4B  magic 'E','F','N','T'
  4        1B  version = 1
  5        1B  保留 = 0
  6        1B  cellW   （CJK 字格宽, 默认 16）
  7        1B  cellH   （字格高, 默认 16）
  8        1B  blockCount
  9..11    3B  保留 = 0
  12       块表：每块 16B
              u32 startCp   起始码点（含）
              u32 endCp     结束码点（含）
              u32 dataOff   该块位图数据起始偏移（文件绝对偏移）
              u8  w, u8 h   该块字格宽高
              u16 保留 = 0
  12+16*N  位图区：块内按码点升序，每字形 ceil(w/8)*h 字节，MSB-left

用法：
  python make_ext_font.py msyh.ttc -o font.bin                  # 默认: 全部 CJK 统一汉字
  python make_ext_font.py simhei.ttf -o font.bin --ranges cjk,punct,fullwidth
  python make_ext_font.py msyh.ttc -o font.bin --index 0        # TTC 选第 0 个子字体
  python make_ext_font.py msyh.ttc -o font.bin --preview        # 额外打印样例字形的点阵(文本核对)
  python make_ext_font.py --dump font.bin 武炼巅峰               # 反解 .bin 里这些字的点阵(自查用)
"""

import argparse
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("需要 Pillow:  pip install pillow", file=sys.stderr)
    raise SystemExit(2)

MAGIC = b"EFNT"
VERSION = 1

# 预设码点范围（**必须是连续区间**：固件端用 (cp - start) 直接算偏移 = O(1) 且零 RAM 索引；
# 稀疏码点集（如 GB2312 收录表）会让下标错位，v1 不支持 —— 需要小体积就用更小的字格或裁范围）
PRESETS = {
    "cjk": (0x4E00, 0x9FA5),        # CJK 统一汉字 20902 字 ≈ 654KB @16x16
    "punct": (0x3000, 0x303F),      # CJK 符号和标点
    "fullwidth": (0xFF00, 0xFFEF),  # 全角字符
    "ascii": (0x0020, 0x007E),      # ASCII（v1 不参与渲染，仅供核对/未来使用）
}


def cell_bytes(w, h):
    return ((w + 7) // 8) * h


def render_glyph(font, ch, w, h, thr=128):
    """把单字渲染到 w×h 的 1bpp 位图（按字形包围盒居中，兼容不同 TTF 的基线差异）"""
    img = Image.new("L", (w + 4, h + 4), 0)     # 留 2px 余量便于居中
    d = ImageDraw.Draw(img)
    try:
        bbox = font.getbbox(ch)
    except Exception:
        bbox = None
    x = y = 2
    if bbox:
        gw, gh = bbox[2] - bbox[0], bbox[3] - bbox[1]
        x = 2 - bbox[0] + max(0, (w - gw) // 2)
        y = 2 - bbox[1] + max(0, (h - gh) // 2)
    d.text((x, y), ch, fill=255, font=font)
    crop = img.crop((2, 2, 2 + w, 2 + h))
    px = crop.load()
    rows = []
    for ry in range(h):
        value = 0
        for rx in range(w):
            value = (value << 1) | (1 if px[rx, ry] >= thr else 0)
        for _ in range((w + 7) // 8):
            rows.append((value >> (8 * ((w + 7) // 8 - 1 - _))) & 0xFF)
    return bytes(rows)


def glyph_is_blank(data):
    return all(b == 0 for b in data)


def build(font_path, out_path, index, cell_w, cell_h, ranges, thr, preview, ascii_art=()):
    font = ImageFont.truetype(font_path, cell_h, index=index)

    blocks = []          # [(start, end, w, h, [glyph bytes...], missing)]
    for name in ranges:
        start, end = PRESETS[name]
        cpset = None
        glyphs, missing = [], 0
        for cp in range(start, end + 1):
            if cpset is not None and cp not in cpset:
                continue
            try:
                ch = chr(cp)
            except ValueError:
                continue
            data = render_glyph(font, ch, cell_w, cell_h, thr)
            if glyph_is_blank(data):
                missing += 1
            glyphs.append(data)
        if glyphs:
            blocks.append([start, end, cell_w, cell_h, glyphs, missing, name])
            print(f"  块 {name:<10} U+{start:04X}..U+{end:04X}  字形 {len(glyphs):>6}  空/缺字 {missing:>5}")

    if not blocks:
        print("没有生成任何字形（范围为空？）", file=sys.stderr)
        return 1

    # 计算偏移并写文件
    header_len = 12 + 16 * len(blocks)
    off = header_len
    with open(out_path, "wb") as f:
        f.write(MAGIC + bytes([VERSION, 0, cell_w, cell_h, len(blocks), 0, 0, 0]))
        for start, end, w, h, glyphs, _missing, _name in blocks:
            f.write(struct.pack("<IIIBBH", start, end, off, w, h, 0))
            off += len(glyphs) * cell_bytes(w, h)
        for start, end, w, h, glyphs, _missing, _name in blocks:
            for g in glyphs:
                f.write(g)

    # 自检：回读校验块声明与实际数据一致（防止"声明连续、数据稀疏"这类错位）
    with open(out_path, "rb") as f:
        chk = f.read()
    for i, (start, end, w, h, glyphs, _m, name) in enumerate(blocks):
        s2, e2, off2, w2, h2, _x = struct.unpack_from("<IIIBBH", chk, 12 + 16 * i)
        assert (s2, e2, w2, h2) == (start, end, w, h), f"块{i} 头部回读不一致"
        need = (end - start + 1) * cell_bytes(w, h)
        have = len(chk) - off2
        assert have >= need, f"块{i} 数据不足: 需要 {need} 实际 {have}"
    first = blocks[0][4][ord("一") - blocks[0][0]] if blocks[0][0] <= ord("一") <= blocks[0][1] else None
    if first is not None:
        assert not glyph_is_blank(first), "自检: 「一」渲染为空, 字体可能没加载成功"
    total = sum(len(b[4]) for b in blocks)
    size = os.path.getsize(out_path)
    print(f"[OK] {out_path}  字形 {total}  文件 {size} B ({size/1024:.1f} KB)  "
          f"字格 {cell_w}x{cell_h}  来源 {os.path.basename(font_path)}#{index}")

    if preview and ascii_art:
        print("\n--- 样例点阵（文本核对）---")
        dump_blocks(blocks, ascii_art)
    return 0


def dump_blocks(blocks, chars):
    for ch in chars:
        cp = ord(ch)
        for start, end, w, h, glyphs, _m, name in blocks:
            idx = cp - start
            if 0 <= idx < len(glyphs):
                print(f"\n[{ch}] U+{cp:04X}  块={name}  字格={w}x{h}")
                data = glyphs[idx]
                stride = (w + 7) // 8
                for y in range(h):
                    row = data[y * stride:(y + 1) * stride]
                    bits = "".join("".join("#" if (row[i] >> (7 - b)) & 1 else "."
                                           for b in range(8)) for i in range(stride))
                    print("   " + bits[:w])
                break


def dump_file(path, chars):
    """反解 .bin（用于验证固件将要读到的东西）"""
    raw = open(path, "rb").read()
    if raw[:4] != MAGIC:
        print("magic 不对", file=sys.stderr)
        return 1
    ver, _r, cw, chh, nblocks = raw[4], raw[5], raw[6], raw[7], raw[8]
    print(f"version={ver} cell={cw}x{chh} blocks={nblocks} size={len(raw)}B")
    blocks = []
    for i in range(nblocks):
        start, end, off, w, h, _x = struct.unpack_from("<IIIBBH", raw, 12 + 16 * i)
        gb = cell_bytes(w, h)
        glyphs = [raw[off + k * gb: off + (k + 1) * gb] for k in range(end - start + 1)]
        blocks.append((start, end, w, h, glyphs, 0, f"块{i}"))
        print(f"  块{i}: U+{start:04X}..U+{end:04X} gb={gb}B off={off}")
    dump_blocks(blocks, chars)
    return 0


def main():
    ap = argparse.ArgumentParser(description="TTF → 墨水屏外部字库 .bin")
    ap.add_argument("ttf", nargs="?", help="源字体文件 (.ttf/.otf/.ttc)")
    ap.add_argument("-o", "--out", help="输出 .bin 路径")
    ap.add_argument("--index", type=int, default=0, help="TTC 子字体序号 (默认 0)")
    ap.add_argument("--cell", type=int, default=16, help="字格边长 (默认 16, 必须与固件一致)")
    ap.add_argument("--ranges", default="cjk", help="逗号分隔: cjk,punct,fullwidth,ascii (默认 cjk; 必须是连续区间)")
    ap.add_argument("--threshold", type=int, default=128, help="二值化阈值 (默认 128)")
    ap.add_argument("--preview", action="store_true", help="打印样例字形点阵")
    ap.add_argument("--dump", metavar="BIN", help="反解已有 .bin")
    ap.add_argument("chars", nargs="*", help="配合 --dump 指定要打印的字")
    a = ap.parse_args()

    if a.dump:
        chars = a.chars or ["武", "炼", "巅", "峰"]
        return dump_file(a.dump, chars)
    if not a.ttf or not a.out:
        ap.print_help()
        return 2

    ranges = [r.strip() for r in a.ranges.split(",") if r.strip()]
    for r in ranges:
        if r not in PRESETS:
            print(f"未知范围 {r}（可选: {', '.join(PRESETS)}）", file=sys.stderr)
            return 2
    print(f"源字体 {a.ttf}  字格 {a.cell}x{a.cell}  范围 {ranges}")
    return build(a.ttf, a.out, a.index, a.cell, a.cell, ranges, a.threshold,
                 a.preview, ("武", "炼", "巅", "A", "，"))


if __name__ == "__main__":
    raise SystemExit(main())

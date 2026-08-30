# -*- coding: utf-8 -*-
"""
make_font_cn.py - 生成 GB2312 全字库 16x16 中文位图字体头文件
输出: font16_cn.h
  - font16_cn_uni[]: 按 Unicode 升序排列的汉字码 (PROGMEM, u16)
  - font16_cn_bmp[][32]: 对应位图, 每字 16 行 x 2 字节, MSB 在前
用法: python make_font_cn.py
"""
import sys
import os
from PIL import Image, ImageDraw, ImageFont

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

FONT_PATH = r"C:\Windows\Fonts\simsun.ttc"
SIZE = 16
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "font16_cn.h")

def collect_gb2312_chars():
    """收集 GB2312 全部可编码字符:
    符号区 (01-09 区, 0xA1A1-0xA9FE: 标点、数字、拉丁、希腊等) +
    汉字区 (16-87 区, 0xB0A1-0xF7FE)"""
    chars = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                b = bytes([hi, lo])
                ch = b.decode('gb2312')
                if len(ch) == 1:
                    chars.add(ch)
            except Exception:
                pass
    return sorted(chars)

def render_char(font, ch):
    """渲染单字到 16x16, bbox 居中"""
    img = Image.new('L', (32, 32), 255)
    d = ImageDraw.Draw(img)
    d.text((1, 1), ch, font=font, fill=0)
    px = img.load()
    # 收集暗像素
    pts = [(x, y) for y in range(32) for x in range(32) if px[x, y] < 128]
    if not pts:
        return [0] * SIZE
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    w, h = x1 - x0 + 1, y1 - y0 + 1
    ox = max(0, (SIZE - w) // 2)
    oy = max(0, (SIZE - h) // 2)
    dark = [0] * SIZE
    for yy in range(min(h, SIZE)):
        if oy + yy >= SIZE:
            break
        row = 0
        for xx in range(min(w, SIZE)):
            if ox + xx >= SIZE:
                break
            if px[x0 + xx, y0 + yy] < 128:
                row |= 1 << (15 - (ox + xx))
        dark[oy + yy] = row
    return dark

def main():
    font = ImageFont.truetype(FONT_PATH, SIZE)
    chars = collect_gb2312_chars()
    print(f"GB2312 汉字总数: {len(chars)}")

    # 渲染所有字符
    bmps = []
    uni = []
    for ch in chars:
        cp = ord(ch)
        rows = render_char(font, ch)
        bmps.append(rows)
        uni.append(cp)

    n = len(chars)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("// font16_cn.h - GB2312 全字库 16x16 (自动生成, 勿手改)\n")
        f.write(f"// 共 {n} 个汉字; 1=黑像素; 位图按 unicode 升序, 与 font16_cn_uni 对应\n")
        f.write("#ifndef FONT16_CN_H\n#define FONT16_CN_H\n\n")
        f.write(f"#define FONT_CN_COUNT {n}\n\n")
        f.write("const unsigned short font16_cn_uni[FONT_CN_COUNT] PROGMEM = {\n")
        for i in range(0, n, 16):
            f.write("  " + ",".join(f"0x{cp:04X}" for cp in uni[i:i+16]) + ",\n")
        f.write("};\n\n")
        f.write("const unsigned char font16_cn_bmp[FONT_CN_COUNT][32] PROGMEM = {\n")
        for i, rows in enumerate(bmps):
            bytes_list = []
            for r in rows:
                bytes_list.append(f"0x{r >> 8:02X}")
                bytes_list.append(f"0x{r & 0xFF:02X}")
            f.write("  {" + ",".join(bytes_list) + "}, // U+%04X %s\n" % (uni[i], chars[i]))
        f.write("};\n\n")
        f.write("// GB2312 code (high/low 0xA1..0xF7/0xFE) -> bitmap index; 0xFFFF=missing\n")
        f.write("const unsigned short font16_cn_gb[94][94] PROGMEM = {\n")
        by_code = {}
        for hi in range(0xA1, 0xF8):
            for lo in range(0xA1, 0xFF):
                try:
                    ch = bytes([hi, lo]).decode('gb2312')
                    by_code[(hi, lo)] = uni.index(ord(ch)) if len(ch) == 1 else -1
                except UnicodeDecodeError:
                    by_code[(hi, lo)] = -1
        for hi in range(0xA1, 0xF8):
            vals = []
            for lo in range(0xA1, 0xFF):
                idx = by_code[(hi, lo)]
                vals.append("0xFFFF" if idx < 0 else str(idx))
            f.write("  {" + ",".join(vals) + "},\n")
        f.write("};\n\n#endif\n")

    print(f"已生成 {OUT} ({os.path.getsize(OUT)} 字节)")

if __name__ == "__main__":
    main()

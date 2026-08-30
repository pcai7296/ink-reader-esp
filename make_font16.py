# -*- coding: utf-8 -*-
"""
make_font16.py v2 - 生成基线对齐的 ASCII 16x16 位图字体
用 anchor='ls' (左基线) 渲染, 所有字符主干底部对齐在同一基线, g/y/p/q 的尾巴自然下垂
字体: Tahoma 16px (Windows 标准无衬线, 比例均衡清晰)
输出: font16x16.h  (95 字符, PROGMEM, 每字 16 行 x 2 字节, MSB 在前)
"""
import os
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = r"C:\Windows\Fonts\tahoma.ttf"
SIZE = 16
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "font16x16.h")

BASELINE_SRC = 20   # 24x24 画布中基线 y
BASELINE_DST = 13   # 16x16 网格中基线 y

def render_char(font, ch):
    """渲染单字符到 16x16, 基线对齐"""
    img = Image.new('L', (24, 24), 255)
    d = ImageDraw.Draw(img)
    d.text((3, BASELINE_SRC), ch, font=font, fill=0, anchor='ls')
    px = img.load()
    pts = [(x, y) for y in range(24) for x in range(24) if px[x, y] < 128]
    if not pts:
        return [0] * SIZE
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    w = x1 - x0 + 1
    ox = max(0, (SIZE - w) // 2)
    oy = BASELINE_DST - (BASELINE_SRC - y0)
    grid = [0] * SIZE
    for yy in range(y0, y1 + 1):
        ty = oy + (yy - y0)
        if ty < 0 or ty >= SIZE:
            continue
        row = 0
        for xx in range(x0, x1 + 1):
            tx = ox + (xx - x0)
            if tx < 0 or tx >= SIZE:
                continue
            if px[xx, yy] < 128:
                row |= 1 << (15 - tx)
        grid[ty] = row
    return grid

def main():
    font = ImageFont.truetype(FONT_PATH, SIZE)
    out = []
    out.append("// font16x16.h - ASCII 16x16 位图字体 (Tahoma 16px, 基线对齐)")
    out.append("// 自动生成, 勿手改. 1=黑像素, 每字 16 行 x 2 字节, MSB 在前")
    out.append("static const unsigned int font16x16[95][16] PROGMEM = {")
    for code in range(0x20, 0x7F):
        grid = render_char(font, chr(code))
        words = []
        for r in grid:
            words.append("0x%04X" % r)
        out.append("  { %s }, // 0x%02X '%s'" % (", ".join(words), code, chr(code)))
    out.append("};")
    text = "\n".join(out)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"font16x16.h 已生成 ({os.path.getsize(OUT)} 字节)")

    # 验证
    for ch in 'Ag1yRW':
        g = render_char(font, ch)
        print(f"\n{ch}:")
        for r in g:
            print(''.join('#' if (r >> (15 - b)) & 1 else '.' for b in range(16)))

if __name__ == "__main__":
    main()

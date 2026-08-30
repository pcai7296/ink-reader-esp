#!/usr/bin/env python3
# make_nav_icons.py — 生成 13x13 首页导航图标位图 nav_icons.h
# 用法: python make_nav_icons.py > nav_icons.h
# 输出: static const uint8_t navIcons[6][26] PROGMEM, 顺序: 续读/文件/时钟/天气/配网/设置
# 1 = 黑像素, 每行 2 字节 MSB left (13 位宽, 后 3 位为 0)
# 风格: 单色黑白极简线条 (e-ink 友好, 区分度高); 设置/齿轮加粗防细线显示不全

import sys
import math
from PIL import Image, ImageDraw

sys.stdout.reconfigure(encoding="utf-8")

SIZE = 13
BLACK = 1
WHITE = 0


def new_img():
    return Image.new("1", (SIZE, SIZE), WHITE)


# 0 续读: 翻开的书 (左右两页 + 中缝)
def icon_book():
    img = new_img()
    d = ImageDraw.Draw(img)
    # 左页
    d.polygon([(1, 2), (6, 2), (6, 10), (1, 11)], fill=BLACK)
    # 右页
    d.polygon([(6, 2), (11, 2), (11, 11), (6, 10)], fill=BLACK)
    # 中缝白线 (区分两页)
    d.line([6, 2, 6, 10], fill=WHITE, width=1)
    return img


# 1 文件: 文件夹 (梯形 + 底矩形, 加粗)
def icon_file():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.rectangle([2, 3, 11, 11], outline=BLACK)
    d.line([2, 3, 4, 3], fill=BLACK)
    d.line([4, 3, 5, 5], fill=BLACK)
    d.line([5, 5, 11, 5], fill=BLACK)
    d.line([2, 5, 2, 11], fill=BLACK)
    return img


# 2 时钟: 表盘 + 时针分针
def icon_clock():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([2, 2, 10, 10], outline=BLACK, width=2)
    d.line([6, 6, 6, 3], fill=BLACK, width=2)
    d.line([6, 6, 9, 6], fill=BLACK, width=2)
    return img


# 3 天气: 太阳 (圆 + 光芒, 加粗)
def icon_weather():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([4, 3, 9, 8], fill=BLACK)
    cx, cy = 6.5, 5.5
    for i in range(8):
        a = i * math.pi / 4
        x1 = cx + 3.2 * math.cos(a)
        y1 = cy + 3.2 * math.sin(a)
        x2 = cx + 4.8 * math.cos(a)
        y2 = cy + 4.8 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=2)
    return img


# 4 配网: WiFi 三弧线 + 底点
def icon_wifi():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([5, 10, 7, 12], fill=BLACK)
    for r, ytop in [(2.2, 8), (4.0, 5), (5.8, 2)]:
        d.arc([6 - r, ytop, 6 + r, ytop + 2 * r], start=180, end=360, fill=BLACK, width=2)
    return img


# 5 设置: 齿轮 (外圆2px + 8 粗齿 + 中心孔, 加粗防细线)
def icon_settings():
    img = new_img()
    d = ImageDraw.Draw(img)
    # 中心孔 (白)
    d.ellipse([4, 4, 8, 8], fill=WHITE)
    d.ellipse([4, 4, 8, 8], outline=BLACK, width=2)
    # 8 个粗齿 (短宽线段)
    for i in range(8):
        a = i * math.pi / 4
        x1 = 6 + 2.8 * math.cos(a)
        y1 = 6 + 2.8 * math.sin(a)
        x2 = 6 + 4.6 * math.cos(a)
        y2 = 6 + 4.6 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=2)
    return img


def img_to_c(img):
    out = []
    px = img.load()
    for y in range(SIZE):
        b0 = 0
        for k in range(8):
            if px[k, y] == BLACK:
                b0 |= 0x80 >> k
        b1 = 0
        for k in range(5):
            if px[8 + k, y] == BLACK:
                b1 |= 0x80 >> k
        out.append(b0)
        out.append(b1)
    return out


def main():
    icons = [icon_book(), icon_file(), icon_clock(),
             icon_weather(), icon_wifi(), icon_settings()]
    names = ["续读", "文件", "时钟", "天气", "配网", "设置"]
    print("// nav_icons.h — 13x13 首页导航图标位图 (1=黑, 每行 2 字节 MSB left)")
    print("// 由 make_nav_icons.py 生成, 顺序: 续读/文件/时钟/天气/配网/设置")
    print("#ifndef NAV_ICONS_H")
    print("#define NAV_ICONS_H")
    print("#include <Arduino.h>")
    print()
    print("static const uint8_t navIcons[6][26] PROGMEM = {")
    for idx, (img, nm) in enumerate(zip(icons, names)):
        bytes_ = img_to_c(img)
        print(f"  // {idx} {nm}")
        print("  {")
        for y in range(SIZE):
            print("    0x%02X, 0x%02X," % (bytes_[y * 2], bytes_[y * 2 + 1]))
        print("  },")
    print("};")
    print()
    print("#endif // NAV_ICONS_H")


if __name__ == "__main__":
    main()

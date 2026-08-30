#!/usr/bin/env python3
# make_nav_icons.py — 生成 13x13 首页导航图标位图 nav_icons.h
# 用法: python make_nav_icons.py > nav_icons.h
# 输出: static const uint8_t navIcons[6][26] PROGMEM, 顺序: 文件/时钟/天气/配网/设置/返回
# 1 = 黑像素, 每行 2 字节 MSB left (13 位宽, 后 3 位为 0)
# 风格: 单色黑白极简线条 (e-ink 友好, 区分度高)

import sys
import math
from PIL import Image, ImageDraw

sys.stdout.reconfigure(encoding="utf-8")

SIZE = 13
BLACK = 1
WHITE = 0


def new_img():
    return Image.new("1", (SIZE, SIZE), WHITE)


# 0 文件: 文件夹 (梯形 + 翻页角)
def icon_file():
    img = new_img()
    d = ImageDraw.Draw(img)
    # 文件夹主体: 顶带(梯形) + 底部矩形
    d.rectangle([2, 3, 11, 11], outline=BLACK)
    d.line([2, 3, 4, 3], fill=BLACK)   # 顶带左
    d.line([4, 3, 5, 5], fill=BLACK)   # 斜带
    d.line([5, 5, 11, 5], fill=BLACK)  # 带右端
    d.line([2, 5, 2, 11], fill=BLACK)  # 左壁
    return img


# 1 时钟: 表盘 + 时针分针
def icon_clock():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([2, 2, 10, 10], outline=BLACK)
    d.line([6, 6, 6, 3], fill=BLACK, width=1)   # 时针
    d.line([6, 6, 9, 6], fill=BLACK, width=1)   # 分针
    return img


# 2 天气: 太阳 (圆 + 光芒)
def icon_weather():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([4, 4, 8, 8], fill=BLACK)
    for i in range(8):
        a = i * math.pi / 4
        x1 = 6 + 3.2 * math.cos(a)
        y1 = 6 + 3.2 * math.sin(a)
        x2 = 6 + 5.2 * math.cos(a)
        y2 = 6 + 5.2 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=1)
    return img


# 3 配网: WiFi 三弧线 + 底点
def icon_wifi():
    img = new_img()
    d = ImageDraw.Draw(img)
    # 三圈弧线 (从外到内) + 底部圆点
    for r, (y0, y1) in [(5.5, (4, 9)), (3.5, (8, 11)), (1.6, (11, 12))]:
        # 用弧线近似: 下半圆
        d.arc([6 - r, y0, 6 + r, y1], start=180, end=360, fill=BLACK)
    d.ellipse([5, 10, 7, 12], fill=BLACK)   # 底点
    return img


# 4 设置: 齿轮 (圆 + 8 齿)
def icon_settings():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([4, 4, 8, 8], outline=BLACK)
    for i in range(8):
        a = i * math.pi / 4
        x1 = 6 + 3.0 * math.cos(a)
        y1 = 6 + 3.0 * math.sin(a)
        x2 = 6 + 4.6 * math.cos(a)
        y2 = 6 + 4.6 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=1)
    d.ellipse([5, 5, 7, 7], fill=WHITE)
    return img


# 5 返回: 左箭头 (横线 + 左三角)
def icon_back():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.line([8, 6, 11, 6], fill=BLACK, width=1)   # 横杆
    d.line([8, 2, 8, 10], fill=BLACK, width=1)   # 竖杆
    d.polygon([(7, 6), (2, 6), (6, 2)], fill=BLACK)   # 左三角头
    d.polygon([(7, 6), (2, 6), (6, 10)], fill=BLACK)
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
    icons = [icon_file(), icon_clock(), icon_weather(),
             icon_wifi(), icon_settings(), icon_back()]
    names = ["文件", "时钟", "天气", "配网", "设置", "返回"]
    print("// nav_icons.h — 13x13 首页导航图标位图 (1=黑, 每行 2 字节 MSB left)")
    print("// 由 make_nav_icons.py 生成, 顺序: 文件/时钟/天气/配网/设置/返回")
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

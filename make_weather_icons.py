#!/usr/bin/env python3
# make_weather_icons.py — 生成 24x24 天气图标位图数组 weather_icons.h
# 用法: python make_weather_icons.py > weather_icons.h
# 输出: static const uint8_t wxIcons[6][72] PROGMEM, 顺序: 晴/多云/阴/雨/雪/雾
# 1 = 黑像素, 每行 3 字节 MSB left

import sys
from PIL import Image, ImageDraw

# Windows stdout 默认 GBK，强制 UTF-8 输出保证 .h 文件为 UTF-8
sys.stdout.reconfigure(encoding="utf-8")

SIZE = 24
BLACK = 1
WHITE = 0


def new_img():
    return Image.new("1", (SIZE, SIZE), WHITE)


def draw_sun(d, cx=17, cy=7, r=4):
    """小太阳: 圆 + 8 条光芒"""
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=BLACK)
    # 光芒
    for i in range(8):
        import math
        a = i * math.pi / 4
        x1 = cx + (r + 1) * math.cos(a)
        y1 = cy + (r + 1) * math.sin(a)
        x2 = cx + (r + 3) * math.cos(a)
        y2 = cy + (r + 3) * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=1)


def draw_cloud(d, x0=3, y0=9, w=18, h=9):
    """云朵: 三个圆 + 底部矩形"""
    d.ellipse([x0, y0 + h // 2, x0 + w * 2 // 3, y0 + h], fill=BLACK)
    d.ellipse([x0 + w // 4, y0, x0 + w * 3 // 4, y0 + h], fill=BLACK)
    d.ellipse([x0 + w // 2, y0 + h // 3, x0 + w, y0 + h], fill=BLACK)
    d.rectangle([x0, y0 + h * 2 // 3, x0 + w, y0 + h], fill=BLACK)


# ---- 各图标 ----
# 0 晴: 大太阳居中
def icon_sunny():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([6, 6, 18, 18], fill=BLACK)
    for i in range(8):
        import math
        a = i * math.pi / 4
        x1 = 12 + 9 * math.cos(a)
        y1 = 12 + 9 * math.sin(a)
        x2 = 12 + 11 * math.cos(a)
        y2 = 12 + 11 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=1)
    return img


# 1 多云: 左云 + 右上小太阳
def icon_cloudy():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_sun(d, 18, 6, 3)
    draw_cloud(d, 2, 10, 19, 10)
    return img


# 2 阴: 大云
def icon_overcast():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_cloud(d, 1, 8, 22, 11)
    return img


# 3 雨: 云 + 3 条斜雨滴
def icon_rain():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_cloud(d, 2, 6, 20, 9)
    for i in range(3):
        x = 5 + i * 7
        d.line([x, 17, x - 1, 21], fill=BLACK, width=1)
    return img


# 4 雪: 云 + 3 个雪花点
def icon_snow():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_cloud(d, 2, 6, 20, 9)
    for i in range(3):
        x = 5 + i * 7
        d.line([x - 2, 19, x + 2, 19], fill=BLACK, width=1)
        d.line([x, 17, x, 21], fill=BLACK, width=1)
    return img


# 5 雾: 云 + 底部横线
def icon_fog():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_cloud(d, 2, 4, 20, 8)
    d.line([5, 16, 19, 16], fill=BLACK, width=1)
    d.line([3, 20, 21, 20], fill=BLACK, width=1)
    return img


def img_to_c(img):
    """24x24 位图 -> 72 字节列表"""
    out = []
    px = img.load()
    for y in range(SIZE):
        for x in range(0, SIZE, 8):
            b = 0
            for k in range(8):
                if x + k < SIZE and px[x + k, y] == BLACK:
                    b |= 0x80 >> k
            out.append(b)
    return out


def main():
    icons = [icon_sunny(), icon_cloudy(), icon_overcast(), icon_rain(), icon_snow(), icon_fog()]
    names = ["晴", "多云", "阴", "雨", "雪", "雾"]
    print("// weather_icons.h — 24x24 天气图标位图 (1=黑, 每行 3 字节 MSB left)")
    print("// 由 make_weather_icons.py 生成, 顺序: 晴/多云/阴/雨/雪/雾")
    print("#ifndef WEATHER_ICONS_H")
    print("#define WEATHER_ICONS_H")
    print("#include <Arduino.h>")
    print()
    print("static const uint8_t wxIcons[6][72] PROGMEM = {")
    for idx, (img, nm) in enumerate(zip(icons, names)):
        bytes_ = img_to_c(img)
        print(f"  // {idx} {nm}")
        print("  {")
        for y in range(SIZE):
            row = bytes_[y * 3:(y + 1) * 3]
            print("    0x%02X, 0x%02X, 0x%02X," % tuple(row))
        print("  },")
    print("};")
    print()
    print("#endif // WEATHER_ICONS_H")


if __name__ == "__main__":
    main()
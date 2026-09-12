#!/usr/bin/env python3
# make_small_icons.py — 生成 13x13 天气页小图标位图 weather_small_icons.h
# 用法: python make_small_icons.py > weather_small_icons.h
# 输出: static const uint8_t wsIcons[6][26] PROGMEM, 顺序: 更新时间/位置/天气状态/UVI/湿度/风力
# 1 = 黑像素, 每行 2 字节 MSB left (13 位宽, 后 3 位为 0)

import sys
import math
from PIL import Image, ImageDraw

# Windows stdout 默认 GBK，强制 UTF-8 输出保证 .h 文件为 UTF-8
sys.stdout.reconfigure(encoding="utf-8")

SIZE = 13
BLACK = 1
WHITE = 0


def new_img():
    return Image.new("1", (SIZE, SIZE), WHITE)


# 0 更新时间: 表盘 + 时针分针
def icon_clock():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([2, 2, 10, 10], outline=BLACK)
    d.line([6, 6, 6, 3], fill=BLACK, width=1)   # 时针
    d.line([6, 6, 9, 6], fill=BLACK, width=1)   # 分针
    return img


# 1 位置: 图钉 (圆头 + 尖)
def icon_location():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.polygon([(3, 6), (9, 6), (6, 12)], fill=BLACK)  # 尖
    d.ellipse([3, 1, 9, 7], fill=BLACK)              # 圆头
    return img


# 2 天气状态: 云 (三个圆 + 底)
def icon_weather():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([2, 6, 6, 10], fill=BLACK)
    d.ellipse([4, 3, 8, 7], fill=BLACK)
    d.ellipse([7, 5, 11, 9], fill=BLACK)
    d.rectangle([2, 8, 11, 10], fill=BLACK)
    return img


# 3 UVI: 太阳 (圆 + 8 光芒)
def icon_uvi():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([4, 4, 8, 8], fill=BLACK)
    for i in range(8):
        a = i * math.pi / 4
        x1 = 6 + 3.5 * math.cos(a)
        y1 = 6 + 3.5 * math.sin(a)
        x2 = 6 + 5.2 * math.cos(a)
        y2 = 6 + 5.2 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=1)
    return img


# 4 湿度: 水滴 (⚠️ 2026-09-12 用户反馈修正: 原实现"上圆 + 下尖"= 倒过来的水滴;
#          正确应为**尖朝上、圆身在下方**, 与 clock_icons.h 的 16x16 水滴一致)
def icon_humidity():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.polygon([(6, 1), (3, 6), (9, 6)], fill=BLACK)  # 尖朝上
    d.ellipse([3, 5, 9, 11], fill=BLACK)            # 圆身在下
    return img


# 5 风力: 三条风线 (长短交错)
def icon_wind():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.line([2, 4, 11, 4], fill=BLACK, width=1)
    d.line([3, 7, 10, 7], fill=BLACK, width=1)
    d.line([2, 10, 11, 10], fill=BLACK, width=1)
    return img


def img_to_c(img):
    """13x13 位图 -> 26 字节 (每行 2 字节)"""
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
    icons = [icon_clock(), icon_location(), icon_weather(),
             icon_uvi(), icon_humidity(), icon_wind()]
    names = ["更新时间", "位置", "天气状态", "UVI", "湿度", "风力"]
    print("// weather_small_icons.h — 13x13 天气页小图标位图 (1=黑, 每行 2 字节 MSB left)")
    print("// 由 make_small_icons.py 生成, 顺序: 更新时间/位置/天气状态/UVI/湿度/风力")
    print("#ifndef WEATHER_SMALL_ICONS_H")
    print("#define WEATHER_SMALL_ICONS_H")
    print("#include <Arduino.h>")
    print()
    print("static const uint8_t wsIcons[6][26] PROGMEM = {")
    for idx, (img, nm) in enumerate(zip(icons, names)):
        bytes_ = img_to_c(img)
        print(f"  // {idx} {nm}")
        print("  {")
        for y in range(SIZE):
            print("    0x%02X, 0x%02X," % (bytes_[y * 2], bytes_[y * 2 + 1]))
        print("  },")
    print("};")
    print()
    print("#endif // WEATHER_SMALL_ICONS_H")


if __name__ == "__main__":
    main()
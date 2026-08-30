#!/usr/bin/env python3
# make_nav_icons.py — 生成 22x22 首页导航线条描边图标 nav_icons.h
# 用法: python make_nav_icons.py > nav_icons.h
# 输出: static const uint8_t navIcons[6][66] PROGMEM, 顺序: 续读/文件/时钟/天气/配网/设置
# 1 = 黑像素, 每行 3 字节 MSB left (22 位宽, 后 2 位为 0)
# 风格: 官方 A7 线条描边 (空心轮廓, 非实心点阵; 22x22 大字下更清晰, e-ink 友好)

import sys
import math
from PIL import Image, ImageDraw

sys.stdout.reconfigure(encoding="utf-8")

SIZE = 22
BLACK = 1
WHITE = 0


def new_img():
    return Image.new("1", (SIZE, SIZE), WHITE)


# 0 续读: 翻开的书 (两页 + 中缝, 线条描边)
def icon_book():
    img = new_img()
    d = ImageDraw.Draw(img)
    # 左页
    d.polygon([(2, 3), (11, 3), (11, 19), (2, 20)], outline=BLACK)
    # 右页
    d.polygon([(11, 3), (20, 3), (20, 20), (11, 19)], outline=BLACK)
    # 中缝
    d.line([11, 3, 11, 19], fill=BLACK, width=2)
    # 页内线
    d.line([4, 8, 9, 8], fill=BLACK, width=1)
    d.line([4, 12, 9, 12], fill=BLACK, width=1)
    d.line([13, 8, 18, 8], fill=BLACK, width=1)
    d.line([13, 12, 18, 12], fill=BLACK, width=1)
    return img


# 1 文件: 文件夹 (梯形 + 底, 线条描边)
def icon_file():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.rectangle([3, 5, 19, 19], outline=BLACK)
    d.line([3, 5, 6, 5], fill=BLACK, width=2)
    d.line([6, 5, 8, 9], fill=BLACK, width=2)
    d.line([8, 9, 19, 9], fill=BLACK, width=2)
    return img


# 2 时钟: 表盘 + 时针分针 (线条描边)
def icon_clock():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([3, 3, 19, 19], outline=BLACK, width=2)
    d.line([11, 11, 11, 6], fill=BLACK, width=2)   # 时针
    d.line([11, 11, 16, 11], fill=BLACK, width=2)  # 分针
    d.ellipse([10, 10, 12, 12], fill=BLACK)        # 中心点
    return img


# 3 天气: 实心太阳 (实心圆 + 8 粗芒, 低分辨率下实心最清晰)
def icon_weather():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([7, 7, 14, 14], fill=BLACK)   # 实心中心圆
    cx, cy = 10.5, 10.5
    # 8 条粗光芒 (固定长度, 分布整齐)
    for i in range(8):
        a = i * math.pi / 4
        x1 = cx + 5.6 * math.cos(a)
        y1 = cy + 5.6 * math.sin(a)
        x2 = cx + 7.8 * math.cos(a)
        y2 = cy + 7.8 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=3)
    return img


# 4 配网: 实心 WiFi (3 粗弧 + 底部单点)
def icon_wifi():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([9, 15, 13, 19], fill=BLACK)   # 底部圆点
    # 3 条粗弧 (同心, 下方张开)
    for r, ytop in [(5, 9), (8, 5), (11, 1)]:
        d.arc([11 - r, ytop, 11 + r, ytop + 2 * r], start=210, end=330, fill=BLACK, width=3)
    return img


# 5 设置: 实心齿轮 (外圆环 + 均匀短齿块)
def icon_settings():
    img = new_img()
    d = ImageDraw.Draw(img)
    # 8 个矩形短齿块 (均匀分布)
    cx, cy = 11, 11
    for i in range(8):
        a = i * math.pi / 4
        x1 = cx + 4.0 * math.cos(a)
        y1 = cy + 4.0 * math.sin(a)
        x2 = cx + 8.0 * math.cos(a)
        y2 = cy + 8.0 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=3)
    # 中心圆环 (外圈描边 + 白孔)
    d.ellipse([5, 5, 17, 17], outline=BLACK, width=3)
    d.ellipse([8, 8, 14, 14], fill=WHITE)   # 中心孔
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
        for k in range(8):
            if px[8 + k, y] == BLACK:
                b1 |= 0x80 >> k
        b2 = 0
        for k in range(6):
            if px[16 + k, y] == BLACK:
                b2 |= 0x80 >> k
        out.append(b0)
        out.append(b1)
        out.append(b2)
    return out


def main():
    icons = [icon_book(), icon_file(), icon_clock(),
             icon_weather(), icon_wifi(), icon_settings()]
    names = ["续读", "文件", "时钟", "天气", "配网", "设置"]
    print("// nav_icons.h — 22x22 首页导航线条描边图标位图 (1=黑, 每行 3 字节 MSB left)")
    print("// 由 make_nav_icons.py 生成, 顺序: 续读/文件/时钟/天气/配网/设置")
    print("#ifndef NAV_ICONS_H")
    print("#define NAV_ICONS_H")
    print("#include <Arduino.h>")
    print()
    print("static const uint8_t navIcons[6][66] PROGMEM = {")
    for idx, (img, nm) in enumerate(zip(icons, names)):
        bytes_ = img_to_c(img)
        print(f"  // {idx} {nm}")
        print("  {")
        for y in range(SIZE):
            print("    0x%02X, 0x%02X, 0x%02X," % (bytes_[y * 3], bytes_[y * 3 + 1], bytes_[y * 3 + 2]))
        print("  },")
    print("};")
    print()
    print("#endif // NAV_ICONS_H")


if __name__ == "__main__":
    main()

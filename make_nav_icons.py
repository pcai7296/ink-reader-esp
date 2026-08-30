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


# 3 天气: 云 + 太阳 (线条描边)
def icon_weather():
    img = new_img()
    d = ImageDraw.Draw(img)
    # 太阳 (右上)
    d.ellipse([12, 2, 19, 9], outline=BLACK, width=2)
    for a in range(8):
        ang = a * math.pi / 4
        x1 = 15.5 + 6.5 * math.cos(ang)
        y1 = 5.5 + 6.5 * math.sin(ang)
        x2 = 15.5 + 8.5 * math.cos(ang)
        y2 = 5.5 + 8.5 * math.sin(ang)
        d.line([x1, y1, x2, y2], fill=BLACK, width=2)
    # 云 (左下, 三圆弧 + 底)
    d.ellipse([3, 10, 9, 16], outline=BLACK, width=2)
    d.ellipse([7, 7, 14, 14], outline=BLACK, width=2)
    d.ellipse([11, 10, 17, 16], outline=BLACK, width=2)
    d.rectangle([3, 14, 17, 16], outline=BLACK, width=2)
    return img


# 4 配网: WiFi 信号 (三弧线 + 底点, 线条)
def icon_wifi():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([9, 17, 13, 21], fill=BLACK)   # 底点
    for r, ytop in [(4, 12), (7, 8), (10, 4)]:
        d.arc([11 - r, ytop, 11 + r, ytop + 2 * r], start=180, end=360, fill=BLACK, width=2)
    return img


# 5 设置: 齿轮 (外圆2px + 8 粗齿 + 中心孔, 线条)
def icon_settings():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.ellipse([7, 7, 15, 15], outline=BLACK, width=2)   # 外圆
    for i in range(8):
        a = i * math.pi / 4
        x1 = 11 + 4.2 * math.cos(a)
        y1 = 11 + 4.2 * math.sin(a)
        x2 = 11 + 7.5 * math.cos(a)
        y2 = 11 + 7.5 * math.sin(a)
        d.line([x1, y1, x2, y2], fill=BLACK, width=2)
    d.ellipse([9, 9, 13, 13], fill=WHITE)   # 中心孔
    d.ellipse([9, 9, 13, 13], outline=BLACK, width=2)
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

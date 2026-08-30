#pragma once
// fb_gfx.h — 帧缓冲 GFX 适配层 (FramebufferGfx) + 屏幕/U8g2 全局对象 extern
// 从 ink-reader-esp.ino 提取: FramebufferGfx 类 + epd/gfx/u8g2Fonts/textRendererReady 对象。
// 其他模块 (阅读器渲染/索引构建/休眠等) 通过 #include "fb_gfx.h" 访问屏幕对象与 U8g2 渲染器。
// 对象定义仍在 ink-reader-esp.ino (唯一定义点), 本头文件只做类型 + extern 声明。

#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "epd_290a.h"

// SCR_W/SCR_H = 逻辑横屏尺寸 (非阅读页与阅读页横屏布局), 不是物理 fb 尺寸。
// 定义在 ink-reader-esp.ino (296×128); 此处 #ifndef 保护, 谁先 include 谁生效。
#ifndef SCR_W
#define SCR_W 296
#define SCR_H 128
#endif

// ---------- 屏幕对象 (定义在 ink-reader-esp.ino) ----------
extern EPD_290A epd;

// ---------- 帧缓冲绘制原语 (定义在 ink-reader-esp.ino; 此处声明供 FramebufferGfx 内联成员调用) ----------
void setPix(int x, int y, bool black);

// ---------- 帧缓冲 GFX 适配 (U8g2 的像素出口) ----------
// drawPixel → setPix: 让 U8g2_for_Adafruit_GFX 能画到自研 fb 帧缓冲。
// setPix 声明在 globals.h (帧缓冲绘制原语)。
class FramebufferGfx final : public Adafruit_GFX {
public:
    FramebufferGfx() : Adafruit_GFX(SCR_W, SCR_H) {}

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        setPix(x, y, color != 0);
    }
};

// ---------- U8g2 文本渲染器对象 (定义在 ink-reader-esp.ino) ----------
extern FramebufferGfx gfx;
extern U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
extern bool textRendererReady;

// 字体表 (PROGMEM, 定义在库内, 此处 extern 供各模块引用)
extern const uint8_t chinese_gb2312[253023] U8G2_FONT_SECTION("chinese_gb2312");
extern const uint8_t u8g2_font_wqy12_t_gb2312[] U8G2_FONT_SECTION("u8g2_font_wqy12_t_gb2312");   // 12px 中文 (菜单小字, 完整 GB2312)

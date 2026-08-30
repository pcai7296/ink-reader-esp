#pragma once
#include <stdint.h>

// 四向旋转坐标映射 —— 唯一权威坐标规范 (file_manager/AGENTS.md 同步维护)
// ------------------------------------------------------------------
// 物理 framebuffer 恒为 128 列 × 296 行 (列 px∈[0,128), 行 py∈[0,296))。
// 逻辑画布尺寸依旋转角度: rot 0/180 → 128×296 (竖类), rot 90/270 → 296×128 (横类)。
// mapFbRot() 输入为当前 rot 对应的逻辑画布坐标, 输出恒为物理 fb 坐标。
// 返回 false = 逻辑坐标越界或 rot 非法 (px/py 不写; 防御保险丝, 不画任何像素)。
// 旋转语义: 内容相对物理面板原生竖屏的顺时针角度; 0→90→180→270→0。
// ⚠️ 旋转角用 uint16_t: 270 超出 uint8_t 上限 (270 会被截断成 14, 单测实测踩坑)。
// 90° 行 px=y, py=295-x 与既有产线代码 (file_manager.ino setPix 横屏分支) 完全一致。

static const int ROT_FB_W = 128;   // 物理 fb 列数 (== EPD_WIDTH)
static const int ROT_FB_H = 296;   // 物理 fb 行数 (== EPD_HEIGHT)

inline bool mapFbRot(uint16_t rot, int x, int y, int &px, int &py) {
    switch (rot) {
        case 0:    // 竖屏 128x296 直映
            if (x < 0 || x >= ROT_FB_W || y < 0 || y >= ROT_FB_H) return false;
            px = x; py = y;
            return true;
        case 90:   // 横屏 296x128 (现状默认)
            if (x < 0 || x >= ROT_FB_H || y < 0 || y >= ROT_FB_W) return false;
            px = y; py = ROT_FB_H - 1 - x;
            return true;
        case 180:  // 竖屏倒置 128x296
            if (x < 0 || x >= ROT_FB_W || y < 0 || y >= ROT_FB_H) return false;
            px = ROT_FB_W - 1 - x; py = ROT_FB_H - 1 - y;
            return true;
        case 270:  // 横屏倒置 296x128
            if (x < 0 || x >= ROT_FB_H || y < 0 || y >= ROT_FB_W) return false;
            px = ROT_FB_W - 1 - y; py = x;
            return true;
        default:
            return false;   // 非法 rot: 防御保险丝, 正常路径不得依赖
    }
}

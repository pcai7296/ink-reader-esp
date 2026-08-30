// bmp_show.h — SD 卡 BMP 图片显示（移植自官方 A7 Bmp.ino，适配 SDFS + 本项目帧缓冲）
// 支持位深 1/4/8/16/24，未压缩(BITMAPINFOHEADER)或 BITFIELDS(565)
// 显示区域：逻辑 296×128 全屏，从 (0,0) 绘制，白色背景；原图超界部分裁掉
#ifndef BMP_SHOW_H
#define BMP_SHOW_H

#include <Arduino.h>

// 从 SD 读取 BMP 文件并逐像素写入帧缓冲（逻辑坐标 setPix）
// 返回 true = 文件有效且已绘制（调用方负责 refresh）
// 返回 false = 打开失败或格式不支持（调用方显示提示并恢复界面）
bool bmpShowFromSd(const char *path);

#endif // BMP_SHOW_H
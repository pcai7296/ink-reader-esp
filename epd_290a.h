/**
 * @file epd_290a.h
 * @brief 2.9寸 A01 墨水屏驱动 (SSD1680)
 * @note 简化版驱动，基于 readguy 项目
 * @resolution 128x296
 */

#ifndef _EPD_290A_H
#define _EPD_290A_H

#include <Arduino.h>
#include <SPI.h>

// 调试开关：1=驱动串口输出调试信息（排障时 -DEPD_DEBUG=1 临时开启）。
// 默认 0: GPIO3=RX 与 KEY3 复用, 刷新热路径串口输出会与按键互相灌噪声（AGENTS 反模式）
#ifndef EPD_DEBUG
#define EPD_DEBUG 0
#endif

// 引脚定义 - MoShuiPing-V2.9 (甘草酸不酸2.9寸ESP8266墨水屏)
// 依据原版固件 V14 源码 (xz014_02A3.ino): CS=15, DC=0, RST=2, BUSY=4
// ⚠️ GPIO0 (D3) 同时被 DC 和按键2 复用 (readguy 官方注释: busy 4 rst 2 dc 0 cs 15)
// ⚠️ GPIO5 (D1) 是 SD卡 CS / 按键3，不是 BUSY！
#define EPD_MOSI_PIN 13  // D7, SPI MOSI
#define EPD_SCLK_PIN 14  // D5, SPI SCLK
#define EPD_CS_PIN   15  // D8
#define EPD_DC_PIN   0   // D3, ⚠️ 与按键2复用 (原版固件: DC=GPIO0)
#define EPD_RST_PIN  2   // D4
#define EPD_BUSY_PIN 4   // D2, ⚠️ 高电平=忙 (原版固件: BUSY=GPIO4)

// 按键引脚（低电平触发，内部上拉）- 实测修正版
// ⚠️ 依据 2026-08 硬件实测 (key_screen 屏幕测试三键全过):
//   左键(KEY1)=GPIO12 (原固件电池开关脚, INPUT_PULLUP)
//   中键(KEY2)=GPIO0  (D3, 与屏幕 DC 复用 → 需临时切 INPUT_PULLUP 读)
//   右键(KEY3)=GPIO3  (RX! V2.41+ 硬件把按键3改到了 RX, 不是 GPIO5)
//   GPIO3 用法: OUTPUT HIGH 强驱动对抗 CH340 上拉, 按下=读到 LOW
//   ⚠️ 用 GPIO3 做按键时不要同时用 Serial 监视(按下时会往串口灌垃圾)
#define KEY1_PIN 12  // D6, 左键
#define KEY2_PIN 0   // D3, 中键 ⚠️ 与屏幕 DC 复用，不能直接 digitalRead 检测
#define KEY3_PIN 3   // RX, 右键 ⚠️ V2.41+ 硬件实测 (旧资料说的 GPIO5 是 SD CS)

// 屏幕参数
#define EPD_WIDTH    128
#define EPD_HEIGHT   296

class EPD_290A {
public:
    EPD_290A();
    
    // 基本操作
    void init();
    void reset();
    void sleep();
    
    // 显示操作
    void display(const uint8_t *image);
    void displayPartial(const uint8_t *image);
    void clear(uint8_t color = 0xFF);
    void powerOff();   // 刷新完成后断电 (对齐官方"画完 display.powerOff()")
    
    // 灰度显示
    void displayGreyscale(const uint8_t *image, uint8_t depth = 16);
    
    // 获取参数
    int getWidth() const { return EPD_WIDTH; }
    int getHeight() const { return EPD_HEIGHT; }

private:
    SPISettings _spiSettings;
    
    // SPI 操作
    void beginTransfer();
    void endTransfer();
    void spiTransfer(uint8_t data);
    
    // EPD 命令
    void sendCommand(uint8_t cmd);
    void sendData(uint8_t data);
    void sendDataBulk(const uint8_t *data, int len);
    
    // 等待忙信号
    void waitBusy(int timeout = 5000);

    // 电源控制 (SSD1680: 0x22 0xC0 -> 0x20 上电; 0x02 断电, 保留 RAM 内容)
    void powerOn();

    // 上次等待忙的耗时 (ms)
    unsigned long _busyMs = 0;
    
    // 初始化序列
    void initDisplay();
    void setMemoryArea(int x, int y, int w, int h);
    void setPointer(int x, int y);
    
    // LUT 表
    static const uint8_t lut_full_update[];
    static const uint8_t lut_partial_update[];
};

#endif

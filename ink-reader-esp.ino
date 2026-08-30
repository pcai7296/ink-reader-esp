/**
 * @file ink-reader-esp.ino
 * @brief SD 卡文件管理器 (屏幕反馈版) - 复刻 xz014 项目文件管理功能
 *
 * 板卡: MoShuiPing-V2.9 (V2.41+)
 * 屏幕: 2.9" SSD1680 A01, 横放 → 逻辑 296x128 (物理 128x296 旋转)
 * 显示: 自研 EPD_290A + 16x16 中文字库 (GB2312 全字库 6763 字) + 16x16/8x8 ASCII
 *
 * 按键 (2 键可用, 左键=复位键):
 *   中键 GPIO0 (D3, 与屏幕 DC 复用, 临时切 INPUT_PULLUP 读)
 *   右键 GPIO3 (RX, OUTPUT HIGH 强驱动对抗 CH340)
 *
 * 交互:
 *   右键短按: 下一项      中键短按: 上一项
 *   右键长按: 进入文件夹  中键长按: 返回上级
 *   (txt 长按右键: 进入阅读器)
 *
 * 注意: 读取 KEY3 时不能开串口监视器 (RX 冲突, 按下会乱码, 属正常)
 */

#define EPD_DEBUG 0
#include "epd_290a.h"
#include "rot_map.h"   // 四向旋转坐标规范 (唯一权威: mapFbRot)
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <SD.h>
#include <SDFS.h>   // SDFS.openDir + Dir::next()（官方 A7 枚举同款; File::openNextFile 每项重开文件, 大目录枚举 0 根因, 弃用）
#include "wifi_manager.h"
#include "fs_cache.h"   // SD 目录树 → LittleFS 缓存（进 AP 前扫描; /fs/list 读缓存不碰 SD）
#include "weather_data.h"
#include "weather_icons.h"
#include "weather_small_icons.h"
#include "hitokoto.h"
#include "bmp_show.h"
#include "progress_sync.h"
#include "file_api_fs.h"   // 上传状态接口 ofsUpSetPhaseCallback（"上传中/上传完毕"墨水屏状态）
#include <ESP8266WiFi.h>
#include "font8x8.h"
#include <user_interface.h>
#include "reader_utils.h"   // 通用 (深睡 ESP.deepSleep 由 core 提供)
#include "fb_gfx.h"         // FramebufferGfx 类型 + epd/gfx/u8g2Fonts/textRendererReady extern + 字体 extern


EPD_290A epd;
#define DIAG_SERIAL 1
#define DIAG_SD 0
// ★ 条件编译: BOOT_AP_MODE=1 时, 重启后默认进入原固件 A7 的配网界面引导 (AP 热点管理页 192.168.4.1)。
// 用于"出厂引导/只进配网"场景; =0 时恢复正常的启动分流 (KEY3窗口→最近阅读/首页/睡眠恢复)。
// 编译可覆盖: 命令行加 -DBOOT_AP_MODE=1 优先 (用 #ifndef 允许外部 -D 覆盖)。
#ifndef BOOT_AP_MODE
#define BOOT_AP_MODE 0
#endif
// 环形日志压缩 (2×128B): 串口实时输出不受影响; 静态 BSS 每减 1B 可用堆增 1B,
// BearSSL TLS 同步需要大堆, 省下的 RAM 全部让给堆。
#define DIAG_RING_COUNT 2
#define DIAG_LINE_MAX 128
char diagRing[DIAG_RING_COUNT][DIAG_LINE_MAX];
uint8_t diagRingHead = 0;
// diagSdBuffer 仅 DIAG_SD=1 时占用: 静态 BSS 每减 1B 可用堆就增 1B,
// BearSSL TLS 需要 ~8-9KB 连续堆, 压缩调试缓冲给同步腾空间。
#if DIAG_SD
char diagSdBuffer[1024];
#endif
uint16_t diagSdUsed = 0;
File traceFile;
uint32_t traceLastFlush = 0;
uint32_t diagLastLoopMs = 0;
uint32_t diagMaxLoopGap = 0;

static const char *diagLevelName(char level) {
    return level == 'E' ? "E" : level == 'W' ? "W" : "I";
}
void traceOpen() {
#if DIAG_SD
    if (SD.exists("/debug_trace.log")) {
        File old = SD.open("/debug_trace.log", FILE_READ);
        bool tooLarge = old && old.size() > 65536;
        if (old) old.close();
        if (tooLarge) SD.remove("/debug_trace.log");
    }
    traceFile = SD.open("/debug_trace.log", FILE_WRITE);
    if (traceFile) {
        diagSdUsed = 0;
        traceLastFlush = millis();
        traceFile.println("--- trace boot ---");
        traceFile.flush();
    }
#else
    traceFile = File();
    diagSdUsed = 0;
#endif
}
void diagFlushSd(bool force) {
#if DIAG_SD
    if (!traceFile || diagSdUsed == 0) return;
    if (!force && millis() - traceLastFlush < 1000 && diagSdUsed < sizeof(diagSdBuffer) - 192) return;
    traceFile.write((const uint8_t *)diagSdBuffer, diagSdUsed);
    traceFile.flush();
    diagSdUsed = 0;
    traceLastFlush = millis();
#else
    (void)force;
#endif
}
void diagLog(char level, const char *msg) {
    uint32_t now = millis();
    uint32_t gap = diagLastLoopMs ? now - diagLastLoopMs : 0;
    if (gap > diagMaxLoopGap) diagMaxLoopGap = gap;
    char line[DIAG_LINE_MAX];
    snprintf(line, sizeof(line), "[u=%lu][%s][heap=%lu stack=%lu gap=%lu maxgap=%lu] %s\\n",
             (unsigned long)now, diagLevelName(level), (unsigned long)ESP.getFreeHeap(),
             (unsigned long)ESP.getFreeContStack(), (unsigned long)gap,
             (unsigned long)diagMaxLoopGap, msg ? msg : "");
    strncpy(diagRing[diagRingHead], line, DIAG_LINE_MAX - 1);
    diagRing[diagRingHead][DIAG_LINE_MAX - 1] = '\0';
    diagRingHead = (diagRingHead + 1) % DIAG_RING_COUNT;
#if DIAG_SERIAL
    Serial.print(line);
#endif
#if DIAG_SD
    size_t n = strlen(line);
    if (traceFile && n < sizeof(diagSdBuffer) - diagSdUsed) {
        memcpy(diagSdBuffer + diagSdUsed, line, n);
        diagSdUsed += (uint16_t)n;
    }
    if (level == 'E') diagFlushSd(true);
#else
    (void)level;
#endif
}
void traceLine(const char *line) { diagLog('I', line); }
void traceError(const char *line) { diagLog('E', line); }
void traceWarn(const char *line) { diagLog('W', line); }
void traceFmt(const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    traceLine(buf);
}
void traceFmtLevel(char level, const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    diagLog(level, buf);
}
uint8_t fb[EPD_WIDTH * EPD_HEIGHT / 8];

// ---------- 按键状态机 (必须在函数原型之前定义) ----------
enum { KEY_RELEASE, KEY_PRESS, KEY_LONGTRIG };
struct KState {
    bool down;
    uint32_t pressStart;
    bool longDone;
    bool rawStable;       // 上一稳定电平 (true=按下); 电源噪声防抖用
    uint32_t rawChangeAt; // 电平最近一次变化时刻
};
KState k2, k3;
const uint32_t LONG_MS = 500;
const uint32_t KEY_DEBOUNCE_MS = 10;   // 电源噪声过滤: 电平需持续 10ms 才确认变化

// ---------- 调试日志 ----------
#define DEBUG_BAUD 115200
void debugLine(const char *msg) { traceLine(msg); }
void debugFmt(const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    traceLine(buf);
}

// ---------- 帧缓冲原语 (物理 128x296; 逻辑画布 0/180=128x296 竖类, 90/270=296x128 横类) ----------
// SCR_W/SCR_H = 逻辑横屏尺寸 (非阅读页与阅读页横屏布局), 不是物理 fb 尺寸。
#define SCR_W 296
#define SCR_H 128

uint16_t fbRot = 90;   // 帧缓冲方向 (0/90/180/270, 规范见 rot_map.h; uint16_t: 270 超 uint8_t 上限): 非阅读页恒 90

inline void setPix(int x, int y, bool black) {
    // ⚠️ 越界检查在 mapFbRot 内部完成, 必须先于任何 fb 数组索引/bit offset 计算
    // (历史坑: 用横屏 SCR_W/SCR_H 当边界会丢行/写坏相邻行 → 旋转后花屏)
    int px, py;
    if (!mapFbRot(fbRot, x, y, px, py)) return;
    int idx = py * (ROT_FB_W / 8) + (px >> 3);
    uint8_t mask = 0x80 >> (px & 7);
    if (black) fb[idx] &= ~mask; else fb[idx] |= mask;
}

void fillRect(int x0, int y0, int w, int h, bool black) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            setPix(x, y, black);
}

void drawRect(int x0, int y0, int w, int h, bool black) {
    fillRect(x0, y0, w, 1, black);
    fillRect(x0, y0 + h - 1, w, 1, black);
    fillRect(x0, y0, 1, h, black);
    fillRect(x0 + w - 1, y0, 1, h, black);
}

// ---------- U8g2 + UTF-8 framebuffer renderer ----------
// FramebufferGfx 类定义在 fb_gfx.h (共享头文件); 此处为对象定义点 (唯一定义)。

FramebufferGfx gfx;
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
bool textRendererReady = false;

void initTextRenderer() {
    u8g2Fonts.begin(gfx);
    u8g2Fonts.setFont(chinese_gb2312);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(1);
    textRendererReady = true;
}

int utf8Width(const char *s) {
    if (!textRendererReady) return 0;
    return u8g2Fonts.getUTF8Width(s);
}

void utf8Truncate(const char *src, char *dst, int maxW, int capacity) {
    if (!textRendererReady || maxW <= 0 || capacity <= 0) {
        dst[0] = '\0';
        return;
    }
    char candidate[5] = {0};
    int used = 0;   // 已写入字节数 (容量边界)
    int px = 0;     // 已消耗像素宽 (视觉边界)
    while (*src) {
        uint8_t first = (uint8_t)*src;
        uint8_t length = 1;
        if (first >= 0xF0 && src[1] && src[2] && src[3]) length = 4;
        else if (first >= 0xE0 && src[1] && src[2]) length = 3;
        else if (first >= 0xC0 && src[1]) length = 2;
        memcpy(candidate, src, length);
        candidate[length] = '\0';
        int width = u8g2Fonts.getUTF8Width(candidate);
        if (px + width > maxW) break;
        if (used + length >= capacity) break;
        memcpy(dst + used, src, length);
        used += length;
        px += width;
        src += length;
    }
    dst[used] = '\0';
}

int drawTextUTF8(int x, int y, const char *s, int maxW, bool black) {
    if (!textRendererReady) return x;
    char clipped[192];
    utf8Truncate(s, clipped, maxW, sizeof(clipped));
    u8g2Fonts.setForegroundColor(black ? 1 : 0);
    u8g2Fonts.setCursor(x, y + 13);
    u8g2Fonts.print(clipped);
    return x + u8g2Fonts.getUTF8Width(clipped);
}

// ---------- 数码管风格 7 段显示 ----------
void drawSevenSegDigit(int x, int y, int w, int h, int t, char digit, bool black) {
    bool a, b, c, d, e, f, g;
    a = b = c = d = e = f = g = false;
    switch (digit) {
        case '0': a = b = c = d = e = f = true; break;
        case '1': b = c = true; break;
        case '2': a = b = g = e = d = true; break;
        case '3': a = b = g = c = d = true; break;
        case '4': f = g = b = c = true; break;
        case '5': a = f = g = c = d = true; break;
        case '6': a = f = g = e = c = d = true; break;
        case '7': a = b = c = true; break;
        case '8': a = b = c = d = e = f = g = true; break;
        case '9': a = b = c = d = f = g = true; break;
        default: return;
    }
    int hw = w - 2 * t;
    int vh = h / 2 - t;
    if (a) fillRect(x + t, y, hw, t, black);
    if (g) fillRect(x + t, y + h / 2 - t / 2, hw, t, black);
    if (d) fillRect(x + t, y + h - t, hw, t, black);
    if (f) fillRect(x, y + t, t, vh, black);
    if (b) fillRect(x + w - t, y + t, t, vh, black);
    if (e) fillRect(x, y + h / 2, t, vh, black);
    if (c) fillRect(x + w - t, y + h / 2, t, vh, black);
}

void drawClockColon(int x, int y, int h, bool black) {
    int dot = 10;
    fillRect(x, y + h / 2 - 24, dot, dot, black);
    fillRect(x, y + h / 2 + 14, dot, dot, black);
}

// ---------- 天气页面绘制 ----------
// 24x24 图标：wxIcons[6][72] PROGMEM，bit=1 黑像素，每行 3 字节 MSB left
void drawWeatherIcon(int x, int y, int idx, bool black) {
    if (idx < 0 || idx > 5) return;
    for (int r = 0; r < 24; r++) {
        for (int c = 0; c < 24; c++) {
            uint8_t byte = pgm_read_byte(&wxIcons[idx][r * 3 + c / 8]);
            if (byte & (0x80 >> (c % 8))) setPix(x + c, y + r, black);
        }
    }
}

// 心知天气代码前 2 位 → 图标下标: 00/01=晴 02=多云 03=阴 04=雨 05=雪 其余=雾
int weatherIconIndex(const char *code) {
    if (!code || !code[0]) return 5;
    int v = 0;
    for (int i = 0; i < 2 && code[i] >= '0' && code[i] <= '9'; i++) v = v * 10 + (code[i] - '0');
    if (v == 0 || v == 1) return 0;
    if (v == 2) return 1;
    if (v == 3) return 2;
    if (v == 4) return 3;
    if (v == 5) return 4;
    return 5;
}

// 13x13 小图标：wsIcons[6][26] PROGMEM，bit=1 黑像素，每行 2 字节 MSB left
// 顺序: 0=更新时间 1=位置 2=天气状态 3=UVI 4=湿度 5=风力
void drawSmallIcon(int x, int y, int idx, bool black) {
    if (idx < 0 || idx > 5) return;
    for (int r = 0; r < 13; r++) {
        for (int c = 0; c < 13; c++) {
            uint8_t byte = pgm_read_byte(&wsIcons[idx][r * 2 + c / 8]);
            if (byte & (0x80 >> (c % 8))) setPix(x + c, y + r, black);
        }
    }
}

// 当前字体下文本像素宽度（u8g2 度量，与 drawTextUTF8 同字体）
int textWidth(const char *s) {
    if (!s || !s[0]) return 0;
    return u8g2Fonts.getUTF8Width(s);
}

// weekdayCn → 已提取到 reader_utils.h/cpp

// 8px ASCII 文本（font8x8.h，1=黑像素，每字符 8x8，MSB left）
void drawText8(int x, int y, const char *s, int maxW, bool black) {
    int cx = x;
    for (const char *p = s; *p && cx - x < maxW; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E) { cx += 8; continue; }   // 非 ASCII 跳过
        const unsigned char *glyph = font8x8[c - 0x20];
        for (int row = 0; row < 8; row++) {
            unsigned char bits = pgm_read_byte(&glyph[row]);
            for (int b = 0; b < 8; b++) {
                if (bits & (0x80 >> b)) setPix(cx + b, y + row, black);
            }
        }
        cx += 8;
    }
}

void drawChar16(int x, int y, char ch, bool black) {
    char text[2] = {ch, '\0'};
    drawTextUTF8(x, y, text, 16, black);
}

#define KEY2_PIN 0   // 中键 (与 DC 复用)
#define KEY3_PIN 3   // 右键 (RX!)

int readKey2() {
    digitalWrite(KEY2_PIN, HIGH);
    pinMode(KEY2_PIN, INPUT_PULLUP);
    delayMicroseconds(30);
    int v = digitalRead(KEY2_PIN);
    pinMode(KEY2_PIN, OUTPUT);
    digitalWrite(KEY2_PIN, HIGH);
    return v;
}

int readKey3() {
    pinMode(KEY3_PIN, OUTPUT);
    digitalWrite(KEY3_PIN, HIGH);
    return digitalRead(KEY3_PIN);
}

// ---------- 电池电量 ----------
// V14 官方 Get_bat_vcc.ino: GPIO12 是电池分压开关, 采样前拉高, 采样后拉低。
// GPIO12 同时是 SD MISO; 仅在 SD CS 保持高电平时短暂采样, 随后恢复输入。
#define BAT_SWITCH_PIN 12
int lastBatteryMV = 0;
int readBatteryMV() {
    // 采样偶发失败 (sum=0, GPIO12 开关/SD 总线竞争) 时重试; 之前实测同一电池
    // 有时 sum=0(显示0%) 有时 sum=15840(4.34V), 重试可消除偶发 0。
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        // SD 与电池开关共用 SPI 相关引脚; 采样时确保 SD 未选中。
        pinMode(5, OUTPUT);
        digitalWrite(5, HIGH);
        pinMode(BAT_SWITCH_PIN, OUTPUT);
        digitalWrite(BAT_SWITCH_PIN, HIGH);
        delay(2);
        uint32_t sum = 0;
        // 对齐官方 A7: 20 次采样平均 (反汇编确认: GPIO12 开 + GPIO5 高 + 20×ADC + 关)
        for (uint8_t i = 0; i < 20; i++) sum += analogRead(A0);
        digitalWrite(BAT_SWITCH_PIN, LOW);
        pinMode(BAT_SWITCH_PIN, INPUT);
        if (sum > 0) {
            lastBatteryMV = (int)((sum * 5607UL) / (20UL * 1024UL));
            // 诊断: 异常读数 (0mV 或 >4.3V) 打日志, 定位"电量显示0"是电池耗尽还是采样链路问题
            if (lastBatteryMV == 0 || lastBatteryMV > 4300) {
                traceFmt("BAT_DIAG sum=%lu mv=%d attempt=%u", (unsigned long)sum, lastBatteryMV, attempt);
            }
            return lastBatteryMV;
        }
        delay(5);   // 重试间隔: 让 GPIO12/SD 状态稳定
    }
    lastBatteryMV = 0;
    traceFmt("BAT_DIAG sum=0 mv=0 all-fail");
    return 0;
}
uint8_t batPercent(int v_mV) {
    // 官方 V14 getBatVolBfb 四阶拟合曲线 (A7 同为多项式, 6 阶系数未完全还原, 用已验证的官方 4 阶):
    //   bfb = 497.50976·x⁴ - 7442.07254·x³ + 41515.70648·x² - 102249.34377·x + 93770.99821
    // 边界对齐 A7 反汇编: <3.2V→0%, >4.2V→100%, 计算负值→3% (V14 下限)
    double x = v_mV / 1000.0;
    if (x < 3.2) return 0;
    if (x > 4.2) return 100;
    double bfb = 497.50976 * x * x * x * x - 7442.07254 * x * x * x
               + 41515.70648 * x * x - 102249.34377 * x + 93770.99821;
    if (bfb > 100) bfb = 100;
    else if (bfb < 0) bfb = 3;
    return (uint8_t)bfb;
}

// 充电检测: 电压阈值 + 迟滞 (4000mV 进入 / 3950mV 退出)。
// 说明: A7 固件反编译确认无充电检测/闪电标 (无闪电位图/充电字符串/电池显示无充电分支),
// 闪电标为复刻自创功能。TP4054 恒压阶段电池维持 ~4.2V, 满电断开带载回落到 ~4.0V 以下;
// 迟滞窗 3950-4050 区分"充电中/满电断开"。恒流充电早期 (电压<3.9V) 无法用电压判定,
// 如需插电即显示, 需硬件读取 TP4054 CHRG 引脚 (开漏, 充电中拉低)。
bool isCharging() {
    static bool charging = false;
    if (lastBatteryMV >= 4050) charging = true;
    else if (lastBatteryMV < 3950) charging = false;
    return charging;
}

// 充电闪电图标：8宽×13高，每行 1 字节 MSB left，1=黑色像素
static const uint8_t bitmapLightning[13] PROGMEM = {
    0x10,  // 00010000
    0x38,  // 00111000
    0x3C,  // 00111100
    0x7E,  // 01111110
    0xFC,  // 11111100
    0xD8,  // 11011000
    0xFC,  // 11111100
    0x7E,  // 01111110
    0x3C,  // 00111100
    0x38,  // 00111000
    0x30,  // 00110000
    0x20,  // 00100000
    0x00,  // 00000000
};

// 在指定坐标绘制闪电图标（宽8高13）
void drawLightningIcon(int x, int y, bool black) {
    for (int r = 0; r < 13; r++) {
        uint8_t row = pgm_read_byte(&bitmapLightning[r]);
        for (int c = 0; c < 8; c++) {
            if (row & (0x80 >> c)) setPix(x + c, y + r, black);
        }
    }
}

// ---------- 文件列表 ----------
// 分页窗口化（官方 A7 同款"任意文件数 + 5 行分页滚动", §11.11）:
// 声明在独立头文件 file_list.h（避开 Arduino 原型生成器对自定义返回类型函数的
// 前置原型插入 → "FileItem does not name a type"）。
#include "file_list.h"
FileItem winItems[LIST_WINDOW];   // 静态窗口缓冲 (12×69B≈828B, 不占堆)
int winCount = 0;                 // 窗口内实际项数
int itemCount = 0;                // 目录总项数（第一遍计数）
int selIndex = 0;                 // 选中项（全局索引）
int topIndex = 0;                 // 窗口首项（全局索引）

String currentPath = "/";

#define RECENT_READ_PATH "/.tiemereader/recentread.dat"
#define SLEEP_RECORD_PATH "/.tiemereader/sleepmode.dat"

// ---------- 主页 / 应用模式 ----------
struct SleepRecord;   // 休眠记录 (定义在休眠区) — Arduino 自动原型需要此前向声明
void saveSleepRecord();
bool readSleepRecord(SleepRecord &rec);
enum AppMode { APP_HOME = 0, APP_BROWSER = 1, APP_READER = 2, APP_CHAPTERS = 3, APP_NETWORK = 4, APP_CLOCK_CONNECT = 5, APP_CLOCK = 6, APP_WEATHER = 7, APP_SETTINGS = 8, APP_BMP = 9, APP_MARKS = 10, APP_CLOCK_DISGUISE = 11 };
int appMode = APP_HOME;
bool sdAvailable = false;
// ---- 天气页面状态（本次开机缓存）----
ActualWeather wActual;
FutureWeather wFuture;
LifeIndex wLife;
bool wDataValid = false;
bool wFetching = false;
bool wNightSkip = false;   // 本次进入因夜间跳过联网
char wErrCode[16] = {0};
char wFetchStep[24] = {0};   // 当前获取步骤提示（对齐 A7: 实况/未来/生活指数）
char wCachedSummary[20] = {0};   // 主页天气摘要缓存（SD 持久化, 重启保留; "天气名|温度"）

// 主页天气摘要: 读 SD 缓存 (对齐 A7 主页简洁天气信息, 本次开机只读一次)
// 读前恢复 SD 总线: 主页渲染时 SPI 可能仍在 EPD 侧 (EPD 刷新后未恢复)
void loadWeatherCache() {
    if (wCachedSummary[0] || !sdAvailable) return;
    if (!reinitSdBus("weather_load")) return;
    File f = SD.open("/.tiemereader/weather.dat", FILE_READ);
    if (!f) {
        traceFmt("WEATHER_CACHE_READ_OPEN_FAIL");
        return;
    }
    int n = f.read((uint8_t *)wCachedSummary, sizeof(wCachedSummary) - 1);
    if (n > 0) wCachedSummary[n] = '\0';
    f.close();
    if (!strchr(wCachedSummary, '|')) wCachedSummary[0] = '\0';   // 格式校验
    traceFmt("WEATHER_CACHE_READ_OK n=%d sum=%s", n, wCachedSummary);
}

// 天气获取成功后写 SD 缓存（主页摘要持久化）; 写前恢复 SD 总线（天气页全刷后 SPI 在 EPD 侧）
void saveWeatherCache() {
    if (!sdAvailable || !wDataValid) return;
    if (!reinitSdBus("weather_cache")) return;
    if (!SD.exists("/.tiemereader")) SD.mkdir("/.tiemereader");
    File f = SD.open("/.tiemereader/weather.dat", "w");
    if (!f) {
        traceFmt("WEATHER_CACHE_WRITE_OPEN_FAIL");
        return;
    }
    f.printf("%s|%s", wActual.weatherName, wActual.temp);
    f.close();
    traceFmt("WEATHER_CACHE_WRITE_OK %s|%s", wActual.weatherName, wActual.temp);
}
String recentReadPath;
uint32_t recentReadPage = 0;
uint32_t recentReadTotalPages = 0;
bool recentReadValid = false;
int homeSel = 0;
File txtFile;
File txtIndexFile;
File txtIndexScanFile;
File txtIndexBuildFile;
File txtChapterBuildFile;
String txtPath;
String txtIndexPath;
String txtChapterPath;
uint32_t txtPage = 1;
uint32_t txtTotalPages = 1;
uint32_t txtPageStart = 0;
uint32_t txtIndexedPages = 1;
uint32_t txtChapterCount = 0;
bool txtIndexBuilding = false;
uint32_t txtPendingProgress = 0;
uint32_t txtIndexLastFlush = 0;
uint16_t readerRot = 90;   // 阅读方向: 0/90/180/270 (内容相对物理面板顺时针角, 规范见 rot_map.h)
bool readerIsPortrait() { return readerRot == 0 || readerRot == 180; }   // 竖类: 逻辑 128x296
uint16_t storedToRot(uint8_t v) {   // EEPROM 兼容编码 → 角度 (0/1 旧值含义不变)
    switch (v) {
        case 1: return 0;     // 旧竖屏
        case 2: return 270;   // 横屏翻转
        case 3: return 180;   // 竖屏翻转
        default: return 90;   // 0 或非法残留 → 旧默认横屏
    }
}
uint8_t rotToStored(uint16_t rot) {   // 角度 → EEPROM 兼容编码
    switch (rot) {
        case 0:   return 1;
        case 90:  return 0;
        case 180: return 3;
        default:  return 2;   // 270
    }
}
const char *rotLabel(uint16_t rot) {   // 菜单值文案: UI 显示度数 (默认横屏=0°, 顺时针递增)
    switch (rot) {
        case 90:  return "0";     // 默认横屏
        case 180: return "90";    // 竖屏翻转
        case 270: return "180";   // 横屏翻转
        default:  return "270";   // 竖屏 (内部 0)
    }
}
uint32_t gRotateResumeOffset = 0;   // 旋转续读: 旋转时记当前页字节偏移 (与方向无关, 新方向索引二分定位)
uint32_t gBootHintPage = 0;         // 启动页号提示: setup 恢复阅读器前由 loadRecentReadSummary 算出,
                                    // startTxtReader 消费后清零 (读走即清零, 同 gRotateResumeOffset 模式)
bool gBootKey3Window = false;       // 启动 KEY3 检测窗口激活: 窗口并入最近阅读页表扫描 (优化③)
bool gBootKey3Held = false;         // 窗口内捕获到 KEY3 按下 (重绘后据此全刷回首页)
uint8_t gBootKey3PollLow = 0;       // 窗口内连续低采样计数 (≥2 次 ≈ 10ms 防抖, 对齐 KEY_DEBOUNCE_MS 策略)
bool gBootPartialRefresh = false;   // 启动恢复局刷: setup 按复位原因/模式判定, startTxtReader 消费清零 (优化④)
String txtLines[18];   // 阅读行缓冲 (横 8 行 / 竖 18 行, 按几何类别参数化)
 
// 阅读方向布局参数 (旋转): 横类 90/270 → 296x128 逻辑 (行宽 283px, 8 行/页)
//                         竖类 0/180 → 128x296 逻辑 (行宽 118px, 18 行/页)
int txtLineCount() { return readerIsPortrait() ? 18 : 8; }
int txtLineWidth() { return readerIsPortrait() ? 118 : 283; }
String indexRows[19];   // 索引行缓冲 (横屏 8+1 / 竖屏 18+1, 旋转参数化)
int8_t indexLine = 0;
uint16_t indexEnCount = 0;
uint16_t indexChCount = 0;
uint8_t indexLineOld = 0;
bool indexHskgState = false;
// 索引构建真实扫描偏移: 块读缓冲时 File::position() 只停在 2048 重填点, 用它记页首会把
// 整张页表量化到 2048 边界 → 连续几条记录偏移相同 → 翻页读到同一段内容 ("翻页没反应")。
// 逐字节累计才是真实偏移 (与 PC 模拟器/官方一致)。peek 不消费不累加。
uint32_t indexScanPos = 0;
// 旧格式坏索引标记: 页表记录非严格递增 (2048 对齐 bug 产物) → 打开时强制全量重建, 不做续建
bool indexFormatCorrupt = false;
uint32_t indexPage = 1;
bool indexPageStartPending = false;
// 续建重扫去重种子: 部分 .i1 页表续建时, 重扫中断页会把已写入 .z1 的章节重复追加;
// 记录 .z1 中该页最后一条章节, 重扫越过它之前跳过写入 (见 appendChapterTracked)。
static char resumeChapterSeed[64] = "";
static uint32_t resumeChapterSeedPage = 0;

struct ChapterRow {
    char title[64];
    uint32_t page;
};
ChapterRow chapterRows[LIST_ROWS];
uint32_t chapterRowOffsets[LIST_ROWS];
uint32_t chapterTopOffset = 0;
uint32_t chapterNextOffset = 0;
uint32_t chapterTopLine = 0;
int chapterSel = 0;
int chapterCountLoaded = 0;
// 章节目录页偏移表: 进入章节时一次性扫描 .z1 建立 (每页首行字节偏移 + 精确总页数)。
// 修复历程: ①向前翻每步 seekChapterOffset 全扫 .z1 (O(n²) 卡死); ②页码缓存/偏移栈在
//   "恢复章节后向前翻" (栈空) 时每步 miss → 每步全扫 2.2s (实测日志 chapter_seek 循环)。
// ③页偏移表: 建表一次 O(n) (6000 章约 2.2s, 建表时有喂狗), 之后向前/向后翻页全 O(1)。
static const int CHAPTER_PAGE_TABLE_MAX = 1024;   // 最多 1024 页 (= 6144 章, 覆盖《武炼巅峰》947 页)
static uint32_t chapterPageOffsets[CHAPTER_PAGE_TABLE_MAX];   // pageOffsets[p-1] = 第 p 页首行偏移 (4KB RAM)

// ---------- 阅读器菜单 (局部刷新弹窗, 对齐 A7 7 项) ----------
bool readerMenuOpen = false;
int readerMenuSel = 0;             // 0..9: 字体选择/退出/自动翻页/全刷间隔：/旋转/跳转/标签/休眠/进度同步/配网
char readerMenuNote[32] = "";     // 提示 (覆盖底部信息行)
int gNetworkReturnMode = APP_HOME; // 配网模式退出后返回的界面 (阅读菜单进入=APP_READER, 首页进入=APP_HOME)
uint8_t autoFlipSpeed = 0;        // 自动翻页: 0=关, 1/2/5/10/25/50/100 (倍率)
uint8_t fixedRefreshEvery = 6;    // 全刷间隔: 每 N 次局刷做一次全刷 (默认 6, 原 FIXED_REFRESH_EVERY)
bool fontExternal = false;        // 字体: false=自带(chinese_gb2312) true=外部(加载失败回落自带)
uint32_t autoFlipNextMs = 0;      // 自动翻页下次触发时刻
bool readerJumpOpen = false;      // 页码跳转弹窗
bool readerRotSelOpen = false;     // 旋转方向选择弹窗 (四选一, 选中方案确认后才切换)
uint8_t rotSelCursor = 0;          // 弹窗光标 0..3: 0=0° 1=90° 2=180° 3=270°
uint32_t jumpPage = 0;            // 跳转目标页 (编辑值, 输入中)
uint8_t jumpCursor = 0;           // 键盘光标 0..11: 0-9=数字1..9 0, 10='<'退格, 11=回车
uint32_t jumpRejectMs = 0;        // 超位数拒绝提示时间戳 (1s 内页码行尾显示 "!")
bool readerSyncOpen = false;      // 进度同步子状态 (直连手机 HTTP, 非阻塞状态机)

// 自动翻页间隔: 倍率越大越快; 上限 25 (对齐 A7 "换页倍率过高" 提示)
uint32_t autoFlipIntervalMs() {
    if (autoFlipSpeed == 0) return 0;
    if (autoFlipSpeed > 25) return 0;
    return 60000UL / autoFlipSpeed;
}

// 外部字体加载: 复刻使用 u8g2 内置 chinese_gb2312 字库 (无 SD 字库加载机制),
// 外部字体暂不可用 → 返回 false, 由菜单回落自带并提示 (对齐 A7 "外部字体初始化失败")
bool initExternalFont() {
    return false;
}

// ---------- 章节目录分页 ----------
#define CHAPTER_ROWS 6             // 每页章数
int chapterPage = 1;               // 当前目录页码 (1-based)
int chapterTotalPages = 1;         // 目录总页数 = ceil(总章数/6)
uint8_t chapterSpeed = 1;          // 倍速翻页: 1/2/5/10/25/50/100
bool chapterSpeedPopup = false;    // 倍速选择弹窗
int chapterSpeedSel = 0;           // 弹窗内选中 0..6

// ---------- 标签系统 (每书一份 <书名>.bm: append-only 8字节ASCII页首偏移, 与索引/方向无关, 上限50) ----------
#define MARK_MAX 50
bool readerMarkMenuOpen = false;   // 阅读菜单"标签"子菜单弹窗 ([标记本页][历史标记])
uint8_t markMenuSel = 0;           // 子菜单光标 0=标记本页 1=历史标记
uint8_t markCount = 0;             // 当前书标签总数 (≤MARK_MAX)
int markPage = 1;                  // 当前列表页 (1-based)
int markTotalPages = 1;
int markSel = 0;                   // 页内光标 0..CHAPTER_ROWS-1
int markCountLoaded = 0;           // 本页实际行数
uint32_t markOffsets[CHAPTER_ROWS];   // 本页标签的页首字节偏移
uint32_t markTxtSize = 0;             // 进入列表时的 txt 大小快照 (百分比用: reinit 后旧句柄 size 不可信)
bool markActionOpen = false;       // 操作框 [跳转][删除][取消]
uint8_t markActSel = 0;            // 操作框光标 0..2

void renderHome(bool full);
void enterHomeCard();
void renderNetworkPage(bool full);
void exitNetworkPage();
void renderUploadStatus(int phase, const char *path);   // 上传状态页: 上传中/上传完毕/上传失败
void renderBootStage(const char *line1, const char *line2);  // 配网启动阶段提示(正在初始化/正在加载储存卡)
void renderClockConnect(bool full);
void enterClockPage();
void renderClockPage(bool full);
void renderSettingsPage(bool full);
void settingsHandleKeys(int r2, int r3);
// 设置页状态（全局，供 enterHomeCard 初始化）
#define SETTINGS_ITEM_CNT 4
int settingsSel = 0;           // 光标所在项 0..SETTINGS_ITEM_CNT-1
bool settingsTzEdit = false;   // 时区编辑态（±30 分钟调整中）
char yiyanText[64] = "";       // 一言（进入时钟页时联网获取；空=不显示）
void showMsg(const char *msg, const char *msg2);
void showMiniPrompt(const char *text);
void refresh(bool full);
void renderAll();
void loadRecentReadSummary();
void saveRecentReadPath(const String &path);
void clearRecentReadPathIfMatches(const String &path);
void openRecentRead();
void closeTxtReader();
void startTxtReader(const char *path, bool forceRebuild);
void beginTxtIndexBuild();
void beginResumeIndexBuildFromPartial();
void indexTaskStep();
void enterChapterList();
void leaveReaderToBrowser();
void nextTxtPage();
void previousTxtPage();
void renderReaderMenu();
void openReaderMenu();
void closeReaderMenu();
void execReaderMenu();
void enterSleepMode();
uint32_t countTxtChapters();
uint32_t seekChapterOffset(uint32_t chapterIndex);
void chapterNextPage();
void chapterPrevPage();
// ---- 标签系统 ----
uint32_t findPageCeil(const String &indexPath, uint32_t saved);
void renderMarkMenuOverlay();
void renderMarkList(bool full);
void enterMarksList();
void markMoveSelection(int delta);
void renderChapterSpeedPopup();

// UTF-8/章节辅助
bool isChapterTitle(const char *line, char *title, size_t titleSize);
uint32_t parsePageRecord(uint32_t page);
bool writeProgress(uint32_t offset);   // 写 .i1[0] / sidecar; 失败重试并返回是否成功
void drawReaderLine(int y, const String &line);

void loadRecentReadSummary() {
    recentReadPath = "";
    recentReadPage = 0;
    recentReadTotalPages = 0;
    recentReadValid = false;
    if (!sdAvailable || !SD.exists(RECENT_READ_PATH)) {
        traceFmt("RECENT_ABORT sd=%d file=%d", sdAvailable ? 1 : 0, SD.exists(RECENT_READ_PATH) ? 1 : 0);
        return;
    }
    File f = SD.open(RECENT_READ_PATH, FILE_READ);
    if (!f) return;
    recentReadPath = f.readStringUntil('\n');
    f.close();
    recentReadPath.trim();
    if (recentReadPath.length() == 0 || !SD.exists(recentReadPath)) {
        traceFmt("RECENT_TXT_MISS len=%u exists=%d", (unsigned)recentReadPath.length(), SD.exists(recentReadPath) ? 1 : 0);
        recentReadPath = "";
        return;
    }
    traceFmt("RECENT_TXT path=%s", recentReadPath.c_str());
    String indexPath = recentReadPath;
    int dot = indexPath.lastIndexOf('.');
    if (dot > 0) indexPath = indexPath.substring(0, dot);
    indexPath += readerIsPortrait() ? ".v1" : ".i1";   // 方向感知: 竖类(0/180).v1 / 横类(90/270).i1 (修复前硬编码 .i1, 竖屏用户启动提示取错索引)
    if (!SD.exists(indexPath.c_str())) {
        String legacyPath = recentReadPath + ".i1";
        if (SD.exists(legacyPath.c_str())) indexPath = legacyPath;
    }
    traceFmt("RECENT_INDEX path=%s", indexPath.c_str());
    // 构建中/构建中断进度在 sidecar (indexPath+"p", 如 小说.i1p), 优先于 .i1 记录[0]:
    // 后台构建或中断续建时主页才能与阅读器显示一致 (阅读器 startTxtReader 同优先级)。
    uint32_t savedOffset = 0;
    String sidecarPath = indexPath + "p";
    if (SD.exists(sidecarPath.c_str())) {
        File sp = SD.open(sidecarPath.c_str(), FILE_READ);
        if (sp && sp.size() >= 8) {
            char rec[9];
            for (uint8_t i = 0; i < 8; i++) rec[i] = (char)sp.read();
            rec[8] = '\0';
            savedOffset = strtoul(rec, nullptr, 10);
        }
        if (sp) sp.close();
    }
    File index = SD.open(indexPath.c_str(), FILE_READ);
    if (!index || index.size() < 16 || index.size() % 8 != 0) {
        if (index) index.close();
        // 索引不完整(构建中/中断): 仍视为"有上次阅读文件"(进入后阅读器从 sidecar/记录[0] 恢复,
        // 与 startTxtReader 同优先级), 但主页只显示文件名、不显示页码。记录[0] 尝试读出备用。
        if (savedOffset == 0) {
            File idx = SD.open(indexPath.c_str(), FILE_READ);
            if (idx && idx.size() >= 8) {
                char rec[9];
                for (uint8_t i = 0; i < 8; i++) rec[i] = (char)idx.read();
                rec[8] = '\0';
                savedOffset = strtoul(rec, nullptr, 10);
            }
            if (idx) idx.close();
        }
        recentReadPage = 0;
        recentReadTotalPages = 0;
        recentReadValid = true;   // 文件存在即视为有上次阅读记录 (页码可为 0)
        traceFmt("RECENT_INCOMPLETE valid=1 page=0 total=0 off=%lu", (unsigned long)savedOffset);
        return;
    }
    recentReadTotalPages = (index.size() / 8) - 1;
    if (savedOffset == 0) {
        char record[9];
        for (uint8_t i = 0; i < 8; i++) record[i] = (char)index.read();
        record[8] = '\0';
        savedOffset = strtoul(record, nullptr, 10);
    }
    recentReadPage = 1;
    // 顺序连续读而非每页 seek: seek 会重载 sector, 大索引(如《武炼巅峰》1.1MB)
    // 14 万次 seek 远超 8s 软看门狗 → Soft WDT 无限复位。批量 read 减少 VFS 调用开销。
    // 必须周期性喂狗, 索引扫描属于长任务。
    // 优化①: 逐条 8B 读以 VFS 调用开销为主 (实测 ~23µs/次, 102k 条 ≈ 2.35s), 改 512B
    // 块读 (64 条/次) → 全表扫 <0.5s。独立静态缓冲, 不共用 indexTaskStep 的 idxBuf
    // (后台索引构建在循环间残留 idxBufPos/idxBufLen, 覆盖会破坏构建)。
    // 优化③: 启动 KEY3 检测窗口并入扫描 — 每块轮询一次 readKey3, 窗口不再串行等待 (见 setup)。
    // 生命周期化: 启动扫描一次性使用 → malloc/free (配网会话不占这 512B 静态 RAM)
    uint8_t *recScanBuf = (uint8_t *)malloc(512);
    if (!recScanBuf) {
        index.close();
        recentReadValid = true;
        traceFmt("RECENT_NOBUF valid=1 page=1 total=%lu off=%lu", (unsigned long)recentReadTotalPages,
                 (unsigned long)savedOffset);
        return;
    }
    uint32_t page = 2;
    while (page <= recentReadTotalPages) {
        uint32_t want = recentReadTotalPages - page + 1;
        if (want > 64) want = 64;
        int got = index.read(recScanBuf, (size_t)want * 8);
        if (got <= 0) break;
        uint32_t recs = (uint32_t)got / 8;
        for (uint32_t i = 0; i < recs; i++) {
            char scanRec[9];
            memcpy(scanRec, &recScanBuf[i * 8], 8);
            scanRec[8] = '\0';
            if (strtoul(scanRec, nullptr, 10) == savedOffset) {
                recentReadPage = page;
                page = recentReadTotalPages + 1;   // 命中: 退出外层循环
                break;
            }
            page++;
        }
        if (page <= recentReadTotalPages) {
            ESP.wdtFeed();
            if (gBootKey3Window) {
                if (readKey3() == 0) {
                    if (gBootKey3PollLow >= 1) gBootKey3Held = true;   // 连续两次低 (块间隔≈10ms) 才算按下
                    else gBootKey3PollLow++;
                } else {
                    gBootKey3PollLow = 0;
                }
            }
        }
    }
    index.close();
    free(recScanBuf);
    recentReadValid = true;
    traceFmt("RECENT_OK valid=1 page=%lu total=%lu off=%lu", (unsigned long)recentReadPage,
             (unsigned long)recentReadTotalPages, (unsigned long)savedOffset);
}

void saveRecentReadPath(const String &path) {
    if (!sdAvailable) return;
    if (!SD.exists("/.tiemereader")) SD.mkdir("/.tiemereader");
    File f = SD.open(RECENT_READ_PATH, "w");
    if (!f) return;
    f.println(path);
    f.close();
}

void clearRecentReadPathIfMatches(const String &path) {
    if (!SD.exists(RECENT_READ_PATH)) return;
    File f = SD.open(RECENT_READ_PATH, FILE_READ);
    if (!f) return;
    String saved = f.readStringUntil('\n');
    f.close();
    saved.trim();
    if (saved == path) SD.remove(RECENT_READ_PATH);
}

void openRecentRead() {
    // 首页已经显示过有效记录时，直接使用缓存路径，避免再次读取 SD 覆盖有效状态。
    if (!recentReadValid || recentReadPath.length() == 0) loadRecentReadSummary();
    traceFmt("RECENT_OPEN valid=%d path=%s page=%lu", recentReadValid ? 1 : 0,
             recentReadPath.c_str(), (unsigned long)recentReadPage);
    if (!recentReadValid || recentReadPath.length() == 0) {
        showMsg("暂无阅读记录", "请先打开TXT");
        return;
    }
    startTxtReader(recentReadPath.c_str(), false);
}

void drawHomeCard(int x, int y, int w, int h, const char *title,
                 const char *detail, bool selected) {
    fillRect(x, y, w, h, selected);
    drawRect(x, y, w, h, !selected);
    drawTextUTF8(x + 4, y + 1, title, w - 8, !selected);
    drawTextUTF8(x + 4, y + 16, detail, w - 8, !selected);
}

void renderHome(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    drawTextUTF8(2, 0, "● ● ●", 64, true);
    drawTextUTF8(70, 0, "MoShuiPing V2.9", 150, true);
    {
        char buf[16];
        int mv = readBatteryMV();
        if (isCharging()) drawLightningIcon(218, 1, true);  // 充电：画闪电图标
        snprintf(buf, sizeof(buf), "电量%d%%", batPercent(mv));
        drawTextUTF8(228, 0, buf, 66, true);
    }
    fillRect(0, 16, SCR_W, 1, true);

    char recentTitle[96];
    char recentDetail[96];
    if (!sdAvailable) {
        snprintf(recentTitle, sizeof(recentTitle), "● 上次阅读");
        snprintf(recentDetail, sizeof(recentDetail), "请插入SD卡");
    } else if (!recentReadValid) {
        traceFmt("HOME_NOREC sd=%d path_len=%u", sdAvailable ? 1 : 0, (unsigned)recentReadPath.length());
        snprintf(recentTitle, sizeof(recentTitle), "● 上次阅读");
        snprintf(recentDetail, sizeof(recentDetail), "暂无阅读记录");
    } else {
        const char *name = strrchr(recentReadPath.c_str(), '/');
        name = name ? name + 1 : recentReadPath.c_str();
        snprintf(recentTitle, sizeof(recentTitle), "● %s", name);
        snprintf(recentDetail, sizeof(recentDetail), "上次阅读");
    }
    drawHomeCard(2, 24, 142, 32, recentTitle, recentDetail, homeSel == 0);
    drawHomeCard(150, 24, 144, 32, "■ 文件管理器", "浏览 SD 卡文件", homeSel == 1);
    drawHomeCard(2, 60, 94, 32, "◎ 时钟", "校准时间", homeSel == 2);
    {
        char weatherDetail[24];
        if (wDataValid) {
            snprintf(weatherDetail, sizeof(weatherDetail), "%s %s℃", wActual.weatherName, wActual.temp);
        } else {
            // 内存无数据时读 SD 缓存（重启后仍显示上次天气摘要, 对齐 A7 主页）
            loadWeatherCache();
            char *sep = strchr(wCachedSummary, '|');
            if (sep && sep != wCachedSummary) {
                *sep = '\0';
                snprintf(weatherDetail, sizeof(weatherDetail), "%s %s℃", wCachedSummary, sep + 1);
                *sep = '|';
            } else {
                snprintf(weatherDetail, sizeof(weatherDetail), "未获取");
            }
        }
        drawHomeCard(101, 60, 94, 32, "○ 天气", weatherDetail, homeSel == 3);
    }
    drawHomeCard(200, 60, 94, 32, "※ 配网", "热点 / Wi-Fi", homeSel == 4);
    drawHomeCard(2, 96, 292, 32, "◆ 设置", "设备参数", homeSel == 5);
    refresh(full);
}

void enterHomeCard() {
    switch (homeSel) {
        case 0: openRecentRead(); break;
        case 1:
            currentPath = "/";
            selIndex = 0;
            topIndex = 0;
            listDir(currentPath.c_str());
            appMode = APP_BROWSER;
            renderAll();
            refresh(true);
            saveSleepRecord();   // 界面快照: 已进入文件管理器根目录
            break;
        case 2:
            appMode = APP_CLOCK_CONNECT;
            clockManagerBegin(renderClockConnect, enterClockPage);
            saveSleepRecord();   // 界面快照: 已进入配网时钟页
            break;
        case 3:
            enterWeatherPage();
            break;
        case 4:
            progressSyncFreeReaderHeap();   // 启动热点前腾堆: 关 txtFile + 清阅读行缓冲 (配网会话堆硬约束)
            freeItemList();   // 大目录 items≈34KB+ 是堆大户, 配网会话堆 ~5KB 必须释放
            fsCacheBuild();   // 进 AP 前扫描 SD 目录树 → LittleFS 缓存（/fs/list 浏览不碰 SD, 避开 SD×AP 崩溃）
            wifiManagerBegin(renderNetworkPage, exitNetworkPage);
            appMode = APP_NETWORK;
            saveSleepRecord();   // 界面快照: 已进入配网页
            break;
        case 5:
            settingsSel = 0;
            settingsTzEdit = false;
            appMode = APP_SETTINGS;
            renderSettingsPage(true);
            saveSleepRecord();   // 界面快照: 已进入设置页
            break;
        default: showMsg("功能暂未实现", "稍后开放"); break;
    }
}

// 配网启动阶段提示页（局刷: 用户要求不用全刷, 全刷阻塞 1s+; 局刷 ~0.45s 不卡）
void renderBootStage(const char *line1, const char *line2) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    drawRect(0, 0, SCR_W, SCR_H, true);
    // 居中两行提示
    int w1 = utf8Width(line1);
    drawTextUTF8((SCR_W - w1) / 2, 42, line1, SCR_W - 8, true);
    if (line2 && line2[0]) {
        int w2 = utf8Width(line2);
        drawTextUTF8((SCR_W - w2) / 2, 64, line2, SCR_W - 8, true);
    }
    refresh(false);
}

// 上传状态页: 上传中 / 上传完毕 / 上传失败（全局刷, 不阻塞上传回调; 由 file_api 状态回调触发）
// ★ 用户要求: 每次都是局部刷新——上传"完毕/失败"先局刷恢复配网页让"上传中"文字消失,
//   再局刷显示新提示(出现), 全程不触发全刷(全刷阻塞 1s+)。
void renderUploadStatus(int phase, const char *path) {
    if (!textRendererReady) return;
    const char *title = NULL, *sub = NULL;
    if (phase == OFS_UP_PHASE_UPLOADING) { title = "上传中"; sub = path; }
    else if (phase == OFS_UP_PHASE_DONE)  { title = "上传完毕"; sub = "文件已保存到 SD 卡"; }
    else if (phase == OFS_UP_PHASE_FAIL)  { title = "上传失败"; sub = "请检查空间/文件名"; }
    else return;   // IDLE 不渲染
    if (!title) return;

    // "完毕/失败": 进入新提示前, 先局刷恢复配网页(清掉"上传中"提示 → 文字消失),
    // 与随后局刷显示新提示(出现)形成"消失→出现", 避免从"上传中"直接变"上传完毕"的残影。
    if (phase == OFS_UP_PHASE_DONE || phase == OFS_UP_PHASE_FAIL) {
        renderNetworkPage(false);   // 恢复配网页(局刷), 提示框消失
    }

    // 居中提示框（局刷）
    const int boxW = 200, boxH = 74;
    const int x = (SCR_W - boxW) / 2, y = (SCR_H - boxH) / 2;
    fillRect(x, y, boxW, boxH, false);
    drawRect(x, y, boxW, boxH, true);
    int wt = utf8Width(title);
    drawTextUTF8(x + (boxW - wt) / 2, y + 12, title, boxW - 12, true);
    if (sub && sub[0]) {
        // 副标题截短到框内
        char buf[64];
        size_t n = strlen(sub);
        // 只显示 basename（末尾路径段）
        const char *b = sub;
        for (const char *p = sub; *p; p++) if (*p == '/') b = p + 1;
        snprintf(buf, sizeof(buf), "%s", b);
        int ws = utf8Width(buf);
        while (ws > boxW - 16 && strlen(buf) > 1) { buf[strlen(buf) - 1] = '\0'; ws = utf8Width(buf); }
        drawTextUTF8(x + (boxW - ws) / 2, y + 42, buf, boxW - 12, true);
    }
    refresh(false);
}

void renderNetworkPage(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    drawTextUTF8(4, 2, "网络配网", 120, true);
    drawTextUTF8(4, 22, wifiManagerStateText(), 288, true);
    char line[96];
    snprintf(line, sizeof(line), "热点: %s", wifiManagerApSsid());
    drawTextUTF8(4, 42, line, 288, true);
    drawTextUTF8(4, 62, "密码: 333333333", 288, true);
    drawTextUTF8(4, 82, "地址: 192.168.4.1", 288, true);
    const char *staIp = wifiManagerStaIp();
    if (staIp && staIp[0]) {
        snprintf(line, sizeof(line), "STA: %s", staIp);
        drawTextUTF8(4, 102, line, 288, true);
    } else {
        drawTextUTF8(4, 102, "中键长按退出", 288, true);
    }
    refresh(full);
}

void exitNetworkPage() {
    int ret = gNetworkReturnMode;
    gNetworkReturnMode = APP_HOME;
    if (ret == APP_READER) {
        // 从阅读菜单进入: 恢复阅读页 (局刷重绘正文)
        appMode = APP_READER;
        progressSyncRestoreReaderHeap();   // 退出配网: 重开 txtFile + 重读当前页 (Free hook 已关句柄/清行缓冲)
        renderTxtPage(false);
    } else {
        appMode = APP_HOME;
        renderHome(true);
    }
    saveSleepRecord();
}

void renderClockConnect(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    drawTextUTF8(4, 2, "时间校准", 100, true);
    const int y = 42;
    const bool routerOk = clockManagerStage() >= 2;
    const bool internetOk = clockManagerStage() >= 3;
    drawTextUTF8(25, y, "设备", 42, true);
    drawTextUTF8(126, y, "路由器", 56, routerOk);
    drawTextUTF8(238, y, "互联网", 56, internetOk);
    drawTextUTF8(66, y + 1, routerOk ? "━━▶" : "···", 52, true);
    drawTextUTF8(178, y + 1, internetOk ? "━━▶" : "···", 52, true);
    drawTextUTF8(4, 76, clockManagerStageText(), 288, true);
    if (clockManagerIsActive()) {
        // 对齐 A7 校准页文案（交互保持右键长按跳过）
        drawTextUTF8(4, 96, "手动跳过校准", 288, true);
        drawTextUTF8(4, 112, "此时 按下按键3 可跳过校准", 288, true);
    } else if (clockManagerSyncSucceeded()) {
        // A7 对齐: 同步时间(校时)后显示时钟芯片写入结果
        // (成功=读取数据正常; 失败=数据出错或不存在，使用软件时钟)
        drawTextUTF8(4, 100, clockManagerClockChipText(), 288, true);
    } else {
        drawTextUTF8(4, 100, "右键长按：进入时钟", 288, true);
    }
    refresh(full);
}

static time_t lastClockDisplayedMinute = 0;

// 获取一言（仅 WiFi 已连且设置开启时；失败静默不打扰时钟页）
void fetchHitokotoFlow() {
    yiyanText[0] = '\0';
    if (settingsGetHitokotoEnabled() == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;
    char err[16];
    if (!fetchHitokoto(yiyanText, sizeof(yiyanText), err, sizeof(err))) {
        yiyanText[0] = '\0';
        debugFmt("HITOKOTO_FAIL %s", err);
    } else {
        debugFmt("HITOKOTO_OK len=%u", (unsigned)strlen(yiyanText));
    }
}

void enterClockPage() {
    appMode = APP_CLOCK;
    lastClockDisplayedMinute = clockManagerNow() / 60;
    // 调试: 打印进入时钟页时的 epoch 与本地时间
    {
        time_t dbgNow = clockManagerNow();
        struct tm *dbgTm = dbgNow > 1600000000UL ? localtime(&dbgNow) : nullptr;
        char dbgBuf[40];
        if (dbgTm) {
            snprintf(dbgBuf, sizeof(dbgBuf), "%04d-%02d-%02d %02d:%02d:%02d", dbgTm->tm_year + 1900,
                     dbgTm->tm_mon + 1, dbgTm->tm_mday, dbgTm->tm_hour, dbgTm->tm_min, dbgTm->tm_sec);
        } else {
            snprintf(dbgBuf, sizeof(dbgBuf), "(invalid)");
        }
        debugFmt("CLOCK_PAGE_ENTER epoch=%lu local=%s", (unsigned long)dbgNow, dbgBuf);
    }
    // 先联网获取一言（屏幕仍显示上一页），成功后随全刷一并显示
    fetchHitokotoFlow();
    renderClockPage(true);
    saveSleepRecord();   // 界面快照: 已进入时钟页
}

// 老板快捷键: 阅读页中长按 → 局刷伪装成时钟 (校准状态), 停用全部按键。
// 伪装 = 数码管时钟页 (renderClockPage 内容), 恒横屏 (fbRot=90, 与阅读方向无关), 不联网不校准。
// 退出: 仅 KEY1 硬件复位 → 开机 1 秒 KEY3 窗口内按 KEY3 → 回主页 (复用 setup 的 key3Held 逻辑);
//       复位后未按 KEY3 → 按睡眠记录恢复伪装页 (保持伪装, 不暴露阅读器)。
void enterClockDisguise() {
    appMode = APP_CLOCK_DISGUISE;
    fbRot = 90;                  // 时钟恒横屏
    yiyanText[0] = '\0';         // 伪装不联网: 不显示一言 (避免暴露联网能力)
    renderClockPage(false);      // 局刷显示时钟页 (校准状态), 与翻页同效: 只清空文字局刷, 不闪屏
    saveSleepRecord();           // 界面快照: 伪装模式 (复位后按记录恢复)
    debugLine("DISGUISE_ENTER");
}

void renderClockPage(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    time_t now = clockManagerNow();
    struct tm *tmNow = now > 1600000000UL ? localtime(&now) : nullptr;
    char line[40];
    if (tmNow) {
        const int dw = 44, dh = 64, dt = 6, gap = 6, colonW = 16;   // dh=64: 底部腾出 68-94 行给一言
        const int totalW = dw * 4 + gap * 3 + colonW;
        int x = (SCR_W - totalW) / 2;
        int y = 4;
        int h24 = tmNow->tm_hour;
        int dispH = h24;
        bool isAm = true;
        if (settingsGetClockFormat() == 1) {
            isAm = h24 < 12;
            dispH = h24 % 12;
            if (dispH == 0) dispH = 12;
        }
        snprintf(line, sizeof(line), "%02d:%02d", dispH, tmNow->tm_min);
        drawSevenSegDigit(x, y, dw, dh, dt, line[0], true);
        drawSevenSegDigit(x + dw + gap, y, dw, dh, dt, line[1], true);
        drawClockColon(x + (dw + gap) * 2, y, dh, true);
        drawSevenSegDigit(x + (dw + gap) * 2 + colonW, y, dw, dh, dt, line[3], true);
        drawSevenSegDigit(x + (dw + gap) * 2 + colonW + dw + gap, y, dw, dh, dt, line[4], true);
        snprintf(line, sizeof(line), "%04d年%02d月%02d日", tmNow->tm_year + 1900, tmNow->tm_mon + 1, tmNow->tm_mday);
        drawTextUTF8(178, 103, line, 114, true);
        if (settingsGetClockFormat() == 1) {
            drawTextUTF8(258, 8, isAm ? "上午" : "下午", 34, true);
        }
        // 一言（数码管与分割线之间，基线 78 → 字占 75-91）
        if (yiyanText[0]) drawTextUTF8(4, 78, yiyanText, 288, true);
    } else {
        drawTextUTF8(78, 25, "时间未知", 140, true);
    }
    fillRect(0, 94, SCR_W, 1, true);
    if (clockManagerWasSkipped()) {
        // 本次跳过了联网校准: 视为未校准 (用户要求: 跳过 ≠ 校准, 不显示"0分钟前校准")
        drawTextUTF8(4, 103, "未校准", 174, true);
    } else {
        drawTextUTF8(4, 103, clockManagerIsSynced() ? "已校准" : "未校准", 174, true);
    }
    refresh(full);
}

// ---------- 设置页面 ----------
// 设置项: 0=时钟格式 1=时区偏移 2=一言开关 3=恢复默认

void formatTzOffset(int16_t min, char *buf, size_t size) {
    int sign = min < 0 ? -1 : 1;
    int h = (min < 0 ? -min : min) / 60;
    int m = (min < 0 ? -min : min) % 60;
    if (m == 0) {
        snprintf(buf, size, "UTC%c%d", sign > 0 ? '+' : '-', h);
    } else {
        snprintf(buf, size, "UTC%c%d:%02d", sign > 0 ? '+' : '-', h, m);
    }
}

void renderSettingsPage(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    drawTextUTF8(4, 2, "设备设置", 100, true);
    fillRect(0, 14, SCR_W, 1, true);
    SettingsConfig s;
    loadSettingsConfig(s);
    char line[48];
    const int rowY[SETTINGS_ITEM_CNT] = { 20, 40, 60, 80 };
    const int rowH = 20;
    for (int i = 0; i < SETTINGS_ITEM_CNT; i++) {
        bool sel = (i == settingsSel);
        fillRect(0, rowY[i], SCR_W, rowH, sel);
        const char *name = nullptr;
        switch (i) {
            case 0: name = "时钟格式"; break;
            case 1: name = "时区偏移"; break;
            case 2: name = "一言"; break;
            case 3: name = "恢复默认"; break;
        }
        drawTextUTF8(6, rowY[i] + 2, name, 110, !sel);
        if (i == 0) {
            snprintf(line, sizeof(line), "%s", s.clockFormat ? "12小时制" : "24小时制");
        } else if (i == 1) {
            formatTzOffset(s.tzOffsetMin, line, sizeof(line));
        } else if (i == 2) {
            snprintf(line, sizeof(line), "%s", s.hitokotoEnabled ? "开启" : "关闭");
        } else {
            snprintf(line, sizeof(line), "%s", "执行");
        }
        drawTextUTF8(150, rowY[i] + 2, line, 140, !sel);
    }
    if (settingsTzEdit) {
        // 编辑态指示条（覆盖底部，局部提示）
        drawTextUTF8(4, 104, "中/右短按调整 右长保存 中长取消", 288, true);
    } else {
        drawTextUTF8(4, 104, "右长执行 中长返回", 288, true);
    }
    refresh(full);
}

void settingsHandleKeys(int r2, int r3) {
    if (settingsTzEdit) {
        // 时区编辑态
        if (r2 == 1) {                       // 中短: -30 分钟
            SettingsConfig s;
            loadSettingsConfig(s);
            int16_t v = s.tzOffsetMin - 30;
            if (v < -720) v = -720;
            settingsSetTzOffsetMin(v);
            configTime(v * 60, 0, "cn.pool.ntp.org");   // 立即生效（当前会话）
            renderSettingsPage(false);
        } else if (r3 == 1) {                // 右短: +30 分钟
            SettingsConfig s;
            loadSettingsConfig(s);
            int16_t v = s.tzOffsetMin + 30;
            if (v > 840) v = 840;
            settingsSetTzOffsetMin(v);
            configTime(v * 60, 0, "cn.pool.ntp.org");   // 立即生效（当前会话）
            renderSettingsPage(false);
        } else if (r3 == 2) {                // 右长: 保存退出编辑
            settingsTzEdit = false;
            renderSettingsPage(false);
        } else if (r2 == 2) {                // 中长: 取消（不保存修改）
            settingsTzEdit = false;
            renderSettingsPage(false);
        }
        delay(30);
        return;
    }
    if (r2 == 1) {                           // 中短: 上移
        settingsSel = (settingsSel + SETTINGS_ITEM_CNT - 1) % SETTINGS_ITEM_CNT;
        renderSettingsPage(false);
    } else if (r3 == 1) {                    // 右短: 下移
        settingsSel = (settingsSel + 1) % SETTINGS_ITEM_CNT;
        renderSettingsPage(false);
    } else if (r3 == 2) {                    // 右长: 执行
        switch (settingsSel) {
            case 0: {                        // 时钟格式切换
                SettingsConfig s;
                loadSettingsConfig(s);
                settingsSetClockFormat(s.clockFormat ? 0 : 1);
                renderSettingsPage(false);
                break;
            }
            case 1: {                        // 进入时区编辑
                settingsTzEdit = true;
                renderSettingsPage(false);
                break;
            }
            case 2: {                        // 一言开关切换
                SettingsConfig s;
                loadSettingsConfig(s);
                settingsSetHitokotoEnabled(s.hitokotoEnabled ? 0 : 1);
                renderSettingsPage(false);
                break;
            }
            case 3: {                        // 恢复默认
                SettingsConfig s;
                memset(&s, 0, sizeof(s));
                s.clockFormat = 0;
                s.tzOffsetMin = 480;
                s.hitokotoEnabled = 1;
                saveSettingsConfig(s);
                configTime(480 * 60, 0, "cn.pool.ntp.org");   // 立即生效（当前会话）
                showMsg("已恢复默认", "24小时 / UTC+8 / 一言开");
                break;
            }
        }
    } else if (r2 == 2) {                    // 中长: 返回首页
        appMode = APP_HOME;
        renderHome(true);
        saveSleepRecord();
    }
    delay(30);
}

// ---------- 天气页面 ----------
// 夜间判断：时间未同步不判夜间；用 localtime（configTime +8 已设本地时区）
bool weatherIsNight() {
    time_t now = clockManagerNow();
    if (now <= 1600000000UL) return false;
    struct tm *tmv = localtime(&now);
    if (!tmv) return false;
    uint8_t h = static_cast<uint8_t>(tmv->tm_hour);
    return (h >= 22 || h < 6);
}

void renderWeatherPage(bool full) {
    // 天气壁纸背景（对齐 A7: SD 卡 天气壁纸N.bmp，按天气类型选 2-6；无文件则留空白底）
    fillRect(0, 0, SCR_W, SCR_H, false);
    if (wDataValid && wActual.weatherCode[0]) {
        char wp[32];
        // weatherIconIndex: 0=晴 1=多云 2=阴 3=雨 4=雪 5=雾 → 壁纸 2..6
        snprintf(wp, sizeof(wp), "/天气壁纸%d.bmp", 2 + weatherIconIndex(wActual.weatherCode));
        bmpShowFromSd(wp);   // 画背景（只写非白像素；文件不存在则保持白底留空）
    }
    char line[48];
    if (wDataValid) {
        // ===== V14 布局：左上/右上 3 行小图标+文字 =====
        // 基线 y=14/33/52（drawTextUTF8 传 y=基线-13=1/20/39），图标 13x13 顶部 y=2/21/40
        // 左列（x=1 图标 / x=16 文字）：更新时间 / 城市 / 天气现象
        drawSmallIcon(1, 2, 0, true);
        snprintf(line, sizeof(line), "%c%c:%c%c", wActual.lastUpdate[11], wActual.lastUpdate[12],
                 wActual.lastUpdate[13], wActual.lastUpdate[14]);
        drawTextUTF8(16, 1, line, 120, true);
        drawSmallIcon(1, 21, 1, true);
        drawTextUTF8(16, 20, wActual.city, 130, true);
        drawSmallIcon(1, 40, 2, true);
        drawTextUTF8(16, 39, wActual.weatherName, 130, true);
        // 右列（x=283 图标 / 文字右对齐到 280）：UVI / 湿度 / 风力
        char uviLine[24], humiLine[16], windLine[16];
        snprintf(uviLine, sizeof(uviLine), "UVI %s", wLife.uvi[0] ? wLife.uvi : "未知");
        snprintf(humiLine, sizeof(humiLine), "%s%%", wFuture.humidity);
        snprintf(windLine, sizeof(windLine), "%s级", wFuture.windScale);
        drawSmallIcon(283, 2, 3, true);
        drawTextUTF8(280 - textWidth(uviLine), 1, uviLine, 120, true);
        drawSmallIcon(283, 21, 4, true);
        drawTextUTF8(280 - textWidth(humiLine), 20, humiLine, 120, true);
        drawSmallIcon(283, 40, 5, true);
        drawTextUTF8(280 - textWidth(windLine), 39, windLine, 120, true);
        // ===== 中部: 7 段温度（在左列右端与右列左端之间居中）+ ℃ + 24x24 图标 =====
        const int dw = 34, dh = 44, dt = 5, gap = 4;
        int len = static_cast<int>(strlen(wActual.temp));
        int numW = len * (dw + gap) - gap;
        int blockW = numW + 4 + 20 + 24;          // 数字 + ℃(20) + 图标(24)
        // 可用区间: [16+左列最宽, 280-右列最宽]，温度块在此区间内居中
        int leftEnd = 16 + textWidth(wActual.city);
        int tmpW = textWidth(wActual.weatherName);
        if (tmpW > leftEnd - 16) leftEnd = 16 + tmpW;
        tmpW = textWidth(line);                   // 更新时间行
        if (tmpW > leftEnd - 16) leftEnd = 16 + tmpW;
        int rightStart = 280 - textWidth(uviLine);
        tmpW = 280 - textWidth(humiLine);
        if (tmpW < rightStart) rightStart = tmpW;
        tmpW = 280 - textWidth(windLine);
        if (tmpW < rightStart) rightStart = tmpW;
        int x = (leftEnd + rightStart - blockW) / 2;
        if (x < leftEnd) x = leftEnd;
        if (x + blockW > rightStart) x = rightStart - blockW;
        int y = 6;                                // 顶部 y=6..50（对齐 V14 温度区）
        for (int i = 0; i < len; i++) {
            if (wActual.temp[i] == '-') {
                fillRect(x + dw / 4, y + dh / 2 - dt / 2, dw / 2, dt, true);
            } else {
                drawSevenSegDigit(x, y, dw, dh, dt, wActual.temp[i], true);
            }
            x += dw + gap;
        }
        drawTextUTF8(x + 2, y + dh - 16, "℃", 20, true);
        x += 24;
        drawWeatherIcon(x + 4, y + (dh - 24) / 2, weatherIconIndex(wActual.weatherCode), true);
        // ===== 双横线 y=56/74 + 中间一行（电量百分比；夜间跳过时提示）=====
        fillRect(0, 56, SCR_W, 1, true);
        fillRect(0, 74, SCR_W, 1, true);
        if (wNightSkip) {
            snprintf(line, sizeof(line), "夜间不更新");
        } else {
            snprintf(line, sizeof(line), "电量%d%%", batPercent(readBatteryMV()));
        }
        int tx = (SCR_W - textWidth(line)) / 2;
        if (isCharging()) drawLightningIcon(tx - 10, 58 - 13, true);  // 充电：画闪电图标
        drawTextUTF8(tx, 58, line, 200, true);
        // ===== 底部 3 天预报：三列对齐（今/明/后 + MM-DD | 星期+天气 | 高/低）=====
        // 星期由 RTC 时间推算（date0=今天，date1=明天，date2=后天）
        int wd0 = -1;
        time_t nowT = clockManagerNow();
        if (nowT > 1600000000UL) {
            struct tm *tmv = localtime(&nowT);
            if (tmv) wd0 = tmv->tm_wday;
        }
        static const char *const dayTag[3] = {"今", "明", "后"};
        char col0[3][16], col1[3][32], col2[3][16];
        int c0w[3], c1w[3], c2w[3], c0max = 0, c1max = 0;
        for (int i = 0; i < 3; i++) {
            snprintf(col0[i], sizeof(col0[i]), "%s %s", dayTag[i], wFuture.date[i]);
            const char *dn = wFuture.textNight[i];
            if (strcmp(wFuture.textDay[i], wFuture.textNight[i]) != 0) {
                snprintf(col1[i], sizeof(col1[i]), "%s %s转%s",
                         (wd0 >= 0) ? weekdayCn(wd0 + i) : "", wFuture.textDay[i], wFuture.textNight[i]);
            } else {
                snprintf(col1[i], sizeof(col1[i]), "%s %s",
                         (wd0 >= 0) ? weekdayCn(wd0 + i) : "", dn);
            }
            snprintf(col2[i], sizeof(col2[i]), "%s/%s", wFuture.high[i], wFuture.low[i]);
            c0w[i] = textWidth(col0[i]);
            c1w[i] = textWidth(col1[i]);
            c2w[i] = textWidth(col2[i]);
            if (c0w[i] > c0max) c0max = c0w[i];
            if (c1w[i] > c1max) c1max = c1w[i];
        }
        // 三行整行居中，取最小 x 为对齐起点（V14 同法）
        int xmin = 999;
        for (int i = 0; i < 3; i++) {
            int total = c0max + 5 + c1max + 5 + c2w[i];
            int cx = (SCR_W - total) / 2;
            if (cx < xmin) xmin = cx;
        }
        xmin += 2;
        int py = 77;   // 基线 90/108/126 → drawTextUTF8 y=77/95/113
        for (int i = 0; i < 3; i++) {
            drawTextUTF8(xmin, py, col0[i], c0max + 4, true);
            drawTextUTF8(xmin + c0max + 5, py, col1[i], c1max + 4, true);
            drawTextUTF8(xmin + c0max + 5 + c1max + 5, py, col2[i], 60, true);
            py += 18;
        }
    } else if (wFetching) {
        // 对齐 A7 天气获取提示文案（步骤回调实时更新）
        drawTextUTF8(4, 55, wFetchStep[0] ? wFetchStep : "获取天气实况数据", 288, true);
        drawTextUTF8(4, 80, "获取未来天气数据/生活指数", 288, true);
    } else if (wNightSkip) {
        drawTextUTF8(4, 55, "夜间不更新，请白天查看", 288, true);
    } else if (wErrCode[0]) {
        // 错误提示：对齐 A7（连接超时用 A7 原文），其余中文说明 + 状态码
        char line[48];
        if (strstr(wErrCode, "TIMEOUT") || strstr(wErrCode, "TIME")) {
            drawTextUTF8(4, 55, "* 连接超时 *", 288, true);
        } else {
            snprintf(line, sizeof(line), "%s [%s]", weatherErrorText(wErrCode), wErrCode);
            drawTextUTF8(4, 55, line, 288, true);
        }
        bool needCfg = (strcmp(wErrCode, "NOKEY") == 0 || strcmp(wErrCode, "NOCFG") == 0);
        drawTextUTF8(4, 80, needCfg ? "请到配网页设置" : "右键长按重试", 288, true);
    } else {
        drawTextUTF8(4, 55, "获取天气实况数据", 288, true);
    }
    refresh(full);
}

// 天气错误小窗：保留当前页面上下文，只在窗口区域叠加错误提示并局部刷新。
// 适用于已有缓存数据的场景（缓存页继续显示，不整页重绘）。
void renderWeatherErrorOverlay(const char *code) {
    const int wx = 12, wy = 100, ww = 272, wh = 24;
    char line[48];
    snprintf(line, sizeof(line), "%s [%s]", weatherErrorText(code), code);
    fillRect(wx, wy, ww, wh, false);
    drawRect(wx, wy, ww, wh, true);
    drawTextUTF8(wx + 6, wy + 4, line, ww - 12, true);
    refresh(false);
}

// 天气获取步骤回调（对齐 A7: 每个端点成功后更新步骤提示并局部刷新）
void weatherStepCb(int step) {
    static const char *const steps[3] = {"获取天气实况数据", "获取未来天气数据", "获取生活指数"};
    if (step >= 0 && step < 3) {
        snprintf(wFetchStep, sizeof(wFetchStep), "%s", steps[step]);
        renderWeatherPage(false);
    }
}

void fetchWeatherFlow(bool force) {
    // 夜间自动跳过（force=手动刷新时跳过夜间限制）
    if (!force && weatherIsNight()) {
        wNightSkip = true;
        renderWeatherPage(false);   // 有缓存: 显示缓存+提示; 无缓存: 显示提示页
        return;
    }
    wNightSkip = false;
    WeatherConfig wc;
    loadWeatherConfig(wc);
    if (wc.key[0] == '\0') {
        wFetching = false;
        strncpy(wErrCode, "NOKEY", sizeof(wErrCode) - 1);
        wErrCode[sizeof(wErrCode) - 1] = '\0';
        if (wDataValid) renderWeatherErrorOverlay(wErrCode);
        else renderWeatherPage(false);
        return;
    }
    // WiFi 未连接且已配置凭据 → 重连（15s 超时，循环内喂狗）；
    // 失败区分"未配置WiFi凭据"(NOCFG) 与"有凭据但连不上"(NOWIFI)
    if (!wifiManagerEnsureSta(15000)) {
        wFetching = false;
        strncpy(wErrCode, wifiManagerHasCredentials() ? "NOWIFI" : "NOCFG",
                sizeof(wErrCode) - 1);
        wErrCode[sizeof(wErrCode) - 1] = '\0';
        if (wDataValid) renderWeatherErrorOverlay(wErrCode);
        else renderWeatherPage(false);
        return;
    }
    if (!wDataValid) {
        wFetching = true;
        wFetchStep[0] = '\0';
        renderWeatherPage(false);   // 无缓存: 先显示"获取中"
    }
    char err[16];
    bool ok = fetchWeather(&wActual, &wFuture, &wLife, wc.key, wc.city, err, sizeof(err), weatherStepCb);
    wFetching = false;
    if (ok) {
        wDataValid = true;
        wErrCode[0] = '\0';
        saveWeatherCache();   // 主页摘要持久化（重启后仍显示）
        renderWeatherPage(false);
    } else {
        strncpy(wErrCode, err, sizeof(wErrCode) - 1);
        wErrCode[sizeof(wErrCode) - 1] = '\0';
        // 已有缓存数据: 保留缓存页，仅叠加错误小窗（不整页重绘）
        if (wDataValid) renderWeatherErrorOverlay(wErrCode);
        else renderWeatherPage(false);
    }
}

void enterWeatherPage() {
    appMode = APP_WEATHER;
    renderWeatherPage(true);        // 全刷一次（显示缓存或"获取中"）
    fetchWeatherFlow(false);        // 进入自动获取（内部含夜间判断，完成后局部刷新）
    saveSleepRecord();              // 界面快照: 已进入天气页
}

void formatSize(uint32_t sz, char *buf, int buflen) {
    if (sz >= 10000000)      snprintf(buf, buflen, "%luMB", (unsigned long)(sz / 1000000));
    else if (sz >= 1000000)  snprintf(buf, buflen, "%lu.%luMB", (unsigned long)(sz / 1000000), (unsigned long)((sz / 100000) % 10));
    else if (sz >= 10000)    snprintf(buf, buflen, "%luKB", (unsigned long)(sz / 1000));
    else if (sz >= 1000)     snprintf(buf, buflen, "%lu.%luKB", (unsigned long)(sz / 1000), (unsigned long)((sz / 100) % 10));
    else                     snprintf(buf, buflen, "%luB", (unsigned long)sz);
}

bool reinitSdBus(const char *reason) {
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(5, HIGH);
    pinMode(5, OUTPUT);
    SPI.begin();
    bool ok = SD.begin(5, SD_SCK_MHZ(20));
    traceFmtLevel(ok ? 'I' : 'E', "SD_REINIT reason=%s ok=%d", reason ? reason : "?", ok ? 1 : 0);
    return ok;
}

// 小写扩展名比较辅助（纯 C, 零堆分配; 目录枚举循环内禁止 String —— 15KB 堆碎片化
// 会导致后续 String 构造失败 → 大目录只枚举到 239 个就全部被当空名跳过, 实测）
static bool extIs(const char *name, const char *ext) {
    if (!name || !ext) return false;
    size_t nl = strlen(name), el = strlen(ext);
    if (el == 0 || nl < el) return false;
    const char *p = name + nl - el;
    for (size_t i = 0; i < el; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c += 32;   // 转小写
        if (c != ext[i]) return false;
    }
    return true;
}

bool isBlacklistedEntry(const char *name) {
    if (!name || !name[0]) return true;
    if (name[0] == '.') return true;
    // 隐藏索引/章节/sidecar 文件 (对齐 A7 过滤表 .i1/.i2/.z1/.z2; 复刻竖屏 .v1/.vz1 + sidecar .i1p/.v1p)
    // .bm=标签存储 / .bmt=标签删除两阶段临时文件 (仅影响浏览器展示, 标签代码按路径直开不受限)
    // 纯 C 后缀匹配（免 String 分配, 大目录枚举稳定）
    const char *dot = strrchr(name, '.');
    if (dot) {
        if (extIs(dot, ".i1") || extIs(dot, ".z1") || extIs(dot, ".i2") ||
            extIs(dot, ".z2") || extIs(dot, ".v1") || extIs(dot, ".vz1") ||
            extIs(dot, ".i1p") || extIs(dot, ".v1p") ||
            extIs(dot, ".bm") || extIs(dot, ".bmt")) return true;
    }
    // 系统目录（纯 C 比较, 免 String）
    struct { const char *s; size_t len; } blocked[] = {
        { "android", 7 }, { "androud", 7 }, { "found.000", 9 }, { "foud.000", 8 },
        { "lost.dir", 8 }, { "system volume information", 23 },
        { ".timereader", 11 }, { ".tiemereader", 12 }
    };
    size_t nl = strlen(name);
    for (const auto &b : blocked) {
        if (nl == b.len) {
            size_t i = 0;
            for (; i < nl; i++) {
                char c = name[i];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c != b.s[i]) break;
            }
            if (i == nl) return true;
        }
    }
    return false;
}

// 文件白名单 (对齐 A7 官方分类表 0x000ea8b4 "txt(UTF-8),bmp,jpg,ttf,bin"):
// 文件管理器显示 txt/bmp/jpg/ttf/bin 五类 + 目录, 其他格式不显示 (官方同款)
// 纯 C 扩展名匹配（零堆分配）
bool isWhitelistedFile(const char *name) {
    if (!name) return false;
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return false;
    return extIs(dot, ".txt") || extIs(dot, ".bmp") || extIs(dot, ".jpg") ||
           extIs(dot, ".ttf") || extIs(dot, ".bin");
}

// 加载窗口: 从 startIdx 开始填充 winItems[0..LIST_WINDOW-1]
// （openDir 从头扫, 纯目录项扫描 ~ms 级; 窗口化方案的核心, 大目录不占堆）
void loadListWindow(const char *path, int startIdx) {
    winCount = 0;
    if (startIdx < 0) startIdx = 0;
    if (startIdx >= itemCount) startIdx = itemCount > 0 ? itemCount - 1 : 0;
    Dir w = SDFS.openDir(path);
    int skipped = 0;
    while (w.next()) {
      String baseName = w.fileName();   // fs::Dir 返回 String; 过滤函数已纯 C（零额外分配）
      const char *nm = baseName.c_str();
      if (isBlacklistedEntry(nm)) continue;
      // 白名单（SDFS._lfn 已扩 256B, 完整文件名正确判定）
      if (!w.isDirectory() && !isWhitelistedFile(nm)) continue;
      if (skipped < startIdx) { skipped++; continue; }   // 跳过窗口之前的项
      if (winCount < LIST_WINDOW) {
        strncpy(winItems[winCount].name, nm, sizeof(winItems[winCount].name) - 1);
        winItems[winCount].name[sizeof(winItems[winCount].name) - 1] = '\0';
        winItems[winCount].size = w.isDirectory() ? 0 : (uint64_t)w.fileSize();
        winItems[winCount].isDir = w.isDirectory();
        winCount++;
      } else break;
      ESP.wdtFeed();
    }
    topIndex = startIdx;
    traceFmtLevel('I', "WIN_LOAD path=%s start=%d win=%d total=%d",
                  path ? path : "(null)", startIdx, winCount, itemCount);
}

bool listDir(const char *path) {
    uint32_t started = millis();
    itemCount = 0;
    // EPD 与 SD 共用 SPI；每次目录操作前恢复 SD 的片选和总线状态。
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(5, HIGH);
    pinMode(5, OUTPUT);
    SPI.begin();
    bool sdBusOk = SD.begin(5, SD_SCK_MHZ(20));
    traceFmtLevel(sdBusOk ? 'I' : 'E', "SD_REINIT path=%s ok=%d", path ? path : "(null)", sdBusOk ? 1 : 0);
    if (!sdBusOk) return false;
    // 使用官方 A7 同款枚举: SDFS.openDir + Dir::next()（纯目录项扫描, 零重开文件）。
    // ⚠️ File::openNextFile() 内部每项 openFile("r") 重开文件 → 大目录(600+) O(n²) 路径解析 +
    //    低堆 malloc 失败 → 首次即 null（实测某些文件夹读不出）; rewindDirectory 是安慰剂, 已弃用。
    diagFlushSd(true);
    bool traceWasOpen = (bool)traceFile;
    if (traceWasOpen) traceFile.close();
    {
      File probe = SD.open(path, FILE_READ);
      bool okDir = probe && probe.isDirectory();
      if (probe) probe.close();
      if (!okDir) return false;
    }
    // 第一遍: 只计数（过滤后）—— 总数 itemCount, 不占堆（分页窗口化）
    // fileName() 返回 String（fs::Dir 包装层）; 过滤函数已纯 C 化（零额外分配）,
    // 每文件仅此 1 次 String 分配, 大幅降低 15KB 堆碎片压力（原 3 次/文件导致 239 bug）
    {
      int rawCount = 0;
      Dir cnt = SDFS.openDir(path);
      while (cnt.next()) {
        rawCount++;
        String baseName = cnt.fileName();
        const char *nm = baseName.c_str();
        if (isBlacklistedEntry(nm)) continue;
        // 白名单: 目录始终显示; 文件只显示 txt/bmp/jpg/ttf/bin (对齐 A7 分类表 0x000ea8b4)
        // SDFS._lfn 已扩到 256B: 完整文件名, 长名 txt 不再因截断丢失扩展名被误过滤
        if (!cnt.isDirectory() && !isWhitelistedFile(nm)) continue;
        itemCount++;
        ESP.wdtFeed();
      }
      traceFmtLevel('I', "DIR_RAW path=%s raw=%d kept=%d", path ? path : "(null)", rawCount, itemCount);
    }
    traceFmtLevel(itemCount == 0 ? 'W' : 'I', "DIR_DONE path=%s count=%d elapsed=%lu",
                  path ? path : "(null)", itemCount, (unsigned long)(millis() - started));
    if (traceWasOpen) {
#if DIAG_SD
        traceFile = SD.open("/debug_trace.log", FILE_WRITE);
        traceLastFlush = millis();
#endif
    }
    // 加载首窗口（起始 0）
    loadListWindow(path, 0);
    return itemCount > 0;
}

void enterDir(int idx) {
    FileItem *it = itemAt(idx);
    if (idx < 0 || idx >= itemCount || !it || !it->isDir) {
        traceFmtLevel('E', "DIR_ENTER_BAD idx=%d count=%d", idx, itemCount);
        return;
    }
    String oldPath = currentPath;
    if (currentPath == "/") currentPath = String("/") + it->name + "/";
    else currentPath = currentPath + it->name + "/";
    selIndex = 0; topIndex = 0;
    bool ok = listDir(currentPath.c_str());
    traceFmtLevel(ok ? 'I' : 'W', "DIR_ENTER to=%s count=%d", currentPath.c_str(), itemCount);
    saveSleepRecord();   // 界面快照: 已进入子目录
}

void upDir() {
    if (currentPath == "/") return;
    // 找倒数第二个 '/'（去掉最后一段目录名, 保留上级路径）
    int lastSlash = currentPath.lastIndexOf('/');
    String parent = currentPath.substring(0, lastSlash);   // 去掉尾部 '/' 及之后的目录名
    int prevSlash = parent.lastIndexOf('/');
    if (prevSlash < 0) {
        currentPath = "/";
    } else {
        currentPath = parent.substring(0, prevSlash + 1);
    }
    selIndex = 0; topIndex = 0;
    listDir(currentPath.c_str());
    saveSleepRecord();   // 界面快照: 已返回上级目录
}

// ---------- 绘制 ----------
#define LIST_Y0 20
#define ROW_H 16
#define TITLE_Y 0

// 分离文件名和扩展名 (UTF-8 安全)
// name 不含扩展名, ext 含点 (如 ".txt"); 无扩展名时 ext 为空
void splitNameExt(const char *full, char *name, int nameLen, char *ext, int extLen) {
    const char *dot = NULL;
    const char *p = full;
    while (*p) {
        if (*p == '.') dot = p;
        uint8_t c = (uint8_t)*p;
        if (c < 0x80) p++;
        else if (c >= 0xE0 && p[1] && p[2]) p += 3;
        else if (c >= 0xC0 && p[1]) p += 2;
        else p++;
    }
    if (dot && dot > full) {
        size_t n = dot - full;
        if (n >= (size_t)nameLen) n = nameLen - 1;
        memcpy(name, full, n);
        name[n] = '\0';
        snprintf(ext, extLen, "%s", dot);
    } else {
        strncpy(name, full, nameLen - 1);
        name[nameLen - 1] = '\0';
        ext[0] = '\0';
    }
}

// 标题行: 当前路径 (截断) + 页码
void renderTitle() {
    fillRect(0, TITLE_Y, SCR_W, 16, false);
    // 路径显示: 若根目录 "/", 否则显示最后一段
    String show = currentPath;
    if (show.length() > 1) {
        String t = show.substring(0, show.length() - 1);
        int lastSlash = t.lastIndexOf('/');
        show = t.substring(lastSlash + 1);
        show = "/" + show;
    }
    char pathBuf[80];
    show.toCharArray(pathBuf, 80);
    char disp[48];
    utf8Truncate(pathBuf, disp, 220, sizeof(disp));
    drawTextUTF8(2, TITLE_Y, disp, 220, true);

    char page[16];
    snprintf(page, sizeof(page), "%d/%d", selIndex + 1, itemCount);
    drawTextUTF8(SCR_W - 4 - utf8Width(page), TITLE_Y, page, 40, true);
    // 分隔线
    fillRect(0, 16, SCR_W, 1, true);
}

void renderListRow(int row, int idx) {
    int y = LIST_Y0 + row * ROW_H;
    FileItem *it = itemAt(idx);   // 窗口化: idx 可能在窗口外(滚动中), 返回 NULL 画空行
    bool selected = (idx == selIndex);
    bool fg = !selected;   // 前景色: 选中时白字
    // 整行底色
    fillRect(0, y, SCR_W, ROW_H, selected);
    if (!it) return;   // 窗口外: 只画底色
    int x = 2;
    drawChar16(x, y, it->isDir ? '>' : ' ', fg);
    x += 16;

    if (it->isDir) {
        // 文件夹: 名称 + '/'
        char name[48];
        utf8Truncate(it->name, name, 250, sizeof(name));
        int ex = drawTextUTF8(x, y, name, 250, fg);
        drawChar16(ex, y, '/', fg);
    } else {
        // 文件: 名称 + 后缀 (16px 与文件名一致)
        char nm[40], ext[8];
        splitNameExt(it->name, nm, sizeof(nm), ext, sizeof(ext));
        int extW = utf8Width(ext);
        int nameMaxW = SCR_W - x - 4 - extW;
        char nameDisp[48];
        utf8Truncate(nm, nameDisp, nameMaxW, sizeof(nameDisp));
        int ex = drawTextUTF8(x, y, nameDisp, nameMaxW, fg);
        drawTextUTF8(ex, y, ext, extW + 4, fg);   // 后缀 16px
    }
}

void renderList() {
    for (int row = 0; row < LIST_ROWS; row++) {
        int idx = topIndex + row;
        if (idx < itemCount) renderListRow(row, idx);
        else fillRect(0, LIST_Y0 + row * ROW_H, SCR_W, ROW_H, false);
    }
}

void renderAll() {
    fillRect(0, 0, SCR_W, SCR_H, false);
    renderTitle();
    renderList();
}

// 纯高亮移动 (不滚动): 重绘旧选中行(恢复白底) + 新选中行(高亮)
void renderRowChange(int oldIdx, int newIdx) {
    int rowOld = oldIdx - topIndex;
    if (rowOld >= 0 && rowOld < LIST_ROWS) renderListRow(rowOld, oldIdx);
    int rowNew = newIdx - topIndex;
    if (rowNew >= 0 && rowNew < LIST_ROWS) renderListRow(rowNew, newIdx);
}

// ---------- 功能框 (菜单) ----------
// 模式: 0=浏览列表, 1=功能框
int mode = 0;
int menuSel = 0;         // 功能框选中项 0..MENU_CNT-1
#define MENU_CNT 4        // 返回/打开/删除/重建
const char *menuLabels[MENU_CNT] = {"返回", "打开", "删除", "重建"};
String rebuildConfirmPath = "";   // 非空 = 重建确认框打开 (长按重建, 短按退出)

// 判断文件名是否为 BMP 图片（不区分大小写）
bool isBmpFile(const char *name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'b' || ext[1] == 'B') &&
            (ext[2] == 'm' || ext[2] == 'M') &&
            (ext[3] == 'p' || ext[3] == 'P'));
}

// 全屏显示 SD 卡 BMP：进入 APP_BMP，绘制后全刷；失败自动恢复文件管理器
void showBmpFile(const char *path) {
    appMode = APP_BMP;
    fillRect(0, 0, SCR_W, SCR_H, false);   // 白底
    if (!bmpShowFromSd(path)) {
        appMode = APP_BROWSER;
        renderAll();
        refresh(true);
        showMsg("打开失败", "不支持的图片格式");
        return;
    }
    refresh(true);
    debugFmt("BMP_SHOW %s", path);
}

// 从文件管理器打开 BMP 图片（返回时回到文件管理器）
void startBmpViewer(const char *path) {
    freeItemList();   // 大目录 items≈34KB+ 是堆大户, 进图片前释放
    showBmpFile(path);
    saveSleepRecord();   // 界面快照: 图片浏览（唤醒恢复为文件管理器）
}

// 功能框区域 (弹窗, 覆盖列表中央)
#define MENU_X0 20
#define MENU_X1 (SCR_W - 20)   // 276
#define MENU_Y0 44
#define MENU_H 40
#define MENU_BTN_H 28
#define MENU_BTN_W 62
#define MENU_BTN_GAP 6
#define MENU_BTN_Y (MENU_Y0 + (MENU_H - MENU_BTN_H) / 2)   // 52

void renderMenuBar() {
    // 白底弹窗 + 黑框
    fillRect(MENU_X0, MENU_Y0, MENU_X1 - MENU_X0, MENU_H, false);
    drawRect(MENU_X0, MENU_Y0, MENU_X1 - MENU_X0, MENU_H, true);

    int totalW = MENU_CNT * MENU_BTN_W + (MENU_CNT - 1) * MENU_BTN_GAP;
    int startX = (SCR_W - totalW) / 2;
    for (int i = 0; i < MENU_CNT; i++) {
        int bx = startX + i * (MENU_BTN_W + MENU_BTN_GAP);
        bool sel = (i == menuSel);
        // 按钮底
        fillRect(bx, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H, sel);
        drawRect(bx, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H, !sel);
        // 文字居中
        int tw = utf8Width(menuLabels[i]);
        drawTextUTF8(bx + (MENU_BTN_W - tw) / 2, MENU_BTN_Y + 6, menuLabels[i], tw + 2, !sel);
    }
}

void showMenu() {
    mode = 1;
    menuSel = 0;
    renderMenuBar();
    refresh(false);   // 不全刷
}

void hideMenu() {
    mode = 0;
    renderAll();      // 恢复完整列表 (功能框覆盖的几行需要重绘)
    refresh(false);   // 不全刷
}

// 重建确认框: 对齐官方 A7 "真的要重建吗！\n长按重建，短按退出" (小窗局刷, 不离开列表页)
void renderConfirmRebuild() {
    const int x = 40, y = 32, w = SCR_W - 80, h = 62;
    fillRect(x, y, w, h, false);
    drawRect(x, y, w, h, true);
    drawTextUTF8(x + 4, y + 8, "真的要重建吗！", w - 8, true);
    drawTextUTF8(x + 4, y + 32, "长按重建，短按退出", w - 8, true);
    refresh(false);
}

// 执行功能框选中项
void execMenu() {
    switch (menuSel) {
        case 0: {  // 返回
            if (currentPath != "/") {
                upDir();
                renderAll();
                refresh(true);
            } else {
                hideMenu();
            }
            break;
        }
        case 1: {  // 打开
            if (itemCount > 0 && selIndex < itemCount) {
                FileItem *it = itemAt(selIndex);
                if (it && it->isDir) {
                    enterDir(selIndex);
                    renderAll();
                    refresh(true);
                } else if (it) {
                    hideMenu();
                    String path = currentPath + it->name;
                    if (isBmpFile(it->name)) {
                        startBmpViewer(path.c_str());
                    } else {
                        startTxtReader(path.c_str(), false);
                    }
                }
            }
            break;
        }
        case 2: {  // 删除
            if (itemCount > 0 && selIndex < itemCount) {
                FileItem *it = itemAt(selIndex);
                if (it && it->isDir) {
                    hideMenu();
                    showMsg("文件夹删除", "暂不支持");
                } else if (it) {
                    String path = currentPath + it->name;
                    if (SD.remove(path.c_str())) {
                        clearRecentReadPathIfMatches(path);
                        // 删除成功: 刷新列表
                        listDir(currentPath.c_str());
                        if (selIndex >= itemCount) selIndex = itemCount - 1;
                        if (selIndex < 0) selIndex = 0;
                        if (topIndex > selIndex) topIndex = selIndex;
                        renderAll();
                        refresh(true);
                    } else {
                        hideMenu();
                        showMsg("删除失败", "");
                    }
                }
            }
            break;
        }
        case 3: {  // 重建索引
            traceFmt("REBUILD_SELECT count=%d sel=%d", itemCount, selIndex);
            FileItem *it = itemAt(selIndex);
            if (itemCount > 0 && selIndex < itemCount && it && !it->isDir) {
                String path = currentPath + it->name;
                traceFmt("REBUILD_CONFIRM path=%s", path.c_str());
                // 对齐 A7: 先弹确认框 (长按重建, 短按退出), 防误触直接截断重建
                mode = 0;                        // 关功能框, 回浏览态
                rebuildConfirmPath = path;
                renderConfirmRebuild();
            } else {
                traceLine("REBUILD_SKIP invalid selection");
            }
            break;
        }
    }
}
void updateKey(struct KState &k, bool nowDown) {
    if (nowDown && !k.down) {
        k.down = true;
        k.pressStart = millis();
        k.longDone = false;
    }
}

// 返回: 0=无, 1=短按, 2=长按
int scanKey(struct KState &k, bool nowDown) {
    // 电源噪声防抖: EPD 刷新/SD 写卡等大电流脉冲会经电源耦合到按键引脚 (GPIO0/GPIO3),
    // 产生瞬时电平毛刺; 电平变化需持续 KEY_DEBOUNCE_MS 才进入状态机, 否则视为噪声忽略。
    if (nowDown != k.rawStable) {
        k.rawStable = nowDown;
        k.rawChangeAt = millis();
        return 0;
    }
    if (millis() - k.rawChangeAt < KEY_DEBOUNCE_MS) return 0;
    if (nowDown) {
        updateKey(k, true);
        if (!k.longDone && (millis() - k.pressStart) >= LONG_MS) {
            k.longDone = true;
            return 2;  // 长按触发
        }
        return 0;
    } else {
        int r = 0;
        if (k.down && !k.longDone) r = 1;  // 松开, 未长按 → 短按
        k.down = false;
        return r;
    }
}

// ---------- 屏幕 ----------
// 局部动作只做局刷; 进入休眠提示也只覆盖提示区域。
void refresh(bool full) {
    uint32_t started = millis();
    traceFmt("RENDER_START kind=%s fb=%u", full ? "FULL" : "PART", (unsigned)sizeof(fb));
    if (full) epd.display(fb);
    else epd.displayPartial(fb);
    uint32_t elapsed = millis() - started;
    traceFmtLevel(elapsed > 3000 ? 'E' : elapsed > 1500 ? 'W' : 'I',
                  "RENDER_END kind=%s elapsed=%lu", full ? "FULL" : "PART", (unsigned long)elapsed);
}

uint32_t lastPhysicalKeyMs = 0;
const uint32_t AUTO_SLEEP_MS = 5UL * 60UL * 1000UL;

void notePhysicalKeyActivity(int r2, int r3) {
    // 只认真实按键事件(短按/长按边沿), 不认持续按压态: 电源噪声(EPD 刷新/SD 写卡的
    // 大电流脉冲经电源耦合到 GPIO3=RX / GPIO0=DC)会误判"一直按住", 永久刷新休眠计时
    // → 构建期间永不自动休眠。真实交互不存在按住超过 5 分钟的操作, 按键事件足以反映活跃。
    if (r2 || r3) lastPhysicalKeyMs = millis();
}

void drawSleepNotice() {
    // 保留当前页面, 仅在右上角覆盖一条局部提示, 不清屏、不全刷。
    // (用户要求: 去掉 "KEY1唤醒" 文字)
    const int x = 174, y = 0, w = 120, h = 16;
    fillRect(x, y, w, h, false);
    fillRect(x, y, w, 1, true);
    fillRect(x, y + h - 1, w, 1, true);
    drawTextUTF8(x + 3, y + 1, "休眠中", w - 6, true);
    refresh(false);
}

// ---------- 主程序 ----------
void redrawCurrentPage() {
    if (appMode == APP_HOME) {
        renderHome(false);
    } else if (appMode == APP_BROWSER) {
        renderAll();
        refresh(false);
    } else if (appMode == APP_READER) {
        renderTxtPage(false);
    } else if (appMode == APP_CHAPTERS) {
        renderChapterList(false);
    } else if (appMode == APP_MARKS) {
        renderMarkList(false);
    } else if (appMode == APP_SETTINGS) {
        renderSettingsPage(false);
    } else if (appMode == APP_CLOCK) {
        renderClockPage(false);
    } else if (appMode == APP_CLOCK_DISGUISE) {
        fbRot = 90;
        renderClockPage(false);   // 伪装模式保持时钟页
    } else if (appMode == APP_BMP) {
        // 图片浏览被覆盖（如提示）后恢复：回文件管理器
        appMode = APP_BROWSER;
        renderAll();
        refresh(false);
    }
}

void showMsg(const char *msg, const char *msg2) {
    // 用户界面已经存在时，提示只覆盖一个小窗口并局刷；避免无意义全刷。
    if (appMode >= APP_HOME && textRendererReady) {
        const int x = 28, y = 38, w = SCR_W - 56, h = 52;
        fillRect(x, y, w, h, false);
        drawRect(x, y, w, h, true);
        int w1 = utf8Width(msg);
        drawTextUTF8(x + (w - w1) / 2, y + 7, msg, w - 8, true);
        if (msg2 && msg2[0]) {
            int w2 = utf8Width(msg2);
            drawTextUTF8(x + (w - w2) / 2, y + 27, msg2, w - 8, true);
        }
        refresh(false);
        delay(900);
        redrawCurrentPage();
        return;
    }
    fillRect(0, 0, SCR_W, SCR_H, false);
    int y = 40;
    int w = utf8Width(msg);
    drawTextUTF8((SCR_W - w) / 2, y, msg, SCR_W, true);
    if (msg2) {
        int w2 = utf8Width(msg2);
        drawTextUTF8((SCR_W - w2) / 2, y + 20, msg2, SCR_W, true);
    }
    epd.display(fb);
}

// 迷你提示框: 中央小框只包住文字 (用户要求: 不要半屏大框)。
// 仅绘制+局刷, 不阻塞; 由调用方后续全刷/局刷覆盖消失。
void showMiniPrompt(const char *text) {
    if (!textRendererReady) return;
    int tw = utf8Width(text);
    int w = tw + 14, h = 22;
    int x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
    fillRect(x, y, w, h, false);
    drawRect(x, y, w, h, true);
    drawTextUTF8(x + 7, y + 3, text, w - 4, true);
    refresh(false);
}

// ---------- TXT 阅读实现 ----------

// isUtf8Space, decodeUtf8, isChapterNumber, isChapterTitle, txtCharWidth,
// formatIndexNumber, weekdayCn → 已提取到 reader_utils.h/cpp

static void appendChapter(File &chapterFile, const String &line, uint32_t page) {
    char title[64];
    if (!isChapterTitle(line.c_str(), title, sizeof(title))) return;
    chapterFile.print(title);
    chapterFile.print('-');
    chapterFile.println(page);
}

void appendIndexRecord(File &indexFile, uint32_t offset) {
    char record[9];
    formatIndexNumber(offset, record);
    indexFile.print(record);
}

void buildTxtIndex() {
    debugFmt("IDX begin txt=%s size=%lu", txtPath.c_str(), (unsigned long)txtFile.size());
    SD.remove(txtIndexPath.c_str());
    SD.remove(txtChapterPath.c_str());
    File indexFile = SD.open(txtIndexPath.c_str(), FILE_WRITE);
    File chapterFile = SD.open(txtChapterPath.c_str(), FILE_WRITE);
    if (!indexFile || !chapterFile) return;
    appendIndexRecord(indexFile, 0);
    txtFile.seek(0);
    String rows[9];
    int8_t line = 0;
    uint16_t enCount = 0;
    uint16_t chCount = 0;
    uint8_t lineOld = 0;
    bool hskgState = false;
    uint32_t page = 1;
    bool pageStartPending = false;
    uint32_t progressMark = 0;
    while (txtFile.available()) {
        ESP.wdtFeed();
        uint32_t consumed = txtFile.position();
        if (consumed - progressMark >= 1048576UL) {
            progressMark = consumed;
            debugFmt("IDX progress=%lu/%lu page=%lu", (unsigned long)consumed,
                     (unsigned long)txtFile.size(), (unsigned long)page);
        }
        if (lineOld != line) { lineOld = line; hskgState = true; }
        if (pageStartPending && line == 0) {
            pageStartPending = false;
            appendIndexRecord(indexFile, txtFile.position());
        }
        int c = txtFile.read();
        while (c == '\n' && line <= 7) {
            if (line == 0) {
                if (rows[line].length() > 0) { appendChapter(chapterFile, rows[line], page); line++; }
                else rows[line].clear();
            } else if (rows[line].length() > 0) {
                appendChapter(chapterFile, rows[line], page);
                line++;
            } else if (rows[line - 1].length() > 0) {
                line++;
            }
            if (line <= 7) c = txtFile.read();
            enCount = 0; chCount = 0;
        }
        if (c == '\t') {
            rows[line] += rows[line].length() == 0 ? "    " : "       ";
        } else if (!((c >= 0 && c <= 31) || c == 127)) {
            rows[line] += (char)c;
        }
        bool asciiState = false;
        uint8_t b = (uint8_t)c & 0xE0;
        if (b == 0xE0) {
            chCount++;
            c = txtFile.read(); rows[line] += (char)c;
            c = txtFile.read(); rows[line] += (char)c;
        } else if (b == 0xC0) {
            enCount += 14;
            c = txtFile.read(); rows[line] += (char)c;
        } else if (c == '\t') {
            enCount += rows[line] == "    " ? 20 : 28;
        } else if (c >= 0 && c <= 255) {
            enCount += txtCharWidth((uint8_t)c) + 1;
            asciiState = true;
        }
        uint16_t stringLength = enCount + chCount * 14;
        if (stringLength >= 260 && hskgState) {
            if (rows[line].length() >= 4 && rows[line][0] == ' ' && rows[line][1] == ' ' &&
                rows[line][2] == ' ' && rows[line][3] == ' ') enCount += 8;
            hskgState = false;
        }
        if (stringLength >= 283) {
            if (!asciiState) {
                appendChapter(chapterFile, rows[line], page);
                rows[line].clear(); line++; enCount = 0; chCount = 0;
            } else if (stringLength >= 286) {
                int t = txtFile.peek();
                int cz = 286 - stringLength;
                int tLength = txtCharWidth((uint8_t)t);
                uint8_t tb = (uint8_t)t & 0xE0;
                if (tb == 0xE0 || tb == 0xC0 || tLength > cz) {
                    rows[line].clear(); line++; enCount = 0; chCount = 0;
                }
            }
        }
        if (line == 8) {
            pageStartPending = true;
            page++;
            line = 0; enCount = 0; chCount = 0;
            for (uint8_t i = 0; i < 9; i++) rows[i].clear();
        }
    }
    if (line >= 0 && line <= 7 && rows[line].length() > 0) appendChapter(chapterFile, rows[line], page);
    appendIndexRecord(indexFile, txtFile.size());
    indexFile.close();
    chapterFile.close();
    removeLegacyIndexFiles();
    debugFmt("IDX done pages=%lu index=%s chapters=%s", (unsigned long)page,
             txtIndexPath.c_str(), txtChapterPath.c_str());
    File finalIndex = SD.open(txtIndexPath.c_str());
    txtTotalPages = finalIndex ? ((finalIndex.size() / 8) - 1) : 1;
    if (finalIndex) finalIndex.close();
    txtFile.seek(0);
}

void appendChapterTracked(File &chapterFile, const String &line, uint32_t page) {
    char title[64];
    if (!isChapterTitle(line.c_str(), title, sizeof(title))) return;
    // 续建重扫去重: 重扫页 (page == resumeChapterSeedPage) 中, 中断前已写入 .z1 的
    // 章节(含最后一条)跳过不写; 越过该条后恢复正常追加 (之后的章节尚未写入, 不能丢)。
    if (resumeChapterSeedPage == page && resumeChapterSeed[0]) {
        if (strcmp(title, resumeChapterSeed) == 0) resumeChapterSeed[0] = '\0';  // 越过边界
        return;
    }
    chapterFile.print(title);
    chapterFile.print('-');
    chapterFile.println(page);
    chapterFile.flush();
    txtChapterCount++;
}

void beginTxtIndexBuild() {
    txtIndexBuilding = false;
    if (txtIndexScanFile) txtIndexScanFile.close();
    if (txtIndexBuildFile) txtIndexBuildFile.close();
    if (txtChapterBuildFile) txtChapterBuildFile.close();
    resetIndexReaderState();   // 硬规则: 任何构建入口先清块读缓冲残留 (见 resetIndexReaderState 注释)
    debugFmt("IDX BUILD_OPEN index=%s chapter=%s txt=%s", txtIndexPath.c_str(), txtChapterPath.c_str(), txtPath.c_str());
    SD.remove(txtIndexPath.c_str());
    SD.remove(txtChapterPath.c_str());
    SD.remove((txtIndexPath + "p").c_str());   // 全新构建: 清旧会话 sidecar (旧进度已在上游恢复进内存)
    txtIndexBuildFile = SD.open(txtIndexPath.c_str(), FILE_WRITE);
    txtChapterBuildFile = SD.open(txtChapterPath.c_str(), FILE_WRITE);
    txtIndexScanFile = SD.open(txtPath.c_str());
    debugFmt("IDX BUILD_HANDLES index=%d chapter=%d scan=%d", (bool)txtIndexBuildFile, (bool)txtChapterBuildFile, (bool)txtIndexScanFile);
    if (!txtIndexBuildFile || !txtChapterBuildFile || !txtIndexScanFile) {
        debugLine("IDX async open failed");
        traceFmt("IDX BUILD_FAIL index=%d chapter=%d scan=%d", (bool)txtIndexBuildFile, (bool)txtChapterBuildFile, (bool)txtIndexScanFile);
        if (txtIndexBuildFile) txtIndexBuildFile.close();
        if (txtChapterBuildFile) txtChapterBuildFile.close();
        if (txtIndexScanFile) txtIndexScanFile.close();
        return;
    }
    appendIndexRecord(txtIndexBuildFile, 0);
    txtIndexBuildFile.flush();
    for (uint8_t i = 0; i < txtLineCount() + 1; i++) indexRows[i] = "";
    indexLine = 0;
    indexEnCount = 0;
    indexChCount = 0;
    indexLineOld = 0;
    indexHskgState = false;
    indexScanPos = 0;        // 全新构建从 0 起累计真实偏移
    indexPage = 1;
    txtIndexedPages = 1;
    txtTotalPages = 1;
    txtChapterCount = 0;
    indexPageStartPending = false;
    resumeChapterSeed[0] = '\0';   // 全新构建: 清续建去重种子
    resumeChapterSeedPage = 0;
    txtIndexBuilding = true;
    debugFmt("IDX async begin pos=%lu size=%lu", (unsigned long)txtIndexScanFile.position(), (unsigned long)txtIndexScanFile.size());
}

void finishTxtIndexBuild() {
    if (indexLine >= 0 && indexLine <= 7 && indexRows[indexLine].length() > 0)
        appendChapterTracked(txtChapterBuildFile, indexRows[indexLine], indexPage);
    appendIndexRecord(txtIndexBuildFile, txtIndexScanFile.size());   // 扫描句柄仍在, 兼容后台构建时 txtFile 已关
    txtIndexBuildFile.flush();
    // 记录[0] 写回当前阅读进度(构建中翻页位置; 未翻页则 0=从头读) — 必须用 "r+" 句柄 (FILE_WRITE 是追加模式, seek 无效)
    // ⚠️ 必须先把追加句柄 close 再开 "r+": 同文件双句柄写有概率破坏 FAT 引发 SD 卸载 (AGENTS.md 实测记录)
    txtIndexBuildFile.close();
    File zeroRec = SD.open(txtIndexPath.c_str(), "r+");
    if (zeroRec) {
        zeroRec.seek(0);
        char rec[9];
        formatIndexNumber(txtPageStart, rec);
        zeroRec.print(rec);
        zeroRec.close();
    }
    // 进度已合并进记录[0], 删除构建期 sidecar
    if (SD.exists((txtIndexPath + "p").c_str())) {
        SD.remove((txtIndexPath + "p").c_str());
        debugLine("IDX sidecar removed after merge");
    }
    txtChapterBuildFile.flush();
    txtChapterBuildFile.close();
    txtIndexScanFile.close();
    txtIndexBuilding = false;
    idxReleaseBuf();   // 生命周期化: 构建完成释放块读缓冲 (配网会话不占)
    txtTotalPages = txtIndexedPages;
    removeLegacyIndexFiles();
    File doneIndex = SD.open(txtIndexPath.c_str());
    File doneChapter = SD.open(txtChapterPath.c_str());
    uint32_t doneIndexSize = doneIndex ? doneIndex.size() : 0;
    uint32_t doneChapterSize = doneChapter ? doneChapter.size() : 0;
    if (doneIndex) doneIndex.close();
    if (doneChapter) doneChapter.close();
    debugFmt("IDX async done pages=%lu chapters=%lu indexSize=%lu z1Size=%lu", (unsigned long)txtTotalPages,
             (unsigned long)txtChapterCount, (unsigned long)doneIndexSize,
             (unsigned long)doneChapterSize);
    if (appMode == APP_READER) showMsg("索引完成", "可翻页/章节/保存进度");
}

// 索引构建块读缓冲: 单字节 read() 约 8.5KB/s, 55MB 需 1.8 小时; 块读+时间片后 ~4 分钟
// 512B (原 2048B): 构建速度略降但省 1.5KB 静态 RAM → 可用堆 +1.5KB (BearSSL TLS 同步需要大堆)
// 生命周期化 (堆预算审计后): 仅索引构建/续建期 malloc(512), finishTxtIndexBuild/abortIndexBuild
// 释放 → 配网会话不占这 512B (配网会话堆 ~3.3KB, 每 1B 都关键)
static uint8_t *idxBuf = NULL;
static uint16_t idxBufPos = 0, idxBufLen = 0;

// 惰性分配 (idxReadByte/idxPeekByte 调用前); 失败返回 false (低堆: 构建降级为单字节读? 不——
// 512B 申请失败即中止构建, 由调用方处理)
static inline bool idxEnsureBuf() {
    if (idxBuf) return true;
    idxBuf = (uint8_t *)malloc(512);
    idxBufPos = 0;
    idxBufLen = 0;
    return idxBuf != NULL;
}

// 构建结束/中止时释放 (配网/阅读空闲不占)
static inline void idxReleaseBuf() {
    if (idxBuf) { free(idxBuf); idxBuf = NULL; }
    idxBufPos = 0;
    idxBufLen = 0;
}

// 索引输入流状态复位: 任何索引构建任务启动前必须先调用。
// 历史 bug: abortIndexBuild/旋转重建只关句柄不重置块读缓冲, idxReadByte 在 idxBufPos<idxBufLen
// 时直接吐上一构建残留的旧字节 → 页表偏移整体错位且能通过单调性校验 (静默损坏)。
// 硬规则: 所有 Index Build 入口 (beginTxtIndexBuild / beginResumeIndexBuildFromPartial /
// abortIndexBuild 后的重建) 都从这里重置缓冲, 禁止任何入口直接依赖残留缓冲。
static inline void resetIndexReaderState() {
    idxBufPos = 0;
    idxBufLen = 0;
}

static inline int idxReadByte(File &f) {
    if (!idxEnsureBuf()) return -1;
    if (idxBufPos >= idxBufLen) {
        idxBufPos = 0;
        idxBufLen = f.read(idxBuf, 512);
        if (idxBufLen == 0) return -1;
    }
    indexScanPos++;
    return idxBuf[idxBufPos++];
}

static inline int idxPeekByte(File &f) {
    if (!idxEnsureBuf()) return -1;
    if (idxBufPos >= idxBufLen) {
        idxBufPos = 0;
        idxBufLen = f.read(idxBuf, 512);
        if (idxBufLen == 0) return -1;
    }
    return idxBuf[idxBufPos];
}

// ---------- 超级抗打断: 部分 .i1 页表续建 (官方 A7 同款机制) ----------
// .i1 布局自带恢复所需全部信息: 记录[N-1] = txt 大小是"完整性标记"(仅构建完成才写入),
// 页表自描述(每条 = 页首字节偏移)。KEY1 复位/断电中断构建后, 重启检测到
// 末记录 ≠ txt 大小 → 上次构建未完成 → 从页表末条记录的页首偏移续扫并追加,
// 最多重扫一页, 不从头全量。无独立断点文件, 无"页间隙补记录"错位问题。
// 注意: 不能写进 .i1 记录[0] — FILE_WRITE 是 "a+" 追加模式, seek(0) 无效会污染页表!
//       写进度必须用独立 "r+" 句柄 (见 writeProgress / finishTxtIndexBuild)。

// 续建去重: 重扫中断页会把已写入 .z1 的该页章节重复追加。
// 语义: .z1 中该页的最后一条章节 = 中断前已写入的边界; 重扫时跳过该条及之前的,
// 越过边界后恢复追加 (之后的章节尚未写入, 不能丢)。txt 未变则重扫逐字一致。
void seedChapterResumeBoundary(uint32_t resumePage) {
    resumeChapterSeed[0] = '\0';
    resumeChapterSeedPage = 0;
    File f = SD.open(txtChapterPath.c_str());
    if (!f || f.size() < 3) { if (f) f.close(); return; }
    // 从末尾窗口向前找最后一条 "page == resumePage" 的条目
    uint32_t size = f.size();
    uint32_t window = size > 512 ? size - 512 : 0;   // 512B 窗口足够覆盖连续章节条目
    f.seek(window);
    String buf;
    while (f.available()) { char c = (char)f.read(); buf += c; }
    f.close();
    int nl = buf.lastIndexOf('\n');
    while (nl >= 0) {
        String last = buf.substring(nl + 1);
        last.trim();
        int dash = last.lastIndexOf('-');
        if (dash > 0) {
            uint32_t pg = (uint32_t)strtoul(last.substring(dash + 1).c_str(), nullptr, 10);
            if (pg == resumePage && dash < (int)sizeof(resumeChapterSeed) - 1) {
                last.substring(0, dash).toCharArray(resumeChapterSeed, sizeof(resumeChapterSeed));
                resumeChapterSeedPage = resumePage;
                return;
            }
            if (pg < resumePage) return;   // 已越过该页条目, 没有该页记录
        }
        if (nl == 0) break;
        buf = buf.substring(0, nl);
        nl = buf.lastIndexOf('\n');
    }
}

// 部分页表续建: 追加模式复用现有 .i1/.z1 (不删除), 从页表末条页首偏移继续扫描。
// 调用前必须已验证: .i1 存在, size%8==0, size>=16, 末条 >0 且 < txt 大小 (startTxtReader 判定)。
void beginResumeIndexBuildFromPartial() {
    txtIndexBuilding = false;
    if (txtIndexScanFile) txtIndexScanFile.close();
    if (txtIndexBuildFile) txtIndexBuildFile.close();
    if (txtChapterBuildFile) txtChapterBuildFile.close();
    resetIndexReaderState();   // 硬规则: 续建入口同样清块读缓冲残留 (旋转切换后旧缓冲会导致页表错位)
    uint32_t offset = 0;
    File last = SD.open(txtIndexPath.c_str());
    if (last && last.size() >= 8) {
        last.seek(last.size() - 8);
        char rec[9];
        for (uint8_t i = 0; i < 8; i++) rec[i] = (char)last.read();
        rec[8] = '\0';
        offset = strtoul(rec, nullptr, 10);
    }
    if (last) last.close();
    if (offset == 0 || offset >= txtFile.size()) {
        debugLine("IDX resume invalid tail -> full rebuild");
        beginTxtIndexBuild();
        return;
    }
    uint32_t pagesDone = 0;
    File sz = SD.open(txtIndexPath.c_str());
    if (sz) { pagesDone = sz.size() / 8; sz.close(); }
    if (pagesDone < 2) pagesDone = 2;
    txtChapterCount = countTxtChapters();   // 已有章节数(在创建句柄前统计, 避免双句柄)
    seedChapterResumeBoundary(pagesDone);   // 重扫页的章节跳过边界
    txtIndexScanFile = SD.open(txtPath.c_str());
    txtIndexBuildFile = SD.open(txtIndexPath.c_str(), FILE_WRITE);      // 追加, 不截断
    txtChapterBuildFile = SD.open(txtChapterPath.c_str(), FILE_WRITE);
    if (!txtIndexScanFile || !txtIndexBuildFile || !txtChapterBuildFile) {
        debugLine("IDX resume open failed -> full rebuild");
        if (txtIndexScanFile) txtIndexScanFile.close();
        if (txtIndexBuildFile) txtIndexBuildFile.close();
        if (txtChapterBuildFile) txtChapterBuildFile.close();
        beginTxtIndexBuild();
        return;
    }
    txtIndexScanFile.seek(offset);
    indexScanPos = txtIndexScanFile.position();   // 续建起点 = 页表末条真实偏移
    for (uint8_t i = 0; i < 9; i++) indexRows[i] = "";
    indexLine = 0;
    indexEnCount = 0; indexChCount = 0;
    indexLineOld = 0;
    indexHskgState = false;
    indexPage = pagesDone;      // 正在重扫的页 = 末条记录对应的页 (页首记录已存在, 不重复写)
    txtIndexedPages = pagesDone;
    txtTotalPages = pagesDone;
    indexPageStartPending = false;   // 该页页首记录已存在, 换页后才 append 下一页
    txtIndexBuilding = true;
    for (uint8_t i = 0; i < txtLineCount() + 1; i++) indexRows[i] = "";
    debugFmt("IDX super-resume pos=%lu pages=%lu chapters=%lu", (unsigned long)offset,
             (unsigned long)pagesDone, (unsigned long)txtChapterCount);
}

void indexTaskStep() {
    if (!txtIndexBuilding || !txtIndexScanFile) return;
    static uint32_t lastDebugPos = 0;
    uint32_t stepStart = txtIndexScanFile.position();
    uint32_t deadline = millis() + 100;   // 每 loop 至多 100ms 构建; 过大则按键短按丢失(loop 周期>短按时长)
    while ((int32_t)(deadline - millis()) > 0) {
        ESP.wdtFeed();
        if (indexLineOld != indexLine) { indexLineOld = indexLine; indexHskgState = true; }
        if (indexPageStartPending && indexLine == 0) {
            indexPageStartPending = false;
            appendIndexRecord(txtIndexBuildFile, indexScanPos);   // 真实字节偏移 (勿用 position(): 块读时是 2048 对齐)
            txtIndexBuildFile.flush();
            txtIndexedPages++;
            txtTotalPages = txtIndexedPages;
        }
        int c = idxReadByte(txtIndexScanFile);
        if (c < 0) break;
        while (c == '\n' && indexLine <= txtLineCount() - 1) {
            if (indexLine == 0) {
                if (indexRows[indexLine].length() > 0) { appendChapterTracked(txtChapterBuildFile, indexRows[indexLine], indexPage); indexLine++; }
                else indexRows[indexLine].clear();
            } else if (indexRows[indexLine].length() > 0) {
                appendChapterTracked(txtChapterBuildFile, indexRows[indexLine], indexPage);
                indexLine++;
            } else if (indexRows[indexLine - 1].length() > 0) {
                indexLine++;
            }
            if (indexLine <= txtLineCount() - 1) { c = idxReadByte(txtIndexScanFile); if (c < 0) break; }
            indexEnCount = 0; indexChCount = 0;
        }
        if (c < 0) break;
        if (c == '\t') indexRows[indexLine] += indexRows[indexLine].length() == 0 ? "    " : "       ";
        else if (!((c >= 0 && c <= 31) || c == 127)) indexRows[indexLine] += (char)c;
        bool asciiState = false;
        uint8_t b = (uint8_t)c & 0xE0;
        if (b == 0xE0) {
            indexChCount++;
            c = idxReadByte(txtIndexScanFile); if (c < 0) break; indexRows[indexLine] += (char)c;
            c = idxReadByte(txtIndexScanFile); if (c < 0) break; indexRows[indexLine] += (char)c;
        } else if (b == 0xC0) {
            indexEnCount += 14;
            c = idxReadByte(txtIndexScanFile); if (c < 0) break; indexRows[indexLine] += (char)c;
        } else if (c == '\t') indexEnCount += indexRows[indexLine] == "    " ? 20 : 28;
        else if (c >= 0 && c <= 255) { indexEnCount += txtCharWidth((uint8_t)c) + 1; asciiState = true; }
        uint16_t stringLength = indexEnCount + indexChCount * 14;
        int lw = txtLineWidth();
        if (stringLength >= lw - 23 && indexHskgState) {
            if (indexRows[indexLine].length() >= 4 && indexRows[indexLine][0] == ' ' && indexRows[indexLine][1] == ' ' &&
                indexRows[indexLine][2] == ' ' && indexRows[indexLine][3] == ' ') indexEnCount += 8;
            indexHskgState = false;
        }
        if (stringLength >= lw) {
            if (!asciiState) { appendChapterTracked(txtChapterBuildFile, indexRows[indexLine], indexPage); indexRows[indexLine].clear(); indexLine++; indexEnCount = 0; indexChCount = 0; }
            else if (stringLength >= lw + 3) {
                int t = idxPeekByte(txtIndexScanFile);
                int cz = lw + 3 - stringLength;
                int tLength = t < 0 ? 0 : txtCharWidth((uint8_t)t);
                uint8_t tb = t < 0 ? 0xE0 : ((uint8_t)t & 0xE0);
                if (tb == 0xE0 || tb == 0xC0 || tLength > cz) { indexRows[indexLine].clear(); indexLine++; indexEnCount = 0; indexChCount = 0; }
            }
        }
        if (indexLine == txtLineCount()) {
            indexPageStartPending = true;
            indexPage++;
            indexLine = 0; indexEnCount = 0; indexChCount = 0;
            for (uint8_t i = 0; i < txtLineCount() + 1; i++) indexRows[i].clear();
            // 无断点文件: 页表本身即断点 (每页页首已随 indexPageStartPending 写入 .i1)
        }
    }
    if (idxBufPos >= idxBufLen && !txtIndexScanFile.available()) {
        traceFmt("IDX EOF stepStart=%lu pos=%lu", (unsigned long)stepStart, (unsigned long)txtIndexScanFile.position());
        finishTxtIndexBuild();
    }
}
uint32_t parsePageRecord(uint32_t page) {
    if (page <= 1) return 0;
    File f = SD.open(txtIndexPath.c_str());
    if (!f) return 0;
    f.seek((page - 1) * 8);
    char record[9];
    for (uint8_t i = 0; i < 8; i++) record[i] = (char)f.read();
    record[8] = '\0';
    f.close();
    return strtoul(record, nullptr, 10);
}

bool writeProgress(uint32_t offset) {
    if (txtIndexBuilding) {
        // 构建中进度 → 独立 sidecar (txtIndexPath+"p", 如 小说.i1p): 不与 .i1 追加句柄
        // 形成同文件双句柄 (实测双句柄写 .i1 有概率破坏 FAT 引发 SD 卸载)。
        // 完成时 finishTxtIndexBuild 合并回记录[0] 并删除 sidecar;
        // 构建被中断 (掉电/休眠/重启) 时 startTxtReader 优先读 sidecar 恢复阅读位置。
        String sidecar = txtIndexPath + "p";
        File f = SD.open(sidecar.c_str(), "r+");
        if (!f) f = SD.open(sidecar.c_str(), FILE_WRITE);   // 首次写入: 新建
        if (!f) {
            traceFmtLevel('E', "PROGRESS_OPEN_FAIL sidecar=%s offset=%lu", sidecar.c_str(), (unsigned long)offset);
            return false;
        }
        f.seek(0);
        char record[9];
        formatIndexNumber(offset, record);
        f.print(record);
        f.close();
        return true;
    }
    String progressPath = txtIndexPath;
    if (!SD.exists(progressPath.c_str())) {
        String legacyPath = txtPath + ".i1";
        if (SD.exists(legacyPath.c_str())) progressPath = legacyPath;
    }
    // 打开失败重试: 电量采样/EPD 刷新可能动过共享 GPIO, 先恢复 SD 总线再试 (实测偶发打开失败)
    File f;
    bool opened = false;
    for (int attempt = 0; attempt < 2 && !opened; attempt++) {
        f = SD.open(progressPath.c_str(), "r+");
        if (f) { opened = true; break; }
        reinitSdBus("progress_retry");
        f = SD.open(progressPath.c_str(), "r+");
        if (f) { opened = true; break; }
        traceFmtLevel('E', "PROGRESS_OPEN_FAIL path=%s offset=%lu", progressPath.c_str(), (unsigned long)offset);
        return false;
    }
    f.seek(0);
    char record[9];
    formatIndexNumber(offset, record);
    f.print(record);
    f.close();
    return true;
}

// 读取当前阅读字节偏移 (.i1 记录[0]); 构建中/构建中断优先读 sidecar (txtIndexPath+"p")。
// 只读索引前 8 字节, 绝不重扫索引/TXT。
uint32_t readProgressOffset() {
    uint32_t off = 0;
    if (txtIndexBuilding) {
        String sidecar = txtIndexPath + "p";
        File sp = SD.open(sidecar.c_str());
        if (sp && sp.size() >= 8) {
            char rec[9];
            for (uint8_t i = 0; i < 8; i++) rec[i] = (char)sp.read();
            rec[8] = '\0';
            off = strtoul(rec, nullptr, 10);
        }
        if (sp) sp.close();
        return off;
    }
    String path = txtIndexPath;
    if (!SD.exists(path.c_str())) {
        String legacy = txtPath + ".i1";
        if (SD.exists(legacy.c_str())) path = legacy;
    }
    // 偶发 SD 打开失败会误报 0 (实测同步 BEGIN 读出 0 而重启后正常): 重试 3 次 + 失败日志
    bool opened = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        File f = SD.open(path.c_str());
        if (f) {
            opened = true;
            if (f.size() >= 8) {
                char rec[9];
                for (uint8_t i = 0; i < 8; i++) rec[i] = (char)f.read();
                rec[8] = '\0';
                off = strtoul(rec, nullptr, 10);
            }
            f.close();
            if (off > 0) break;   // 读到有效进度立即返回
        }
        if (attempt < 2) { delay(10); ESP.wdtFeed(); }
    }
    if (!opened) traceFmtLevel('E', "PROGRESS_READ_FAIL path=%s", path.c_str());
    else if (off == 0) traceFmtLevel('W', "PROGRESS_READ0 path=%s", path.c_str());
    return off;
}

// 页表二分: 页首偏移表 (记录[1..N-2], 单调递增) 中找出 >= offset 所在页的页码。
// 记录 idx 对应 页码 idx+1 (记录[1]=第2页首); 记录[0]=进度, 记录[N-1]=txt大小。
// O(log N) 只读 8 字节/条, 不扫描整个 .i1。
uint32_t offsetToPage(uint32_t offset) {
    File f = SD.open(txtIndexPath.c_str());
    if (!f) return 1;
    uint32_t n = f.size() / 8;
    if (n < 2) { f.close(); return 1; }
    uint32_t lo = 1, hi = n - 2, best = 0;   // 记录[1..n-2] 为页首偏移表
    char rec[9];
    while (lo <= hi) {
        uint32_t mid = (lo + hi) / 2;
        f.seek(mid * 8);
        for (uint8_t i = 0; i < 8; i++) rec[i] = (char)f.read();
        rec[8] = '\0';
        uint32_t v = strtoul(rec, nullptr, 10);
        if (v <= offset) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    f.close();
    if (best == 0) return 1;
    return best + 1;
}

// 阅读器行渲染: 逐字固定步进 (CJK 14px / ASCII txtCharWidth+1),
// 与索引算法(逆向官方布局)的度量完全一致, 字形由 U8g2 绘制 (UTF-8 解码)。
void drawReaderLine(int y, const String &line) {
    if (!textRendererReady) return;
    u8g2Fonts.setForegroundColor(1);
    int x = 2;
    int xMax = txtLineWidth();   // 旋转参数化: 横 283 / 竖 118
    const uint8_t *p = (const uint8_t *)line.c_str();
    while (*p && x < xMax) {
        if (*p < 0x80) {
            u8g2Fonts.drawGlyph(x, y + 13, *p);
            x += txtCharWidth(*p) + 1;
            p++;
        } else {
            int len = 0;
            uint32_t cp = decodeUtf8((const char *)p, len);
            u8g2Fonts.drawGlyph(x, y + 13, cp);
            x += 14;
            p += len;
        }
    }
}

// 与索引布局一致的宽度度量 (ASCII: txtCharWidth+1; CJK/全角 2/3 字节: 14px)
uint16_t readerLineWidth(const String &s) {
    uint16_t w = 0;
    const uint8_t *p = (const uint8_t *)s.c_str();
    while (*p) {
        if (*p < 0x80) { w += txtCharWidth(*p) + 1; p++; }
        else if ((*p & 0xE0) == 0xE0) { w += 14; p += 3; }
        else if ((*p & 0xC0) == 0xC0) { w += 14; p += 2; }
        else p++;
    }
    return w;
}

bool txtPageStartsParagraph = true;
void normalizeReaderLines(String *displayLines) {
    int lines = txtLineCount();
    for (uint8_t i = 0; i < lines; i++) displayLines[i] = "";
    uint8_t out = 0;
    bool paragraphStart = txtPageStartsParagraph;
    for (uint8_t i = 0; i < lines && out < lines; i++) {
        String line = txtLines[i];
        uint16_t p = 0;
        uint16_t leadingSpaces = 0;
        while (p < line.length() && (line[p] == ' ' || line[p] == '\t')) {
            leadingSpaces++;
            p++;
        }
        if (p >= line.length()) {
            paragraphStart = true;
            continue;
        }
        String content = line.substring(p);
        bool title = content.length() >= 3 &&
                     (uint8_t)content[0] == 0xE7 && (uint8_t)content[1] == 0xAC && (uint8_t)content[2] == 0xAC;
        if ((paragraphStart || leadingSpaces >= 2) && !title) {
            // 段首缩进 = 2 个全角空格 (28px = 两字, 用户预期"空2格"; 原 7 半角空格=35px 偏大约3字)。
            // 布局(索引)不含缩进: 内容行接近满行(>行宽-19px)时加缩进会超出屏幕右缘截断末字 → 此行不缩进。
            if (readerLineWidth(content) <= txtLineWidth() - 19) content = String("\xE3\x80\x80\xE3\x80\x80") + content;
        }
        displayLines[out++] = content;
        paragraphStart = false;
    }
}

void readTxtPage(uint32_t offset) {
    traceFmt("PAGE_READ_BEGIN page=%lu offset=%lu", (unsigned long)txtPage, (unsigned long)offset);
    txtPageStartsParagraph = false;
    if (offset == 0) txtPageStartsParagraph = true;
    else {
        txtFile.seek(offset - 1);
        int prev = txtFile.read();
        txtPageStartsParagraph = (prev == '\n' || prev == '\r');
    }
    txtFile.seek(offset);
    int lines = txtLineCount();
    for (uint8_t i = 0; i < lines; i++) txtLines[i] = "";
    int8_t line = 0;
    uint16_t enCount = 0, chCount = 0;
    uint8_t lineOld = 0;
    bool hskgState = true;
    while (line < lines && txtFile.available()) {
        ESP.wdtFeed();
        if (lineOld != line) { lineOld = line; hskgState = true; }
        int c = txtFile.read();
        if (c < 0) {
            // SD 读失败(卡接触不良/坏块): 立即停止填充本页, 防止 available() 卡真导致
            // 死循环(翻页卡死根因: 循环内每圈喂狗 → 按键永不响应, 仅 KEY1 硬复位可恢复)
            traceFmtLevel('E', "TXT_READ_FAIL file=%s pos=%lu line=%d",
                         txtPath.c_str(), (unsigned long)txtFile.position(), line);
            break;
        }
        while (c == '\n' && line <= lines - 1) {
            bool hadText = txtLines[line].length() > 0;
            if (line == 0) {
                if (hadText) line++;
                else txtLines[line] = "";
            } else if (hadText) {
                line++;
            } else if (txtLines[line - 1].length() > 0) {
                line++;
            }
            if (line <= lines - 1) { c = txtFile.read(); if (c < 0) break; }
            traceFmt("PAGE_NEWLINE line=%d pos=%lu text=%d", line, (unsigned long)txtFile.position(), hadText ? 1 : 0);
            enCount = 0; chCount = 0;
        }
        if (c < 0) break;
        if (c == '\t') txtLines[line] += txtLines[line].length() == 0 ? "    " : "       ";
        else if (!((c >= 0 && c <= 31) || c == 127)) txtLines[line] += (char)c;
        bool asciiState = false;
        uint8_t b = (uint8_t)c & 0xE0;
        if (b == 0xE0) {
            chCount++;
            c = txtFile.read(); if (c < 0) break; txtLines[line] += (char)c;
            c = txtFile.read(); if (c < 0) break; txtLines[line] += (char)c;
        } else if (b == 0xC0) { enCount += 14; c = txtFile.read(); if (c < 0) break; txtLines[line] += (char)c; }
        else if (c == '\t') enCount += txtLines[line] == "    " ? 20 : 28;
        else if (c >= 0 && c <= 255) { enCount += txtCharWidth((uint8_t)c) + 1; asciiState = true; }
        uint16_t stringLength = enCount + chCount * 14;
        int lw = txtLineWidth();
        if (stringLength >= lw - 23 && hskgState) {
            if (txtLines[line].length() >= 2 && txtLines[line][0] == ' ' && txtLines[line][1] == ' ' &&
                txtLines[line][2] == ' ' && txtLines[line][3] == ' ') enCount += 8;
            hskgState = false;
        }
        if (stringLength >= lw) {
            if (!asciiState) { line++; enCount = 0; chCount = 0; }
            else if (stringLength >= lw + 3) {
                int t = txtFile.peek();
                int cz = lw + 3 - stringLength;
                int tLength = txtCharWidth((uint8_t)t);
                uint8_t tb = (uint8_t)t & 0xE0;
                if (tb == 0xE0 || tb == 0xC0 || tLength > cz) { line++; enCount = 0; chCount = 0; }
            }
        }
    }
    traceFmt("PAGE_READ_DONE page=%lu pos=%lu lengths=%u,%u,%u,%u,%u,%u,%u,%u",
             (unsigned long)txtPage, (unsigned long)txtFile.position(),
             txtLines[0].length(), txtLines[1].length(), txtLines[2].length(), txtLines[3].length(),
             txtLines[4].length(), txtLines[5].length(), txtLines[6].length(), txtLines[7].length());
}

// 数字键盘单个键 (选中项下方画向下小三角光标)
static void drawJumpKey(int x, int y, int w, int h, const char *label, bool sel) {
    fillRect(x, y, w, h, sel);
    drawRect(x, y, w, h, !sel);
    int tw = utf8Width(label);
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;   // 文字比键宽: 左对齐, 防负坐标
    drawTextUTF8(tx, y + (h - 16) / 2, label, w - 4, !sel);
    if (sel) {
        // 向下的 < 箭头 (实心小三角): 键下方 2px, 宽 7 高 5
        int cx = x + w / 2;
        int ty = y + h + 2;
        for (int i = 0; i < 5; i++) {
            int half = i / 2;
            for (int dx = -half; dx <= half; dx++) {
                int px = cx + dx;
                if (px >= 0 && px < SCR_W) setPix(px, ty + i, true);   // 纵向裁剪交给 setPix (竖屏直映下 SCR_H=128 会误裁 y≥128 的三角)
            }
        }
    }
}

// 页码跳转弹窗 (数字键盘, 局部刷新): 右短/中短 移光标(行优先), 右长 执行, 中长 取消。
// 布局: 横屏 1-6 / 7-0 < 回车 / 取消(回车正下方小键);
//       竖屏 1-3 / 4-6 / 7-9 / 0 < 回车 / 取消(回车正下方小键)。
// 13 键线性索引 0..12 = 1..9 0 < 回车 取消; 行优先排布, 移动即 (idx±1)%13。
void renderJumpOverlay() {
    static const char *const jumpKeys[13] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "<", "回车", "取消"};
    fbRot = readerRot;   // 阅读渲染: 竖屏→0°, 横屏→90° (四向由 readerRot 扩展)
    int jx, jy, jw, jh, pageY;
    if (readerIsPortrait()) {
        jx = 5; jy = 48; jw = 118; jh = 200;
        pageY = jy + 164;
    } else {
        jx = 8; jy = 0; jw = 280; jh = 128;
        pageY = jy + 110;
    }
    fillRect(jx, jy, jw, jh, false);
    drawRect(jx, jy, jw, jh, true);
    int idx = 0;
    if (readerIsPortrait()) {
        // 前三行: 1-9 (3×3, 键 36×28, 行距 4, 行内距 2)
        const int kw = 36, kh = 28;
        const int rowW = 3 * kw + 2 * 2;
        const int x0 = jx + (jw - rowW) / 2;
        for (int r = 0; r < 3; r++) {
            int y = jy + 6 + r * (kh + 4);
            for (int c = 0; c < 3; c++, idx++) {
                drawJumpKey(x0 + c * (kw + 2), y, kw, kh, jumpKeys[idx], idx == (int)jumpCursor);
            }
        }
        // 第 4 行: 0 < 回车
        int y4 = jy + 6 + 3 * (kh + 4);
        for (int c = 0; c < 3; c++, idx++) {
            drawJumpKey(x0 + c * (kw + 2), y4, kw, kh, jumpKeys[idx], idx == (int)jumpCursor);
        }
        // 取消: 回车正下方 (小方块 36×22, 容纳"取消"32px)
        int enterX = x0 + 2 * (kw + 2);
        int cancelW = 36, cancelH = 22;
        drawJumpKey(enterX + (kw - cancelW) / 2, y4 + kh + 4, cancelW, cancelH, jumpKeys[12], 12 == (int)jumpCursor);
    } else {
        // 行0: 1-6 (键 42×32, 行内距 4)
        const int kh = 32, gap = 6;
        const int rowW = 6 * 42 + 5 * 4;
        const int x0 = jx + (jw - rowW) / 2;
        int y0 = jy + 6;
        for (int c = 0; c < 6; c++, idx++) {
            drawJumpKey(x0 + c * (42 + 4), y0, 42, kh, jumpKeys[idx], idx == (int)jumpCursor);
        }
        // 行1: 7 8 9 0 < 回车
        int y1 = y0 + kh + gap;
        for (int c = 0; c < 6; c++, idx++) {
            drawJumpKey(x0 + c * (42 + 4), y1, 42, kh, jumpKeys[idx], idx == (int)jumpCursor);
        }
        // 取消: 回车正下方 (小方块 36×22, 容纳"取消"32px)
        int enterX = x0 + 5 * (42 + 4);
        int cancelW = 36, cancelH = 22;
        drawJumpKey(enterX + (42 - cancelW) / 2, y1 + kh + gap, cancelW, cancelH, jumpKeys[12], 12 == (int)jumpCursor);
    }
    char buf[40];
    if (jumpRejectMs && (uint32_t)(millis() - jumpRejectMs) < 1000) {
        snprintf(buf, sizeof(buf), "%lu/%lu页 !", (unsigned long)jumpPage, (unsigned long)txtTotalPages);
    } else {
        snprintf(buf, sizeof(buf), "%lu/%lu页", (unsigned long)jumpPage, (unsigned long)txtTotalPages);
    }
    drawTextUTF8(jx + 4, pageY, buf, jw - 8, true);
    refresh(false);
    fbRot = 90;
}

void jumpToPage() {
    if (jumpPage < 1) jumpPage = 1;
    if (jumpPage > txtTotalPages) jumpPage = txtTotalPages;
    readerJumpOpen = false;
    if (txtIndexBuilding && jumpPage > txtIndexedPages) jumpPage = txtIndexedPages;
    txtPage = jumpPage;
    txtPageStart = parsePageRecord(txtPage);
    readTxtPage(txtPageStart);
    writeProgress(txtPageStart);
    renderTxtPage(true);
}

// ---------- FixedRefresh 定次刷新 (对齐官方 DisplaySetup.ino) ----------
// 局刷残影逐次累积 → 每 N 次局刷做一次完全刷新防残影; 常规翻页仍是局刷(不闪屏)。
// N 由阅读菜单"全刷间隔："调节 (fixedRefreshEvery, 默认 6)。
// 进入阅读页/章节跳转的结构性全刷视为干净起点(计数归零)。
// 注: 官方 %6 分支做"黑+白两次全刷再局刷"≈3s 太慢, 复刻改为单次全刷直接显示本页(≈1.5s)。
static uint8_t fixedRefreshCount = 0;

void drawTxtPageLines(String *displayLines) {
    // 清屏尺寸按方向: 横屏 296x128 / 竖屏 128x296 (竖屏用 SCR_W/SCR_H 会只清一半 → 残留)
    fillRect(0, 0, readerIsPortrait() ? EPD_WIDTH : SCR_W,
                  readerIsPortrait() ? EPD_HEIGHT : SCR_H, false);
    int lines = txtLineCount();
    // 横屏 8 行 x 16px = 128 满屏; 竖屏 18 行 x 16px = 288 (屏 296, 底部留 8px)
    for (uint8_t i = 0; i < lines; i++) drawReaderLine(i * 16, displayLines[i]);
}

void renderTxtPage(bool full) {
    String displayLines[18];
    normalizeReaderLines(displayLines);
    fbRot = readerRot;   // 竖屏渲染: setPix 直映映射 (阅读页布局已按方向参数化)
    if (full) {
        fixedRefreshCount = 0;   // 结构变化全刷 = 干净起点
        drawTxtPageLines(displayLines);
        refresh(true);
        fbRot = 90;      // 恢复横屏映射 (其他页面)
        return;
    }
    if (++fixedRefreshCount >= fixedRefreshEvery) {
        fixedRefreshCount = 0;
        drawTxtPageLines(displayLines);
        refresh(true);   // 定次全刷(仅 1 次): 清残影 + 显示本页
        fbRot = 90;
        return;
    }
    drawTxtPageLines(displayLines);
    refresh(false);
    fbRot = 90;
}

// 优化④: 启动阅读恢复免刷 — 面板物理保留复位前同页画面, 仅渲染进帧缓冲 (不写屏, 0s)。
// fixedRefreshCount 保持 0 → 首次翻页局刷写屏, 第 6 次翻页定次全刷自愈残影。
void renderTxtPageNoRefresh() {
    String displayLines[18];
    normalizeReaderLines(displayLines);
    fbRot = readerRot;   // 竖屏渲染: setPix 直映映射 (阅读页布局已按方向参数化)
    drawTxtPageLines(displayLines);
    fbRot = 90;
}

// ---------- 阅读器菜单 (局部刷新) ----------
// A7 流式按钮布局: 7 项 (字体选择/退出/自动翻页/全刷间隔：/旋转/跳转/休眠),
// 流式小矩形按钮 (高度一致、宽度随文字自适应、一行排满换行),
// 底部两行信息: 时间(HH:MM:SS)+空格+电量xx% / 页数+空格+百分比(构建中显示进度)。
// 时间只在菜单打开/光标移动(局部刷新)时取当前值。
void renderReaderMenu() {
    fbRot = readerRot;   // 竖屏阅读页: 菜单也走直映映射
    const int MX = readerIsPortrait() ? 8 : 20;
    const int MY = readerIsPortrait() ? 8 : 0;
    const int MW = readerIsPortrait() ? 112 : 236;
    const int MH = readerIsPortrait() ? 280 : 128;
    fillRect(MX, MY, MW, MH, false);
    drawRect(MX, MY, MW, MH, true);

    // ---- 菜单项按钮 (含当前值): 流式排布, 12px 小字 + 小按钮, 容纳 11 项 ----
    // 11 项: 字体选择/退出/自动翻页/全刷间隔：/旋转/跳转/章节/标签/休眠/进度同步/配网
    // (12px 中文字库 u8g2_font_wqy12_t_gb2312: 修复 10 项时 i<9 漏渲染末项的幽灵选项问题,
    //  且缩小字体/按钮面积使 11 项在横竖屏面板内放得下)
    static const char *const itemNames[11] = {"字体选择", "退出", "自动翻页", "全刷间隔：", "旋转", "跳转", "章节", "标签", "休眠", "进度同步", "配网"};
    char itemText[11][24];
    for (int i = 0; i < 11; i++) {
        switch (i) {
            case 0: snprintf(itemText[i], sizeof(itemText[i]), "%s%s", itemNames[i], fontExternal ? " 外" : " 自"); break;
            case 2: snprintf(itemText[i], sizeof(itemText[i]), "%s%s", itemNames[i], autoFlipSpeed ? " 开" : " 关"); break;
            case 3: snprintf(itemText[i], sizeof(itemText[i]), "%s %d", itemNames[i], fixedRefreshEvery); break;
            case 4: snprintf(itemText[i], sizeof(itemText[i]), "%s %s°", itemNames[i], rotLabel(readerRot)); break;
            default: snprintf(itemText[i], sizeof(itemText[i]), "%s", itemNames[i]); break;
        }
    }
    // 菜单临时切 12px 中文字体 (按钮/文字更小); 渲染完恢复 16px 主字体。
    // ⚠️ u8g2_SetFont 会重置 is_transparent=0 (见库 U8g2_for_Adafruit_GFX.cpp u8g2_SetFont):
    // 必须重新 setFontMode(1) 透明模式, 否则字形背景用 bg_color(默认白) 填充 → 选中黑底上出现白矩形。
    u8g2Fonts.setFont(u8g2_font_wqy12_t_gb2312);
    u8g2Fonts.setFontMode(1);
    const int btnH = 17, btnGap = 4, btnPad = 6;
    int x = MX + 4, y = MY + 4;
    int maxY = y;
    for (int i = 0; i < 11; i++) {
        int bw = utf8Width(itemText[i]) + btnPad * 2;
        if (x + bw > MX + MW - 4) { x = MX + 4; y += btnH + btnGap; }
        bool sel = readerMenuSel == i;
        fillRect(x, y, bw, btnH, sel);
        drawRect(x, y, bw, btnH, !sel);
        // 12px 字体基线 ≈ y+13 (wqy12 字高 15px, ascent 11): 文字下移避免与按钮上边框重叠
        // (16px 的 drawTextUTF8 用 +13; 12px 实测 +11 顶到上边框, 用户反馈重叠)
        u8g2Fonts.setForegroundColor(sel ? 0 : 1);
        u8g2Fonts.setCursor(x + btnPad, y + 13);
        u8g2Fonts.print(itemText[i]);
        x += bw + btnGap;
        if (y + btnH > maxY) maxY = y + btnH;
    }
    u8g2Fonts.setFont(chinese_gb2312);
    u8g2Fonts.setFontMode(1);   // 恢复主字体同样重设透明模式 (setFont 会重置)
    // ---- 面板: 包住按钮区 + 底部两行信息 ----
    int infoTop = maxY + 6;
    if (infoTop + 2 * 16 + 8 > MY + MH) infoTop = MY + MH - 2 * 16 - 8;
    char line1[48], line2[48];
    time_t nowT = clockManagerNow();
    struct tm *tmv = nowT > 1600000000UL ? localtime(&nowT) : nullptr;
    if (tmv) snprintf(line1, sizeof(line1), "%02d:%02d:%02d 电量%d%%", tmv->tm_hour, tmv->tm_min, tmv->tm_sec, batPercent(lastBatteryMV));
    else snprintf(line1, sizeof(line1), "--:--:-- 电量%d%%", batPercent(lastBatteryMV));
    if (txtIndexBuilding) {
        // 竖屏面板窄 (MW=112, 信息行 maxW=104): "索引建立中 64322页" 超宽截断 → 用短文案完整显示
        if (readerIsPortrait()) snprintf(line2, sizeof(line2), "已建%lu页", (unsigned long)txtIndexedPages);
        else snprintf(line2, sizeof(line2), "索引建立中 %lu页", (unsigned long)txtIndexedPages);
    } else {
        uint32_t pct = txtTotalPages ? (uint32_t)((uint64_t)txtPage * 1000 / txtTotalPages) : 0;
        snprintf(line2, sizeof(line2), "%lu.%lu%% %lu/%lu页", (unsigned long)(pct / 10), (unsigned long)(pct % 10),
                 (unsigned long)txtPage, (unsigned long)txtTotalPages);
    }
    if (readerMenuNote[0]) snprintf(line2, sizeof(line2), "%s", readerMenuNote);
    drawTextUTF8(MX + 4, infoTop, line1, MW - 8, true);
    drawTextUTF8(MX + 4, infoTop + 16, line2, MW - 8, true);
    refresh(false);
    fbRot = 90;
}

void openReaderMenu() {
    traceFmt("EVENT_MENU_OPEN page=%lu offset=%lu", (unsigned long)txtPage,
             (unsigned long)txtPageStart);
    if (txtFile) writeProgress(txtPageStart);   // 构建中自动走 sidecar, 不碰 .i1 双句柄
    readerMenuOpen = true;
    readerMenuSel = 0;
    readerMenuNote[0] = '\0';
    renderReaderMenu();
}

void closeReaderMenu() {
    readerMenuOpen = false;
    renderTxtPage(false);   // 重绘正文 + 局刷 (菜单覆盖了帧缓冲)
}

// 弹窗光标序 ↔ 内部旋转角: UI 显示度数升序排列 (默认横屏(内部90°)=0°, 每档顺时针 +90°)
static const uint16_t rotSelTable[4] = {90, 180, 270, 0};
static uint8_t rotSelIndexOf(uint16_t rot) {
    for (uint8_t i = 0; i < 4; i++) if (rotSelTable[i] == rot) return i;
    return 0;
}

// 旋转方向选择弹窗 (四选一): 菜单"旋转"进入, 光标选中方案后 右长 才切换; 中长 取消回菜单。
// 跟随当前阅读方向渲染 (reader-direction UI); 当前方向行右缘实心方块标记 (图形标记, 零字形风险)。
static void renderRotSelOverlay() {
    fbRot = readerRot;
    const int MX = readerIsPortrait() ? 8 : 20;
    const int MY = readerIsPortrait() ? 8 : 0;
    const int MW = readerIsPortrait() ? 112 : 236;
    const int MH = readerIsPortrait() ? 280 : 128;
    fillRect(MX, MY, MW, MH, false);
    drawRect(MX, MY, MW, MH, true);
    drawTextUTF8(MX + 5, MY + 4, "旋转方向", MW - 10, true);
    static const char *const opts[4] = {"0° 横屏", "90° 竖翻", "180° 横翻", "270° 竖屏"};
    const int rowH = 20, step = 24, top = MY + 22;
    for (int i = 0; i < 4; i++) {
        bool sel = rotSelCursor == i;
        fillRect(MX + 6, top + i * step, MW - 12, rowH, sel);
        drawRect(MX + 6, top + i * step, MW - 12, rowH, !sel);
        drawTextUTF8(MX + 12, top + i * step + 3, opts[i], MW - 36, !sel);
        if (rotSelTable[i] == readerRot) {   // 当前方向: 行右缘实心方块
            fillRect(MX + MW - 17, top + i * step + 7, 6, 6, !sel);
        }
    }
    refresh(false);
    fbRot = 90;
}

// 应用目标阅读方向 (弹窗确认后调用): 中止构建→关句柄→记续读偏移→切向重开。
// 构建中先中止当前方向构建 (reinitSdBus 会 SD.begin 重挂载, 打开的构建句柄会失效,
// 不能保留后台继续); 目标方向索引完整 → 秒开当前页, 不完整 → 重建 + 旋转续读显示当前页。
static void applyReaderRotation(uint16_t target) {
    if (txtIndexBuilding) abortIndexBuild();
    readerMenuOpen = false;
    readerRotSelOpen = false;
    readerSyncOpen = false;
    // 记录当前页字节偏移 (与方向无关): 新方向索引二分定位到同一阅读位置, 不跳旧进度
    gRotateResumeOffset = txtPageStart;
    // 关闭当前方向阅读的文件句柄 (不调 closeTxtReader: 它会渲染文件管理器全刷,
    // 旋转时闪浏览器画面, 用户误以为跳回文件管理器)
    if (txtIndexScanFile) txtIndexScanFile.close();
    if (txtIndexBuildFile) txtIndexBuildFile.close();
    if (txtChapterBuildFile) txtChapterBuildFile.close();
    if (txtFile) txtFile.close();
    readerRot = target;
    settingsSetPortrait(rotToStored(readerRot));   // 全局持久: 下次开机/打开阅读保持此方向
    startTxtReader(txtPath.c_str(), false);   // 按新方向重开 (新索引不存在则后台构建)
}

// 标签子菜单弹窗 (跟随阅读方向): [标记本页][历史标记], 右长执行, 中长取消回菜单
void renderMarkMenuOverlay() {
    fbRot = readerRot;
    const int MX = readerIsPortrait() ? 8 : 20;
    const int MY = readerIsPortrait() ? 8 : 0;
    const int MW = readerIsPortrait() ? 112 : 236;
    const int MH = readerIsPortrait() ? 280 : 128;
    fillRect(MX, MY, MW, MH, false);
    drawRect(MX, MY, MW, MH, true);
    drawTextUTF8(MX + 5, MY + 4, "标签", MW - 10, true);
    static const char *const opts[2] = {"标记本页", "历史标记"};
    const int rowH = 24, step = 30, top = MY + 26;
    for (int i = 0; i < 2; i++) {
        bool sel = markMenuSel == i;
        fillRect(MX + 6, top + i * step, MW - 12, rowH, sel);
        drawRect(MX + 6, top + i * step, MW - 12, rowH, !sel);
        drawTextUTF8(MX + 14, top + i * step + 4, opts[i], MW - 32, !sel);
    }
    refresh(false);
    fbRot = 90;
}

void execReaderMenu() {
    switch (readerMenuSel) {
        case 0: {  // 字体选择: 自带/外部 (外部加载失败回落自带, 对齐 A7 "外部字体初始化失败")
            fontExternal = !fontExternal;
            if (fontExternal) {
                if (!initExternalFont()) {
                    fontExternal = false;
                    snprintf(readerMenuNote, sizeof(readerMenuNote), "外部字体初始化失败");
                } else snprintf(readerMenuNote, sizeof(readerMenuNote), "已使用外部字体");
            } else {
                u8g2Fonts.setFont(chinese_gb2312);
                snprintf(readerMenuNote, sizeof(readerMenuNote), "已使用自带字体");
            }
            renderReaderMenu();
            break;
        }
        case 1:  // 退出
            closeReaderMenu();
            break;
        case 2: {  // 自动翻页: 循环档位 0=关/1/2/5/10/25; 超上限提示 (对齐 A7 "换页倍率过高")
            static const uint8_t speeds[6] = {0, 1, 2, 5, 10, 25};
            int idx = 0;
            while (idx < 5 && speeds[idx] != autoFlipSpeed) idx++;
            if (speeds[idx] == 25) {
                snprintf(readerMenuNote, sizeof(readerMenuNote), "换页倍率过高");
            } else {
                autoFlipSpeed = speeds[idx + 1];
                autoFlipNextMs = millis() + autoFlipIntervalMs();
                snprintf(readerMenuNote, sizeof(readerMenuNote), "自动翻页:%s", autoFlipSpeed ? "开" : "关");
            }
            renderReaderMenu();
            break;
        }
        case 3:  // 全刷间隔：1..10 次局刷一次全刷
            fixedRefreshEvery = (uint8_t)((fixedRefreshEvery % 10) + 1);
            snprintf(readerMenuNote, sizeof(readerMenuNote), "全刷间隔:%d次", fixedRefreshEvery);
            renderReaderMenu();
            break;
        case 4:  // 旋转: 打开四向选择弹窗 (光标选中方案, 右长确认才切换; 同向确认=仅回正文)
            readerMenuOpen = false;
            readerRotSelOpen = true;
            rotSelCursor = rotSelIndexOf(readerRot);   // 初始光标 = 当前方向
            renderRotSelOverlay();
            break;
        case 5:  // 跳转: 页码输入弹窗 (数字键盘)
            readerMenuOpen = false;
            readerJumpOpen = true;
            jumpPage = txtPage;
            jumpCursor = 0;
            jumpRejectMs = 0;
            renderJumpOverlay();
            break;
        case 6:  // 章节: 进入章节目录 (全刷, 固定横屏; 中长按返回阅读/文件管理器)
            readerMenuOpen = false;
            enterChapterList();
            break;
        case 7:  // 标签: 打开子菜单弹窗 ([标记本页][历史标记], 右长执行/中长取消)
            readerMenuOpen = false;
            readerMarkMenuOpen = true;
            markMenuSel = 0;
            renderMarkMenuOverlay();
            break;
        case 8:  // 休眠
            readerMenuOpen = false;
            enterSleepMode();
            break;
        case 9: {  // 进度同步 (直连手机 HTTP)
            // 前置检查: 仅需当前 TXT 有效且有大小。同步只依赖 txtSize + .i1[0]/sidecar 的 offset +
            // seek(offset) 读页, 不依赖完整索引/页码(索引构建中也可同步, 页码仅显示近似)。
            if (txtIndexPath.length() == 0 || !txtFile || txtFile.size() == 0) {
                snprintf(readerMenuNote, sizeof(readerMenuNote), "无法同步");
                renderReaderMenu();
                break;
            }
            readerMenuOpen = false;
            readerSyncOpen = true;
            progressSyncBegin(txtPath);
            if (progressSyncState() == SYNC_IDLE) {
                // begin 后状态恒为 PREPARE(快照在状态机内); 快照失败 → 错误页展示
                readerMenuOpen = true;
                readerSyncOpen = false;
                strncpy(readerMenuNote, progressSyncStatusText(), sizeof(readerMenuNote) - 1);
                renderReaderMenu();
                break;
            }
            break;
        }
        case 10: {  // 配网: 直接启动热点 (管理 web), 退出后回到阅读
            gNetworkReturnMode = APP_READER;
            readerMenuOpen = false;
            readerSyncOpen = false;
            progressSyncFreeReaderHeap();   // 启动热点前腾堆: 关 txtFile + 清阅读行缓冲 (配网会话堆硬约束)
            freeItemList();   // 大目录 items≈34KB+ 是堆大户, 配网会话堆 ~5KB 必须释放
            fsCacheBuild();   // 进 AP 前扫描 SD → LittleFS 缓存（浏览不碰 SD）
            appMode = APP_NETWORK;
            saveSleepRecord();
            wifiManagerBegin(renderNetworkPage, exitNetworkPage);
            break;
        }
    }
}

// ---------- 进度同步：宿主钩子 (progress_sync.cpp 回调) ----------
// 本地进度读取/应用/UI/结束收尾, 由本文件访问全局阅读器状态; progress_sync.cpp 不碰 .i1。

bool progressSyncSnapshot(const String& txtPath, uint32_t& localOffset, uint32_t& txtSize, float& localPercent) {
    if (!txtFile) {
        reinitSdBus("sync_snap");   // 网络阶段 GPIO12/GPIO5 可能被电量采样动过, 先恢复 SD 总线
        txtFile = SD.open(txtPath.c_str());
    }
    if (!txtFile) return false;
    txtSize = txtFile.size();
    if (txtSize == 0) return false;
    localOffset = readProgressOffset();
    localPercent = (float)localOffset * 100.0f / (float)txtSize;
    if (localPercent > 100.0f) localPercent = 100.0f;
    return true;
}

// 网络阶段腾堆: BearSSL TLS 握手需要 ~4KB+ 堆 (context + iobuf), 阅读器打开时堆只剩 ~6KB。
// 释放: 阅读行缓冲 txtLines (~1KB, 可随时重读) + 关闭 txtFile 句柄 (~200B, ApplyRemote/Done 重开)。
// 构建用 indexRows/txtIndexScanFile 不受影响 (构建中同步也安全)。
void progressSyncFreeReaderHeap() {
    for (int i = 0; i < txtLineCount(); i++) txtLines[i] = String();
    if (txtFile) txtFile.close();
}
void progressSyncRestoreReaderHeap() {
    if (!txtFile) {
        reinitSdBus("sync_restore");
        txtFile = SD.open(txtPath.c_str());
    }
    if (txtFile && txtPageStart <= txtFile.size()) readTxtPage(txtPageStart);
}

bool progressSyncApplyRemote(uint32_t offset) {
    reinitSdBus("sync_apply");   // 网络阶段 GPIO12/GPIO5 可能被电量采样动过, 先恢复 SD 总线
    if (!txtFile) txtFile = SD.open(txtPath.c_str());   // 网络阶段 Free hook 可能已关闭, 重开
    if (!txtFile || offset > txtFile.size()) {
        traceFmtLevel('E', "APPLY_FAIL txtFile=%d offset=%lu size=%lu",
                      (int)(txtFile ? 1 : 0), (unsigned long)offset,
                      txtFile ? (unsigned long)txtFile.size() : 0UL);
        return false;
    }
    uint32_t page = 1;
    uint32_t pageStart = offset;   // 默认: 手机 offset 直读 (索引不可用时, 构建中以 offset 为准)
    if (!txtIndexBuilding && txtTotalPages > 0) {
        // 分页制度: 手机 offset 若落在两页之间 → 向下取整到所在页页首。
        // 页表单调递增: offsetToPage 二分找"最大页首 ≤ offset"的页码, parsePageRecord 取该页页首偏移。
        page = offsetToPage(offset);
        uint32_t ps = (page > 1) ? parsePageRecord(page) : 0;
        if (ps <= offset) pageStart = ps;   // 页首 ≤ offset 才采用 (向下取整, 保留所在页完整内容)
    }
    if (!writeProgress(pageStart)) return false;   // 持久化 .i1[0] 失败 → 如实报错, 不假装成功
    txtPage = page;
    txtPageStart = pageStart;
    readTxtPage(pageStart);     // 从页首读, 保证分页排版对齐(不破坏页表一致性)
    return true;
}

void progressSyncDone(bool ok) {
    (void)ok;
    wifiManagerStopSta();       // Wi-Fi OFF
    SPI.begin();                // 恢复 SPI (网络不占 SPI, 保险恢复)
    progressSyncRestoreReaderHeap();   // 重读当前页, 填充网络阶段被释放的行缓冲
    readerSyncOpen = false;
    appMode = APP_READER;
    renderTxtPage(true);        // 渲染当前 txtPage (同步/取消/错误后都回到阅读页)
}

// 渲染当前同步界面: 状态/比较/错误 页 (局部刷新覆盖, 不跳独立页面)
void progressSyncRender(int state) {
    fbRot = readerRot;
    const int MX = readerIsPortrait() ? 6 : 8, MY = readerIsPortrait() ? 6 : 0;
    const int MW = readerIsPortrait() ? 116 : 280, MH = readerIsPortrait() ? 284 : 128;
    fillRect(MX, MY, MW, MH, false);
    drawRect(MX, MY, MW, MH, true);
    int y = MY + 4;
    if (progressSyncFileFingerprintState() == 2) {
        // v3 MISMATCH: 黑底白字醒目警告替换标题 (⚠ 不在 GB2312 字库, 用 !! )
        uint8_t mm = progressSyncFileMismatch();
        // 4 段全置时最长 24+4×7=52 字节 + NUL; 旧 48B 缓冲 4 位全中时 snprintf 返回值虚增
        // → warn[wl-1] 越界读/写 (栈破坏)。逐段追加后重取实际 strlen, 不用 snprintf 返回值。
        char warn[56] = "！！文件不一致：";
        size_t wl = strlen(warn);
        if (mm & 1) { snprintf(warn + wl, sizeof(warn) - wl, "大小/"); wl = strlen(warn); }
        if (mm & 2) { snprintf(warn + wl, sizeof(warn) - wl, "头部/"); wl = strlen(warn); }
        if (mm & 4) { snprintf(warn + wl, sizeof(warn) - wl, "中部/"); wl = strlen(warn); }
        if (mm & 8) { snprintf(warn + wl, sizeof(warn) - wl, "尾部/"); wl = strlen(warn); }
        if (wl > 0 && warn[wl - 1] == '/') warn[--wl] = '\0';
        fillRect(MX + 4, MY + 4, MW - 8, 16, true);          // 黑底 (盖标题区, 不碰下方)
        drawTextUTF8(MX + 6, MY + 4, warn, MW - 12, false);  // 白字同基线
        y += 16;   // 标题行占位不变, 后续行不下移
    } else {
        drawTextUTF8(MX + 4, y, "进度同步", 100, true); y += 16;
    }
    int i = txtPath.lastIndexOf('/');
    String name = i >= 0 ? txtPath.substring(i + 1) : txtPath;
    drawTextUTF8(MX + 4, y, name.c_str(), MW - 8, false); y += 18;
    char line[40];

    if (state == SYNC_COMPARE) {
        snprintf(line, sizeof(line), "本地 %.2f%%", (double)progressSyncLocalPercent());
        drawTextUTF8(MX + 4, y, line, MW - 8, true); y += 16;
        snprintf(line, sizeof(line), "%lu B", (unsigned long)progressSyncLocalSize());
        drawTextUTF8(MX + 4, y, line, MW - 8, false); y += 18;
        snprintf(line, sizeof(line), "手机 %.2f%%", (double)progressSyncRemotePercent());
        drawTextUTF8(MX + 4, y, line, MW - 8, true); y += 16;
        snprintf(line, sizeof(line), "%lu B", (unsigned long)progressSyncRemoteOffset());
        drawTextUTF8(MX + 4, y, line, MW - 8, false); y += 18;
        uint64_t ts = progressSyncRemoteTsMs();
        if (ts > 0) {
            time_t t = (time_t)(ts / 1000ULL);
            struct tm* tmv = localtime(&t);
            if (tmv) {
                snprintf(line, sizeof(line), "更新 %04d-%02d-%02d %02d:%02d",
                         tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_hour, tmv->tm_min);
                drawTextUTF8(MX + 4, y, line, MW - 8, false); y += 16;
            }
        }
        // [同步=从手机拉取] [覆盖=推送到手机] 两键按钮 (右短/中短 移动, 右长 执行, 中长 取消)
        // 语义: 选项0=手机→本地(拉取), 选项1=本地→手机(推送); 名字带方向消除歧义
        const int bh = 18;
        int by = MY + MH - bh - 8;   // 按钮行 y (提示行按此上移 16px)
        const int sel = progressSyncCompareSel();
        const bool b1sel = (sel == 0);
        const bool b2sel = (sel == 1);
        if (readerIsPortrait()) {
            // 竖屏: 按钮上下叠放
            const int bw = MW - 12;
            const int by2 = MY + MH - 8 - bh;
            const int by1 = by2 - bh - 6;
            by = by1;
            fillRect(MX + 6, by1, bw, bh, b1sel);
            drawRect(MX + 6, by1, bw, bh, !b1sel);
            drawTextUTF8(MX + 12, by1 + 3, "同步=从手机拉取", bw - 12, !b1sel);
            fillRect(MX + 6, by2, bw, bh, b2sel);
            drawRect(MX + 6, by2, bw, bh, !b2sel);
            drawTextUTF8(MX + 12, by2 + 3, "覆盖=推送到手机", bw - 12, !b2sel);
        } else {
            const int bw = 126;
            const int b1x = MX + 8, b2x = MX + MW - 8 - bw;
            fillRect(b1x, by, bw, bh, b1sel);
            drawRect(b1x, by, bw, bh, !b1sel);
            drawTextUTF8(b1x + 6, by + 3, "同步=从手机拉取", bw - 4, !b1sel);
            fillRect(b2x, by, bw, bh, b2sel);
            drawRect(b2x, by, bw, bh, !b2sel);
            drawTextUTF8(b2x + 6, by + 3, "覆盖=推送到手机", bw - 4, !b2sel);
        }
        drawTextUTF8(MX + 4, by - 16,
                     progressSyncConfirmUploadPending() ? "再按右长确认推送到手机" : "中短/右短 移动  中长取消 右长确认",
                     MW - 8, progressSyncConfirmUploadPending());
    } else if (state == SYNC_ERROR) {
        drawTextUTF8(MX + 4, y, progressSyncStatusText(), MW - 8, true);
        drawTextUTF8(MX + 4, MY + MH - 26, "中长/右长 返回", MW - 8, false);
    } else {
        drawTextUTF8(MX + 4, y, progressSyncStatusText(), MW - 8, true);
        drawTextUTF8(MX + 4, MY + MH - 26, "进度同步中...", MW - 8, false);
    }
    refresh(false);
    fbRot = 90;
}

// ---------- 界面快照 (SD 文件, KEY1 断电复位后恢复) ----------
// 每次界面切换时保存当前 appMode 和位置; KEY1 复位重启后据此恢复同一界面
// (首页→首页, 文件管理器→同目录, 阅读→同页, 章节目录→同目录位置), 避免误跳阅读页。
struct SleepRecord {
    uint32_t magic;               // 0x55495354 'UIST'
    uint8_t  mode;                // 保存时 appMode
    uint8_t  chapterSpeed;        // 章节倍速 1/2/5/10/25/50/100
    uint8_t  chapterSpeedPopup;   // 倍速弹窗是否打开
    uint8_t  chapterSpeedSel;     // 弹窗选中 0..6
    int16_t  chapterPage;         // 章节目录页码 1-based
    int16_t  chapterSel;          // 章节选中 0..8 (0-5 章节, 6 上一页, 7 下一页, 8 倍速)
    int16_t  selIndex;            // 文件列表选中项
    int16_t  topIndex;            // 文件列表可视区顶部项
    char     path[64];            // 浏览器当前路径 (UTF-8)
    uint8_t  pad[1];
};
const uint32_t SLEEP_RECORD_MAGIC = 0x55495354UL;   // 'UIST' 新版界面快照

void saveSleepRecord() {
    SleepRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = SLEEP_RECORD_MAGIC;
    // 标签列表非阅读上下文: 快照降级为阅读页, 唤醒直接回正文且方向/进度原样 (仿 BMP→浏览器先例)
    rec.mode = (uint8_t)(appMode == APP_MARKS ? APP_READER : appMode);
    rec.chapterSpeed = chapterSpeed;
    rec.chapterSpeedPopup = chapterSpeedPopup ? 1 : 0;
    rec.chapterSpeedSel = (uint8_t)chapterSpeedSel;
    rec.chapterPage = (int16_t)chapterPage;
    rec.chapterSel = (int16_t)chapterSel;
    rec.selIndex = (int16_t)selIndex;
    rec.topIndex = (int16_t)topIndex;
    currentPath.toCharArray(rec.path, sizeof(rec.path));
    // 快照可能发生在 EPD 刷新之后；写 SD 前统一恢复共享 SPI 总线。
    if (!reinitSdBus("ui_save")) {
        debugLine("SLEEP_SAVE sd-reinit-fail");
        return;
    }
    if (!SD.exists("/.tiemereader")) SD.mkdir("/.tiemereader");
    // FILE_WRITE 在当前 SD 库中可能是追加模式；先删除旧快照，避免读取到旧记录。
    bool removed = SD.exists(SLEEP_RECORD_PATH) ? SD.remove(SLEEP_RECORD_PATH) : true;
    debugFmt("UI_SAVE_PRE exists=%d removed=%d", SD.exists(SLEEP_RECORD_PATH) ? 1 : 0, removed ? 1 : 0);
    File f = SD.open(SLEEP_RECORD_PATH, FILE_WRITE);
    if (!f) {
        debugLine("SLEEP_SAVE open-fail");
        return;
    }
    size_t wrote = f.write((const uint8_t *)&rec, sizeof(rec));
    f.flush();
    uint32_t pos = f.position();
    f.close();
    // 写后验证: 文件存在且大小正确
    File vf = SD.open(SLEEP_RECORD_PATH, FILE_READ);
    uint32_t vsize = vf ? vf.size() : 0;
    if (vf) vf.close();
    debugFmt("SLEEP_SAVE mode=%d speed=%u popup=%u sel=%u page=%d chSel=%d selIdx=%d top=%d path=%s exist=%d size=%u wrote=%u pos=%lu",
             rec.mode, rec.chapterSpeed, rec.chapterSpeedPopup, rec.chapterSpeedSel,
             (int)rec.chapterPage, (int)rec.chapterSel,
             (int)rec.selIndex, (int)rec.topIndex, rec.path,
             SD.exists(SLEEP_RECORD_PATH) ? 1 : 0, (unsigned)vsize,
             (unsigned)wrote, (unsigned long)pos);
}

// 读取并清除休眠记录; 返回 true 且填充 rec 表示有有效记录。
bool readSleepRecord(SleepRecord &rec) {
    if (!SD.exists(SLEEP_RECORD_PATH)) {
        debugLine("SLEEP_READ no-file");
        return false;
    }
    File f = SD.open(SLEEP_RECORD_PATH, FILE_READ);
    if (!f) {
        debugLine("SLEEP_READ open-fail");
        return false;
    }
    size_t got = f.read((uint8_t *)&rec, sizeof(rec));
    f.close();
    SD.remove(SLEEP_RECORD_PATH);   // 一次性: 读完即删, 避免下次误用
    if (got != sizeof(rec)) {
        debugFmt("SLEEP_READ short got=%u", (unsigned)got);
        return false;
    }
    if (rec.magic != SLEEP_RECORD_MAGIC) {
        debugFmt("SLEEP_READ bad-magic=%08lX", (unsigned long)rec.magic);
        return false;
    }
    debugFmt("SLEEP_READ mode=%d speed=%u popup=%u sel=%u page=%d chSel=%d selIdx=%d top=%d path=%s",
             rec.mode, rec.chapterSpeed, rec.chapterSpeedPopup, rec.chapterSpeedSel,
             (int)rec.chapterPage, (int)rec.chapterSel,
             (int)rec.selIndex, (int)rec.topIndex, rec.path);
    return true;
}

void enterSleepMode() {
    traceLine("SLEEP_ENTER");
    debugFmt("SLEEP mode=%d page=%lu chPage=%d chSel=%d speed=%u popup=%d",
             appMode, (unsigned long)txtPage, chapterPage, chapterSel, chapterSpeed,
             chapterSpeedPopup ? 1 : 0);
    if (appMode == APP_READER && txtIndexPath.length()) writeProgress(txtPageStart);
    if (txtIndexScanFile) txtIndexScanFile.close();
    if (txtIndexBuildFile) txtIndexBuildFile.close();
    if (txtChapterBuildFile) txtChapterBuildFile.close();
    txtIndexBuilding = false;
    if (txtFile) txtFile.close();

    // 记录休眠前的界面模式到 SD (断电保留), KEY1 唤醒后恢复同一界面。
    // 必须在 EPD 深睡之前写入 — 之后 SPI 总线已归 EPD, SD 不可用。
    reinitSdBus("sleep_save");
    saveSleepRecord();

    // 进入休眠前 1 秒中央显示"休眠"提示 (用户要求), 随后右上角"休眠中"并深睡
    showMsg("休眠", "");
    drawSleepNotice();
    epd.sleep();   // SSD1680 深睡 (面板掉电), 保留 RAM 中的当前页状态

    // 深睡直到 KEY1 硬件复位 (与低电休眠 enterLowBatterySleep 同机制, RST 唤醒→setup):
    // 原浅睡循环 (wifi_fpm_do_sleep 150ms + delay 循环) 实测会 Soft WDT / Exception(4)
    // (crash 在 fpm_do_sleep), 崩溃时 FAT 目录未落盘 → 休眠记录/SD 状态丢失,
    // 重启后首页"上次阅读"记录也随之丢失。KEY1 直接驱动 RST, 深睡唤醒即复位,
    // 行为一致且更省电 (~20µA)。
    ESP.deepSleep(0);
    // 实际不会执行到这里 — deepSleep(0) 睡眠直到 RST
}

// ---------- 低电压休眠 ----------
// 电池 ≤3300mV 时调用：关闭已打开文件 -> 全刷低电通知 -> deepSleep(0) 彻底断电。
// KEY1 硬件复位唤醒 -> setup() 正常启动（从上次成功写入的 sleep record 恢复界面）。
// 刻意不写 SD（saveSleepRecord/索引）：电压不稳时写卡可能损坏数据，唤醒后
// 从上次成功保存的 record 恢复即可；低压目标是最快断电。
void drawLowBatteryNotice() {
    fillRect(0, 0, SCR_W, SCR_H, false);          // 结构变化页面，允许全刷
    drawRect(0, 0, SCR_W - 1, SCR_H - 1, true);
    // 电池图标（横屏居中）
    const int bx = (SCR_W - 80) / 2, by = 16, bw = 80, bh = 32;
    drawRect(bx, by, bw, bh, true);
    fillRect(bx + bw, by + 8, 6, bh - 16, true);  // 正极帽
    fillRect(bx + 8, by + 8, 28, bh - 16, true);  // 低电量填充条
    const char *t1 = "电量过低";
    int w1 = utf8Width(t1);
    drawTextUTF8((SCR_W - w1) / 2, 66, t1, SCR_W - 8, true);
    const char *t2 = "请充电后再开机";
    int w2 = utf8Width(t2);
    drawTextUTF8((SCR_W - w2) / 2, 92, t2, SCR_W - 8, true);
    refresh(true);
}

void enterLowBatterySleep() {
    traceLine("LOWBAT_SLEEP");
    if (txtIndexScanFile) txtIndexScanFile.close();
    if (txtIndexBuildFile) txtIndexBuildFile.close();
    if (txtChapterBuildFile) txtChapterBuildFile.close();
    txtIndexBuilding = false;
    if (txtFile) txtFile.close();
    // 释放 SPI 总线给 EPD 画通知（SD CS 保持高，不重新挂载 SD）
    digitalWrite(EPD_CS_PIN, HIGH);
    digitalWrite(5, HIGH);
    pinMode(5, OUTPUT);
    SPI.begin();
    drawLowBatteryNotice();
    ESP.deepSleep(0);  // 断电（自动关闭 RF）；KEY1 复位唤醒
}

// ---------- 低电压检测 ----------
#define BAT_LOW_MV 3300
const uint32_t BAT_CHECK_MS = 60000UL;

// 周期采样：≤3300mV 时二次采样确认（滤瞬时波动），仍低则进入低电休眠。
// 索引构建期间跳过（避免打断耗时流程 + 采样时 GPIO12 与 SD MISO 冲突）。
// 返回 true 表示已进入休眠（loop 应停止后续处理）。
bool checkLowBattery() {
    if (txtIndexBuilding) return false;
    int v1 = readBatteryMV();
    if (v1 > BAT_LOW_MV) return false;
    delay(50);
    int v2 = readBatteryMV();
    if (v2 > BAT_LOW_MV) return false;
    traceFmt("LOWBAT detect v1=%d v2=%d", v1, v2);
    enterLowBatterySleep();
    return true;
}

void nextTxtPage() {
    traceFmt("PAGE_NEXT from=%lu", (unsigned long)txtPage);
    if (txtIndexBuilding && txtPage >= txtIndexedPages) {
        showMsg("索引构建中", "请稍候再翻页");
        return;
    }
    if (txtPage >= txtTotalPages) return;
    txtPage++;
    txtPageStart = parsePageRecord(txtPage);
    readTxtPage(txtPageStart);
    writeProgress(txtPageStart);
    renderTxtPage(false);
}

void previousTxtPage() {
    traceFmt("PAGE_PREVIOUS from=%lu", (unsigned long)txtPage);
    if (txtPage <= 1) {
        showMsg("已是第一页", "");
        return;
    }
    txtPage--;
    txtPageStart = parsePageRecord(txtPage);
    readTxtPage(txtPageStart);
    writeProgress(txtPageStart);
    renderTxtPage(false);
}

void loadChapterRows(uint32_t offset) {
    // ⚠️ 必须恢复 SD 总线: 翻页前列表/倍速弹窗都是局刷(EPD 侧), 直接 SD.open 会失败
    // → chapterCountLoaded=0 → 列表空 (实测 100x 翻页后列表直接空, 根因同标签系统白屏)。
    if (!reinitSdBus("chapter_load")) {
        chapterCountLoaded = 0;
        return;
    }
    chapterCountLoaded = 0;
    chapterTopOffset = offset;
    File f = SD.open(txtChapterPath.c_str());
    if (!f) return;
    f.seek(offset);
    while (chapterCountLoaded < CHAPTER_ROWS && f.available()) {
        uint32_t rowOffset = f.position();
        String line = f.readStringUntil('\n');
        line.trim();
        int dash = line.lastIndexOf('-');
        if (dash <= 0) continue;
        chapterRowOffsets[chapterCountLoaded] = rowOffset;
        String title = line.substring(0, dash);
        title.toCharArray(chapterRows[chapterCountLoaded].title, sizeof(chapterRows[chapterCountLoaded].title));
        chapterRows[chapterCountLoaded].page = strtoul(line.substring(dash + 1).c_str(), nullptr, 10);
        chapterCountLoaded++;
        ESP.wdtFeed();   // 单行处理喂狗 (大目录加载)
    }
    chapterNextOffset = f.position();   // 下一页起始偏移
    f.close();
}

// 章节目录页偏移表构建: 一次性扫描 .z1, 每 CHAPTER_ROWS 章记录一个页首偏移。
// chapterPageOffsets[p-1] = 第 p 页首行偏移 (第 1 页 = 0); chapterTotalPages 据此精确修正
// (修复 .z1 末尾空行/非标准行使 countTxtChapters 偏大的问题)。建表一次 O(n) (6000 章约
// 2.2s, 逐行喂狗), 之后 chapterNextPage/chapterPrevPage 全部 O(1) 查表, 不再任何全扫。
static uint32_t chapterPageTableCount = 0;   // 已建表页数 (== chapterTotalPages 精确值)

static void chapterBuildPageTable() {
    if (!reinitSdBus("chapter_table")) return;
    File f = SD.open(txtChapterPath.c_str());
    if (!f) { chapterPageTableCount = 0; return; }
    chapterPageOffsets[0] = 0;
    chapterPageTableCount = 1;
    uint32_t chapterIdx = 0;   // 已读有效章节行数
    while (f.available()) {
        uint32_t pos = f.position();
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.lastIndexOf('-') > 0) {   // 与 loadChapterRows 同判定 (dash>0 即有效行)
            chapterIdx++;
            if ((chapterIdx % CHAPTER_ROWS) == 0) {
                if (chapterPageTableCount < CHAPTER_PAGE_TABLE_MAX)
                    chapterPageOffsets[chapterPageTableCount++] = pos;   // 下一页首偏移
            }
        }
        ESP.wdtFeed();
    }
    f.close();
    // 最后一页可能不足 CHAPTER_ROWS 行, 但 offset 已由"翻页时 loadChapterRows"定位, 表只需覆盖
    // chapterPageTableCount-1 个完整页; chapterTotalPages 精确 = 完整页数 + 1 (末页可能有残行)
    traceFmt("CH_TABLE built pages=%lu", (unsigned long)chapterPageTableCount);
}

// 统计 .z1 总章节数 (每行一章)
uint32_t countTxtChapters() {
    if (!reinitSdBus("chapter_count")) return 0;   // 可能被局刷后调用, 先恢复总线
    File f = SD.open(txtChapterPath.c_str());
    if (!f) return 0;
    uint32_t n = 0;
    while (f.available()) {
        if (f.read() == '\n') n++;
        ESP.wdtFeed();   // 逐字节扫描大 .z1 (6000+ 章) 必须喂狗
    }
    f.close();
    return n;
}

// 定位第 chapterIndex 章 (0-based) 在 .z1 中的字节偏移 (从0扫描计数)
uint32_t seekChapterOffset(uint32_t chapterIndex) {
    if (!reinitSdBus("chapter_seek")) return 0;   // 可能被局刷后调用, 先恢复总线
    File f = SD.open(txtChapterPath.c_str());
    if (!f) return 0;
    uint32_t idx = 0;
    uint32_t off = 0;
    while (f.available()) {
        uint32_t pos = f.position();
        String line = f.readStringUntil('\n');
        line.trim();
        int dash = line.lastIndexOf('-');
        if (dash > 0) {
            if (idx == chapterIndex) { off = pos; break; }
            idx++;
        }
        ESP.wdtFeed();   // 全文件扫描兜底喂狗 (缓存 miss 时才会走到这里)
    }
    f.close();
    return off;
}

void chapterNextPage() {
    int n = chapterSpeed ? chapterSpeed : 1;
    for (int i = 0; i < n; i++) {
        if (chapterPage >= chapterTotalPages) break;
        // 下一页定位: 直接用上一页读完后的 chapterNextOffset (增量, O(1), 不扫文件)
        uint32_t prevOff = chapterTopOffset;   // 回退用: 翻页前的当前页偏移
        uint32_t nextOff = chapterNextOffset;
        chapterPage++;
        loadChapterRows(nextOff);
        if (chapterCountLoaded == 0) {
            // 翻过末尾/EOF: 回退页码并 O(1) 重载翻页前的页 (不查缓存不 seek, 否则 6000 章全扫 2.5s/次)
            chapterPage--;
            // .z1 末尾若有空行/非标准行, countTxtChapters 算出的总页数偏大 → 收缩, 防"最后一页翻不动"假死
            if (chapterTotalPages > chapterPage) chapterTotalPages = chapterPage;
            loadChapterRows(prevOff);
            break;
        }
        ESP.wdtFeed();
    }
    chapterSel = 7;   // 翻页后光标停在"下一页"按钮, 便于连续翻页
    renderChapterList(false);
}

void chapterPrevPage() {
    int n = chapterSpeed ? chapterSpeed : 1;
    for (int i = 0; i < n; i++) {
        if (chapterPage <= 1) break;
        // 向前翻页定位: 查页偏移表 (chapterPageOffsets[目标页-1], O(1))。
        // 表由 chapterBuildPageTable 一次性建立 (进入章节/恢复时), 覆盖全书任意页,
        // 不再需要偏移栈/seek 兜底 (修复"恢复章节后向前翻栈空 → 每步全扫 2.2s 卡死")。
        uint32_t prevOff = chapterTopOffset;   // 回退用: 翻页前的当前页偏移
        uint32_t targetPage = (uint32_t)(chapterPage - 1);
        uint32_t targetOff = 0;
        if (targetPage >= 1 && targetPage <= chapterPageTableCount) {
            targetOff = chapterPageOffsets[targetPage - 1];
        } else {
            // 表未覆盖 (理论上仅在表未建/建表失败时): 回退全文件扫描 (带喂狗)
            targetOff = seekChapterOffset((uint32_t)(targetPage - 1) * CHAPTER_ROWS);
        }
        chapterPage--;
        loadChapterRows(targetOff);
        if (chapterCountLoaded == 0) {
            // 定位失败: 回退页码并 O(1) 重载翻页前的页, 防空列表
            chapterPage++;
            loadChapterRows(prevOff);
            break;
        }
        ESP.wdtFeed();
    }
    chapterSel = 6;   // 翻页后光标停在"上一页"按钮, 便于连续翻页
    renderChapterList(false);
}

void chapterMoveSelection(int delta) {
    const int rows = chapterCountLoaded;
    const int orderCount = rows + 3; // 有效章节行 + 上一页/下一页/倍速
    if (orderCount <= 0) return;
    int pos;
    if (chapterSel < rows) pos = chapterSel;
    else if (chapterSel == 6) pos = rows;
    else if (chapterSel == 7) pos = rows + 1;
    else pos = rows + 2;
    pos = (pos + delta + orderCount) % orderCount;
    if (pos < rows) chapterSel = pos;
    else if (pos == rows) chapterSel = 6;
    else if (pos == rows + 1) chapterSel = 7;
    else chapterSel = 8;
}

// 章节目录底部栏元素 (上一页/下一页/倍速), idx: 6/7/8
void drawChapterBottomElement(int idx, const char *label) {
    static const int ex[3] = {77, 150, 223};
    static const int ew[3] = {69, 69, 69};
    int x = ex[idx - 6], w = ew[idx - 6];
    bool sel = chapterSel == idx;
    fillRect(x, 110, w, 18, sel);
    drawRect(x, 110, w, 18, !sel);
    int tw = utf8Width(label);
    drawTextUTF8(x + (w - tw) / 2, 111, label, tw + 2, !sel);
}

void renderChapterList(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    // 顶栏: 标题 + 右上角 "本页最后项/总章节数" (如 6/35)
    drawTextUTF8(2, 0, "章节目录", 100, true);
    uint32_t total = txtChapterCount;
    uint32_t lastShown = 0;
    if (total > 0) {
        lastShown = (uint32_t)(chapterPage - 1) * CHAPTER_ROWS + chapterCountLoaded;
        if (lastShown > total) lastShown = total;
    }
    char headerInfo[20];
    snprintf(headerInfo, sizeof(headerInfo), "%lu/%lu", (unsigned long)lastShown, (unsigned long)total);
    drawTextUTF8(SCR_W - 4 - utf8Width(headerInfo), 0, headerInfo, 90, true);
    fillRect(0, 16, SCR_W, 1, true);
    // 6 行, 15px 行距: 标题 (无序号) + 页码右对齐
    for (int i = 0; i < chapterCountLoaded; i++) {
        int y = 18 + i * 15;
        bool selected = i == chapterSel;
        fillRect(0, y, SCR_W, 15, selected);
        int fg = !selected;
        char pageStr[12];
        snprintf(pageStr, sizeof(pageStr), "%lu", (unsigned long)chapterRows[i].page);
        int pageW = utf8Width(pageStr);
        int titleMax = SCR_W - 8 - pageW - 6;
        char disp[56];
        utf8Truncate(chapterRows[i].title, disp, titleMax, sizeof(disp));
        drawTextUTF8(2, y, disp, titleMax, fg);
        drawTextUTF8(SCR_W - 4 - pageW, y, pageStr, pageW + 4, fg);
    }
    // 底部栏: [页码 n1/n2] [上一页] [下一页] [x倍速]
    fillRect(0, 108, SCR_W, 20, false);
    fillRect(0, 108, SCR_W, 1, true);
    char pageInd[16];
    snprintf(pageInd, sizeof(pageInd), "%d/%d", chapterPage, chapterTotalPages);
    drawTextUTF8(4, 111, pageInd, 69, true);
    drawChapterBottomElement(6, "上一页");
    drawChapterBottomElement(7, "下一页");
    char spd[8];
    snprintf(spd, sizeof(spd), "x%d", chapterSpeed);
    drawChapterBottomElement(8, spd);
    refresh(full);
}

void renderChapterSpeedPopup() {
    static const uint8_t speeds[7] = {1, 2, 5, 10, 25, 50, 100};
    const int MX = 76, MY = 42, MW = 144, MH = 50;   // 42..92
    fillRect(MX, MY, MW, MH, false);
    drawRect(MX, MY, MW, MH, true);
    const int r1w = (MW - 8 - 12) / 4;   // 31
    for (int i = 0; i < 4; i++) {
        int bx = MX + 4 + i * (r1w + 4);
        bool sel = chapterSpeedSel == i;
        fillRect(bx, MY + 4, r1w, 19, sel);
        drawRect(bx, MY + 4, r1w, 19, !sel);
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "x%d", speeds[i]);
        int tw = utf8Width(lbl);
        drawTextUTF8(bx + (r1w - tw) / 2, MY + 4 + 2, lbl, tw + 2, !sel);
    }
    const int r2w = (MW - 8 - 8) / 3;   // 42
    for (int i = 0; i < 3; i++) {
        int bx = MX + 4 + i * (r2w + 4);
        int idx = 4 + i;
        bool sel = chapterSpeedSel == idx;
        fillRect(bx, MY + 27, r2w, 19, sel);
        drawRect(bx, MY + 27, r2w, 19, !sel);
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "x%d", speeds[idx]);
        int tw = utf8Width(lbl);
        drawTextUTF8(bx + (r2w - tw) / 2, MY + 27 + 2, lbl, tw + 2, !sel);
    }
    refresh(false);
}

void enterChapterList() {
    traceLine("CHAPTER_ENTER");
    if (txtIndexBuilding) {
        showMsg("索引构建中", "请稍候再进章节");
        return;
    }
    if (txtChapterCount == 0) txtChapterCount = countTxtChapters();
    appMode = APP_CHAPTERS;
    chapterPage = 1;
    // 建页偏移表 (一次 O(n) 扫描, 之后翻页全 O(1)); chapterTotalPages 用精确页数
    chapterBuildPageTable();
    chapterTotalPages = chapterPageTableCount > 0 ? (int)chapterPageTableCount : 0;
    chapterSel = 0;
    loadChapterRows(0);
    renderChapterList(true);
    saveSleepRecord();   // 界面快照: 已进入章节目录
}

// 中止后台索引构建 (旋转切换方向用): 关闭构建句柄 + 清标志 + 删 sidecar。
// 部分 .i1/.z1 页表保留 (下次打开续建, 不从头全量); sidecar 是"构建中进度",
// 中止后进度失效, 删除避免下次恢复旧进度跳页。
static void abortIndexBuild() {
    if (txtIndexScanFile) txtIndexScanFile.close();
    if (txtIndexBuildFile) txtIndexBuildFile.close();
    if (txtChapterBuildFile) txtChapterBuildFile.close();
    txtIndexBuilding = false;
    idxReleaseBuf();   // 生命周期化: 中止构建释放块读缓冲
    resetIndexReaderState();   // 中止即清残留: 后续 beginTxtIndexBuild/续建入口再 reset 一次, 双保险
    String sidecarPath = txtIndexPath + "p";
    if (SD.exists(sidecarPath.c_str())) SD.remove(sidecarPath.c_str());
}

void closeTxtReader() {
    if (txtIndexBuilding) {
        // 构建中退出: 保留构建句柄后台继续 (loop 公共 indexTaskStep 继续喂),
        // 只关 txtFile; finishTxtIndexBuild 用扫描句柄取 size, 不受影响。
        if (txtFile) txtFile.close();
        appMode = APP_BROWSER;
        listDir(currentPath.c_str());
        renderAll();
        refresh(true);
        saveSleepRecord();
        debugLine("CLOSE keep index building in background");
        return;
    }
    if (txtIndexScanFile) txtIndexScanFile.close();
    if (txtIndexBuildFile) txtIndexBuildFile.close();
    if (txtChapterBuildFile) txtChapterBuildFile.close();
    txtIndexBuilding = false;
    if (txtFile) txtFile.close();
    appMode = APP_BROWSER;
    listDir(currentPath.c_str());
    renderAll();
    refresh(true);
    saveSleepRecord();
}

void leaveReaderToBrowser() { closeTxtReader(); }

bool isTxtPath(const char *path) {
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path || dot[1] == '\0') return false;
    String ext = dot + 1;
    ext.toLowerCase();
    return ext == "txt";
}

void removeLegacyIndexFiles() {
    String legacyIndex = txtPath + ".i1";
    String legacyChapter = txtPath + ".z1";
    if (legacyIndex != txtIndexPath && SD.exists(legacyIndex.c_str())) SD.remove(legacyIndex.c_str());
    if (legacyChapter != txtChapterPath && SD.exists(legacyChapter.c_str())) SD.remove(legacyChapter.c_str());
}

// 页表二分查找: 记录[1..N-2] 严格递增 (已校验) → 二分找 offset==saved 的页。
// 替代 14 万条顺序扫描 (1.1MB 顺序读阻塞 2-4s, 旋转/启动恢复时屏幕长时间无变化 = "卡")。
// 返回页号 (≥2) 或 0 (未找到/索引打不开)。
uint32_t findPageByOffset(const String &indexPath, uint32_t saved) {
    File f = SD.open(indexPath.c_str(), FILE_READ);
    if (!f || f.size() < 24) {
        if (f) f.close();
        return 0;
    }
    uint32_t totalPages = (f.size() / 8) - 1;   // 记录数 - 1 (记录[0]=进度, 末条=size)
    uint32_t lo = 2, hi = totalPages, best = 0;
    while (lo <= hi) {
        uint32_t mid = (lo + hi) / 2;
        f.seek((mid - 1) * 8);   // 页 p 的记录在文件偏移 (p-1)*8
        char rec[9];
        for (uint8_t i = 0; i < 8; i++) rec[i] = (char)f.read();
        rec[8] = '\0';
        uint32_t v = strtoul(rec, nullptr, 10);
        if (v == saved) { best = mid; break; }
        if (v < saved) { best = mid; lo = mid + 1; }
        else { hi = mid - 1; }
        if ((mid & 0x3FF) == 0) { ESP.wdtFeed(); delay(0); }
    }
    f.close();
    return best;
}

// 页表二分"向上取整": 找页首 >= saved 的最小页 (旋转进度转换用)。
// ceil 语义: saved 恰在页首 → 该页 (不跳); saved 在页中间 → 下一页 (跳过已读部分, 不重复显示)。
// 返回页号 (≥2) 或 0 (saved 超过最后页首 → 调用方用最后一页/兜底)。
uint32_t findPageCeil(const String &indexPath, uint32_t saved) {
    File f = SD.open(indexPath.c_str(), FILE_READ);
    if (!f || f.size() < 24) {
        if (f) f.close();
        return 0;
    }
    uint32_t totalPages = (f.size() / 8) - 1;
    uint32_t lo = 2, hi = totalPages, best = 0;
    while (lo <= hi) {
        uint32_t mid = (lo + hi) / 2;
        f.seek((mid - 1) * 8);
        char rec[9];
        for (uint8_t i = 0; i < 8; i++) rec[i] = (char)f.read();
        rec[8] = '\0';
        uint32_t v = strtoul(rec, nullptr, 10);
        if (v >= saved) { best = mid; hi = mid - 1; }
        else { lo = mid + 1; }
        if ((mid & 0x3FF) == 0) { ESP.wdtFeed(); delay(0); }
    }
    f.close();
    return best;
}

void startTxtReader(const char *path, bool forceRebuild) {
    if (!isTxtPath(path)) {
        traceFmtLevel('W', "TXT unsupported path=%s", path ? path : "(null)");
        showMsg("不支持打开", "仅支持TXT文件");
        return;
    }
    freeItemList();   // 大目录 items≈34KB+ 是堆大户, 进阅读器前释放 (浏览模式回退时 listDir 重建)
    debugFmt("TXT open path=%s rebuild=%d", path, forceRebuild ? 1 : 0);
    // 旋转续读: 读走即清零 (任何路径都不残留; 失败提前 return 也安全)
    uint32_t rotateResume = gRotateResumeOffset;
    gRotateResumeOffset = 0;
    uint32_t bootHintPage = gBootHintPage;   // 启动页号提示: 复用最近阅读扫描结果, 跳过重复二分 (优化②)
    gBootHintPage = 0;
    bool bootPartialRestore = gBootPartialRefresh;   // 启动恢复局刷: 仅 setup 设置, 读走即清零 (优化④)
    gBootPartialRefresh = false;
    txtPath = path;
    txtIndexPath = txtPath;
    int txtDot = txtIndexPath.lastIndexOf('.');
    if (txtDot > 0) txtIndexPath = txtIndexPath.substring(0, txtDot);
    // 横竖屏各一份索引和章节 (对齐 A7): 竖屏用 .v1/.vz1, 横屏用 .i1/.z1
    txtIndexPath += readerIsPortrait() ? ".v1" : ".i1";
    txtChapterPath = txtPath;
    if (txtDot > 0) txtChapterPath = txtChapterPath.substring(0, txtDot);
    txtChapterPath += readerIsPortrait() ? ".vz1" : ".z1";
    if (!reinitSdBus("txt_open")) {
        debugLine("TXT SD reinit failed");
        showMsg("SD错误", "无法读取TXT");
        return;
    }
    // 兼容改名前已经生成的 `小说.txt.i1/.z1`，避免升级后所有大文件被强制重建。
    String legacyIndexPath = txtPath + (readerIsPortrait() ? ".v1" : ".i1");
    String legacyChapterPath = txtPath + (readerIsPortrait() ? ".vz1" : ".z1");
    if (!SD.exists(txtIndexPath.c_str()) && SD.exists(legacyIndexPath.c_str())) {
        txtIndexPath = legacyIndexPath;
        debugLine("TXT using legacy index name");
    }
    if (!SD.exists(txtChapterPath.c_str()) && SD.exists(legacyChapterPath.c_str())) {
        txtChapterPath = legacyChapterPath;
        debugLine("TXT using legacy chapter name");
    }
    txtFile = SD.open(txtPath.c_str());
    if (!txtFile) { debugLine("TXT open failed"); showMsg("打开失败", ""); return; }
    saveRecentReadPath(txtPath);
    recentReadPath = txtPath;
    recentReadValid = true;

    // 先检查索引有效性 (毫秒级, 仅 open+读最后8字节),
    // 有效则直接恢复进度一次全刷; 无效才显示第一页 + 后台建索引。
    // 避免"先闪第一页再全刷到进度页"的两次全刷。
    File index = SD.open(txtIndexPath.c_str());
    File chapters = SD.open(txtChapterPath.c_str());
    bool indexValid = index && index.size() >= 16 && (index.size() % 8) == 0;
    bool chaptersValid = chapters && chapters.size() > 0;
    if (indexValid) {
        index.seek(index.size() - 8);
        char record[9];
        for (uint8_t i = 0; i < 8; i++) record[i] = (char)index.read();
        record[8] = '\0';
        indexValid = strtoul(record, nullptr, 10) == txtFile.size();
    }
    // 污染检测: 旧版本(断点误写 .i1, FILE_WRITE=追加模式)会在 size 记录前追加
    // "00000000" 垃圾行, 导致页偏移表错位(翻页随机跳)。倒数第二条==0 → 强制重建。
    indexFormatCorrupt = false;
    if (indexValid && index.size() >= 24) {
        index.seek(index.size() - 16);
        char recPrev[9];
        for (uint8_t i = 0; i < 8; i++) recPrev[i] = (char)index.read();
        recPrev[8] = '\0';
        if (strtoul(recPrev, nullptr, 10) == 0) {
            debugLine("TXT index polluted, force rebuild");
            indexValid = false;
        }
    }
    // 单调性校验: 页表记录[1..N-2] 必须严格递增 (每页至少消费 1 字节)。
    // 历史 bug: 块读缓冲的 position() 是 2048 对齐的重填点, 用它记页首会把连续多条
    // 记录写成同一偏移 → 翻页读到同一段内容(画面"没反应", 仅每 ~5 页偏移前进一次)。
    // 检测到 → 强制全量重建(不续建, 续建会保留坏记录)。
    if (indexValid && index.size() >= 24) {
        index.seek(8);   // 跳过记录[0] (进度)
        uint32_t prevOff = 0;
        uint32_t checks = index.size() / 8 - 2;   // 页表记录数 (不含记录[0] 与 size 记录)
        if (checks > 16) checks = 16;
        for (uint32_t i = 0; i < checks; i++) {
            char rec[9];
            for (uint8_t k = 0; k < 8; k++) rec[k] = (char)index.read();
            rec[8] = '\0';
            uint32_t v = strtoul(rec, nullptr, 10);
            if (v <= prevOff) { indexFormatCorrupt = true; break; }
            prevOff = v;
        }
        if (indexFormatCorrupt) {
            debugLine("TXT index non-monotonic (aligned bug) -> force full rebuild");
            indexValid = false;
        }
    }
    if (index) index.close();
    if (chapters) chapters.close();

    bool rebuild = forceRebuild || !indexValid || !chaptersValid;
    debugFmt("TXT index=%d chapters=%d rebuild=%d", indexValid ? 1 : 0,
             chaptersValid ? 1 : 0, rebuild ? 1 : 0);
    if (rebuild) {
        // 超级抗打断: 仅当索引未完成(indexValid=false)且非强制重建时,
        // 若 .i1 是"完整页表但缺尾部大小字段"(上次构建被 KEY1/断电中断)
        // → 从部分页表续建, 不从头全量 (官方 A7 同款机制, 无 .i1b 断点文件)。
        uint32_t resumeOffset = 0;
        if (!forceRebuild && !indexValid && !indexFormatCorrupt) {
            File part = SD.open(txtIndexPath.c_str());
            if (part && part.size() >= 16 && (part.size() % 8) == 0) {
                // 污染检测: 倒数第二条==0 (历史版本断点误写 .i1 的垃圾行) → 不续建
                part.seek(part.size() - 16);
                char recPrev[9];
                for (uint8_t i = 0; i < 8; i++) recPrev[i] = (char)part.read();
                recPrev[8] = '\0';
                if (strtoul(recPrev, nullptr, 10) != 0) {
                    part.seek(part.size() - 8);
                    char rec[9];
                    for (uint8_t i = 0; i < 8; i++) rec[i] = (char)part.read();
                    rec[8] = '\0';
                    uint32_t v = strtoul(rec, nullptr, 10);
                    if (v > 0 && v < txtFile.size()) resumeOffset = v;
                }
            }
            if (part) part.close();
        }
        // 恢复构建中断前的阅读进度(而非总是第一页), 后台建索引 (不阻塞阅读)。
        // 仅续建(!forceRebuild)时恢复进度; 强制重建(确认"重建")从第一页开始 — 旧 .i1 记录[0]/sidecar 不读。
        txtPage = 1;
        txtPageStart = 0;
        uint32_t savedOffset = 0;
        if (rotateResume > 0) {
            // 旋转续读: 新方向索引不存在/不完整 → 直接用当前页字节偏移读页 (内容=当前阅读位置)
            savedOffset = rotateResume;
            debugFmt("TXT rotate-resume offset=%lu", (unsigned long)savedOffset);
        } else if (!forceRebuild) {
            // 构建中进度优先: sidecar (txtIndexPath+"p") 是重建/续建期间实时写入的阅读位置;
            // 完成时已合并回记录[0] 并删除, 因此存在即代表上次构建被中断。
            String sidecarPath = txtIndexPath + "p";
            if (SD.exists(sidecarPath.c_str())) {
                File sp = SD.open(sidecarPath.c_str());
                if (sp && sp.size() >= 8) {
                    char rec[9];
                    for (uint8_t i = 0; i < 8; i++) rec[i] = (char)sp.read();
                    rec[8] = '\0';
                    savedOffset = strtoul(rec, nullptr, 10);
                    debugFmt("TXT restore from sidecar offset=%lu", (unsigned long)savedOffset);
                }
                if (sp) sp.close();
            }
            if (savedOffset == 0) {
                // 无 sidecar: 索引存在但续建(如 .z1 缺失/构建中断) → 进度在旧 .i1 记录[0]
                File ready = SD.open(txtIndexPath.c_str());
                if (ready && ready.size() >= 8) {
                    char rec[9];
                    for (uint8_t i = 0; i < 8; i++) rec[i] = (char)ready.read();
                    rec[8] = '\0';
                    savedOffset = strtoul(rec, nullptr, 10);
                }
                if (ready) ready.close();
            }
            if (savedOffset > 0) {
                if (rotateResume > 0) {
                    // 旋转进度转换 (向上取整); 目标方向索引可能不完整 → ceil 找不到则兜底显示当前 offset 内容
                    uint32_t found = findPageCeil(txtIndexPath, savedOffset);
                    if (found > 0) {
                        txtPage = found;
                        txtPageStart = parsePageRecord(found);
                    }
                } else if (bootHintPage >= 2 && parsePageRecord(bootHintPage) == savedOffset) {
                    // 启动路径页号复用 (优化②): 索引完整但章节缺失触发的续建同样生效
                    txtPage = bootHintPage;
                    txtPageStart = savedOffset;
                } else {
                    // 启动/续建恢复: 精确进度 (偏移即页首)
                    uint32_t found = findPageByOffset(txtIndexPath, savedOffset);
                    if (found > 0) {
                        txtPage = found;
                        txtPageStart = savedOffset;
                    }
                }
            }
        }
        if (txtPage == 1) txtPageStart = savedOffset;   // 找不到: 显示当前 offset 内容 (页码=1, 构建完成前翻页受限)
        readTxtPage(txtPageStart);
        appMode = APP_READER;
        if (bootPartialRestore) {
            renderTxtPageNoRefresh();   // 优化④: 面板已显示同页, 仅渲染不写屏
        } else {
            renderTxtPage(true);
        }
        if (resumeOffset > 0) {
            debugFmt("TXT super-resume from page-table offset=%lu page=%lu", (unsigned long)resumeOffset, (unsigned long)txtPage);
            beginResumeIndexBuildFromPartial();
            debugLine("TXT async index resumed");
        } else {
            debugFmt("TXT first page displayed before index page=%lu", (unsigned long)txtPage);
            beginTxtIndexBuild();
            debugLine("TXT async index scheduled");
        }
    } else {
        txtChapterCount = countTxtChapters();   // 修复 "0章": 有效索引时从未加载章节数
        File ready = SD.open(txtIndexPath.c_str());
        txtTotalPages = (ready.size() / 8) - 1;
        ready.seek(0);
        char progress[9];
        for (uint8_t i = 0; i < 8; i++) progress[i] = (char)ready.read();
        progress[8] = '\0';
        uint32_t saved = strtoul(progress, nullptr, 10);
        ready.close();
        txtPage = 1;
        txtPageStart = 0;
        if (rotateResume > 0) saved = rotateResume;   // 旋转: 用当前页字节偏移转换页码
        if (saved > 0) {
            if (rotateResume > 0) {
                // 旋转进度转换 (索引完整): 向上取整一页 (saved 在页中间 → 下一页,
                // 不重复显示已读部分); 目标页页首由 parsePageRecord 取, 显示完整目标页
                uint32_t found = findPageCeil(txtIndexPath, saved);
                if (found > 0) {
                    txtPage = found;
                    txtPageStart = parsePageRecord(found);
                } else {
                    txtPage = txtTotalPages;   // saved 超最后页首 → 最后一页
                    txtPageStart = saved;
                }
            } else if (bootHintPage >= 2 && bootHintPage <= txtTotalPages &&
                       parsePageRecord(bootHintPage) == saved) {
                // 启动路径 (优化②): loadRecentReadSummary 已对同一索引算出偏移→页号,
                // 记录[p-1]==saved 校验通过即直接复用, 跳过 17 次 seek 的二分 (≈1.1s)。
                // 校验防呆: 横竖屏索引不同 / 目录名不一致 / 页表变更时自动回退二分。
                txtPage = bootHintPage;
                txtPageStart = saved;
            } else {
                // 启动/续建恢复: 精确进度 (偏移即页首)
                uint32_t found = findPageByOffset(txtIndexPath, saved);
                if (found > 0) {
                    txtPage = found;
                    txtPageStart = saved;
                }
            }
        }
        if (txtPage == 1) txtPageStart = 0;
        else if (txtPageStart == 0) txtPageStart = saved;
        if (txtPage == 1 && saved == 0) txtPageStart = 0;
        readTxtPage(txtPageStart);
        appMode = APP_READER;
        debugFmt("TXT restored page=%lu/%lu offset=%lu", (unsigned long)txtPage,
                 (unsigned long)txtTotalPages, (unsigned long)txtPageStart);
        if (bootPartialRestore) {
            renderTxtPageNoRefresh();   // 优化④: 面板已显示同页, 仅渲染不写屏 (0s)
        } else {
            renderTxtPage(true);        // 一次全刷直接到进度页
        }
    }
    saveSleepRecord();   // 界面快照: 已进入阅读器
}
// ---------- 标签系统数据层 (.bm: append-only 8字节ASCII页首偏移记录; 与索引/方向无关) ----------
// 路径与书同目录同名 (txtPath 去扩展名 + ".bm"), 不同目录同名书互不影响。
// 格式上限 ~99,999,999B 与 .i1 相同, 直接继承。损坏(size%8!=0): 读只认完整记录, 写时自愈重写。

// 标签操作内部多次 reinitSdBus, 且列表/弹窗局刷后总线在 EPD 侧: 此前的 txtFile 句柄
// 一律视为不可信 (实测: 不恢复就跳转 → 索引打不开 findPageCeil=0 → 兜底页1 → 读正文
// TXT_READ_FAIL 全零行白屏)。恢复方式 = 恢复总线 + 无条件关掉重开正文句柄。
void markEnsureTxtFile() {
    reinitSdBus("mark_txt");
    if (txtFile) txtFile.close();
    txtFile = SD.open(txtPath.c_str());
}

String markPath() {
    String p = txtPath;
    int dot = p.lastIndexOf('.');
    if (dot > 0) p = p.substring(0, dot);
    return p + ".bm";
}

uint8_t markCountRead(const String &path) {
    reinitSdBus("mark_cnt");
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return 0;
    uint32_t size = f.size();
    f.close();
    if (size % 8 != 0) traceFmtLevel('W', "BM_CORRUPT size=%lu", (unsigned long)size);
    uint32_t n = size / 8;
    return n > MARK_MAX ? MARK_MAX : (uint8_t)n;
}

// 追加一条标签 (off=标记时页首字节偏移); 满容量/SD 失败返回 false
bool markAppend(uint32_t off) {
    String path = markPath();
    if (markCountRead(path) >= MARK_MAX) return false;
    reinitSdBus("mark_append");
    // 损坏自愈: 残缺尾部先重写为完整记录再追加
    File f = SD.open(path.c_str(), FILE_READ);
    uint32_t size = f ? f.size() : 0;
    if (f) f.close();
    if (size % 8 != 0 && size > 0) {
        uint32_t keep[MARK_MAX];
        uint8_t n = 0;
        f = SD.open(path.c_str(), FILE_READ);
        if (f) {
            while (n < MARK_MAX) {
                char rec[9];
                if (f.read((uint8_t *)rec, 8) != 8) break;
                rec[8] = '\0';
                keep[n++] = strtoul(rec, nullptr, 10);
            }
            f.close();
        }
        SD.remove(path.c_str());
        File wf = SD.open(path.c_str(), FILE_WRITE);
        if (!wf) return false;
        for (uint8_t i = 0; i < n; i++) {
            char rec[9];
            formatIndexNumber(keep[i], rec);
            wf.print(rec);
        }
        wf.close();
    }
    f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) return false;
    f.seek(f.size());   // 无论 FILE_WRITE 语义如何都定位到尾部追加
    char rec[9];
    formatIndexNumber(off, rec);
    size_t w = f.print(rec);
    f.close();
    return w == 8;
}

void markLoadPage(int page) {
    markCountLoaded = 0;
    String path = markPath();
    reinitSdBus("mark_load");
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return;
    uint32_t total = f.size() / 8;
    uint32_t start = (uint32_t)(page - 1) * CHAPTER_ROWS;
    if (start < total && f.seek(start * 8)) {
        while (markCountLoaded < CHAPTER_ROWS) {
            char rec[9];
            if (f.read((uint8_t *)rec, 8) != 8) break;
            rec[8] = '\0';
            markOffsets[markCountLoaded++] = strtoul(rec, nullptr, 10);
        }
    }
    f.close();
}

// 删除第 idx 条 (0-based)。两阶段防数据丢失: 全部记录先进 RAM → 写 .bmt 回读校验 → 替换 .bm;
// 阶段1 任一步失败原 .bm 不动; 阶段2 兜底按 RAM 缓冲直写+回读校验。全失败由调用方提示"删除失败"。
bool markDeleteOne(uint8_t idx) {
    String path = markPath();
    uint32_t buf[MARK_MAX];
    uint8_t n = 0;
    {
        reinitSdBus("mark_del");
        File f = SD.open(path.c_str(), FILE_READ);
        if (!f) return false;
        while (n < MARK_MAX) {
            char rec[9];
            if (f.read((uint8_t *)rec, 8) != 8) break;   // 残缺尾部忽略
            rec[8] = '\0';
            buf[n++] = strtoul(rec, nullptr, 10);
        }
        f.close();
    }
    if (idx >= n) return false;
    for (uint8_t i = idx; i + 1 < n; i++) buf[i] = buf[i + 1];   // 内存移除目标条
    n--;

    // 阶段1: .bmt 中转 (rename 前 .bm 完好)
    reinitSdBus("mark_del");
    String tmp = path + "t";
    File tf = SD.open(tmp.c_str(), FILE_WRITE);
    if (tf) {
        bool ok = true;
        for (uint8_t i = 0; i < n && ok; i++) {
            char rec[9];
            formatIndexNumber(buf[i], rec);
            ok = tf.print(rec) == 8;
        }
        tf.close();
        File vf = SD.open(tmp.c_str(), FILE_READ);
        uint32_t vsize = vf ? vf.size() : 0;
        if (vf) vf.close();
        if (ok && vsize == (uint32_t)n * 8) {
            SD.remove(path.c_str());
            if (SD.rename(tmp.c_str(), path.c_str())) return true;
        }
        SD.remove(tmp.c_str());
    }

    // 阶段2 兜底: 总线恢复后按 RAM 缓冲直写 .bm + 回读校验
    reinitSdBus("mark_del_retry");
    SD.remove(path.c_str());
    File df = SD.open(path.c_str(), FILE_WRITE);
    if (!df) return false;
    for (uint8_t i = 0; i < n; i++) {
        char rec[9];
        formatIndexNumber(buf[i], rec);
        df.print(rec);
    }
    df.close();
    File vf2 = SD.open(path.c_str(), FILE_READ);
    uint32_t vs2 = vf2 ? vf2.size() : 0;
    if (vf2) vf2.close();
    return vs2 == (uint32_t)n * 8;
}

// 历史标记列表 (固定横屏 Fixed-Landscape UI, 仿章节目录骨架)。
// 顶栏右上 = 选中序号/总条数 (项维度); 底栏 = 当前列表页/总页数 (页维度), 两种计数不重复。
void renderMarkList(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    drawTextUTF8(2, 0, "历史标记", 100, true);
    int gsel = markCount ? (int)((markPage - 1) * CHAPTER_ROWS + markSel + 1) : 0;
    if (gsel > (int)markCount) gsel = markCount;
    char selInfo[16];
    snprintf(selInfo, sizeof(selInfo), "%d/%d", gsel, (int)markCount);
    drawTextUTF8(SCR_W - 4 - utf8Width(selInfo), 0, selInfo, 90, true);
    fillRect(0, 16, SCR_W, 1, true);
    if (markCountLoaded == 0) {
        drawTextUTF8((SCR_W - utf8Width("暂无标签")) / 2, 40, "暂无标签", SCR_W - 8, true);
        drawTextUTF8(4, 111, "菜单-标签-标记本页添加", SCR_W - 8, true);
        refresh(full);
        return;
    }
    uint32_t tsize = markTxtSize;   // 进入列表时快照 (reinit 后旧句柄 size 不可信)
    for (int i = 0; i < markCountLoaded; i++) {
        int y = 18 + i * 15;
        bool selected = i == markSel;
        fillRect(0, y, SCR_W, 15, selected);
        int fg = !selected;
        char name[16];
        snprintf(name, sizeof(name), "标记%lu", (unsigned long)((markPage - 1) * CHAPTER_ROWS + i + 1));
        uint64_t off = markOffsets[i];
        uint32_t pct10 = tsize ? (uint32_t)(off * 1000ULL / tsize) : 0;   // 截断取整, 同进度千分位口径
        if (pct10 > 1000) pct10 = 1000;
        char pctStr[12];
        snprintf(pctStr, sizeof(pctStr), "%lu.%lu%%", (unsigned long)(pct10 / 10), (unsigned long)(pct10 % 10));
        int pctW = utf8Width(pctStr);
        drawTextUTF8(2, y, name, SCR_W - 8 - pctW - 6, fg);
        drawTextUTF8(SCR_W - 4 - pctW, y, pctStr, pctW + 4, fg);
    }
    fillRect(0, 108, SCR_W, 20, false);
    fillRect(0, 108, SCR_W, 1, true);
    char pageInd[16];
    snprintf(pageInd, sizeof(pageInd), "%d/%d 页", markPage, markTotalPages);
    drawTextUTF8(4, 111, pageInd, 100, true);
    refresh(full);
}

// 标签操作框: [跳转][删除][取消] (长按行内右键弹出)
static void renderMarkActionPopup() {
    const int MW = 168, MH = 46;
    const int MX = (SCR_W - MW) / 2, MY = (SCR_H - MH) / 2;
    fillRect(MX, MY, MW, MH, false);
    drawRect(MX, MY, MW, MH, true);
    static const char *const opts[3] = {"跳转", "删除", "取消"};
    const int bw = (MW - 8 - 8) / 3;
    for (int i = 0; i < 3; i++) {
        int bx = MX + 4 + i * (bw + 4);
        bool sel = markActSel == i;
        fillRect(bx, MY + 13, bw, 20, sel);
        drawRect(bx, MY + 13, bw, 20, !sel);
        int tw = utf8Width(opts[i]);
        drawTextUTF8(bx + (bw - tw) / 2, MY + 13 + 2, opts[i], tw + 2, !sel);
    }
    refresh(false);
}

void markMoveSelection(int delta) {
    if (markCount == 0) return;
    int g = (markPage - 1) * CHAPTER_ROWS + markSel;   // 全局序号 0-based
    g = (g + delta + (int)markCount) % (int)markCount; // 行边界自动翻页
    markPage = g / CHAPTER_ROWS + 1;
    markSel = g % CHAPTER_ROWS;
    markLoadPage(markPage);
}

// 跳转到标签: offset 恒为标记时页首(txtPageStart), findPageCeil 仅复用既有定位机制;
// 构建中沿用 jumpToPage 的 clamp 语义。失效(offset≥当前文件大小)拦截提示, 不跳转。
void jumpToMark(uint32_t off) {
    markEnsureTxtFile();   // 局刷后总线在 EPD 侧 + 旧句柄可能失效: 先恢复 (否则索引打不开/读正文白屏)
    if (!txtFile || off >= txtFile.size()) {
        showMsg("标记失效", "文件已变更");
        return;
    }
    uint32_t page = findPageCeil(txtIndexPath, off);
    if (page == 0) {
        showMsg("跳转失败", "索引不可用");   // 不静默兜底到第1页 (曾致白屏误导)
        return;
    }
    if (txtIndexBuilding && page > txtIndexedPages) page = txtIndexedPages;
    txtPage = page;
    txtPageStart = parsePageRecord(txtPage);
    readTxtPage(txtPageStart);
    writeProgress(txtPageStart);
    appMode = APP_READER;
    renderTxtPage(true);
}

void enterMarksList() {
    traceLine("MARK_ENTER");
    markCount = markCountRead(markPath());   // 只读 .bm, 不依赖索引 (构建中可进)
    markTotalPages = markCount ? (int)((markCount + CHAPTER_ROWS - 1) / CHAPTER_ROWS) : 1;
    markPage = 1;
    markSel = 0;
    markCountLoaded = 0;
    markActionOpen = false;
    if (markCount > 0) markLoadPage(1);
    markEnsureTxtFile();   // 恢复正文句柄 + 快照大小 (列表百分比不再实时碰可能失效的句柄)
    markTxtSize = txtFile ? txtFile.size() : 0;
    appMode = APP_MARKS;
    renderMarkList(true);
    // 睡眠快照降级: saveSleepRecord 将 APP_MARKS 映射为 APP_READER (唤醒回正文)
}

void setup() {
    // 调试日志写 SD，避免 GPIO3/RX 与串口冲突。
    // 注意: 不在此处全局禁用看门狗!
    // ESP.wdtDisable() 只停软 WDT, 硬 WDT (8s) 仍在, 停止超过 6s 会触发硬件看门狗复位
    // (此前版本在 setup 里 ESP.wdtDisable() 后未恢复, 导致每 8s 复位一次 = "9秒全刷一遍屏幕")

    pinMode(KEY2_PIN, OUTPUT);
    digitalWrite(KEY2_PIN, HIGH);
    pinMode(KEY3_PIN, OUTPUT);
    digitalWrite(KEY3_PIN, HIGH);

    Serial.begin(DEBUG_BAUD);
    delay(20);
    Serial.printf("[u=%lu][I][heap=%lu stack=%lu] BOOT reason=%s info=%s\n",
                  (unsigned long)millis(), (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getFreeContStack(), ESP.getResetReason().c_str(),
                  ESP.getResetInfo().c_str());

    // 全局持久设置: 恢复上次阅读旋转方向 (横/竖), 打开小说/恢复阅读/重建索引都按此方向
    readerRot = storedToRot(settingsGetPortrait());
    debugFmt("ROT restore=%u", (unsigned)readerRot);

    epd.init();
    initTextRenderer();
    // 注册上传状态回调: file_api_fs 上传(START/END/ABORTED)时调 renderUploadStatus 显示
    // "上传中/上传完毕/上传失败"(墨水屏), 对齐官方 A7 web 上传状态显示。
    ofsUpSetPhaseCallback(renderUploadStatus);

    // 启动阶段不先绘制首页/启动画面: 先完成 SD 初始化, 再统一按 KEY3/最近阅读分流。
    // 这样 KEY1 复位后不会短暂跳首页, 默认只全刷恢复页一次。

    // 仅 SD 挂载期间短暂停软 WDT (防挂载超时触发), 挂载后立即恢复
    ESP.wdtDisable();
    bool sdOk = SD.begin(5, SD_SCK_MHZ(20));
    ESP.wdtEnable(8000);   // 恢复软 WDT, 8s 超时; loop 里 delay(30) 会自动喂狗
    if (sdOk) {
        sdAvailable = true;
        traceOpen();
        traceFmt("BOOT reason=%s info=%s", ESP.getResetReason().c_str(), ESP.getResetInfo().c_str());
        traceFmt("SD_READY cs=5 speed=20MHz");
    }
    debugFmt("SD begin=%d", sdOk ? 1 : 0);

    if (!sdOk) {
        showMsg("SD挂载失败", "请检查SD卡");
        while (1) delay(1000);
    }

    if (!listDir("/")) {
        appMode = APP_HOME;
        sdAvailable = true;
        renderHome(true);
        epd.display(fb);
        return;
    }

    // 开机早期探测外挂 BL8025T (用户重要决策: 时间只读外挂 RTC)。
    // 必须在此探测: 否则 rtc8025Present=false, 菜单/时钟读 BL8025T 直接失败 → 回退内存旧值。
    clockManagerProbeRtc();

    // RTC 探测用 Wire.begin(4,5) 扫过 GPIO5(=SD CS) 且只 SPI.begin() 不恢复 GPIO5,
    // 探测后立即访问 SD (loadRecentReadSummary) 会失败 → 首页误显示"暂无阅读记录"
    // (日志: RECENT_ABORT sd=1 file=0, 但稍后同一路径又能读到)。这里重挂载一次 SD。
    reinitSdBus("recent_load");

    // ---------- BOOT_AP_MODE: 重启默认进原固件配网界面引导 (AP 热点管理页) ----------
    // 条件编译开启时, 跳过 KEY3 窗口/最近阅读分流, 直接启动 AP 配网 (192.168.4.1)。
    // 进 AP 前必须: 腾堆 (关阅读句柄) + 扫 SD 目录树到 LittleFS 缓存 (避开 SD×AP 崩溃)。
#if BOOT_AP_MODE
    {
        debugLine("BOOT route=ap-setup (BOOT_AP_MODE)");
        // ★ 配网启动分阶段提示(对齐官方 A7 行为): 腾堆→"正在初始化", 构建缓存→"正在加载储存卡",
        //   热点彻底就绪(wifiManagerBegin 末尾 renderPage) 才显示热点信息。启动早期无 restorable page, 可全刷。
        renderBootStage("正在初始化", "请稍候");
        progressSyncFreeReaderHeap();
        freeItemList();
        renderBootStage("正在加载储存卡", "构建索引缓存");
        fsCacheBuild();
        wifiManagerBegin(renderNetworkPage, exitNetworkPage);
        appMode = APP_NETWORK;
        saveSleepRecord();
        // 配网开始后进入 loop (wifiManagerLoop 处理 AP), 不返回启动分流
        return;
    }
#endif

    // ---------- 统一启动分流 ----------
    // KEY1 是硬件复位键, 无论是普通启动还是休眠唤醒都从这里进入:
    // 屏幕物理保留复位前画面(e-ink), 先局刷叠加"选择"提示(不全刷), 随后 1 秒 KEY3 检测窗口;
    // 按 KEY3 显示"按键三"并全刷回首页; 未按才全刷恢复最终界面(睡眠记录/最近阅读)。
    // ---------- 静默 KEY3 检测窗口 (≥1 秒, 放在界面重绘之前) ----------
    // 界面重绘(开书恢复索引 + 全刷 ~3.5s)期间按键扫描被阻塞, 若窗口放在重绘之后,
    // 用户"复位后立即按 KEY3"会在窗口开始前就错过 → 回不了主页。
    // 提前到重绘前检测: 按住/按下即捕获, 重绘完成后按 key3Held 决定是否回首页。
    // 优化③: 窗口起点提前到 loadRecentReadSummary 之前, 页表扫描期间每块轮询 KEY3
    // (扫描 0~3.2s 天然覆盖窗口), 扫描后仅补足剩余时间到最短 1s → 启动提速 ~1s。
    gBootKey3Held = false;
    gBootKey3PollLow = 0;
    gBootKey3Window = true;
    uint32_t keyWindow = millis();
    loadRecentReadSummary();
    lastPhysicalKeyMs = millis();
    // 调试: 启动时检查休眠记录文件状态 (断电后是否保留 / 是否被消费)
    {
        File sf = SD.open(SLEEP_RECORD_PATH, FILE_READ);
        uint32_t ssize = sf ? sf.size() : 0;
        uint32_t smagic = 0;
        if (sf && ssize >= 4) {
            uint8_t m[4];
            sf.read(m, 4);
            smagic = ((uint32_t)m[0]) | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
        }
        if (sf) sf.close();
        debugFmt("BOOT_SLEEPFILE exist=%d size=%u magic=%08lX", SD.exists(SLEEP_RECORD_PATH) ? 1 : 0,
                 (unsigned)ssize, (unsigned long)smagic);
    }
    // 只读取睡眠记录
    SleepRecord bootRec;
    bool haveRec = readSleepRecord(bootRec);

    // 窗口补足: 扫描未覆盖满 1s 时继续轮询, 保证最短检测期
    while (millis() - keyWindow < 1000) {
        if (!gBootKey3Held) {
            if (readKey3() == 0) {
                if (gBootKey3PollLow >= 1) gBootKey3Held = true;   // 连续两次低 (≈10ms) 才算按下
                else gBootKey3PollLow++;
            } else {
                gBootKey3PollLow = 0;
            }
        }
        delay(10);
        ESP.wdtFeed();
    }
    gBootKey3Window = false;
    bool key3Held = gBootKey3Held;
    traceFmt("WAKE key3Held=%d", key3Held ? 1 : 0);

    // ---------- 优化④: 启动恢复刷新策略 (局刷/免刷替代全刷) ----------
    // 面板 (e-ink 双稳态) 在任意复位/唤醒/断电后都物理保留复位前画面。不能按复位原因
    // 判定 —— 本机 KEY1 复位实测上报 "Power On" (REASON_DEFAULT_RST, 硬件差异), 原因门
    // 会让 ④ 永远不生效 (用户实测: 重启必定全刷)。改为: 有休眠记录且恢复模式=首页/
    // 阅读/文件浏览 (恢复内容与面板一致) 才允许局刷/免刷。强制全刷的例外:
    //   · 其他模式兜底回首页 (天气/时钟/设置页 → 首页内容不同);
    //   · 章节目录恢复 — 先渲染阅读页再渲染目录 (双渲染), 中间态无意义;
    //   · 低电休眠唤醒 — 面板是整屏"电量过低"通知, 局刷擦除留整屏残影。
    gBootPartialRefresh =
        haveRec && (bootRec.mode == APP_HOME || bootRec.mode == APP_READER ||
                    bootRec.mode == APP_BROWSER);
    if (gBootPartialRefresh && readBatteryMV() <= BAT_LOW_MV) gBootPartialRefresh = false;
    debugFmt("BOOT partial-restore=%d mode=%d mv=%d", gBootPartialRefresh ? 1 : 0,
             (int)bootRec.mode, lastBatteryMV);

    // ---------- 先重绘恢复界面 (内容页, 必要重绘; 不画白/黑画面) ----------
    // 阅读器重绘当前进度页, 其他界面重绘当前页面。
    // KEY3 已按 (优化④b): 直接一次全刷回首页, 跳过恢复渲染 —
    // 避免"先重刷恢复界面再重刷首页"的冗余双击刷新 (实测 2×全刷)。
    if (key3Held) {
        freeItemList();   // setup 探测 listDir("/") 分配的 items 在首页无用, 释放腾堆
        appMode = APP_HOME;
        renderHome(true);          // 内容与面板不同 (面板=复位前界面) → 必须全刷
        saveSleepRecord();
        debugLine("WAKE route=key3-home");
    } else if (haveRec) {
        if (bootRec.mode == APP_HOME) {
            // 休眠前在首页 → 唤醒后仍回首页 (不误入阅读页)
            freeItemList();   // setup 探测 listDir("/") 分配的 items 在首页无用, 释放腾堆
            appMode = APP_HOME;
            renderHome(!gBootPartialRefresh);   // 优化④: 面板与首页一致时局刷恢复
            debugLine("WAKE route=sleep-home");
        } else if (bootRec.mode == APP_CLOCK_DISGUISE) {
            // 老板快捷键伪装模式唤醒: 未按 KEY3 → 继续伪装时钟页 (局刷, 面板已是时钟页)。
            // 按了 KEY3 会走上面的 key3Held 分支全刷回主页 (退出伪装 = KEY1 复位 + 1 秒内按 KEY3)。
            appMode = APP_CLOCK_DISGUISE;
            fbRot = 90;
            yiyanText[0] = '\0';
            lastClockDisplayedMinute = clockManagerNow() / 60;
            renderClockPage(false);
            debugLine("WAKE route=sleep-disguise");
        } else if (bootRec.mode == APP_READER && recentReadValid) {
            // 休眠前在阅读页 → 继续恢复最近阅读 (重绘当前进度页)
            gBootHintPage = recentReadPage;   // 优化②: 复用最近阅读页号, 跳过重复二分
            startTxtReader(recentReadPath.c_str(), false);
            debugLine("WAKE route=sleep-reader");
        } else if (bootRec.mode == APP_BROWSER) {
            // 复位前在文件管理器 → 恢复目录和光标位置
            String savedPath = bootRec.path;
            if (savedPath.length() == 0 || savedPath[0] != '/') savedPath = "/";
            currentPath = savedPath;
            selIndex = bootRec.selIndex;
            topIndex = bootRec.topIndex;
            if (selIndex < 0) selIndex = 0;
            if (topIndex < 0) topIndex = 0;
            appMode = APP_BROWSER;
            if (listDir(currentPath.c_str())) {
                if (selIndex >= itemCount) selIndex = itemCount > 0 ? itemCount - 1 : 0;
                if (topIndex > selIndex) topIndex = selIndex;
                // 恢复的 topIndex 可能不在首窗口 → 重新加载恢复位置窗口（窗口化）
                if (topIndex != 0) loadListWindow(currentPath.c_str(), topIndex);
            } else {
                currentPath = "/";
                selIndex = 0;
                topIndex = 0;
                listDir(currentPath.c_str());
            }
            renderAll();
            refresh(!gBootPartialRefresh);   // 优化④: 面板与目录一致时局刷恢复
            debugFmt("WAKE route=sleep-browser path=%s sel=%d/%d top=%d",
                     currentPath.c_str(), selIndex, itemCount, topIndex);
        } else if (bootRec.mode == APP_CHAPTERS && recentReadValid) {
            // 休眠前在章节目录 → 先开书, 再恢复目录位置
            gBootHintPage = recentReadPage;   // 优化②: 复用最近阅读页号, 跳过重复二分
            startTxtReader(recentReadPath.c_str(), false);
            if (txtFile && SD.exists(txtChapterPath.c_str())) {
                txtChapterCount = countTxtChapters();
                chapterSpeed = bootRec.chapterSpeed ? bootRec.chapterSpeed : 1;
                chapterSpeedPopup = bootRec.chapterSpeedPopup != 0;
                chapterSpeedSel = bootRec.chapterSpeedSel > 6 ? 0 : bootRec.chapterSpeedSel;
                // 建页偏移表 (恢复后向前/向后翻页全 O(1); 修复恢复章节后向前翻每步全扫 2.2s 卡死)
                chapterBuildPageTable();
                chapterTotalPages = chapterPageTableCount > 0 ? (int)chapterPageTableCount : 0;
                chapterPage = bootRec.chapterPage;
                if (chapterPage < 1) chapterPage = 1;
                if (chapterPage > chapterTotalPages) chapterPage = chapterTotalPages;
                chapterSel = bootRec.chapterSel;
                if (chapterSel < 0 || chapterSel > 8) chapterSel = 0;
                if (txtChapterCount > 0) {
                    appMode = APP_CHAPTERS;
                    // 恢复页定位: 用页偏移表 (O(1)), 表未覆盖才回退 seek
                    uint32_t restoreOff = 0;
                    if (chapterPage >= 1 && chapterPage <= chapterPageTableCount)
                        restoreOff = chapterPageOffsets[chapterPage - 1];
                    else
                        restoreOff = seekChapterOffset((uint32_t)(chapterPage - 1) * CHAPTER_ROWS);
                    loadChapterRows(restoreOff);
                    renderChapterList(true);
                    if (chapterSpeedPopup) renderChapterSpeedPopup();
                    debugFmt("WAKE route=sleep-chapters page=%d/%d sel=%d loaded=%d speed=%u popup=%d",
                             chapterPage, chapterTotalPages, chapterSel, chapterCountLoaded,
                             chapterSpeed, chapterSpeedPopup ? 1 : 0);
                } else {
                    // 章节文件为空 → 留在阅读页
                    debugLine("WAKE route=sleep-chapters-empty");
                }
            } else {
                // 章节目录文件缺失 → 留在 startTxtReader 已渲染的阅读页
                debugLine("WAKE route=sleep-chapters-fallback-reader");
            }
        } else if (recentReadValid) {
            // 其他模式(非HOME/READER/BROWSER/CHAPTERS) → 回首页，避免因旧记录误入阅读页
            freeItemList();   // setup 探测 listDir("/") 分配的 items 在首页无用
            appMode = APP_HOME;
            renderHome(true);
            debugLine("WAKE route=sleep-other-home");
        } else {
            freeItemList();   // 同上
            appMode = APP_HOME;
            renderHome(true);
            debugLine("WAKE route=sleep-other-home");
        }
    } else if (recentReadValid) {
        // 无休眠记录(正常开机): 默认回首页，避免正常开机也自动进阅读页
        freeItemList();   // setup 探测 listDir("/") 分配的 items 在首页无用
        appMode = APP_HOME;
        renderHome(true);
        debugLine("WAKE route=recent-read-home");
    } else {
        freeItemList();   // 同上
        appMode = APP_HOME;
        renderHome(true);
        saveSleepRecord();
        debugLine("WAKE route=home");
    }

    // 未按: 保持上面重绘的恢复界面, 无提示框需清除, 不做额外画面操作
    gBootHintPage = 0;            // 未消费(未走阅读恢复)则清掉, 防后续 openRecentRead 误用陈旧页号
    gBootPartialRefresh = false;  // 同上: 浏览/首页分支未消费则清掉, 防后续 startTxtReader 误用
}

void loop() {
    uint32_t loopStarted = millis();
    if (diagLastLoopMs) {
        uint32_t gap = loopStarted - diagLastLoopMs;
        if (gap > diagMaxLoopGap) diagMaxLoopGap = gap;
    }
    diagFlushSd(false);
    int raw2 = readKey2();
    int raw3 = readKey3();
    int r2 = scanKey(k2, raw2 == 0);
    int r3 = scanKey(k3, raw3 == 0);
    notePhysicalKeyActivity(r2, r3);
    static uint32_t lastKeyLogMs = 0;
    static int lastRaw2 = -1, lastRaw3 = -1;
    if (raw2 != lastRaw2 || raw3 != lastRaw3 || r2 || r3 || millis() - lastKeyLogMs >= 5000) {
        traceFmt("KEY raw2=%d raw3=%d event2=%d event3=%d down2=%d down3=%d mode=%d home=%d page=%lu",
                 raw2, raw3, r2, r3, k2.down ? 1 : 0, k3.down ? 1 : 0,
                 appMode, homeSel, (unsigned long)txtPage);
        lastRaw2 = raw2;
        lastRaw3 = raw3;
        lastKeyLogMs = millis();
    }
    static uint32_t lastSerialKeyMs = 0;
    // 仅按键事件才打印, 去掉无条件 250ms 轮询: 持续串口输出会加剧电源噪声对
    // GPIO3=RX 的耦合(误判按键 → 构建期间休眠计时被假事件刷新 → 不自动休眠)。
    if ((r2 || r3) && millis() - lastSerialKeyMs >= 50) {
        Serial.printf("KEY raw2=%d raw3=%d event2=%d event3=%d down2=%d down3=%d mode=%d home=%d page=%lu\n",
                      raw2, raw3, r2, r3, k2.down ? 1 : 0, k3.down ? 1 : 0,
                      appMode, homeSel, (unsigned long)txtPage);
        Serial.flush();
        lastSerialKeyMs = millis();
    }
    diagLastLoopMs = loopStarted;

    // 时钟：每小时将当前已知时间写入 EEPROM，使掉电后能恢复到接近关机时刻的时间
    static uint32_t lastClockPersistMs = 0;
    if (millis() - lastClockPersistMs >= 3600000UL && clockManagerIsSynced()) {
        clockManagerPersistNow();
        lastClockPersistMs = millis();
    }

    // 低压检测：60s 周期采样；≤3300mV 二次确认后进入低电休眠（优先级高于自动休眠）
    static uint32_t lastBatteryCheckMs = 0;
    if (millis() - lastBatteryCheckMs >= BAT_CHECK_MS) {
        lastBatteryCheckMs = millis();
        if (checkLowBattery()) return;
    }

    if (millis() - lastPhysicalKeyMs >= AUTO_SLEEP_MS) {
        // 伪装模式 (老板快捷键) + AP 配网模式不自动休眠:
        //  - 伪装模式: 休眠会画"休眠"提示暴露非时钟功能, 且闹钟应持续显示
        //  - AP 配网(APP_NETWORK): 用户用手机管理页上传/浏览, 不按设备按键, 5 分钟无按键会
        //    误判 idle → 自动休眠重启 → 手机断连 → 上传 POST 到不了设备(实测根因 SLEEP_AUTO mode=4)
        if (appMode != APP_CLOCK_DISGUISE && appMode != APP_NETWORK) {
            traceFmt("SLEEP_AUTO idle=%lu building=%d mode=%d", (unsigned long)(millis() - lastPhysicalKeyMs),
                     txtIndexBuilding ? 1 : 0, appMode);
            enterSleepMode();
            return;
        }
    }

    if (r2 == 1) debugLine("KEY middle short");
    else if (r2 == 2) debugLine("KEY middle long");
    if (r3 == 1) debugLine("KEY right short");
    else if (r3 == 2) debugLine("KEY right long");

    if (appMode == APP_HOME) {
        if (r3 == 1) {
            homeSel = (homeSel + 1) % 6;
            renderHome(false);
        } else if (r2 == 1) {
            homeSel = (homeSel + 5) % 6;
            renderHome(false);
        } else if (r3 == 2) {
            enterHomeCard();
        }
        delay(30);
        return;
    }

    if (appMode == APP_NETWORK) {
        wifiManagerLoop();
        wifiManagerHandleKeys(r2, r3);
        delay(30);
        return;
    }

    if (appMode == APP_CLOCK_CONNECT) {
        clockManagerLoop();
        clockManagerHandleKeys(r2, r3);
        delay(30);
        return;
    }

    if (appMode == APP_CLOCK) {
        if (r2 == 2) {
            clockManagerPersistNow();
            appMode = APP_HOME;
            renderHome(true);
        } else {
            time_t now = clockManagerNow();
            time_t displayedMinute = now > 1600000000UL ? now / 60 : 0;
            if (displayedMinute != 0 && displayedMinute != lastClockDisplayedMinute) {
                renderClockPage(false);
                lastClockDisplayedMinute = displayedMinute;
            }
        }
        delay(30);
        return;
    }

    if (appMode == APP_CLOCK_DISGUISE) {
        // 老板快捷键伪装模式: 停用全部按键, 保持时钟页 (伪装成小闹钟而非阅读器)。
        // 退出 = KEY1 硬件复位 → 开机 1 秒 KEY3 窗口按 KEY3 → 回主页 (setup 分流);
        // 复位后未按 KEY3 → 按睡眠记录恢复伪装页 (continue 伪装)。
        // 仍每分钟刷一次时间 (时钟页本身会走时, 与真时钟一致, 不暴露阅读器)。
        time_t now = clockManagerNow();
        time_t displayedMinute = now > 1600000000UL ? now / 60 : 0;
        if (displayedMinute != 0 && displayedMinute != lastClockDisplayedMinute) {
            renderClockPage(false);
            lastClockDisplayedMinute = displayedMinute;
        }
        delay(30);
        return;
    }

    if (appMode == APP_WEATHER) {
        // 中键短按/长按: 退出回主页; 右键长按: 手动刷新（夜间也联网）
        if (r2 == 1 || r2 == 2) {
            appMode = APP_HOME;
            renderHome(true);
        } else if (r3 == 2) {
            fetchWeatherFlow(true);
        }
        // r3 == 1 忽略
        delay(30);
        return;
    }

    if (appMode == APP_SETTINGS) {
        settingsHandleKeys(r2, r3);
        return;
    }

    if (appMode == APP_BMP) {
        // 图片浏览：任意按键返回文件管理器
        if (r2 != 0 || r3 != 0) {
            appMode = APP_BROWSER;
            renderAll();
            refresh(true);
        }
        delay(30);
        return;
    }

    if (appMode == APP_READER) {
        if (readerJumpOpen) {
            // 页码跳转弹窗 (数字键盘): 右键下移 1 / 中键上移 2 (与阅读菜单一致的设计),
            // 右长 执行, 中长 取消
            if (r3 == 1) {
                jumpCursor = (jumpCursor + 1) % 13;
                renderJumpOverlay();
            } else if (r2 == 1) {
                jumpCursor = (jumpCursor + 11) % 13;   // +11 ≡ -2 (mod 13): 上移 2 个
                renderJumpOverlay();
            } else if (r3 == 2) {
                if (jumpCursor < 10) {   // 数字 1..9 0: 末尾追加 (超总页数位数拒绝)
                    uint8_t digit = (jumpCursor == 9) ? 0 : (jumpCursor + 1);
                    char tbuf[16];
                    snprintf(tbuf, sizeof(tbuf), "%lu", (unsigned long)txtTotalPages);
                    int maxDigits = strlen(tbuf);
                    snprintf(tbuf, sizeof(tbuf), "%lu", (unsigned long)jumpPage);
                    if ((int)strlen(tbuf) < maxDigits) {
                        jumpPage = (jumpPage == 0) ? digit : jumpPage * 10 + digit;
                    } else {
                        jumpRejectMs = millis();   // 超位数: 页码行尾显示 "!" 1s
                    }
                    renderJumpOverlay();
                } else if (jumpCursor == 10) {   // < 退格
                    jumpPage /= 10;
                    renderJumpOverlay();
                } else if (jumpCursor == 11) {   // 回车: 确认跳转 (写进度 + 全刷)
                    jumpToPage();
                } else {   // 12 取消: 回阅读菜单
                    readerJumpOpen = false;
                    openReaderMenu();
                }
            } else if (r2 == 2) {
                readerJumpOpen = false;
                openReaderMenu();
            }
        } else if (readerSyncOpen) {
            // 进度同步状态机 (直连手机 HTTP, 非阻塞); 结束后经 progressSyncDone 恢复阅读器
            progressSyncLoop();
            progressSyncHandleKeys(r2, r3);
        } else if (readerRotSelOpen) {
            // 旋转方向选择弹窗: 中短上移 / 右短下移 (mod 4, 同 chapterSpeedPopup),
            // 右长 应用所选方案, 中长 取消回菜单
            if (r3 == 1) {
                rotSelCursor = (rotSelCursor + 1) % 4;
                renderRotSelOverlay();
            } else if (r2 == 1) {
                rotSelCursor = (rotSelCursor + 3) % 4;
                renderRotSelOverlay();
            } else if (r3 == 2) {
                uint16_t target = rotSelTable[rotSelCursor];
                if (target == readerRot) {
                    readerRotSelOpen = false;   // 选了当前方向: 仅关弹窗回正文 (不刷新屏幕结构)
                    renderTxtPage(false);
                } else {
                    applyReaderRotation(target);
                }
            } else if (r2 == 2) {
                readerRotSelOpen = false;
                openReaderMenu();   // 取消回阅读菜单 (同跳转键盘取消路径)
            }
        } else if (readerMarkMenuOpen) {
            // 标签子菜单 ([标记本页][历史标记]): 中短/右短 移光标(mod 2), 右长执行, 中长取消回菜单
            if (r3 == 1) {
                markMenuSel = (markMenuSel + 1) % 2;
                renderMarkMenuOverlay();
            } else if (r2 == 1) {
                markMenuSel = (markMenuSel + 1) % 2;
                renderMarkMenuOverlay();
            } else if (r3 == 2) {
                readerMarkMenuOpen = false;
                if (markMenuSel == 0) {
                    // 标记本页: 追加当前页首字节偏移 (重复标记同一页合法)
                    bool ok = markAppend(txtPageStart);
                    markEnsureTxtFile();   // markAppend 内部 reinit 过总线, 恢复阅读句柄再回正文
                    if (ok) showMsg("已标记", "");
                    else if (markCountRead(markPath()) >= MARK_MAX) showMsg("标签已满", "上限50个");
                    else showMsg("保存失败", "");
                } else {
                    enterMarksList();
                }
            } else if (r2 == 2) {
                readerMarkMenuOpen = false;
                openReaderMenu();   // 取消回阅读菜单
            }
        } else if (readerMenuOpen) {
            // 菜单光标: 右键下移 1 / 中键上移 2 (用户设计: 中键跨 2 个选项, 右键跨 1 个)
            if (r3 == 1) {
                readerMenuSel = (readerMenuSel + 1) % 11;
                readerMenuNote[0] = '\0';
                renderReaderMenu();
            } else if (r2 == 1) {
                readerMenuSel = (readerMenuSel + 9) % 11;   // +9 ≡ -2 (mod 11): 上移 2 个
                readerMenuNote[0] = '\0';
                renderReaderMenu();
            } else if (r2 == 2) {
                closeReaderMenu();
            } else if (r3 == 2) {
                execReaderMenu();
            }
        } else {
            if (r3 == 1) nextTxtPage();
            else if (r2 == 1) previousTxtPage();
            else if (r3 == 2) openReaderMenu();
            else if (r2 == 2) enterClockDisguise();   // 老板快捷键: 中长按 → 局刷伪装时钟 (原为 leaveReaderToBrowser)
            // 自动翻页 (对齐 A7): 定时翻页, 构建中/菜单打开时不自动翻
            else if (autoFlipSpeed && autoFlipIntervalMs()) {
                if ((int32_t)(millis() - autoFlipNextMs) >= 0) {
                    autoFlipNextMs = millis() + autoFlipIntervalMs();
                    nextTxtPage();
                }
            }
        }
        indexTaskStep();
        delay(txtIndexBuilding ? 5 : 30);
        return;
    }
    if (appMode == APP_CHAPTERS) {
        if (chapterSpeedPopup) {
            if (r3 == 1) {
                chapterSpeedSel = (chapterSpeedSel + 1) % 7;
                renderChapterSpeedPopup();
            } else if (r2 == 1) {
                chapterSpeedSel = (chapterSpeedSel + 6) % 7;
                renderChapterSpeedPopup();
            } else if (r2 == 2) {
                chapterSpeedPopup = false;
                renderChapterList(false);
            } else if (r3 == 2) {
                static const uint8_t speeds[7] = {1, 2, 5, 10, 25, 50, 100};
                chapterSpeed = speeds[chapterSpeedSel];
                chapterSpeedPopup = false;
                renderChapterList(false);
            }
        } else {
            if (r3 == 1) {
                chapterMoveSelection(+1);
                renderChapterList(false);
            } else if (r2 == 1) {
                chapterMoveSelection(-1);
                renderChapterList(false);
            } else if (r2 == 2) {
                closeTxtReader();
            } else if (r3 == 2) {
                if (chapterSel < CHAPTER_ROWS && chapterSel < chapterCountLoaded) {
                    // 跳转到章节
                    txtPage = chapterRows[chapterSel].page;
                    txtPageStart = parsePageRecord(txtPage);
                    readTxtPage(txtPageStart);
                    writeProgress(txtPageStart);
                    appMode = APP_READER;
                    renderTxtPage(true);
                } else if (chapterSel == 6) {
                    chapterPrevPage();
                } else if (chapterSel == 7) {
                    chapterNextPage();
                } else if (chapterSel == 8) {
                    static const uint8_t speeds[7] = {1, 2, 5, 10, 25, 50, 100};
                    chapterSpeedPopup = true;
                    chapterSpeedSel = 0;
                    for (int i = 0; i < 7; i++) {
                        if (speeds[i] == chapterSpeed) { chapterSpeedSel = i; break; }
                    }
                    renderChapterSpeedPopup();
                }
            }
        }
        delay(txtIndexBuilding ? 5 : 30);
        indexTaskStep();
        return;
    }
    if (appMode == APP_MARKS) {
        if (markActionOpen) {
            // 操作框 [跳转][删除][取消]: 中短左移/右短右移(mod 3)/右长执行/中长取消
            // 独占消费: 执行后关闭操作框, 不得再触发列表的右长逻辑
            if (r3 == 1) {
                markActSel = (markActSel + 1) % 3;
                renderMarkActionPopup();
            } else if (r2 == 1) {
                markActSel = (markActSel + 2) % 3;
                renderMarkActionPopup();
            } else if (r2 == 2) {
                markActionOpen = false;
                renderMarkList(false);
            } else if (r3 == 2) {
                markActionOpen = false;
                if (markActSel == 0 && markCountLoaded > 0 && markSel < markCountLoaded) {
                    jumpToMark(markOffsets[markSel]);   // 跳转成功内部已切回阅读页
                } else if (markActSel == 1 && markCountLoaded > 0 && markSel < markCountLoaded) {
                    uint8_t gidx = (uint8_t)((markPage - 1) * CHAPTER_ROWS + markSel);
                    bool delOk = markDeleteOne(gidx);
                    markEnsureTxtFile();   // 删除内部多次 reinit, 恢复阅读句柄
                    if (delOk) {
                        markCount = markCountRead(markPath());
                        markTotalPages = markCount ? (int)((markCount + CHAPTER_ROWS - 1) / CHAPTER_ROWS) : 1;
                        int g = gidx;                   // 光标指顺延后的同一条
                        if (g >= (int)markCount) g = markCount - 1;
                        if (g < 0) g = 0;
                        markPage = g / CHAPTER_ROWS + 1;
                        markSel = g % CHAPTER_ROWS;
                        markCountLoaded = 0;
                        if (markCount > 0) markLoadPage(markPage);
                        renderMarkList(false);
                    } else {
                        showMsg("删除失败", "");
                    }
                } else {
                    renderMarkList(false);              // 取消
                }
            }
        } else {
            if (r3 == 1) {
                markMoveSelection(+1);
                renderMarkList(false);
            } else if (r2 == 1) {
                markMoveSelection(-1);
                renderMarkList(false);
            } else if (r2 == 2) {
                // 中长: 返回阅读正文 (局刷); 恢复句柄防下次翻页 TXT_READ_FAIL
                appMode = APP_READER;
                markEnsureTxtFile();
                renderTxtPage(false);
            } else if (r3 == 2) {
                if (markCountLoaded > 0 && markSel < markCountLoaded) {
                    markActSel = 0;
                    markActionOpen = true;   // 长按行 → 操作框
                    renderMarkActionPopup();
                }
            }
        }
        delay(txtIndexBuilding ? 5 : 30);
        indexTaskStep();
        return;
    }

    if (rebuildConfirmPath.length() > 0) {
        // ===== 重建确认框 (长按重建, 短按退出) =====
        if (r3 == 2) {            // 长按右键 = 确认重建
            String path = rebuildConfirmPath;
            rebuildConfirmPath = "";
            traceFmt("REBUILD_START path=%s", path.c_str());
            startTxtReader(path.c_str(), true);
        } else if (r2 == 2 || r3 == 1 || r2 == 1) {   // 中长/任意短按 = 退出
            rebuildConfirmPath = "";
            renderAll();
            refresh(false);
        }
    } else if (mode == 1) {
        // ===== 功能框模式 =====
        if (r2 == 1) {
            menuSel = (menuSel + MENU_CNT - 1) % MENU_CNT;
            renderMenuBar();
            refresh(false);
        } else if (r2 == 2) {
            hideMenu();
        } else if (r3 == 1) {
            menuSel = (menuSel + 1) % MENU_CNT;
            renderMenuBar();
            refresh(false);
        } else if (r3 == 2) {
            execMenu();
        }
    } else {
        // ===== 浏览模式 =====
        if (r2 == 1) {
            if (selIndex > 0) {
                int oldIdx = selIndex;
                // 官方 A7 分页滚动 (§11.11): 光标在页首行(第 1 行)再按 → 整页回退 LIST_ROWS 行
                if (selIndex == topIndex) {
                    int target = selIndex - LIST_ROWS;
                    if (target < 0) target = 0;
                    selIndex = target;
                    topIndex = selIndex;
                    loadListWindow(currentPath.c_str(), topIndex);
                    renderTitle();
                    renderList();
                } else {
                    selIndex--;
                    renderRowChange(oldIdx, selIndex);
                }
                refresh(false);
            }
        } else if (r2 == 2) {
            if (currentPath != "/") {
                upDir();
                renderAll();
                refresh(true);
            } else {
                freeItemList();   // 浏览模式回首页: 窗口复位
                appMode = APP_HOME;
                loadRecentReadSummary();
                renderHome(true);
            }
        } else if (r3 == 1) {
            if (selIndex < itemCount - 1) {
                int oldIdx = selIndex;
                // 官方 A7 分页滚动 (§11.11): 光标到页末行(第 6 行)再按 → 整页跳 LIST_ROWS 行,
                // 窗口显示下一页(光标在新页末行, 官方"跳到真实第10行, 屏幕显示6-10行")
                if (selIndex >= topIndex + LIST_ROWS - 1) {
                    int target = selIndex + LIST_ROWS;
                    if (target > itemCount - 1) target = itemCount - 1;   // 末页不足一屏
                    selIndex = target;
                    topIndex = selIndex - LIST_ROWS + 1;
                    if (topIndex < 0) topIndex = 0;
                    loadListWindow(currentPath.c_str(), topIndex);
                    renderTitle();
                    renderList();
                } else {
                    selIndex++;
                    renderRowChange(oldIdx, selIndex);
                }
                refresh(false);
            }
        } else if (r3 == 2) {
            if (itemCount > 0 && selIndex < itemCount) {
                FileItem *it = itemAt(selIndex);
                if (it && it->isDir) {
                    enterDir(selIndex);
                    renderAll();
                    refresh(true);
                } else showMenu();
            }
        }
    }
    indexTaskStep();
    delay(txtIndexBuilding ? 5 : 30);
}

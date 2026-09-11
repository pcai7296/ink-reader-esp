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
#include <LittleFS.h>
#include "wifi_manager.h"
#include "fs_cache.h"   // SD 目录树 → LittleFS 缓存（进 AP 前扫描; /fs/list 读缓存不碰 SD）
#include "weather_data.h"
#include "weather_icons.h"
#include "weather_small_icons.h"
#include "nav_icons.h"   // 首页导航 13x13 图标: 文件/时钟/天气/配网/设置/返回
#include "hitokoto.h"
#include "bili_fans.h"

// ===== P7 A/B 编译开关 (2026-09) =====
// 0 = 阅读页局刷后保持面板上电 (现状; 连续局刷最稳)
// 1 = 阅读页局刷后 powerOff (官方 DisplayTxt 每页断电行为; 省电但每次翻页多一次 powerOn)
#ifndef READER_EPD_PAGEOFF
#define READER_EPD_PAGEOFF 0
#endif

// ===== P3/P4 自动化验收钩子 (仅测试固件; 默认 0=完全不参与编译) =====
// 打开 LittleFS 上的测试书并连续翻页 N 次, 打印 AUTOTEST_* 统计后停在阅读页。
#ifndef READER_AUTOTEST
#define READER_AUTOTEST 0
#endif
#ifndef READER_AUTOTEST_PAGES
#define READER_AUTOTEST_PAGES 300
#endif
#ifndef READER_AUTOTEST_BOOK
#define READER_AUTOTEST_BOOK "/T1_300k.txt"
#endif
// 验收专用: 强制本地介质(不改 EEPROM 的 sdEnabled, 免去 SD 目录扫描, 保证阅读/管理器都走 LittleFS)
#ifndef FORCE_LOCAL_MEDIUM_TEST
#define FORCE_LOCAL_MEDIUM_TEST 0
#endif
#include "bmp_show.h"
#include "progress_sync.h"
#include "file_api_fs.h"   // 上传状态接口 ofsUpSetPhaseCallback（"上传中/上传完毕"墨水屏状态）
#include <ESP8266WiFi.h>
#include "font8x8.h"
#include <user_interface.h>
#include "reader_utils.h"   // 通用 (深睡 ESP.deepSleep 由 core 提供)
#include "fb_gfx.h"         // FramebufferGfx 类型 + epd/gfx/u8g2Fonts/textRendererReady extern + 字体 extern
#include "stats.h"          // 阅读行为统计 V1 (翻页/会话/连续/排行)


EPD_290A epd;
#define DIAG_SERIAL 1
#define DIAG_SD 0
// ★ 条件编译: BOOT_AP_MODE=1 时, 重启后默认进入原固件 A7 的配网界面引导 (AP 热点管理页 192.168.4.1)。
// 用于"出厂引导/只进配网"场景; =0 时恢复正常的启动分流 (KEY3窗口→最近阅读/首页/睡眠恢复)。
// 编译可覆盖: 命令行加 -DBOOT_AP_MODE=1 优先 (用 #ifndef 允许外部 -D 覆盖)。
#ifndef BOOT_AP_MODE
#define BOOT_AP_MODE 0
#endif
// ★ 条件编译: SERIAL_REMOTE=1 时进入"远程控制专用版"——
//   GPIO3(RX/KEY3) 全程让给串口接收按键命令, 物理 KEY3 失效(摸不到设备无妨);
//   物理 KEY2(GPIO0) 照常工作; 串口行命令注入按键事件 (复用现有 appMode 分发, 效果=真按)。
// =0 时恢复标准硬件按键, 不影响正常使用。编译可覆盖: -DSERIAL_REMOTE=0。
#ifndef SERIAL_REMOTE
#define SERIAL_REMOTE 0
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
        File old = SD.open("/debug_trace.log", "r");
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

// 纯文案 flash 重载: PSTR/F 字面量 → 短 RAM 副本 → RAM 版 drawTextUTF8。
// ⚠️ ESP8266 flash 区禁逐字节数据读, 搬运交给 snprintf_P (内部 4B 对齐安全读);
// 仅适用无 '%' 的纯文案 (带格式走 snprintf_P + RAM 实参)。副本 ≤64B 栈, 与
// RAM 版自身 clipped[192] 相比增量很小, 不改变 4KB loop 栈节奏。
int drawTextUTF8(int x, int y, const __FlashStringHelper *s, int maxW, bool black) {
    if (!textRendererReady) return x;
    char tmp[64];
    snprintf_P(tmp, sizeof(tmp), reinterpret_cast<PGM_P>(s));
    return drawTextUTF8(x, y, tmp, maxW, black);
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

// 22x22 首页导航图标：navIcons[6][66] PROGMEM（本机实际编码: 位=1 黑 / 位=0 白, MSB left）
// 顺序: 0=统计 1=文件 2=时钟 3=天气 4=配网 5=设置
void drawNavIcon(int x, int y, int idx, bool black) {
    if (idx < 0 || idx > 5) return;
    for (int r = 0; r < 22; r++) {
        for (int c = 0; c < 22; c++) {
            uint8_t byte = pgm_read_byte(&navIcons[idx][r * 3 + c / 8]);
            if (byte & (0x80 >> (c % 8))) setPix(x + c, y + r, black);   // 位=1 黑
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
#if SERIAL_REMOTE
    // 远程控制专用版: GPIO3 是串口 RX, 全程让给串口 (接收命令字节), 不驱动。
    // 物理 KEY3 因此失效 (摸不到设备时无所谓); 按键改由串口命令注入。
    return 1;   // 恒视为"未按下" (不驱动总线, 保证 RX 能收命令)
#else
    pinMode(KEY3_PIN, OUTPUT);
    digitalWrite(KEY3_PIN, HIGH);
    return digitalRead(KEY3_PIN);
#endif
}

// ---------- 远程控制专用: 串口按键注入 (仅 SERIAL_REMOTE=1 编译) ----------
// 物理 KEY3(GPIO3=RX) 让给串口后, 按键事件改由 PC 通过串口行命令注入。
// 命令按行(以 \n 或 \r 结尾), 支持:
//   K2S  中键短按   K2L  中键长按   K3S  右键短按   K3L  右键长按
//   B    组合键(中短 → 右短 ≤1s)回主页   ?    帮助
// 注入结果写入 gInjR2/gInjR3(0=无,1=短,2=长), 由 loop() 每圈消费一次并覆盖 r2/r3,
// 从而 100% 复用现有 appMode 分发 —— 注入按键与物理按键效果完全一致。
#if SERIAL_REMOTE
static int gInjR2 = 0;      // 待注入的中键事件 (0=无,1=短,2=长)
static int gInjR3 = 0;      // 待注入的右键事件 (0=无,1=短,2=长)
static char gInjLine[24];   // 命令行缓冲
static uint8_t gInjLineLen = 0;

void serialRemoteInject(char c) {
    if (c == '\n' || c == '\r') {
        gInjLine[gInjLineLen] = '\0';
        gInjLineLen = 0;
        // 忽略空行
        if (gInjLine[0] == '\0') return;
        if (strcmp(gInjLine, "K2S") == 0) { gInjR2 = 1; }
        else if (strcmp(gInjLine, "K2L") == 0) { gInjR2 = 2; }
        else if (strcmp(gInjLine, "K3S") == 0) { gInjR3 = 1; }
        else if (strcmp(gInjLine, "K3L") == 0) { gInjR3 = 2; }
        else if (strcmp(gInjLine, "B") == 0) { gInjR2 = 1; gInjR3 = 1; }
        else if (strcmp(gInjLine, "?") == 0) {
            Serial.println("REMOTE_CMDS: K2S|K2L|K3S|K3L|B|?  (换行结尾)");
        }
        else {
            traceFmt("REMOTE_UNKNOWN cmd=%s", gInjLine);
        }
        // 置脏后 loop() 消费
    } else if (gInjLineLen < sizeof(gInjLine) - 1) {
        gInjLine[gInjLineLen++] = c;
    }
    // 超长则丢弃(不解析)
}

void serialRemotePoll() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        serialRemoteInject(c);
    }
}
#endif


// ---------- 电池电量 ----------
// V14 官方 Get_bat_vcc.ino: GPIO12 是电池分压开关, 采样前拉高, 采样后拉低。
// GPIO12 同时是 SD MISO; 仅在 SD CS 保持高电平时短暂采样, 随后恢复输入。
#define BAT_SWITCH_PIN 12
int lastBatteryMV = 0;
int readBatteryMV() {
    // 采样偶发失败 (sum=0, GPIO12 开关/SD 总线竞争) 时重试; 之前实测同一电池
    // 有时 sum=0(显示0%) 有时 sum=15840(4.34V), 重试可消除偶发 0。
    // 官方对照 (REVERSE_NOTES §13): V14 Get_bat_vcc.ino:9-42 只驱动 GPIO12(bat_switch_pin=SD MISO),
    // 3s 周期/3 次平均, **不碰 GPIO5(SD_CS)**; 本项目按 A7 反汇编笔记额外拉高 GPIO5 并按 60s/20 次采样
    // (A7 该函数体现在未反汇编, 序列未复核)。此差异是"采样后首次 SD 访问失败"的嫌疑点之一。
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

#define RECENT_READ_PATH "/recentread.dat"   // P1: 阅读状态归 LittleFS(内部介质), 不再放 SD 的 /.tiemereader
#define SLEEP_RECORD_PATH "/sleepmode.dat"

// ---------- 主页 / 应用模式 ----------
struct SleepRecord;   // 休眠记录 (定义在休眠区) — Arduino 自动原型需要此前向声明
void saveSleepRecord();
bool readSleepRecord(SleepRecord &rec);
enum AppMode { APP_HOME = 0, APP_BROWSER = 1, APP_READER = 2, APP_CHAPTERS = 3, APP_NETWORK = 4, APP_CLOCK_CONNECT = 5, APP_CLOCK = 6, APP_WEATHER = 7, APP_SETTINGS = 8, APP_BMP = 9, APP_MARKS = 10, APP_CLOCK_DISGUISE = 11, APP_STATS = 12 };
int appMode = APP_HOME;
bool sdAvailable = false;
// ---- 天气页面状态（本次开机缓存）----
ActualWeather wActual;
FutureWeather wFuture;
LifeIndex wLife;
bool wDataValid = false;

// ===== 阅读数据源抽象 (2026-09 官方同款: 媒体跟随) =====
// 官方 A7 用 fsSetBySdState() 把全局 fileSystem 在 LittleFS/SDFS 间切换 —— 阅读数据源 = **当前介质**:
//   SD 启用(默认) → 书/`.i1`/`.z1` 都在 SD, 直读 SD(大书 55MB 无障碍);
//   内部介质模式(sdEnabled=0/无卡) → 全部走 LittleFS。
// 本轮稳定性/续航修复(页表读取失败重试+不翻页、进度节流、RF 关断、EPD 生命周期)对两种介质同时生效。
// 定义在 browseFs() 之后(见 activeFileFs 段), 这里只做前向声明。
static fs::FS &readerFs();
static bool readerBusReady(const char *reason);
bool wFetching = false;
bool wNightSkip = false;   // 本次进入因夜间跳过联网
char wErrCode[16] = {0};
char wFetchStep[24] = {0};   // 当前获取步骤提示（对齐 A7: 实况/未来/生活指数）
char wCachedSummary[20] = {0};   // 主页天气摘要缓存（SD 持久化, 重启保留; "天气名|温度"）

// 主页天气摘要: 读 LittleFS 缓存 (P1/P2: 阅读期间 SD 已卸载; 天气摘要属设备状态 → LittleFS)
void loadWeatherCache() {
    if (wCachedSummary[0]) return;
    File f = readerFs().open("/weather.dat", "r");
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

// 天气获取成功后写 LittleFS 缓存（主页摘要持久化）
void saveWeatherCache() {
    if (!wDataValid) return;
    File f = readerFs().open("/weather.dat", "w");
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
bool recentReadBuilding = false;   // 索引未完成/构建中/中断: 主页主卡页码后缀"构建中" (页码不即时刷新)
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
uint32_t gIndexStepKeyYield = 0;   // 构建步进因按键让步次数 (诊断: 构建期按键灵敏度)
uint32_t gIndexStepMaxGapMs = 0;   // 构建中两次按键扫描的最大间隔 (诊断)
uint32_t txtChapterCount = 0;
bool txtIndexBuilding = false;
uint32_t txtPendingProgress = 0;
uint32_t txtIndexLastFlush = 0;
// ---- P6 (2026-09) 进度节流状态: RAM 待写, 50 页/5 分钟/退出/换书 才落盘 LittleFS ----
static uint32_t gProgPending = 0;
static bool     gProgDirty = false;
static bool     gProgPendingBuilding = false;
static uint16_t gProgPagesSince = 0;
static uint32_t gProgLastFlushMs = 0;
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

// ---------- 阅读器菜单 (局部刷新弹窗, 对齐 A7 7 项 + 扩展) ----------
bool readerMenuOpen = false;
int readerMenuSel = 0;             // 0..11: 字体选择/退出/自动翻页/全刷间隔：/旋转/跳转/章节/标签/休眠/进度同步/配网/重建索引
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
void renderStatsPage(bool full);
void settingsHandleKeys(int r2, int r3);
// 设置页状态（全局，供 enterHomeCard 初始化）
#define SETTINGS_MAX_TABS 5
#define SETTINGS_TAB_WIFI 0
#define SETTINGS_TAB_SD 1
#define SETTINGS_TAB_CLOCK 2
#define SETTINGS_TAB_WEATHER 3
#define SETTINGS_TAB_BAT 4
// 每分类标签的设置项数量（官方 5 分类：WiFi/存储卡/时钟/天气&其他/电池校准）
static const uint8_t kSettingsTabItemCnt[SETTINGS_MAX_TABS] = { 3, 2, 5, 5, 2 };
static const char *const kSettingsTabNames[SETTINGS_MAX_TABS] = {"WIFI", "存储卡", "时钟", "天气", "电池"};
int settingsTab = 0;        // 当前分类标签 0..4
int settingsSel = 0;        // 当前分类下的项索引
int settingsLevel = 0;      // 0=右栏项列表(子层) 1=左栏父标签(父层; 进入设置默认光标在此=WIFI)
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
bool readTxtPage(uint32_t offset);   // 读一页正文; false=失败(txtLines空, 调用方勿渲染白屏)
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
bool writeProgress(uint32_t offset);   // 登记待写进度(RAM 节流; 50页/5分钟自动落盘)
bool progressFlushForce(const char *reason);   // 立即落盘待写进度(退出/换书/休眠)
void progressTick();                            // 主 loop 每圈调用
void drawReaderLine(int y, const String &line);

void loadRecentReadSummary() {
    recentReadPath = "";
    recentReadPage = 0;
    recentReadTotalPages = 0;
    recentReadValid = false;
    recentReadBuilding = false;
    if (!readerFs().exists(RECENT_READ_PATH)) {
        traceFmt("RECENT_ABORT file=%d", readerFs().exists(RECENT_READ_PATH) ? 1 : 0);
        return;
    }
    File f = readerFs().open(RECENT_READ_PATH, "r");
    if (!f) {   // 构建写流/EPD 抢占后偶发打不开 → 恢复总线重试一次, 避免误报"暂无阅读记录"
        readerBusReady("recent_retry");
        f = readerFs().open(RECENT_READ_PATH, "r");
    }
    if (!f) return;
    recentReadPath = f.readStringUntil('\n');
    f.close();
    recentReadPath.trim();
    if (recentReadPath.length() == 0 || !readerFs().exists(recentReadPath)) {
        readerBusReady("recent_txt_retry");   // 同上: 大书构建期 SD 忙, 存在性判定失败会误隐藏主卡
        if (recentReadPath.length() == 0 || !readerFs().exists(recentReadPath)) {
            traceFmt("RECENT_TXT_MISS len=%u exists=%d", (unsigned)recentReadPath.length(), readerFs().exists(recentReadPath) ? 1 : 0);
            recentReadPath = "";
            return;
        }
    }
    traceFmt("RECENT_TXT path=%s", recentReadPath.c_str());
    String indexPath = recentReadPath;
    int dot = indexPath.lastIndexOf('.');
    if (dot > 0) indexPath = indexPath.substring(0, dot);
    indexPath += readerIsPortrait() ? ".v1" : ".i1";   // 方向感知: 竖类(0/180).v1 / 横类(90/270).i1 (修复前硬编码 .i1, 竖屏用户启动提示取错索引)
    if (!readerFs().exists(indexPath.c_str())) {
        String legacyPath = recentReadPath + ".i1";
        if (readerFs().exists(legacyPath.c_str())) indexPath = legacyPath;
    }
    traceFmt("RECENT_INDEX path=%s", indexPath.c_str());
    // 构建中/构建中断进度在 sidecar (indexPath+"p", 如 小说.i1p), 优先于 .i1 记录[0]:
    // 后台构建或中断续建时主页才能与阅读器显示一致 (阅读器 startTxtReader 同优先级)。
    uint32_t savedOffset = 0;
    String sidecarPath = indexPath + "p";
    if (readerFs().exists(sidecarPath.c_str())) {
        File sp = readerFs().open(sidecarPath.c_str(), "r");
        if (sp && sp.size() >= 8) {
            char rec[9];
            for (uint8_t i = 0; i < 8; i++) rec[i] = (char)sp.read();
            rec[8] = '\0';
            savedOffset = strtoul(rec, nullptr, 10);
        }
        if (sp) sp.close();
    }
    // 构建中/中断判定 (主页主卡"构建中"标注用):
    // ① 构建期 sidecar 存在(构建进行中或中断未合并); ② 运行时正在构建同一本书。
    // ③ 在索引"看似完整"分支再校验尾部 size 标记, 见下 (半截 .i1 不能当完整总页数显示)。
    recentReadBuilding = readerFs().exists(sidecarPath.c_str()) ||
                         (txtIndexBuilding && txtPath == recentReadPath);
    uint32_t txtSize = 0;
    File tf = readerFs().open(recentReadPath.c_str(), "r");
    if (tf) { txtSize = tf.size(); tf.close(); }
    File index = readerFs().open(indexPath.c_str(), "r");
    if (!index) {   // 构建写流/EPD 抢占后偶发打不开 → 恢复总线重试一次
        readerBusReady("recent_idx_retry");
        index = readerFs().open(indexPath.c_str(), "r");
    }
    if (!index || index.size() < 16 || index.size() % 8 != 0) {
        if (index) index.close();
        // 索引不完整(构建中/中断): 仍视为"有上次阅读文件"(进入后阅读器从 sidecar/记录[0] 恢复,
        // 与 startTxtReader 同优先级), 但主页只显示文件名、不显示页码。记录[0] 尝试读出备用。
        if (savedOffset == 0) {
            File idx = readerFs().open(indexPath.c_str(), "r");
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
        recentReadBuilding = true;   // 索引太小/损坏: 属构建中/待重建, 主卡显示"构建中"
        recentReadValid = true;   // 文件存在即视为有上次阅读记录 (页码可为 0)
        traceFmt("RECENT_INCOMPLETE valid=1 page=0 total=0 off=%lu", (unsigned long)savedOffset);
        return;
    }
    recentReadTotalPages = (index.size() / 8) - 1;
    // 半截检测: 记录[N-1] 应为 txt 大小 (构建完成才写入); 不相等 → 中断构建/文件变更 → 标"构建中"。
    // (构建中 .i1 已具 8 对齐页表, 若不校验会把"已建条数-1"当总页数显示, 误导进度条/页码)
    {
        index.seek(index.size() - 8);
        char tailRec[9];
        for (uint8_t i = 0; i < 8; i++) tailRec[i] = (char)index.read();
        tailRec[8] = '\0';
        uint32_t tailV = strtoul(tailRec, nullptr, 10);
        if (txtSize && tailV != txtSize) recentReadBuilding = true;
        index.seek(0);
    }
    if (savedOffset == 0) {
        char record[9];
        for (uint8_t i = 0; i < 8; i++) record[i] = (char)index.read();
        record[8] = '\0';
        savedOffset = strtoul(record, nullptr, 10);
    }
    index.seek(8);   // 扫描必须从记录[1](页2页首) 起: sidecar 分支未消费记录[0], 不 seek 会把记录[0]=进度误当页2 → 页码错位
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
                    // 任一 LOW 采样即计按下: 用户"1秒内点按/按住 KEY3"都能触发 (对齐官方宽松行为)。
                    // 原"连续两次低采样"要求按住跨采样点, 快速点按会被清零错过。
                    gBootKey3Held = true;
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
    File f = readerFs().open(RECENT_READ_PATH, "w");
    if (!f) return;
    f.println(path);
    f.close();
}

void clearRecentReadPathIfMatches(const String &path) {
    if (!readerFs().exists(RECENT_READ_PATH)) return;
    File f = readerFs().open(RECENT_READ_PATH, "r");
    if (!f) return;
    String saved = f.readStringUntil('\n');
    f.close();
    saved.trim();
    if (saved == path) readerFs().remove(RECENT_READ_PATH);
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

void renderHome(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);

    // ── 状态栏 y0-16: 时间 | 日期星期 | 城市温度 | 电量(充电) ──
    time_t nowT = clockManagerNow();
    struct tm *tmv = nowT > 1600000000UL ? localtime(&nowT) : nullptr;
    char tbuf[12], dbuf[24];
    if (tmv) {
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv->tm_hour, tmv->tm_min);
        snprintf(dbuf, sizeof(dbuf), "%02d-%02d %s", tmv->tm_mon + 1, tmv->tm_mday, weekdayCn(tmv->tm_wday));
    } else {
        snprintf(tbuf, sizeof(tbuf), "--:--");
        snprintf(dbuf, sizeof(dbuf), "-- -- ---");
    }
    drawTextUTF8(4, 0, tbuf, 60, true);
    drawTextUTF8(70, 0, dbuf, 90, true);
    // 城市+温度 (城市=Web 配置 wc.city EEPROM 持久; 温度=内存 wActual.temp 或 SD 缓存兜底)
    {
        WeatherConfig wc;
        loadWeatherConfig(wc);
        char wbuf[40];
        if (wDataValid && wActual.temp[0]) {
            snprintf(wbuf, sizeof(wbuf), "%s %s℃", wc.city[0] ? wc.city : "--", wActual.temp);
        } else {
            loadWeatherCache();
            char *sep = strchr(wCachedSummary, '|');
            if (sep && sep != wCachedSummary) {
                snprintf(wbuf, sizeof(wbuf), "%s %s℃", wc.city[0] ? wc.city : "--", sep + 1);
            } else {
                snprintf(wbuf, sizeof(wbuf), "%s --℃", wc.city[0] ? wc.city : "--");
            }
        }
        drawTextUTF8(162, 0, wbuf, 88, true);
    }
    int mv = readBatteryMV();
    char bbuf[12];
    snprintf(bbuf, sizeof(bbuf), "电量%d%%", batPercent(mv));
    drawTextUTF8(228, 0, bbuf, 66, true);
    if (isCharging()) drawLightningIcon(276, 1, true);  // 充电: 画闪电图标
    fillRect(0, 15, SCR_W, 1, true);

    // ── 主卡: 继续阅读 (y18-54, 高37, 仅信息展示) ──
    const int mY = 18, mH = 37;
    char recentTitle[96];
    char recentDetail[56];
    if (!sdAvailable) {
        snprintf(recentTitle, sizeof(recentTitle), "请插入SD卡");
        snprintf(recentDetail, sizeof(recentDetail), "上次阅读");
    } else if (!recentReadValid) {
        snprintf(recentTitle, sizeof(recentTitle), "暂无阅读记录");
        snprintf(recentDetail, sizeof(recentDetail), "上次阅读");
    } else {
        const char *name = strrchr(recentReadPath.c_str(), '/');
        name = name ? name + 1 : recentReadPath.c_str();
        snprintf(recentTitle, sizeof(recentTitle), "%s", name);
    }
    fillRect(2, mY, 292, mH, false);
    drawTextUTF8(6, mY + 3, recentTitle, 250, true);   // 书名 (去掉"继续阅读"标题行, 避免与进度条重叠)
    // 进度条 + 页码百分比 (索引未完成/构建中: 只显页码+"构建中", 不显误导性总页数/百分比; 页码不即时刷新)
    if (recentReadValid && recentReadTotalPages > 0 && !recentReadBuilding) {
        uint32_t pct = (uint32_t)(((uint64_t)recentReadPage * 100) / recentReadTotalPages);
        if (pct > 100) pct = 100;
        const int pbX = 6, pbY = mY + 25, pbW = 140, pbH = 5;
        fillRect(pbX, pbY, pbW, pbH, false);
        drawRect(pbX, pbY, pbW, pbH, true);
        int fillW = (int)((uint64_t)pbW * pct / 100);
        if (fillW > 0) fillRect(pbX + 1, pbY + 1, fillW - 1, pbH - 2, true);
        char pr[12];
        formatProgressPercent((uint64_t)recentReadPage, (uint64_t)recentReadTotalPages, pr);
        drawTextUTF8(pbX + pbW + 6, pbY - 5, pr, 60, true);
        char pg[20];
        snprintf(pg, sizeof(pg), "%lu/%lu页", (unsigned long)recentReadPage, (unsigned long)recentReadTotalPages);
        drawTextUTF8(228, pbY - 5, pg, 68, true);
    } else if (recentReadValid && recentReadBuilding) {
        char pg[32];
        if (recentReadPage >= 1)
            snprintf(pg, sizeof(pg), "第%lu页 构建中", (unsigned long)recentReadPage);
        else
            snprintf(pg, sizeof(pg), "构建中…");
        drawTextUTF8(6, mY + 25, pg, 230, true);
    } else {
        drawTextUTF8(6, mY + 25, recentDetail, 200, true);
    }
    drawRect(2, mY, 292, mH, true);   // 主卡 (homeSel==0 时画选中框, 可点按续读)
    if (homeSel == 0) drawRect(0, mY - 2, SCR_W, mH + 4, true);   // 主卡选中: 外围框 (提示可点按续读)

    // ── 导航 2行×3列 (y58-124) ──
    // 位1-6: 统计/文件/时钟 | 天气/配网/设置 (统计替换续读放第一位; 续读功能移到主卡)
    static const char *const navNames[6] = {"统计", "文件", "时钟", "天气", "配网", "设置"};
    static const int navX[3] = {2, 99, 196};
    static const int navY[2] = {58, 94};
    const int nw = 93, nh = 32;
    for (int i = 0; i < 6; i++) {
        int col = i % 3, row = i / 3;
        int nx = navX[col], ny = navY[row];
        bool sel = homeSel == (i + 1);
        fillRect(nx, ny, nw, nh, false);
        int tw = utf8Width(navNames[i]);
        // 图标(22) + 间距(6) + 文字 整体水平居中; 图标与文字垂直中心对齐
        int blockW = 22 + 6 + tw;
        int bx = nx + (nw - blockW) / 2;
        int iconY = ny + (nh - 22) / 2;   // 图标垂直居中
        int textY = ny + nh / 2 - 5;      // 文字基线 (垂直居中于图标)
        drawNavIcon(bx, iconY, i, true);
        drawTextUTF8(bx + 28, textY, navNames[i], nw - 4, true);
        if (sel) drawRect(nx, ny, nw, nh, true);
    }
    refresh(full);
}

void enterHomeCard() {
    switch (homeSel) {
        case 0: openRecentRead(); break;   // 主卡: 续读 (功能移到主卡)
        case 1:                            // 统计 (替换续读的导航位)
            appMode = APP_STATS;
            renderStatsPage(true);
            saveSleepRecord();
            break;
        case 2:
            currentPath = "/";
            selIndex = 0;
            topIndex = 0;
            listDir(currentPath.c_str());
            appMode = APP_BROWSER;
            renderAll();
            refresh(true);
            saveSleepRecord();   // 界面快照: 已进入文件管理器根目录
            break;
        case 3:
            if (!wifiManagerHasCredentials()) {
                // 未保存 WiFi 配置: 校准页显示"未配网", 自动跳过校准流程直接进入时钟页
                appMode = APP_CLOCK_CONNECT;
                renderClockNoWifi();   // 全刷提示"未配网"(停留≈1.5s)
                enterClockPage();      // 直接进时钟(芯片/软件时间, 状态显示"未校准")
                break;
            }
            appMode = APP_CLOCK_CONNECT;
            clockManagerBegin(renderClockConnect, enterClockPage);
            saveSleepRecord();   // 界面快照: 已进入配网时钟页
            break;
        case 4:
            enterWeatherPage();
            break;
        case 5:
            progressSyncFreeReaderHeap();   // 启动热点前腾堆: 关 txtFile + 清阅读行缓冲 (配网会话堆硬约束)
            freeItemList();   // 大目录 items≈34KB+ 是堆大户, 配网会话堆 ~5KB 必须释放
            fsCacheBuild();   // 进 AP 前扫描 SD 目录树 → LittleFS 缓存（/fs/list 浏览不碰 SD, 避开 SD×AP 崩溃）
            wifiManagerBegin(renderNetworkPage, exitNetworkPage);
            appMode = APP_NETWORK;
            saveSleepRecord();   // 界面快照: 已进入配网页
            break;
        case 6:
            settingsTab = 0;      // 默认父标签 = WIFI
            settingsLevel = 1;    // 进入时光标默认停在父标签层(WIFI), 右长进入项列表
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

// 上传状态通知: 上传中 / 上传完毕 / 上传失败（不弹窗不整屏切换——
// 复用配网页底行 gWebNotifyLine, 与 Web 设置"修改成功"通知同款显示: 局刷底行, 配网页保持可见）。
// 由 file_api 上传状态回调触发。
extern char gWebNotifyLine[160];   // 定义在 renderNetworkPage 前（配网页底行消息缓冲）
void renderUploadStatus(int phase, const char *path) {
    if (!textRendererReady) return;
    char titleBuf[24];
    if (phase == OFS_UP_PHASE_UPLOADING) snprintf_P(titleBuf, sizeof(titleBuf), PSTR("上传中"));
    else if (phase == OFS_UP_PHASE_DONE)  snprintf_P(titleBuf, sizeof(titleBuf), PSTR("上传完毕"));
    else if (phase == OFS_UP_PHASE_FAIL)  snprintf_P(titleBuf, sizeof(titleBuf), PSTR("上传失败"));
    else return;   // IDLE 不渲染
    const char *title = titleBuf;
    // 底行消息 = "上传中: <basename>"（path 截到末尾段, 防超宽）
    const char *b = path;
    if (b) for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    gWebNotifyLine[0] = '\0';
    if (title) snprintf_P(gWebNotifyLine, sizeof(gWebNotifyLine), PSTR("%s"), title);
    if (phase == OFS_UP_PHASE_UPLOADING) {
        char nb[96];
        snprintf_P(nb, sizeof(nb), PSTR("%s"), b ? b : "");
        // 超长文件名截短（防底行溢出）
        while (utf8Width(nb) > 250 && strlen(nb) > 2) nb[strlen(nb) - 1] = '\0';
        size_t a = strlen(gWebNotifyLine);
        snprintf_P(gWebNotifyLine + a, sizeof(gWebNotifyLine) - a, PSTR(":%s"), nb);
    } else if (phase == OFS_UP_PHASE_DONE) {
        snprintf_P(gWebNotifyLine + strlen(gWebNotifyLine), sizeof(gWebNotifyLine) - strlen(gWebNotifyLine), PSTR(":文件已保存"));
    } else if (phase == OFS_UP_PHASE_FAIL) {
        snprintf_P(gWebNotifyLine + strlen(gWebNotifyLine), sizeof(gWebNotifyLine) - strlen(gWebNotifyLine), PSTR(":请检查空间/文件名"));
    }
    Serial.printf_P(PSTR("UP_NOTIFY_LINE %s\n"), gWebNotifyLine);
    // 局刷配网页底行（不上传/配网之外界面则仅更新缓冲, 不刷新屏幕）
    if (appMode == APP_NETWORK) renderNetworkPage(false);
}

// 下载状态通知: 下载中 / 下载完毕 / 下载失败（同上传, 配网页底行 gWebNotifyLine 显示, 不弹窗）
// 由 file_api 下载 handler 起止回调触发。
void renderDownloadStatus(int phase, const char *path) {
    if (!textRendererReady) return;
    char titleBuf[24];
    if (phase == OFS_DL_PHASE_START) snprintf_P(titleBuf, sizeof(titleBuf), PSTR("下载中"));
    else if (phase == OFS_DL_PHASE_DONE) snprintf_P(titleBuf, sizeof(titleBuf), PSTR("下载完毕"));
    else if (phase == OFS_DL_PHASE_FAIL) snprintf_P(titleBuf, sizeof(titleBuf), PSTR("下载失败"));
    else return;
    const char *title = titleBuf;
    const char *b = path;
    if (b) for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    gWebNotifyLine[0] = '\0';
    if (title) snprintf_P(gWebNotifyLine, sizeof(gWebNotifyLine), PSTR("%s"), title);
    if (phase == OFS_DL_PHASE_START) {
        char nb[96];
        snprintf_P(nb, sizeof(nb), PSTR("%s"), b ? b : "");
        while (utf8Width(nb) > 250 && strlen(nb) > 2) nb[strlen(nb) - 1] = '\0';
        size_t a = strlen(gWebNotifyLine);
        snprintf_P(gWebNotifyLine + a, sizeof(gWebNotifyLine) - a, PSTR(":%s"), nb);
    } else if (phase == OFS_DL_PHASE_DONE) {
        snprintf_P(gWebNotifyLine + strlen(gWebNotifyLine), sizeof(gWebNotifyLine) - strlen(gWebNotifyLine), PSTR(":已下载"));
    } else if (phase == OFS_DL_PHASE_FAIL) {
        snprintf_P(gWebNotifyLine + strlen(gWebNotifyLine), sizeof(gWebNotifyLine) - strlen(gWebNotifyLine), PSTR(":请重试"));
    }
    Serial.printf_P(PSTR("DL_NOTIFY_LINE %s\n"), gWebNotifyLine);
    if (appMode == APP_NETWORK) renderNetworkPage(false);
}

// 通用文件管理操作通知（新建文件/夹、删除、重命名/移动; 与上传/下载同款: 配网页底行局刷）
// 由 file_api_fs 的 ofsOpReport 回调触发。
void renderFileOpStatus(int phase, const char *msg) {
    if (!textRendererReady) return;
    const char *prefix = NULL;
    if (phase == OFS_OP_PHASE_START) prefix = "操作中";
    else if (phase == OFS_OP_PHASE_DONE) prefix = "操作成功";
    else if (phase == OFS_OP_PHASE_FAIL) prefix = "操作失败";
    else return;
    // ⚠️ 零栈缓冲(handler 深链剩余栈≈0): 直接写全局 gWebNotifyLine; UTF-8 尾字节安全截断
    gWebNotifyLine[0] = '\0';
    snprintf_P(gWebNotifyLine, sizeof(gWebNotifyLine), PSTR("%s"), prefix);
    if (msg && msg[0]) {
        size_t a = strlen(gWebNotifyLine);
        size_t cap = sizeof(gWebNotifyLine) - a - 2;
        if (cap > 2) {
            gWebNotifyLine[a++] = ':';
            size_t n = strlen(msg);
            if (n > cap - 1) n = cap - 1;
            memcpy(gWebNotifyLine + a, msg, n);
            size_t end = a + n;
            while (end > a && (((unsigned char)gWebNotifyLine[end - 1]) & 0xC0) == 0x80) end--;
            if (end > a && (((unsigned char)gWebNotifyLine[end - 1]) & 0xC0) == 0xC0) end--;
            gWebNotifyLine[end] = '\0';
        }
    }
    Serial.printf_P(PSTR("OP_NOTIFY_LINE %s\n"), gWebNotifyLine);
    // ⚠️ 不在此渲染 EPD: PUT/DELETE handler 深链实测 stack≈0, 内部渲染→溢出/忙等→Soft WDT 复位。
    // 渲染统一由 wifiManagerLoop 在 handleClient 后(浅栈)检测 ofsOpGetPhase 变化执行 renderNetworkPage。
}

// 配网页底行常驻"Web 修改成功"消息（wifi_manager 保存端点回调写入; 左对齐可右溢出屏外自然裁剪,
// 下一次修改到来前不消失; 退出配网时清空 → 恢复"中键长按退出"）
char gWebNotifyLine[160] = "";

void renderNetworkPage(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    char line[96];
    if (wifiManagerIsStaOnly()) {
        // 已连 WiFi(局域网管理): 显示 IP + 局域网访问地址(无热点)
        drawTextUTF8(4, 2, F("网络配网"), 120, true);
        drawTextUTF8(4, 22, F("WiFi 已连接(局域网管理)"), 288, true);
        snprintf_P(line, sizeof(line), PSTR("IP: %s"), wifiManagerStaIp());
        drawTextUTF8(4, 42, line, 288, true);
        snprintf_P(line, sizeof(line), PSTR("管理: http://%s"), wifiManagerStaIp());
        drawTextUTF8(4, 62, line, 288, true);
        drawTextUTF8(4, 82, F("同一WiFi下手机/电脑访问"), 288, true);
    } else if (wifiManagerIsTryingSta()) {
        // 正在试连 WiFi(尚未开热点)
        drawTextUTF8(4, 2, F("网络配网"), 120, true);
        drawTextUTF8(4, 22, F("正在连接 WiFi..."), 288, true);
        drawTextUTF8(4, 42, F("成功: 显示IP, 局域网管理"), 288, true);
        drawTextUTF8(4, 62, F("失败: 自动开启热点"), 288, true);
    } else {
        drawTextUTF8(4, 2, F("网络配网"), 120, true);
        drawTextUTF8(4, 22, wifiManagerStateText(), 288, true);
        snprintf_P(line, sizeof(line), PSTR("热点: %s"), wifiManagerApSsid());
        drawTextUTF8(4, 42, line, 288, true);
        drawTextUTF8(4, 62, F("密码: 333333333"), 288, true);
        drawTextUTF8(4, 82, F("地址: 192.168.4.1"), 288, true);
    }
    const char *staIp = wifiManagerStaIp();
    if (gWebNotifyLine[0]) {
        // 常驻修改消息: 左对齐, 超宽向右溢出屏幕(画布边界自然裁剪), 不换行
        drawTextUTF8(4, 102, gWebNotifyLine, 2000, true);
    } else if (!wifiManagerIsStaOnly() && staIp && staIp[0]) {
        snprintf_P(line, sizeof(line), PSTR("STA: %s"), staIp);
        drawTextUTF8(4, 102, line, 288, true);
    } else if (!wifiManagerIsStaOnly() && !wifiManagerIsTryingSta()) {
        drawTextUTF8(4, 102, F("中键长按退出"), 288, true);
    }
    refresh(full);
}

// Web 设置修改成功提示（wifi_manager 回调）: 拼"修改成功：选项=值"写入底行常驻消息并局刷配网页
void webSettingsNotify(const char *line1, const char *line2) {
    gWebNotifyLine[0] = '\0';
    if (line1 && line1[0]) snprintf_P(gWebNotifyLine, sizeof(gWebNotifyLine), PSTR("%s"), line1);
    if (line2 && line2[0]) {
        size_t a = strlen(gWebNotifyLine);
        if (a > 0) {
            snprintf_P(gWebNotifyLine + a, sizeof(gWebNotifyLine) - a, PSTR("："));
            a = strlen(gWebNotifyLine);
        }
        snprintf_P(gWebNotifyLine + a, sizeof(gWebNotifyLine) - a, PSTR("%s"), line2);
    }
    Serial.printf_P(PSTR("WEB_NOTIFY_LINE %s\n"), gWebNotifyLine);
    if (textRendererReady && appMode == APP_NETWORK) {
        renderNetworkPage(false);   // 局刷底行(消息常驻, 下次修改才更新)
    }
}

void exitNetworkPage() {
    gWebNotifyLine[0] = '\0';   // 退出配网清空底行消息(下次进入显示"中键长按退出")
    wifiManagerRfOff("network_exit");   // P5: 配网退出统一关 RF (官方 WifiShutdown 对齐)
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

// 未保存 WiFi 配置: 校准页骨架 + "未配网" 提示（全刷 ≈1.5s 停留）, 不启动网络校准, 随后直接进时钟页
void renderClockNoWifi() {
    fillRect(0, 0, SCR_W, SCR_H, false);
    drawTextUTF8(4, 2, "时间校准", 100, true);
    const int y = 42;
    drawTextUTF8(25, y, "设备", 42, true);
    drawTextUTF8(126, y, "路由器", 56, false);
    drawTextUTF8(238, y, "互联网", 56, false);
    drawTextUTF8(66, y + 1, "···", 52, true);
    drawTextUTF8(178, y + 1, "···", 52, true);
    drawTextUTF8(4, 76, "未配网", 288, true);
    drawTextUTF8(4, 96, "未保存 WiFi 配置，跳过校准", 288, true);
    drawTextUTF8(4, 112, "进入时钟", 288, true);
    refresh(true);   // 全刷呈现提示(约1.5s)后由调用方进入时钟页
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
bool gClockDisguiseMode = false;   // 伪装时钟: 恒用简洁外观、不联网 (进入/启动恢复时置位)

// 一行 5 位时间数码管 (HH:MM, 7 段; 冒号含在内)
static void drawClockTimeDigits(int x, int y, int dw, int dh, int dt, int gap, int colonW, const char *hm) {
    drawSevenSegDigit(x, y, dw, dh, dt, hm[0], true);
    drawSevenSegDigit(x + dw + gap, y, dw, dh, dt, hm[1], true);
    drawClockColon(x + (dw + gap) * 2, y, dh, true);
    drawSevenSegDigit(x + (dw + gap) * 2 + colonW, y, dw, dh, dt, hm[3], true);
    drawSevenSegDigit(x + (dw + gap) * 2 + colonW + dw + gap, y, dw, dh, dt, hm[4], true);
}

// 小号 7 段数字序列 (温湿度用; 非数字字符跳过), 返回已占宽度
static int drawMini7Seq(int x, int y, int dw, int dh, int dt, int gap, const char *num, bool black) {
    int cx = x;
    for (const char *p = num; *p; p++) {
        if (*p >= '0' && *p <= '9') { drawSevenSegDigit(cx, y, dw, dh, dt, *p, black); cx += dw + gap; }
        else cx += dw;   // 符号位占位
    }
    return cx - x - gap;
}

// 校准状态短文案 (跳过校准也视为未校准)
static const char *clockCalibText() {
    return (clockManagerWasSkipped() || !clockManagerIsSynced()) ? "未校准" : "已校准";
}

// 获取一言（仅 WiFi 已连、设置开启且时钟为精美类型时；失败静默不打扰时钟页）
void fetchHitokotoFlow() {
    yiyanText[0] = '\0';
    if (settingsGetHitokotoEnabled() == 0) return;
    if (settingsGetClockMod() != 1) return;   // 2026-09: 一言只在"精美"时钟显示; 简洁不拉取不显示
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
    gClockDisguiseMode = false;
    clockCheckInAWordReset();   // 文本="重置系统" → 恢复默认并重启(函数不返回)
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
        debugFmt("CLOCK_PAGE_ENTER epoch=%lu local=%s mod=%u", (unsigned long)dbgNow, dbgBuf,
                 (unsigned)settingsGetClockMod());
    }
    // 联网数据: 一言(仅精美拉取) + B粉(倒计时不需网络); 失败静默, 屏幕仍显示上一页
    clockFansRequest();
    fetchHitokotoFlow();
    renderClockPage(true);
    saveSleepRecord();   // 界面快照: 已进入时钟页
}

// 老板快捷键: 阅读页中长按 → 局刷伪装成时钟 (校准状态), 停用全部按键。
// 伪装 = 简洁时钟外观 (renderClockPage 强制简洁), 恒横屏 (fbRot=90, 与阅读方向无关), 不联网不校准。
// 退出: 仅 KEY1 硬件复位 → 开机 1 秒 KEY3 窗口内按 KEY3 → 回主页 (复用 setup 的 key3Held 逻辑);
//       复位后未按 KEY3 → 按睡眠记录恢复伪装页 (保持伪装, 不暴露阅读器)。
void enterClockDisguise() {
    appMode = APP_CLOCK_DISGUISE;
    gClockDisguiseMode = true;   // 伪装: 强制简洁外观 (屏蔽精美/一言)
    fbRot = 90;                  // 时钟恒横屏
    yiyanText[0] = '\0';         // 伪装不联网: 不显示一言 (避免暴露联网能力)
    renderClockPage(false);      // 局刷显示时钟页 (校准状态), 与翻页同效: 只清空文字局刷, 不闪屏
    saveSleepRecord();           // 界面快照: 伪装模式 (复位后按记录恢复)
    debugLine("DISGUISE_ENTER");
}

// 温湿度参考文本: 无有效数据返回 false 且 out 置空; 有则 "26℃ 62%"
static bool clockThText(char *out, size_t cap) {
    out[0] = '\0';
    if (!wDataValid) return false;
    const char *tp = wActual.temp[0] ? wActual.temp : "--";
    const char *hum = wActual.humidity[0] ? wActual.humidity
                      : (wFuture.humidity[0] ? wFuture.humidity : "--");
    snprintf(out, cap, "%s℃ %s%%", tp, hum);
    return true;
}

// ---------- 多功能输入框 InAWord 屏幕端 (2026-09 P2) ----------
// 文本由 web 只存原样; 每次使用前现分类现解析 (不另存 mod 字段)。
// 官方语法: 空=一言(0) / 自定义句≤21汉字(1) / 倒yyyymmdd事件(2) / B粉UID(3) / 文本"重置系统"(4)
static const char UTF8_DAI[] = "\xE5\x80\x92";      // 倒
static const char UTF8_FEN[] = "\xE7\xB2\x89";      // 粉
static const char UTF8_RESET[] = "\xE9\x87\x8D\xE7\xBD\xAE\xE7\xB3\xBB\xE7\xBB\x9F"; // 重置系统
enum { IAM_YIYAN = 0, IAM_CUSTOM = 1, IAM_COUNTDOWN = 2, IAM_FANS = 3, IAM_RESET = 4 };

static int classifyInAWord(const char *t) {
    if (!t || !t[0]) return IAM_YIYAN;
    size_t n = strlen(t);
    if (n == strlen(UTF8_RESET) && memcmp(t, UTF8_RESET, n) == 0) return IAM_RESET;
    if (n >= 3 && memcmp(t, UTF8_DAI, 3) == 0) return IAM_COUNTDOWN;
    if (n >= 4 && t[0] == 'B' && memcmp(t + 1, UTF8_FEN, 3) == 0) return IAM_FANS;
    return IAM_CUSTOM;
}

// 解析 "倒" + 8 位 yyyymmdd + 事件(可为空); 成功返回 true
static bool parseCountdown(const char *t, int *year, unsigned *mon, unsigned *day, char *ev, size_t evCap) {
    if (!t || memcmp(t, UTF8_DAI, 3) != 0) return false;
    const char *p = t + 3;
    if (p[0] < '0' || p[0] > '9') return false;
    unsigned nums[3] = {0, 0, 0};
    for (int seg = 0; seg < 3; seg++) {
        int len = (seg == 0) ? 4 : 2;
        for (int i = 0; i < len; i++) {
            if (p[0] < '0' || p[0] > '9') return false;
            nums[seg] = nums[seg] * 10 + (unsigned)(p[0] - '0');
            p++;
        }
    }
    if (nums[0] < 2000 || nums[0] > 2100 || nums[1] < 1 || nums[1] > 12 || nums[2] < 1 || nums[2] > 31)
        return false;
    *year = (int)nums[0];
    *mon = nums[1];
    *day = nums[2];
    if (ev && evCap) {   // 事件 = 剩余文本 (空格压缩首尾)
        size_t w = 0;
        const char *q = p;
        while (*q == ' ') q++;
        while (*q && w + 1 < evCap) ev[w++] = *q++;
        while (w > 0 && ev[w - 1] == ' ') w--;
        ev[w] = '\0';
    }
    return true;
}

// Hinnant days_from_civil (仅同源免时区日期差用)
static int64_t inaDaysFromCivil(int y, unsigned m, unsigned d) {
    y -= (int)(m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// B粉会话缓存 (进入时钟页/配网会话有网时尝试一次, 之后复用)
static uint32_t gFansVal = 0;
static bool gFansOk = false;
static char gFansErr[16] = "";

static void clockFansRequest() {
    const char *t = settingsGetInAWord();
    if (classifyInAWord(t) != IAM_FANS) return;
    if (WiFi.status() != WL_CONNECTED) { gFansOk = false; snprintf(gFansErr, sizeof(gFansErr), "无网络"); return; }
    const char *p = t + 1 + 3;   // 跳过 'B'+粉
    char uid[33];
    size_t w = 0;
    while (p[0] >= '0' && p[0] <= '9' && w + 1 < sizeof(uid)) uid[w++] = *p++;
    uid[w] = '\0';
    if (w == 0) { gFansOk = false; snprintf(gFansErr, sizeof(gFansErr), "无UID"); return; }
    uint32_t v = 0;
    char err[16];
    gFansOk = fetchBiliFollower(uid, &v, err, sizeof(err));
    if (gFansOk) { gFansVal = v; gFansErr[0] = '\0'; }
    else snprintf(gFansErr, sizeof(gFansErr), "%s", err);
    debugFmt("BILI_FANS ok=%d val=%lu err=%s", gFansOk ? 1 : 0, (unsigned long)gFansVal, gFansErr);
}

// 文本"重置系统" → 全设置恢复 + 重启 (每次开机只处理一次; 恢复后文本清空)
static void resetAllSettingsToDefault() {
    wifiManagerClearConfig();   // 清除 WiFi 凭据
    SettingsConfig s;
    memset(&s, 0, sizeof(s));
    s.magic = 0x53455433UL;
    s.version = 3;
    s.clockFormat = 0;
    s.tzOffsetMin = 480;
    s.hitokotoEnabled = 1;
    s.portrait = 0;
    s.longPressMs = 500;
    strncpy(s.ntpServer, "cn.pool.ntp.org", sizeof(s.ntpServer) - 1);
    s.sdFrequency = 20;
    s.fullRefreshMin = 25;
    s.calibIntervalMin = 60;
    s.batDisplayType = 1;
    s.nightUpdate = 1;
    s.fastFlip = 1;
    s.setRotation = 1;
    s.outputPower = 19;
    s.sdEnabled = 1;
    s.albumAuto = 0;
    s.historyEnabled = 1;
    s.clockCalibrationState = 1;
    s.clockMod = 0;
    s.clockCompensate = 0;
    saveSettingsConfig(s);
    settingsSetInAWord("");
    WeatherConfig wc;
    memset(&wc, 0, sizeof(wc));
    wc.magic = 0x57544852UL;
    strncpy(wc.city, "深圳", sizeof(wc.city) - 1);
    saveWeatherConfig(wc);
    debugLine("INWORD_RESET all defaults");
    ESP.restart();
    delay(3000);   // 重启前防御
}

static void clockCheckInAWordReset() {
    static bool once = false;
    if (once) return;
    const char *t = settingsGetInAWord();
    if (t && t[0] && classifyInAWord(t) == IAM_RESET) {
        once = true;
        resetAllSettingsToDefault();
    }
}

// 构建时钟副文本行 (一言/自定义句/倒计时/B粉): 输出到 out
// pretty=false(简洁): 一言/自定义句不输出 (官方: 简洁不支持), 倒计时/B粉两风格都输出
static void clockBuildSubText(char *out, size_t cap, bool pretty) {
    out[0] = '\0';
    const char *t = settingsGetInAWord();
    int mode = classifyInAWord(t);
    if (mode == IAM_YIYAN) {
        if (pretty && yiyanText[0]) snprintf(out, cap, "%s", yiyanText);
        return;
    }
    if (mode == IAM_CUSTOM) {
        if (pretty) { snprintf(out, cap, "%s", t); while (utf8Width(out) > 286 && strlen(out) > 1) out[strlen(out) - 1] = '\0'; }
        return;
    }
    if (mode == IAM_COUNTDOWN) {
        int y = 0; unsigned m = 0, d = 0; char ev[40];
        if (parseCountdown(t, &y, &m, &d, ev, sizeof(ev))) {
            time_t nowT = clockManagerNow();
            struct tm *nw = nowT > 1600000000UL ? localtime(&nowT) : nullptr;
            if (!nw) { snprintf(out, cap, "倒计时 %d-%02u-%02u", y, m, d); return; }
            long diff = (long)(inaDaysFromCivil(y, m, d) -
                               inaDaysFromCivil(nw->tm_year + 1900, (unsigned)(nw->tm_mon + 1), (unsigned)nw->tm_mday));
            const char *evn = ev[0] ? ev : "目标";
            if (diff > 0) snprintf(out, cap, "距%s还有%ld天", evn, diff);
            else if (diff == 0) snprintf(out, cap, "今天是%s", evn);
            else snprintf(out, cap, "%s已过%ld天", evn, -diff);
        } else snprintf(out, cap, "倒计时格式错误");
        return;
    }
    if (mode == IAM_FANS) {
        if (gFansOk) {
            if (gFansVal < 10000) snprintf(out, cap, "BiliBili: %lu", (unsigned long)gFansVal);
            else snprintf(out, cap, "BiliBili: %lu.%luW",
                          (unsigned long)(gFansVal / 10000), (unsigned long)((gFansVal % 10000) / 1000));
        } else snprintf(out, cap, "B粉获取错误：%s", gFansErr[0] ? gFansErr : "未获取");
        return;
    }
    // RESET 不显示
}

// 时钟页 (2026-09 双风格): clockMod=0 简洁(数码管调大、无一言、小温湿度并入底部行)
//                         clockMod=1 精美(新布局: 顶信息行/中置时间/温湿度行/一言行, 数码管风格不变)
// 伪装(gClockDisguiseMode) 恒走简洁。
void renderClockPage(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    time_t now = clockManagerNow();
    struct tm *tmNow = now > 1600000000UL ? localtime(&now) : nullptr;
    char line[40];
    if (!tmNow) {
        drawTextUTF8(78, 25, "时间未知", 140, true);
        refresh(full);
        return;
    }
    int h24 = tmNow->tm_hour;
    int dispH = h24;
    bool isAm = true;
    if (settingsGetClockFormat() == 1) {
        isAm = h24 < 12;
        dispH = h24 % 12;
        if (dispH == 0) dispH = 12;
    }
    bool pretty = (settingsGetClockMod() == 1) && !gClockDisguiseMode;
    if (!pretty) {
        // ---------- 简洁 (用户定稿): 数码管调大, 无一言/自定义句 ----------
        const int dw = 46, dh = 78, dt = 7, gap = 6, colonW = 16;
        const int totalW = dw * 4 + gap * 3 + colonW;
        int x = (SCR_W - totalW) / 2, y = 2;
        snprintf(line, sizeof(line), "%02d:%02d", dispH, tmNow->tm_min);
        drawClockTimeDigits(x, y, dw, dh, dt, gap, colonW, line);
        if (settingsGetClockFormat() == 1) drawTextUTF8(256, 4, isAm ? "上午" : "下午", 36, true);
        fillRect(0, 94, SCR_W, 1, true);
        // 底部 A 行: 左 校准 + 小温湿度 (紧凑), 右 日期
        snprintf(line, sizeof(line), "%04d年%02d月%02d日", tmNow->tm_year + 1900, tmNow->tm_mon + 1, tmNow->tm_mday);
        char left[72];
        char th[32];
        th[0] = '\0';
        if (clockThText(th, sizeof(th))) snprintf(left, sizeof(left), "%s %s", clockCalibText(), th);
        else snprintf(left, sizeof(left), "%s", clockCalibText());
        while (utf8Width(left) > 168 && strlen(left) > 1) left[strlen(left) - 1] = '\0';   // 不压右侧日期
        drawTextUTF8(4, 96, left, 168, true);
        drawTextUTF8(180, 96, line, 112, true);
        // 底部 B 行: 倒计时/B粉 简洁也支持 (一言/自定义句不显示)
        char sub[96];
        clockBuildSubText(sub, sizeof(sub), false);
        if (sub[0]) {
            int sw = utf8Width(sub);
            int sx = (SCR_W - sw) / 2;
            if (sx < 0) sx = 0;
            drawTextUTF8(sx, 114, sub, SCR_W - 2, true);
        }
    } else {
        // ---------- 精美 (重新设计布局; 7 段数码管风格不变) ----------
        // 顶行: 日期 | 校准 | (12h 上午/下午)
        snprintf(line, sizeof(line), "%04d年%02d月%02d日", tmNow->tm_year + 1900, tmNow->tm_mon + 1, tmNow->tm_mday);
        drawTextUTF8(4, 2, line, 132, true);
        drawTextUTF8(150, 2, clockCalibText(), 60, true);
        if (settingsGetClockFormat() == 1) drawTextUTF8(258, 2, isAm ? "上午" : "下午", 36, true);
        // 中置时间数码管 (尺寸适中)
        const int dw = 46, dh = 60, dt = 6, gap = 6, colonW = 16;
        const int totalW = dw * 4 + gap * 3 + colonW;
        int x = (SCR_W - totalW) / 2, y = 16;
        snprintf(line, sizeof(line), "%02d:%02d", dispH, tmNow->tm_min);
        drawClockTimeDigits(x, y, dw, dh, dt, gap, colonW, line);
        // 温湿度: 7 段小号数字大字感 (标签+温度+℃ / 标签+湿度+%), 顺序排布不重叠; 缺失则留空
        if (wDataValid) {
            const int mdw = 22, mdh = 24, mdt = 4, mgap = 4;
            const int gy = 84;   // 温湿度区 84..108, 为下方副文本行让位
            const char *tp = wActual.temp[0] ? wActual.temp : "--";
            const char *hum = wActual.humidity[0] ? wActual.humidity
                              : (wFuture.humidity[0] ? wFuture.humidity : "--");
            int gx = 42;
            drawTextUTF8(gx, gy + 6, "温", 20, true);
            gx += 16 + 8;
            gx += drawMini7Seq(gx, gy, mdw, mdh, mdt, mgap, tp, true);
            drawTextUTF8(gx + 4, gy + 6, "℃", 20, true);
            gx += 24 + 18;
            drawTextUTF8(gx, gy + 6, "湿", 20, true);
            gx += 16 + 8;
            gx += drawMini7Seq(gx, gy, mdw, mdh, mdt, mgap, hum, true);
            drawTextUTF8(gx + 4, gy + 6, "%", 20, true);
        }
        // 副文本行: 一言/自定义句(仅精美) 与 倒计时/B粉(两风格共用入口); 无分割线避免与温湿度区交错
        char sub[96];
        clockBuildSubText(sub, sizeof(sub), true);
        if (sub[0]) drawTextUTF8(4, 112, sub, 288, true);
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

    // ── 左栏: 5 个父标签分类 (竖排, 同级间分割线, 选中=选择框) ──
    const int tabY0 = 4, tabH = 24;      // 5×24=120 → 底 124 ≤ 128
    for (int t = 0; t < SETTINGS_MAX_TABS; t++) {
        int y = tabY0 + t * tabH;
        bool cur = (t == settingsTab);
        const char *tn = kSettingsTabNames[t];
        int nw = utf8Width(tn);
        drawTextUTF8(8, y + 4, tn, 60, true);
        if (cur) drawRect(2, y + 1, 64, tabH - 2, true);   // 选择框(非反色; 父层/子层均标识当前分类)
        if (t < SETTINGS_MAX_TABS - 1) fillRect(0, y + tabH - 1, 70, 1, true);  // 同级分割线
    }
    // ── 左右分隔线(左移: x=70) ──
    fillRect(70, 0, 1, SCR_H, true);

    // ── 右 2/3: 当前分类下的详细设置选项 (同级间分割线) ──
    int cnt = kSettingsTabItemCnt[settingsTab];
    const int itemX0 = 76, itemY0 = 4, rowH = 24;
    SettingsConfig s;
    loadSettingsConfig(s);
    for (int i = 0; i < cnt; i++) {
        bool sel = (settingsLevel == 0) && (i == settingsSel);
        int y = itemY0 + i * rowH;
        char name[24];
        char lastDetail[64];
        lastDetail[0] = '\0';
        bool dev = false;   // 未开发
        switch (settingsTab) {
            case SETTINGS_TAB_WIFI:
                if (i == 0) { snprintf(name, sizeof(name), "输出功率"); snprintf(lastDetail, sizeof(lastDetail), "%udB", settingsGetOutputPower()); }
                else if (i == 1) { snprintf(name, sizeof(name), "NTP服务器"); snprintf(lastDetail, sizeof(lastDetail), "%s", settingsGetNtpServer()); }
                else { snprintf(name, sizeof(name), "SD频率"); snprintf(lastDetail, sizeof(lastDetail), "%uMHz", settingsGetSdFrequency()); }
                break;
            case SETTINGS_TAB_SD:
                if (i == 0) { snprintf(name, sizeof(name), "SD卡启用"); snprintf(lastDetail, sizeof(lastDetail), "%s", settingsGetSdEnabled() ? "启用" : "未启用"); }
                else { snprintf(name, sizeof(name), "相册自动播放"); dev = true; }
                break;
            case SETTINGS_TAB_CLOCK:
                if (i == 0) { snprintf(name, sizeof(name), "时钟格式"); snprintf(lastDetail, sizeof(lastDetail), "%s", s.clockFormat ? "12小时制" : "24小时制"); }
                else if (i == 1) { snprintf(name, sizeof(name), "时区"); formatTzOffset(s.tzOffsetMin, lastDetail, sizeof(lastDetail)); }
                else if (i == 2) { snprintf(name, sizeof(name), "全刷间隔"); snprintf(lastDetail, sizeof(lastDetail), "%u分钟", settingsGetFullRefreshMin()); }
                else if (i == 3) { snprintf(name, sizeof(name), "校准间隔"); snprintf(lastDetail, sizeof(lastDetail), "%u分钟", settingsGetCalibIntervalMin()); }
                else { snprintf(name, sizeof(name), "误差补偿"); dev = true; }
                break;
            case SETTINGS_TAB_WEATHER:
                if (i == 0) {
                    snprintf(name, sizeof(name), "天气城市");
                    WeatherConfig wc; loadWeatherConfig(wc);
                    snprintf(lastDetail, sizeof(lastDetail), "%s", wc.city[0] ? wc.city : "深圳");
                } else if (i == 1) { snprintf(name, sizeof(name), "夜间更新"); snprintf(lastDetail, sizeof(lastDetail), "%s", settingsGetNightUpdate() ? "更新" : "不更新"); }
                else if (i == 2) { snprintf(name, sizeof(name), "长按触发"); snprintf(lastDetail, sizeof(lastDetail), "%ums", settingsGetLongPressMs()); }
                else if (i == 3) { snprintf(name, sizeof(name), "屏幕旋转"); snprintf(lastDetail, sizeof(lastDetail), "方向%u", settingsGetSetRotation()); }
                else { snprintf(name, sizeof(name), "快速翻页"); snprintf(lastDetail, sizeof(lastDetail), "%s", settingsGetFastFlip() ? "开" : "关"); }
                break;
            case SETTINGS_TAB_BAT:
                if (i == 0) { snprintf(name, sizeof(name), "电池显示"); snprintf(lastDetail, sizeof(lastDetail), "%s", settingsGetBatDisplayType() ? "百分比" : "电压"); }
                else { snprintf(name, sizeof(name), "电压校准"); dev = true; }
                break;
            default: snprintf(name, sizeof(name), "?"); break;
        }
        int nw = utf8Width(name);
        drawTextUTF8(itemX0, y + 4, name, SCR_W - itemX0 - 8, true);
        int vx = itemX0 + nw + 12;
        int vmax = SCR_W - 2 - vx;
        if (vmax < 24) vmax = 24;
        if (dev)      drawTextUTF8(vx, y + 4, "未开发", vmax, true);
        else          drawTextUTF8(vx, y + 4, lastDetail, vmax, true);
        if (sel) drawRect(itemX0 - 4, y + 1, SCR_W - itemX0 + 2, rowH - 2, true);  // 选择框(不压分割线)
        if (i < cnt - 1) fillRect(70, y + rowH - 1, SCR_W - 70, 1, true);          // 同级分割线
    }
    // 底部按键描述已删除 (遮挡选项; 按键逻辑全局一致无需提醒)
    refresh(full);
}

// 执行设置页当前项 (toggle 切换 / enum 循环 / 进入编辑 / 未开发提示)
void settingsExecItem() {
    int tab = settingsTab, i = settingsSel;
    // 未开发项: 统一提示
    if ((tab == SETTINGS_TAB_SD && i == 1) || (tab == SETTINGS_TAB_CLOCK && i == 4) ||
        (tab == SETTINGS_TAB_BAT && i == 1)) {
        showMsg("未开发", "此项后续版本开放");
        return;
    }
    switch (tab) {
        case SETTINGS_TAB_WIFI:
            if (i == 0) { // 输出功率 10-20 循环
                uint8_t v = settingsGetOutputPower();
                settingsSetOutputPower(v >= 20 ? 10 : v + 1);
            } else if (i == 1) { // NTP 编辑(简化提示, 真编辑走 Web)
                showMsg("NTP服务器", "请在配网页修改");
            } else { // SD频率 5-40 循环
                uint8_t v = settingsGetSdFrequency();
                settingsSetSdFrequency(v >= 40 ? 5 : v + 5);
            }
            break;
        case SETTINGS_TAB_SD:
            if (i == 0) settingsSetSdEnabled(settingsGetSdEnabled() ? 0 : 1);
            break;
        case SETTINGS_TAB_CLOCK:
            if (i == 0) settingsSetClockFormat(settingsGetClockFormat() ? 0 : 1);
            else if (i == 1) settingsTzEdit = true;
            else if (i == 2) { // 全刷间隔 1-120 循环
                uint8_t v = settingsGetFullRefreshMin();
                settingsSetFullRefreshMin(v >= 120 ? 1 : v + 5);
            } else if (i == 3) { // 校准间隔 10-720 循环
                uint8_t v = settingsGetCalibIntervalMin();
                settingsSetCalibIntervalMin(v >= 720 ? 10 : v * 2);
            }
            break;
        case SETTINGS_TAB_WEATHER:
            if (i == 0) showMsg("天气城市", "请在配网页修改");
            else if (i == 1) settingsSetNightUpdate(settingsGetNightUpdate() ? 0 : 1);
            else if (i == 2) { // 长按触发 100-5000 循环
                uint16_t v = settingsGetLongPressMs();
                settingsSetLongPressMs(v >= 5000 ? 100 : v + 100);
            } else if (i == 3) settingsSetSetRotation((settingsGetSetRotation() + 1) % 4);
            else if (i == 4) settingsSetFastFlip(settingsGetFastFlip() ? 0 : 1);
            break;
        case SETTINGS_TAB_BAT:
            if (i == 0) settingsSetBatDisplayType(settingsGetBatDisplayType() ? 0 : 1);
            break;
    }
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
    int cnt = kSettingsTabItemCnt[settingsTab];
    if (settingsLevel == 0) {
        // Level 0: 项列表
        if (r2 == 1) {                         // 中短: 上移项
            settingsSel = (settingsSel + cnt - 1) % cnt;
            renderSettingsPage(false);
        } else if (r3 == 1) {                  // 右短: 下移项
            settingsSel = (settingsSel + 1) % cnt;
            renderSettingsPage(false);
        } else if (r2 == 2) {                  // 中长: 回到标签行层
            settingsLevel = 1;
            renderSettingsPage(false);
        } else if (r3 == 2) {                  // 右长: 执行/编辑当前项
            settingsExecItem();
            renderSettingsPage(false);
        }
    } else {
        // Level 1: 标签行
        if (r3 == 1) {                         // 右短: 下一个标签
            settingsTab = (settingsTab + 1) % SETTINGS_MAX_TABS;
            settingsSel = 0;
            renderSettingsPage(false);
        } else if (r2 == 1) {                  // 中短: 上一个标签
            settingsTab = (settingsTab + SETTINGS_MAX_TABS - 1) % SETTINGS_MAX_TABS;
            settingsSel = 0;
            renderSettingsPage(false);
        } else if (r3 == 2) {                  // 右长: 进入项列表层
            settingsLevel = 0;
            renderSettingsPage(false);
        } else if (r2 == 2) {                  // 中长: 返回首页
            appMode = APP_HOME;
            renderHome(true);
            saveSleepRecord();
        }
    }
    delay(30);
}

// ---------- 阅读统计页 (V1: 翻页/会话/连续/排行) ----------
void renderStatsPage(bool full) {
    fillRect(0, 0, SCR_W, SCR_H, false);
    drawTextUTF8(4, 2, "阅读统计", 100, true);
    fillRect(0, 14, SCR_W, 1, true);
    const StatsGlobal &g = statsGetGlobal();
    const BookStat *books = statsGetBooks();

    char line[48];
    int y = 20;
    // 今日 / 本周 / 累计 翻页+会话
    snprintf(line, sizeof(line), "今日   %lu页  %lu次", (unsigned long)g.dayPageTurns, (unsigned long)g.daySessions);
    drawTextUTF8(6, y, line, 280, true); y += 16;
    snprintf(line, sizeof(line), "本周   %lu页  %lu次", (unsigned long)g.weekPageTurns, (unsigned long)g.weekSessions);
    drawTextUTF8(6, y, line, 280, true); y += 16;
    snprintf(line, sizeof(line), "累计   %lu页  %lu次", (unsigned long)g.totalPageTurns, (unsigned long)g.totalSessions);
    drawTextUTF8(6, y, line, 280, true); y += 16;
    snprintf(line, sizeof(line), "连续阅读  %lu天", (unsigned long)g.streak);
    drawTextUTF8(6, y, line, 280, true); y += 18;

    fillRect(0, y, SCR_W, 1, true); y += 4;
    drawTextUTF8(6, y, "阅读最多", 100, true); y += 16;

    // TOP8 按 pageTurns 降序, 同页数 lastReadTime 新优先 (简单插入排序)
    int idx[MAX_BOOK_STATS];
    for (int i = 0; i < MAX_BOOK_STATS; i++) idx[i] = i;
    for (int i = 0; i < MAX_BOOK_STATS; i++)
        for (int j = i + 1; j < MAX_BOOK_STATS; j++) {
            int a = idx[i], b = idx[j];
            bool swap = false;
            if (books[b].pageTurns > books[a].pageTurns) swap = true;
            else if (books[b].pageTurns == books[a].pageTurns && books[b].lastReadTime > books[a].lastReadTime) swap = true;
            if (swap) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
        }
    int shown = 0;
    for (int i = 0; i < MAX_BOOK_STATS && shown < 5 && y < 110; i++) {
        int bi = idx[i];
        if (!books[bi].path[0] || books[bi].pageTurns == 0) continue;
        const char *nm = strrchr(books[bi].path, '/');
        nm = nm ? nm + 1 : books[bi].path;
        snprintf(line, sizeof(line), "%d. %s  %lu页", shown + 1, nm, (unsigned long)books[bi].pageTurns);
        drawTextUTF8(6, y, line, 280, true); y += 16; shown++;
    }
    if (shown == 0) drawTextUTF8(6, y, "暂无阅读数据", 200, true);
    refresh(full);
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
    // 未保存天气密钥或城市 → 弹窗提示, 页面直接停在"请到配网页设置"错误态, 不发起天气获取
    WeatherConfig wc;
    loadWeatherConfig(wc);
    if (wc.key[0] == '\0' || wc.city[0] == '\0') {
        wFetching = false;
        strncpy(wErrCode, "NOKEY", sizeof(wErrCode) - 1);
        wErrCode[sizeof(wErrCode) - 1] = '\0';
        showMsg("未配置天气", "未保存天气密钥或城市");
        if (wDataValid) renderWeatherErrorOverlay(wErrCode);
        else renderWeatherPage(true);
        saveSleepRecord();
        return;
    }
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

// ---- SD 介质切换: 文件管理器可浏览 SD 或本地 LittleFS ----
// gBrowseLocal=false(默认)=SD 介质(现行为, 零回归); true=本地 flash(LittleFS), 于 sdEnabled==0 或 SD 检测不到时置位
bool gBrowseLocal = false;
static fs::FS &browseFs() { return gBrowseLocal ? LittleFS : SDFS; }

// ---- 文件管理器介质抽象 (2026-09): 管理器(/fs/*)跟随介质选择, 不再硬编码 SD ----
fs::FS &activeFileFs() { return browseFs(); }
bool activeFsIsLocal() { return gBrowseLocal; }
bool activeFsBusReady(const char *reason) {
    if (gBrowseLocal) return true;        // 内部 LittleFS: 不与 EPD/电池采样共用总线
    return reinitSdBus(reason);
}

// ===== 阅读数据源 = 当前介质 (官方 fsSetBySdState 同款) =====
static fs::FS &readerFs() { return browseFs(); }
static bool readerBusReady(const char *reason) {
    if (gBrowseLocal) return true;        // 内部介质: 无共享总线问题
    return reinitSdBus(reason);           // SD 介质: 阅读链路每次 SD 访问前恢复总线(EPD/电池采样争抢)
}
// 本地介质隐藏系统资源: web 界面文件/系统缓存/统计目录（仅本地浏览层过滤, 不写入共享黑名单以免污染 SD 同名目录）
static bool localSystemEntry(const char *name) {
    if (!name || !name[0]) return false;
    if (name[0] == '.') return true;                              // 隐藏点文件/目录
    if (strcmp(name, "fslist") == 0 || strcmp(name, "stats") == 0) return true;  // 系统缓存/统计
    if (strcmp(name, "set.htm") == 0 || strcmp(name, "manager.htm") == 0) return true;  // web 界面文件
    return false;
}

// 加载窗口: 从 startIdx 开始填充 winItems[0..LIST_WINDOW-1]
// （openDir 从头扫, 纯目录项扫描 ~ms 级; 窗口化方案的核心, 大目录不占堆）
void loadListWindow(const char *path, int startIdx) {
    winCount = 0;
    if (startIdx < 0) startIdx = 0;
    if (startIdx >= itemCount) startIdx = itemCount > 0 ? itemCount - 1 : 0;
    Dir w = browseFs().openDir(path);
    int skipped = 0;
    while (w.next()) {
      String baseName = w.fileName();   // fs::Dir 返回 String; 过滤函数已纯 C（零额外分配）
      const char *nm = baseName.c_str();
      if (isBlacklistedEntry(nm)) continue;
      if (gBrowseLocal && localSystemEntry(nm)) continue;   // 本地介质: 隐藏系统资源
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
    // EPD 与 SD 共用 SPI；每次目录操作前恢复 SD 的片选和总线状态。（本地 LittleFS 介质无需 SD 总线）
    if (!gBrowseLocal) {
        digitalWrite(EPD_CS_PIN, HIGH);
        digitalWrite(5, HIGH);
        pinMode(5, OUTPUT);
        SPI.begin();
        bool sdBusOk = SD.begin(5, SD_SCK_MHZ(20));
        traceFmtLevel(sdBusOk ? 'I' : 'E', "SD_REINIT path=%s ok=%d", path ? path : "(null)", sdBusOk ? 1 : 0);
        if (!sdBusOk) return false;
    }
    // 使用官方 A7 同款枚举: openDir + Dir::next()（纯目录项扫描, 零重开文件）。
    // ⚠️ File::openNextFile() 内部每项 openFile("r") 重开文件 → 大目录(600+) O(n²) 路径解析 +
    //    低堆 malloc 失败 → 首次即 null（实测某些文件夹读不出）; rewindDirectory 是安慰剂, 已弃用。
    diagFlushSd(true);
    bool traceWasOpen = (bool)traceFile;
    if (traceWasOpen) traceFile.close();
    {
      File probe = browseFs().open(path, "r");
      bool okDir = probe && probe.isDirectory();
      if (probe) probe.close();
      if (!okDir) return false;
    }
    // 第一遍: 只计数（过滤后）—— 总数 itemCount, 不占堆（分页窗口化）
    // fileName() 返回 String（fs::Dir 包装层）; 过滤函数已纯 C 化（零额外分配）,
    // 每文件仅此 1 次 String 分配, 大幅降低 15KB 堆碎片压力（原 3 次/文件导致 239 bug）
    {
      int rawCount = 0;
      Dir cnt = browseFs().openDir(path);
      while (cnt.next()) {
        rawCount++;
        String baseName = cnt.fileName();
        const char *nm = baseName.c_str();
        if (isBlacklistedEntry(nm)) continue;
        if (gBrowseLocal && localSystemEntry(nm)) continue;   // 本地介质: 隐藏系统资源
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
    bool fg = true;   // 官方 A7 光标样式: 文字恒黑 (不再反色白字)
    // 整行白底 (空心框样式: 选中=画框, 不填充反色底)
    fillRect(0, y, SCR_W, ROW_H, false);
    if (!it) return;   // 窗口外: 只画白底
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
    if (selected) drawRect(0, y, SCR_W, ROW_H, true);   // 选中画空心框 (官方 A7 光标)
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
    if (gBrowseLocal) {   // 本地 LittleFS 介质仅浏览, 不支持图片查看
        showMsg("本地空间", "仅浏览，不支持查看");
        return;
    }
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
                    if (browseFs().remove(path.c_str())) {
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
    if (full) {
        epd.display(fb);          // 全刷末尾内部已 powerOff (对齐官方)
    } else {
        epd.displayPartial(fb);
#if READER_EPD_PAGEOFF
        // ===== P7 A/B (2026-09): 阅读页局刷后也断电 =====
        // 官方每页画完 display.powerOff() (DisplayTxt.ino:433/847/1076); 本机原为"局刷后保持上电"。
        // 仅阅读模式生效(菜单/弹窗不受影响); 下次局刷 displayPartial 内部会先 powerOn。
        if (appMode == APP_READER) epd.powerOff();
#endif
    }
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
    } else if (appMode == APP_STATS) {
        renderStatsPage(false);
    } else if (appMode == APP_CLOCK) {
        renderClockPage(false);
    } else if (appMode == APP_CLOCK_DISGUISE) {
        fbRot = 90;
        renderClockPage(false);   // 伪装模式保持时钟页
    } else if (appMode == APP_NETWORK) {
        renderNetworkPage(false);   // 配网页被提示框覆盖后恢复(局刷)
    } else if (appMode == APP_WEATHER) {
        renderWeatherPage(false);   // 天气页被提示框覆盖后恢复(局刷, 不触发网络动作)
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
    readerFs().remove(txtIndexPath.c_str());
    readerFs().remove(txtChapterPath.c_str());
    File indexFile = readerFs().open(txtIndexPath.c_str(), "w");
    File chapterFile = readerFs().open(txtChapterPath.c_str(), "w");
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
    File finalIndex = readerFs().open(txtIndexPath.c_str(), "r");
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
    readerFs().remove(txtIndexPath.c_str());
    readerFs().remove(txtChapterPath.c_str());
    readerFs().remove((txtIndexPath + "p").c_str());   // 全新构建: 清旧会话 sidecar (旧进度已在上游恢复进内存)
    txtIndexBuildFile = readerFs().open(txtIndexPath.c_str(), "w");
    txtChapterBuildFile = readerFs().open(txtChapterPath.c_str(), "w");
    txtIndexScanFile = readerFs().open(txtPath.c_str(), "r");
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
    File zeroRec = readerFs().open(txtIndexPath.c_str(), "r+");
    if (zeroRec) {
        zeroRec.seek(0);
        char rec[9];
        formatIndexNumber(txtPageStart, rec);
        zeroRec.print(rec);
        zeroRec.close();
    }
    // 进度已合并进记录[0], 删除构建期 sidecar (P6: 同时清待写标记, 避免重复落盘)
    gProgDirty = false;
    gProgPagesSince = 0;
    if (readerFs().exists((txtIndexPath + "p").c_str())) {
        readerFs().remove((txtIndexPath + "p").c_str());
        debugLine("IDX sidecar removed after merge");
    }
    txtChapterBuildFile.flush();
    txtChapterBuildFile.close();
    txtIndexScanFile.close();
    txtIndexBuilding = false;
    idxReleaseBuf();   // 生命周期化: 构建完成释放块读缓冲 (配网会话不占)
    txtTotalPages = txtIndexedPages;
    removeLegacyIndexFiles();
    File doneIndex = readerFs().open(txtIndexPath.c_str(), "r");
    File doneChapter = readerFs().open(txtChapterPath.c_str(), "r");
    uint32_t doneIndexSize = doneIndex ? doneIndex.size() : 0;
    uint32_t doneChapterSize = doneChapter ? doneChapter.size() : 0;
    if (doneIndex) doneIndex.close();
    if (doneChapter) doneChapter.close();
    debugFmt("IDX async done pages=%lu chapters=%lu indexSize=%lu z1Size=%lu", (unsigned long)txtTotalPages,
             (unsigned long)txtChapterCount, (unsigned long)doneIndexSize,
             (unsigned long)doneChapterSize);
    traceFmt("IDX_KEYSTATS maxGapMs=%lu yields=%lu", (unsigned long)gIndexStepMaxGapMs, (unsigned long)gIndexStepKeyYield);
    // 主页正显示该书且构建刚在后台完成: 刷新最近阅读(页码回归真实总页数、去掉"构建中"标注), 局刷主卡
    if (appMode == APP_HOME && recentReadPath == txtPath) {
        loadRecentReadSummary();
        renderHome(false);
    }
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
    File f = readerFs().open(txtChapterPath.c_str(), "r");
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
    File last = readerFs().open(txtIndexPath.c_str(), "r");
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
    File sz = readerFs().open(txtIndexPath.c_str(), "r");
    if (sz) { pagesDone = sz.size() / 8; sz.close(); }
    if (pagesDone < 2) pagesDone = 2;
    txtChapterCount = countTxtChapters();   // 已有章节数(在创建句柄前统计, 避免双句柄)
    seedChapterResumeBoundary(pagesDone);   // 重扫页的章节跳过边界
    txtIndexScanFile = readerFs().open(txtPath.c_str(), "r");
    txtIndexBuildFile = readerFs().open(txtIndexPath.c_str(), "a");      // 追加, 不截断
    txtChapterBuildFile = readerFs().open(txtChapterPath.c_str(), "a");
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
    // ===== 构建期间保持可交互 (对齐官方 A7: 构建中翻页/菜单/导航都能用, 只有"跳到未建页"被阻止) =====
    // 原实现每 loop 构建至多 100ms → 这 100ms 内不扫键, 短按可能整段落在两次扫描之间被丢掉
    // ("构建索引期间按键灵敏度降低")。现: 每 loop 至多 10ms, 且每 2ms 让步检测按键, 按下立即返回。
    uint32_t deadline = millis() + 10;
    uint32_t lastKeyChk = millis();
    while ((int32_t)(deadline - millis()) > 0) {
        ESP.wdtFeed();
        if (millis() - lastKeyChk >= 2) {
            lastKeyChk = millis();
            if (readKey2() == 0 || readKey3() == 0) {   // 按键按下 → 立刻把控制权交回主循环
                gIndexStepKeyYield++;
                break;
            }
        }
        if (indexLineOld != indexLine) { indexLineOld = indexLine; indexHskgState = true; }
        if (indexPageStartPending && indexLine == 0) {
            indexPageStartPending = false;
            appendIndexRecord(txtIndexBuildFile, indexScanPos);   // 真实字节偏移 (勿用 position(): 块读时是 2048 对齐)
            if ((txtIndexedPages & 7) == 0) txtIndexBuildFile.flush();   // 每 8 页落盘(掉电最多丢 8 页重建), 避免每页 flush 拖慢构建
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
// 页首偏移读取 (带总线恢复重试 + 严格校验)。返回 false 表示**读失败**, 调用方不得把 0 当第 1 页翻页。
// 修复: 原实现 SD.open 偶发失败即返回 0 → nextTxtPage 页码+1 却渲染第 1 页, 并把进度写成 0
// (实测: 1024 页按右键显示第 1 页, 再按恢复 1026)。
// 官方对照 (见 REVERSE_NOTES §13): V14 DisplayTxt.ino:50-52/333-336/369-378 把 .i/.txt 放 **LittleFS**,
// 翻页路径**不判空、无重试、无 0 校验**; SD 失效恢复原语 = SdInit.ino:11-29 sdBeginCheck()(SDFS.end+SD.begin, 关WDT)。
// 我们的书与索引在 SD 且逐页 open → 该失败面必须自己兜(失败重试 + reinitSdBus, 失败不递增页码)。
static bool parsePageRecordEx(uint32_t page, uint32_t *out) {
    if (!out) return false;
    if (page <= 1) { *out = 0; return true; }
    String path = txtIndexPath;
    if (!readerFs().exists(path.c_str())) {
        String legacy = txtPath + ".i1";
        if (readerFs().exists(legacy.c_str())) path = legacy;
    }
    for (uint8_t attempt = 0; attempt < 2; attempt++) {
        File f = readerFs().open(path.c_str(), "r");
        if (!f) {
            if (attempt == 0) { readerBusReady("page_rec_retry"); continue; }
            traceFmtLevel('E', "PAGE_REC_OPEN_FAIL page=%lu path=%s", (unsigned long)page, path.c_str());
            return false;
        }
        if (!f.seek((page - 1) * 8)) {
            f.close();
            traceFmtLevel('E', "PAGE_REC_SEEK_FAIL page=%lu", (unsigned long)page);
            return false;
        }
        char rec[9];
        int got = (int)f.read((uint8_t *)rec, 8);
        f.close();
        if (got != 8) {
            traceFmtLevel('E', "PAGE_REC_SHORT page=%lu got=%d", (unsigned long)page, got);
            return false;
        }
        rec[8] = '\0';
        for (uint8_t i = 0; i < 8; i++) {
            if (rec[i] < '0' || rec[i] > '9') {
                traceFmtLevel('E', "PAGE_REC_BAD page=%lu rec=%s", (unsigned long)page, rec);
                return false;
            }
        }
        uint32_t v = strtoul(rec, nullptr, 10);
        if (v == 0) {   // 页 >1 的页首偏移不可能为 0 (0 只属第 1 页)
            traceFmtLevel('E', "PAGE_REC_ZERO page=%lu", (unsigned long)page);
            return false;
        }
        *out = v;
        if (attempt > 0) traceFmt("PAGE_REC_RETRY_OK page=%lu off=%lu", (unsigned long)page, (unsigned long)v);
        return true;
    }
    return false;
}

uint32_t parsePageRecord(uint32_t page) {
    uint32_t v = 0;
    return parsePageRecordEx(page, &v) ? v : 0;
}

// ---- P6 (2026-09): 进度节流 ----
// 目标: 不再"每翻一页写 Flash"。RAM 保存实时 offset, 满足任一条件才落盘:
//   ① 每 50 页  ② 每 5 分钟  ③ 退出阅读  ④ 换书/休眠
// 断电最多丢 ≤50 页或 ≤5 分钟进度 (验收 D 允许)。构建中同样节流(sidecar)。
static bool progressWriteToDisk(uint32_t offset) {
    // 防御: 页 >1 的进度偏移不可能为 0 (0=第 1 页)。写入 0 会把"当前阅读位置"打回开头
    // (原 parsePageRecord 失败返回 0 时曾把进度写成 0)。
    if (offset == 0 && txtPage > 1) {
        traceFmtLevel('E', "PROGRESS_ZERO_SKIP page=%lu", (unsigned long)txtPage);
        return false;
    }
    if (txtIndexBuilding) {
        // 构建中进度 → 独立 sidecar (txtIndexPath+"p", 如 小说.i1p): 不与 .i1 追加句柄
        // 形成同文件双句柄 (实测双句柄写 .i1 有概率破坏 FAT 引发 SD 卸载)。
        // 完成时 finishTxtIndexBuild 合并回记录[0] 并删除 sidecar;
        // 构建被中断 (掉电/休眠/重启) 时 startTxtReader 优先读 sidecar 恢复阅读位置。
        String sidecar = txtIndexPath + "p";
        File f = readerFs().open(sidecar.c_str(), "r+");
        if (!f) f = readerFs().open(sidecar.c_str(), "w");   // 首次写入: 新建
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
    if (!readerFs().exists(progressPath.c_str())) {
        String legacyPath = txtPath + ".i1";
        if (readerFs().exists(legacyPath.c_str())) progressPath = legacyPath;
    }
    // 打开失败重试: 电量采样/EPD 刷新可能动过共享 GPIO, 先恢复 SD 总线再试 (实测偶发打开失败)
    File f;
    bool opened = false;
    for (int attempt = 0; attempt < 2 && !opened; attempt++) {
        f = readerFs().open(progressPath.c_str(), "r+");
        if (f) { opened = true; break; }
        readerBusReady("progress_retry");
        f = readerFs().open(progressPath.c_str(), "r+");
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

// 立即落盘待写进度 (退出/换书/休眠调用); 无待写内容则直接成功
bool progressFlushForce(const char *reason) {
    if (!gProgDirty) return true;
    uint32_t off = gProgPending;
    bool ok = progressWriteToDisk(off);
    if (ok) {
        gProgDirty = false;
        gProgPagesSince = 0;
        gProgLastFlushMs = millis();
        traceFmt("PROG_FLUSH reason=%s off=%lu", reason ? reason : "?", (unsigned long)off);
    } else {
        traceFmtLevel('E', "PROG_FLUSH_FAIL reason=%s off=%lu", reason ? reason : "?", (unsigned long)off);
    }
    return ok;
}

// 每圈调用: 满足"50 页 / 5 分钟"任一条件即落盘
void progressTick() {
    if (!gProgDirty) return;
    if (gProgPagesSince >= 50 || (millis() - gProgLastFlushMs) >= 300000UL) {
        progressFlushForce("auto");
    }
}

// 对外 API: 只登记待写(RAM), 到达阈值才真正写 Flash
bool writeProgress(uint32_t offset) {
    // 防御: 页 >1 的进度偏移不可能为 0 (0=第 1 页)。写入 0 会把"当前阅读位置"打回开头
    if (offset == 0 && txtPage > 1) {
        traceFmtLevel('E', "PROGRESS_ZERO_SKIP page=%lu", (unsigned long)txtPage);
        return false;
    }
    gProgPending = offset;
    gProgDirty = true;
    gProgPendingBuilding = txtIndexBuilding;
    gProgPagesSince++;
    if (gProgLastFlushMs == 0) gProgLastFlushMs = millis();
    if (gProgPagesSince >= 50 || (millis() - gProgLastFlushMs) >= 300000UL) {
        return progressFlushForce("threshold");
    }
    return true;   // 已登记待写
}

// 读取当前阅读字节偏移 (.i1 记录[0]); 构建中/构建中断优先读 sidecar (txtIndexPath+"p")。
// 只读索引前 8 字节, 绝不重扫索引/TXT。
uint32_t readProgressOffset() {
    uint32_t off = 0;
    if (txtIndexBuilding) {
        String sidecar = txtIndexPath + "p";
        File sp = readerFs().open(sidecar.c_str(), "r");
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
    if (!readerFs().exists(path.c_str())) {
        String legacy = txtPath + ".i1";
        if (readerFs().exists(legacy.c_str())) path = legacy;
    }
    // 偶发 SD 打开失败会误报 0 (实测同步 BEGIN 读出 0 而重启后正常): 重试 3 次 + 失败日志
    bool opened = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        File f = readerFs().open(path.c_str(), "r");
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
    File f = readerFs().open(txtIndexPath.c_str(), "r");
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

// 读一页正文到 txtLines。返回是否实际读到内容 (读到内容/页首推进 = true)。
// 失败(TXT_READ_FAIL 或 全空)返回 false, 由外层 readTxtPage 做总线自愈重试。
static bool readTxtPageCore(uint32_t offset) {
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
    // 判定是否实际读到内容: 位置推进 且 至少一行有字 (TXT_READ_FAIL/空读 → false)。
    uint32_t pos = txtFile.position();
    bool content = pos > offset;
    if (content) {
        content = false;
        for (uint8_t i = 0; i < txtLineCount(); i++)
            if (txtLines[i].length()) { content = true; break; }
    }
    return content;
}

// 翻页读页: 优先尝试; 若失败(SD 总线空闲后损坏/电量采样竞争等, 偶发) →
// 自愈一次: 恢复 SD 总线 + 重开 txtFile + 重读。仍失败留空行由调用方拦截(不渲染白屏)。
// 返回 true = 本页内容已就绪 (txtLines 有效); false = 读取失败 (txtLines 空, 调用方不得渲染)。
bool readTxtPage(uint32_t offset) {
    if (readTxtPageCore(offset)) return true;
    traceFmtLevel('W', "PAGE_READ_RETRY page=%lu offset=%lu", (unsigned long)txtPage, (unsigned long)offset);
    if (readerBusReady("page_retry")) {
        if (txtFile) txtFile.close();
        txtFile = readerFs().open(txtPath.c_str(), "r");
        if (txtFile && readTxtPageCore(offset)) return true;
    }
    traceFmtLevel('E', "PAGE_READ_FAIL page=%lu offset=%lu", (unsigned long)txtPage, (unsigned long)offset);
    return false;
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
    uint32_t off = 0;
    if (!parsePageRecordEx(jumpPage, &off)) {   // 页首偏移读失败: 不静默当第 1 页
        showMsg("跳转失败", "读取失败");
        return;
    }
    if (!readTxtPage(off)) {
        showMsg("读取失败", "请重试");
        return;
    }
    txtPage = jumpPage;
    txtPageStart = off;
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
    // 空页守卫(白屏最终兜底): 读页失败(TXT_READ_FAIL/全空)时 displayLines 全空,
    // 若直接 drawTxtPageLines 会先全白清屏 → 白屏。拒绝渲染, 保持当前面板画面。
    {
        bool any = false;
        for (uint8_t i = 0; i < txtLineCount(); i++)
            if (displayLines[i].length()) { any = true; break; }
        if (!any) {
            traceFmtLevel('W', "RENDER_SKIP_BLANK page=%lu", (unsigned long)txtPage);
            return;
        }
    }
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

// 深睡唤醒清理: 面板物理保留休眠前画面 (正文 + 右上角"休眠中"), 免刷恢复只画 fb 不写屏,
// "休眠中"会残留 → 这里局刷一次把纯净正文送上屏覆盖。仅深睡唤醒路径调用一次。
extern bool gBootFromSleep;   // 定义在休眠记录区 (saveSleepRecord 附近), 前向声明供此处使用
void clearSleepNoticeAfterBoot() {
    if (!gBootFromSleep) return;
    gBootFromSleep = false;
    traceLine("BOOT_CLEAR_SLEEP_NOTICE");
    refresh(false);   // 局刷: 只更新变化像素, 擦掉右上角"休眠中"
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

    // ---- 菜单项按钮 (含当前值): 流式排布, 12px 小字 + 小按钮, 容纳 12 项 ----
    // 12 项: 字体选择/退出/自动翻页/全刷间隔：/旋转/跳转/章节/标签/休眠/进度同步/配网/重建索引
    // (12px 中文字库 u8g2_font_wqy12_t_gb2312: 修复 10 项时 i<9 漏渲染末项的幽灵选项问题,
    //  且缩小字体/按钮面积使 12 项在横竖屏面板内放得下)
    static const char *const itemNames[12] = {"字体选择", "退出", "自动翻页", "全刷间隔：", "旋转", "跳转", "章节", "标签", "休眠", "进度同步", "配网", "重建索引"};
    char itemText[12][24];
    for (int i = 0; i < 12; i++) {
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
    const int btnH = 15, btnGap = 3, btnPad = 5;
    int x = MX + 4, y = MY + 4;
    int maxY = y;
    for (int i = 0; i < 12; i++) {
        int bw = utf8Width(itemText[i]) + btnPad * 2;
        if (x + bw > MX + MW - 4) { x = MX + 4; y += btnH + btnGap; }
        bool sel = readerMenuSel == i;
        fillRect(x, y, bw, btnH, false);   // 恒白底 (官方 A7 光标: 选中=空心框)
        drawRect(x, y, bw, btnH, false);   // 默认不画框 (未选中无框)
        // 12px 字体基线 ≈ y+13 (wqy12 字高 15px, ascent 11): 文字下移避免与按钮上边框重叠
        // (16px 的 drawTextUTF8 用 +13; 12px 实测 +11 顶到上边框, 用户反馈重叠)
        u8g2Fonts.setForegroundColor(1);   // 恒黑字 (选中不再反色白字)
        u8g2Fonts.setCursor(x + btnPad, y + 13);
        u8g2Fonts.print(itemText[i]);
        if (sel) drawRect(x, y, bw, btnH, true);   // 选中画空心框
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
        char pr[12];
        formatProgressPercent((uint64_t)txtPage, (uint64_t)txtTotalPages, pr);
        snprintf(line2, sizeof(line2), "%s %lu/%lu页", pr,
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
        fillRect(MX + 6, top + i * step, MW - 12, rowH, false);   // 恒白底 (空心框样式)
        drawTextUTF8(MX + 12, top + i * step + 3, opts[i], MW - 36, true);   // 恒黑字
        if (rotSelTable[i] == readerRot) {   // 当前方向: 行右缘实心方块
            fillRect(MX + MW - 17, top + i * step + 7, 6, 6, true);
        }
        if (sel) drawRect(MX + 6, top + i * step, MW - 12, rowH, true);   // 选中画空心框
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
        fillRect(MX + 6, top + i * step, MW - 12, rowH, false);   // 恒白底 (空心框样式)
        drawTextUTF8(MX + 14, top + i * step + 4, opts[i], MW - 32, true);   // 恒黑字
        if (sel) drawRect(MX + 6, top + i * step, MW - 12, rowH, true);   // 选中画空心框
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
        case 11: {  // 重建索引: 对齐文件管理器"重建" — 删除当前使用的索引 → 回第一页 → 后台重建
            if (txtPath.length() == 0 || txtIndexPath.length() == 0) {
                snprintf(readerMenuNote, sizeof(readerMenuNote), "无索引可重建");
                renderReaderMenu();
                break;
            }
            traceFmt("REBUILD_READER path=%s", txtPath.c_str());
            readerMenuOpen = false;
            // 关键: 重建会 SD.remove 当前 .i1/.z1(+p), 必须先在阅读器内关掉旧句柄
            // (txtFile/txtIndexScanFile 等; 关 txtFile 后 startTxtReader 会重开)。
            if (txtIndexScanFile) txtIndexScanFile.close();
            if (txtIndexBuildFile) txtIndexBuildFile.close();
            if (txtChapterBuildFile) txtChapterBuildFile.close();
            if (txtFile) txtFile.close();
            txtIndexBuilding = false;
            // startTxtReader(forceRebuild=true): 内部 SD.remove 索引/章节/sidecar
            // → txtPage=1 → 渲染第一页 → beginTxtIndexBuild() 后台全量重建 (indexTaskStep 继续喂)。
            startTxtReader(txtPath.c_str(), true);
            break;
        }
    }
}

// ---------- 进度同步：宿主钩子 (progress_sync.cpp 回调) ----------
// 本地进度读取/应用/UI/结束收尾, 由本文件访问全局阅读器状态; progress_sync.cpp 不碰 .i1。

bool progressSyncSnapshot(const String& txtPath, uint32_t& localOffset, uint32_t& txtSize, float& localPercent) {
    if (!txtFile) {
        readerBusReady("sync_snap");   // 网络阶段 GPIO12/GPIO5 可能被电量采样动过, 先恢复 SD 总线
        txtFile = readerFs().open(txtPath.c_str(), "r");
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
        readerBusReady("sync_restore");
        txtFile = readerFs().open(txtPath.c_str(), "r");
    }
    if (txtFile && txtPageStart <= txtFile.size()) readTxtPage(txtPageStart);
}

bool progressSyncApplyRemote(uint32_t offset) {
    readerBusReady("sync_apply");   // 网络阶段 GPIO12/GPIO5 可能被电量采样动过, 先恢复 SD 总线
    if (!txtFile) txtFile = readerFs().open(txtPath.c_str(), "r");   // 网络阶段 Free hook 可能已关闭, 重开
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
        uint32_t ps = 0;
        if (page > 1) {
            if (!parsePageRecordEx(page, &ps)) {
                // 索引记录读失败: 保留手机 offset 直读(不静默当第 1 页)
                traceFmtLevel('W', "SYNC_RECFAIL page=%lu off=%lu", (unsigned long)page, (unsigned long)offset);
                ps = offset;
            }
        }
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
    uint8_t  fromSleep;           // 1=本次保存来自 enterSleepMode(深睡), 唤醒需擦右上角"休眠中"残留
};
const uint32_t SLEEP_RECORD_MAGIC = 0x55495354UL;   // 'UIST' 新版界面快照
bool gSleepRecordFromSleep = false;   // saveSleepRecord 置位前设: 记录本次保存是否来自深睡路径
bool gBootFromSleep = false;          // setup 读到 sleep record.fromSleep=1: 深睡唤醒, 恢复后需擦"休眠中"残留

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
    rec.fromSleep = gSleepRecordFromSleep ? 1 : 0;
    gSleepRecordFromSleep = false;   // 一次性标志: 读走即清, 防误传
    currentPath.toCharArray(rec.path, sizeof(rec.path));
    // 快照写入内部 LittleFS(阅读状态归 LittleFS): 不再需要 SD 总线恢复(P4 要求阅读期间零 SD 访问)

    // FILE_WRITE 在当前 SD 库中可能是追加模式；先删除旧快照，避免读取到旧记录。
    bool removed = readerFs().exists(SLEEP_RECORD_PATH) ? readerFs().remove(SLEEP_RECORD_PATH) : true;
    debugFmt("UI_SAVE_PRE exists=%d removed=%d", readerFs().exists(SLEEP_RECORD_PATH) ? 1 : 0, removed ? 1 : 0);
    File f = readerFs().open(SLEEP_RECORD_PATH, "w");
    if (!f) {
        debugLine("SLEEP_SAVE open-fail");
        return;
    }
    size_t wrote = f.write((const uint8_t *)&rec, sizeof(rec));
    f.flush();
    uint32_t pos = f.position();
    f.close();
    // 写后验证: 文件存在且大小正确
    File vf = readerFs().open(SLEEP_RECORD_PATH, "r");
    uint32_t vsize = vf ? vf.size() : 0;
    if (vf) vf.close();
    debugFmt("SLEEP_SAVE mode=%d speed=%u popup=%u sel=%u page=%d chSel=%d selIdx=%d top=%d path=%s exist=%d size=%u wrote=%u pos=%lu",
             rec.mode, rec.chapterSpeed, rec.chapterSpeedPopup, rec.chapterSpeedSel,
             (int)rec.chapterPage, (int)rec.chapterSel,
             (int)rec.selIndex, (int)rec.topIndex, rec.path,
             readerFs().exists(SLEEP_RECORD_PATH) ? 1 : 0, (unsigned)vsize,
             (unsigned)wrote, (unsigned long)pos);
}

// 读取并清除休眠记录; 返回 true 且填充 rec 表示有有效记录。
bool readSleepRecord(SleepRecord &rec) {
    if (!readerFs().exists(SLEEP_RECORD_PATH)) {
        debugLine("SLEEP_READ no-file");
        return false;
    }
    File f = readerFs().open(SLEEP_RECORD_PATH, "r");
    if (!f) {
        debugLine("SLEEP_READ open-fail");
        return false;
    }
    size_t got = f.read((uint8_t *)&rec, sizeof(rec));
    f.close();
    readerFs().remove(SLEEP_RECORD_PATH);   // 一次性: 读完即删, 避免下次误用
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
    progressFlushForce("sleep");   // P6: 休眠前落盘待写进度
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

    // 记录休眠前的界面模式到内部 LittleFS (断电保留), KEY1 唤醒后恢复同一界面。
    // P4: 阅读期间零 SD 访问 — 睡眠快照已迁 LittleFS, 不再 reinit SD 总线。
    redrawCurrentPage();          // 立即关菜单画面, 重绘纯正文/当前页
    drawSleepNotice();            // 右上角"休眠中" (局刷)
    gSleepRecordFromSleep = true; // 标记本次为深睡: 唤醒后需局刷擦掉"休眠中"残留
    saveSleepRecord();
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
#if SERIAL_REMOTE
    // 远程控制专用版: 低压深睡同样只能 KEY1 硬件复位唤醒, 摸不到设备时睡着不可逆。
    // 远程调试多接 USB 供电, 电压检测常误判 → 只记日志不深睡, 保住串口远程通道。
    traceFmt("LOWBAT_SKIP remote mode v1=%d v2=%d", v1, v2);
    return false;
#else
    enterLowBatterySleep();
    return true;
#endif
}

void nextTxtPage() {
    traceFmt("PAGE_NEXT from=%lu", (unsigned long)txtPage);
    if (txtIndexBuilding && txtPage >= txtIndexedPages) {
        showMsg("索引构建中", "请稍候再翻页");
        return;
    }
    if (txtPage >= txtTotalPages) return;
    uint32_t prevPage = txtPage, prevStart = txtPageStart;
    uint32_t off = 0;
    // 关键: 先取下一页页首偏移, 失败就**不递增页码**(原实现把 0 当第 1 页渲染并写坏进度)
    if (!parsePageRecordEx(txtPage + 1, &off)) {
        showMsg("读取失败", "请重试");
        traceFmtLevel('E', "PAGE_NEXT_RECFAIL page=%lu", (unsigned long)(txtPage + 1));
        return;
    }
    txtPage = txtPage + 1;
    txtPageStart = off;
    if (!readTxtPage(txtPageStart)) {
        // 读失败(偶发 SD 总线坏): 回退页码保持原显示, 不渲染空页(白屏)。下一翻页自愈后继续。
        txtPage = prevPage;
        txtPageStart = prevStart;
        showMsg("读取失败", "请重试");
        traceFmtLevel('E', "PAGE_NEXT_FAIL page=%lu", (unsigned long)txtPage);
        return;
    }
    writeProgress(txtPageStart);
    renderTxtPage(false);
    statsOnPageTurn();   // 阅读统计: 成功显示新页, 计 1 翻页
}

void previousTxtPage() {
    traceFmt("PAGE_PREVIOUS from=%lu", (unsigned long)txtPage);
    if (txtPage <= 1) {
        showMsg("已是第一页", "");
        return;
    }
    uint32_t prevPage = txtPage, prevStart = txtPageStart;
    uint32_t off = 0;
    if (!parsePageRecordEx(txtPage - 1, &off)) {
        showMsg("读取失败", "请重试");
        traceFmtLevel('E', "PAGE_PREV_RECFAIL page=%lu", (unsigned long)(txtPage - 1));
        return;
    }
    txtPage = txtPage - 1;
    txtPageStart = off;
    if (!readTxtPage(txtPageStart)) {
        // 读失败: 回退页码保持原显示, 不渲染空页(白屏)。
        txtPage = prevPage;
        txtPageStart = prevStart;
        showMsg("读取失败", "请重试");
        traceFmtLevel('E', "PAGE_PREV_FAIL page=%lu", (unsigned long)txtPage);
        return;
    }
    writeProgress(txtPageStart);
    renderTxtPage(false);
    statsOnPageTurn();   // 阅读统计: 翻页(上一页也计, 指标=翻页次数)
}

void loadChapterRows(uint32_t offset) {
    // ⚠️ 必须恢复 SD 总线: 翻页前列表/倍速弹窗都是局刷(EPD 侧), 直接 SD.open 会失败
    // → chapterCountLoaded=0 → 列表空 (实测 100x 翻页后列表直接空, 根因同标签系统白屏)。
    if (!readerBusReady("chapter_load")) {
        chapterCountLoaded = 0;
        return;
    }
    chapterCountLoaded = 0;
    chapterTopOffset = offset;
    File f = readerFs().open(txtChapterPath.c_str(), "r");
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
    if (!readerBusReady("chapter_table")) return;
    File f = readerFs().open(txtChapterPath.c_str(), "r");
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
    if (!readerBusReady("chapter_count")) return 0;   // 可能被局刷后调用, 先恢复总线
    File f = readerFs().open(txtChapterPath.c_str(), "r");
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
    if (!readerBusReady("chapter_seek")) return 0;   // 可能被局刷后调用, 先恢复总线
    File f = readerFs().open(txtChapterPath.c_str(), "r");
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
    fillRect(x, 110, w, 18, false);   // 恒白底 (官方 A7 光标: 选中=空心框)
    int tw = utf8Width(label);
    drawTextUTF8(x + (w - tw) / 2, 111, label, tw + 2, true);   // 恒黑字
    if (sel) drawRect(x, 110, w, 18, true);   // 选中画空心框
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
        fillRect(0, y, SCR_W, 15, false);   // 恒白底 (空心框样式)
        int fg = true;   // 恒黑字
        char pageStr[12];
        snprintf(pageStr, sizeof(pageStr), "%lu", (unsigned long)chapterRows[i].page);
        int pageW = utf8Width(pageStr);
        int titleMax = SCR_W - 8 - pageW - 6;
        char disp[56];
        utf8Truncate(chapterRows[i].title, disp, titleMax, sizeof(disp));
        drawTextUTF8(2, y, disp, titleMax, fg);
        drawTextUTF8(SCR_W - 4 - pageW, y, pageStr, pageW + 4, fg);
        if (selected) drawRect(0, y, SCR_W, 15, true);   // 选中画空心框
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
    if (readerFs().exists(sidecarPath.c_str())) readerFs().remove(sidecarPath.c_str());
}

void closeTxtReader() {
    progressFlushForce("close");   // P6: 退出阅读落盘待写进度
    if (txtIndexBuilding) {
        // 构建中退出: 保留构建句柄后台继续 (loop 公共 indexTaskStep 继续喂),
        // 只关 txtFile; finishTxtIndexBuild 用扫描句柄取 size, 不受影响。
        if (txtFile) txtFile.close();
        statsOnSessionEnd();   // 阅读统计: 会话结束
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
    statsOnSessionEnd();   // 阅读统计: 会话结束
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
    if (legacyIndex != txtIndexPath && readerFs().exists(legacyIndex.c_str())) readerFs().remove(legacyIndex.c_str());
    if (legacyChapter != txtChapterPath && readerFs().exists(legacyChapter.c_str())) readerFs().remove(legacyChapter.c_str());
}

// 页表二分查找: 记录[1..N-2] 严格递增 (已校验) → 二分找 offset==saved 的页。
// 替代 14 万条顺序扫描 (1.1MB 顺序读阻塞 2-4s, 旋转/启动恢复时屏幕长时间无变化 = "卡")。
// 返回页号 (≥2) 或 0 (未找到/索引打不开)。
uint32_t findPageByOffset(const String &indexPath, uint32_t saved) {
    File f = readerFs().open(indexPath.c_str(), "r");
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
    File f = readerFs().open(indexPath.c_str(), "r");
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

// ---- P10 最小版: SD → LittleFS 导入（点 SD 上的 TXT 即自动导入后阅读）----
// 规则: >100MB 直接拒绝(官方规则); 需要空间 = 正文 + 索引估算(≈2.07%) + 16KB 余量, 超 LittleFS 可用 → 拒绝。
// 已存在且大小一致 → 直接复用(秒开, 不重复拷贝)。旁带(.i1/.v1/.i1p/.z1/.vz1/.bm)存在则一并复制, 免整本重建。
// 阅读数据源仍是 LittleFS: SD 只作为"导入来源", 不进入阅读实时链路(架构见 docs/reader-lfs-migration.md)。
static bool readerImportFromSd(const char *sdPath, String &outLfsPath) {
    if (!sdPath || !sdPath[0]) { showMsg("导入失败", "路径无效"); return false; }
    String base = String(sdPath);
    int slash = base.lastIndexOf('/');
    if (slash >= 0) base = base.substring(slash + 1);
    if (base.length() == 0) { showMsg("导入失败", "路径无效"); return false; }
    String lfsPath = "/" + base;

    if (!LittleFS.begin()) { showMsg("内部存储", "挂载失败"); return false; }
    if (!activeFsBusReady("import")) { showMsg("SD 读取失败", "请重试"); return false; }
    File src = SD.open(sdPath, "r");
    if (!src) { showMsg("打开失败", "SD 读取失败"); return false; }
    uint32_t srcSize = src.size();
    const uint32_t LIMIT_100MB = 100UL * 1024UL * 1024UL;
    if (srcSize == 0) { src.close(); showMsg("导入失败", "文件为空"); return false; }
    if (srcSize > LIMIT_100MB) { src.close(); showMsg("文件过大", "超过100MB"); return false; }

    File exist = LittleFS.open(lfsPath, "r");
    if (exist && exist.size() == srcSize) {   // 已导入且一致 → 秒开
        exist.close(); src.close();
        traceFmt("IMPORT_SKIP %s size=%lu", lfsPath.c_str(), (unsigned long)srcSize);
        outLfsPath = lfsPath;
        return true;
    }
    if (exist) exist.close();

    FSInfo info;
    LittleFS.info(info);
    uint32_t freeB = (uint32_t)(info.totalBytes - info.usedBytes);
    uint32_t need = srcSize + (uint32_t)(((uint64_t)srcSize * 8ULL) / 386ULL) + 16384UL;
    if (need > freeB) {
        src.close();
        char m1[24], m2[24];
        snprintf(m1, sizeof(m1), "需%luKB", (unsigned long)(need / 1024));
        snprintf(m2, sizeof(m2), "可用%luKB", (unsigned long)(freeB / 1024));
        traceFmtLevel('W', "IMPORT_NOSPACE need=%lu free=%lu size=%lu",
                      (unsigned long)need, (unsigned long)freeB, (unsigned long)srcSize);
        showMsg(m1, m2);
        return false;
    }

    traceFmt("IMPORT_BEGIN %s size=%lu need=%lu free=%lu",
             lfsPath.c_str(), (unsigned long)srcSize, (unsigned long)need, (unsigned long)freeB);
    showMsg("导入中", "0%");
    File dst = LittleFS.open(lfsPath, "w");
    if (!dst) { src.close(); showMsg("导入失败", "创建失败"); return false; }
    static uint8_t buf[1024];   // static: 循环栈仅 4KB, 大缓冲不上栈
    uint32_t done = 0;
    uint8_t lastBucket = 0xFF;
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        if (n == 0) break;
        if (dst.write(buf, n) != n) {
            dst.close(); src.close(); LittleFS.remove(lfsPath);
            traceFmtLevel('E', "IMPORT_WRITE_FAIL done=%lu", (unsigned long)done);
            showMsg("导入失败", "空间不足");
            return false;
        }
        done += n;
        uint8_t pct = (uint8_t)(((uint64_t)done * 100ULL) / srcSize);
        if (pct / 20 != lastBucket / 20) {   // 每 20% 提示一次
            lastBucket = pct;
            char p[12];
            snprintf(p, sizeof(p), "%u%%", (unsigned)pct);
            showMsg("导入中", p);
        }
        ESP.wdtFeed();
    }
    dst.close();
    src.close();
    File chk = LittleFS.open(lfsPath, "r");
    uint32_t got = chk ? chk.size() : 0;
    if (chk) chk.close();
    if (got != srcSize) {
        LittleFS.remove(lfsPath);
        traceFmtLevel('E', "IMPORT_VERIFY_FAIL got=%lu want=%lu", (unsigned long)got, (unsigned long)srcSize);
        showMsg("导入失败", "校验不一致");
        return false;
    }
    traceFmt("IMPORT_OK %s size=%lu", lfsPath.c_str(), (unsigned long)srcSize);

    // 旁带(页表/章节/标签): 存在且内部尚无 → 复制, 免整本重建索引
    {
        const char *sfx[] = {".i1", ".v1", ".i1p", ".z1", ".vz1", ".bm", nullptr};
        for (int i = 0; sfx[i]; i++) {
            String s = String(sdPath) + sfx[i];
            String d = lfsPath + sfx[i];
            if (!SD.exists(s.c_str()) || LittleFS.exists(d.c_str())) continue;
            File a = SD.open(s.c_str(), "r");
            if (!a) continue;
            if (a.size() > 262144UL) { a.close(); continue; }   // 大索引不复制(内部会重建)
            FSInfo fi;
            LittleFS.info(fi);
            if ((uint32_t)(fi.totalBytes - fi.usedBytes) < a.size() + 4096U) { a.close(); continue; }
            File b = LittleFS.open(d.c_str(), "w");
            if (!b) { a.close(); continue; }
            bool ok = true;
            while (a.available()) {
                size_t n = a.read(buf, sizeof(buf));
                if (n == 0) break;
                if (b.write(buf, n) != n) { ok = false; break; }
                ESP.wdtFeed();
            }
            b.close(); a.close();
            if (!ok) LittleFS.remove(d.c_str());
            else traceFmt("IMPORT_SIDECAR %s", d.c_str());
        }
    }
    outLfsPath = lfsPath;
    return true;
}

void startTxtReader(const char *path, bool forceRebuild) {
    // ===== 阅读数据源 = 当前介质 (2026-09 官方同款: fsSetBySdState 媒体跟随) =====
    // SD 启用 → 书/.i1/.z1 都在 SD, 直读 SD(大书无容量问题);
    // 内部介质(sdEnabled=0/无卡) → 走 LittleFS。稳定性修复对两种介质同时生效。
    String localPath = path ? String(path) : String();
    if (gBrowseLocal) {
        // 内部介质: 确保 LittleFS 已挂载(只挂一次, 重复 begin 会各吃 ~1KB 堆)
        static bool lfsReady = false;
        if (!lfsReady) {
            lfsReady = LittleFS.begin();
            if (!lfsReady) {
                traceFmtLevel('E', "LFS_MOUNT_FAIL");
                showMsg("内部存储", "挂载失败");
                return;
            }
            traceFmt("LFS_MOUNT ok");
        }
    } else {
        // SD 介质: 阅读前恢复 SD 总线(EPD 刷新/电池采样与 SD 共用引脚)
        if (!readerBusReady("reader_enter")) {
            traceFmtLevel('E', "READER_SD_BUS_FAIL");
            showMsg("SD 读取失败", "请重试");
            return;
        }
        traceFmt("READER_SRC sd path=%s", localPath.c_str());
    }
    traceFmt("TXT open step=flush_begin");
    progressFlushForce("book_change");   // P6: 换书前把上一本的待写进度落盘
    traceFmt("TXT open step=flush_done");
    // 仅"内部介质阅读"时关闭 SD(省电 + 免 GPIO5/GPIO12 争抢); SD 介质阅读时 SD 就是数据源, 必须在线。
    if (gBrowseLocal) {
        SD.end();
        digitalWrite(5, HIGH);
        pinMode(5, OUTPUT);
        traceFmt("SD_OFF reader_enter");
    }
    traceFmt("TXT open step=path_check");
    if (!isTxtPath(path)) {
        traceFmtLevel('W', "TXT unsupported path=%s", path ? path : "(null)");
        showMsg("不支持打开", "仅支持TXT文件");
        return;
    }
    traceFmt("TXT open step=list_free_begin");
    freeItemList();   // 大目录 items≈34KB+ 是堆大户, 进阅读器前释放 (浏览模式回退时 listDir 重建)
    traceFmt("TXT open step=list_free_done");
    wifiManagerRfOff("reader_enter");   // P5: 进入阅读强制关 RF (官方 DisplayTxt.ino:854 WifiShutdown 对齐)
    traceFmt("TXT open step=rf_off_done");
    debugFmt("TXT open path=%s rebuild=%d", localPath.c_str(), forceRebuild ? 1 : 0);
    statsOnSessionStart(localPath.c_str());   // 阅读统计: 会话开始, 记录当前书 + 今日/本周/连续天数检查
    // 旋转续读: 读走即清零 (任何路径都不残留; 失败提前 return 也安全)
    uint32_t rotateResume = gRotateResumeOffset;
    gRotateResumeOffset = 0;
    uint32_t bootHintPage = gBootHintPage;   // 启动页号提示: 复用最近阅读扫描结果, 跳过重复二分 (优化②)
    gBootHintPage = 0;
    bool bootPartialRestore = gBootPartialRefresh;   // 启动恢复局刷: 仅 setup 设置, 读走即清零 (优化④)
    gBootPartialRefresh = false;
    txtPath = localPath;   // P10: SD 来源已导入 → 阅读一律用 LittleFS 路径
    txtIndexPath = txtPath;
    int txtDot = txtIndexPath.lastIndexOf('.');
    if (txtDot > 0) txtIndexPath = txtIndexPath.substring(0, txtDot);
    // 横竖屏各一份索引和章节 (对齐 A7): 竖屏用 .v1/.vz1, 横屏用 .i1/.z1
    txtIndexPath += readerIsPortrait() ? ".v1" : ".i1";
    txtChapterPath = txtPath;
    if (txtDot > 0) txtChapterPath = txtChapterPath.substring(0, txtDot);
    txtChapterPath += readerIsPortrait() ? ".vz1" : ".z1";
    if (!readerBusReady("txt_open")) {
        debugLine("TXT SD reinit failed");
        showMsg("SD错误", "无法读取TXT");
        return;
    }
    // 兼容改名前已经生成的 `小说.txt.i1/.z1`，避免升级后所有大文件被强制重建。
    String legacyIndexPath = txtPath + (readerIsPortrait() ? ".v1" : ".i1");
    String legacyChapterPath = txtPath + (readerIsPortrait() ? ".vz1" : ".z1");
    if (!readerFs().exists(txtIndexPath.c_str()) && readerFs().exists(legacyIndexPath.c_str())) {
        txtIndexPath = legacyIndexPath;
        debugLine("TXT using legacy index name");
    }
    if (!readerFs().exists(txtChapterPath.c_str()) && readerFs().exists(legacyChapterPath.c_str())) {
        txtChapterPath = legacyChapterPath;
        debugLine("TXT using legacy chapter name");
    }
    txtFile = readerFs().open(txtPath.c_str(), "r");
    if (!txtFile) { debugLine("TXT open failed"); showMsg("打开失败", ""); return; }
    saveRecentReadPath(txtPath);
    recentReadPath = txtPath;
    recentReadValid = true;

    // 先检查索引有效性 (毫秒级, 仅 open+读最后8字节),
    // 有效则直接恢复进度一次全刷; 无效才显示第一页 + 后台建索引。
    // 避免"先闪第一页再全刷到进度页"的两次全刷。
    File index = readerFs().open(txtIndexPath.c_str(), "r");
    File chapters = readerFs().open(txtChapterPath.c_str(), "r");
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
            File part = readerFs().open(txtIndexPath.c_str(), "r");
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
            if (readerFs().exists(sidecarPath.c_str())) {
                File sp = readerFs().open(sidecarPath.c_str(), "r");
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
                File ready = readerFs().open(txtIndexPath.c_str(), "r");
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
                        uint32_t ps = 0;
                        if (parsePageRecordEx(found, &ps)) { txtPage = found; txtPageStart = ps; }
                        else { txtPage = 1; txtPageStart = savedOffset; traceFmtLevel('W', "RESTORE_RECFAIL found=%lu", (unsigned long)found); }
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
            clearSleepNoticeAfterBoot();   // 深睡唤醒: 局刷擦右上角"休眠中"残留
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
        File ready = readerFs().open(txtIndexPath.c_str(), "r");
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
                    uint32_t ps = 0;
                    if (parsePageRecordEx(found, &ps)) { txtPage = found; txtPageStart = ps; }
                    else { txtPage = 1; txtPageStart = saved; traceFmtLevel('W', "RESTORE_RECFAIL2 found=%lu", (unsigned long)found); }
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
            clearSleepNoticeAfterBoot();   // 深睡唤醒: 局刷擦右上角"休眠中"残留
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
    readerBusReady("mark_txt");
    if (txtFile) txtFile.close();
    txtFile = readerFs().open(txtPath.c_str(), "r");
}

String markPath() {
    String p = txtPath;
    int dot = p.lastIndexOf('.');
    if (dot > 0) p = p.substring(0, dot);
    return p + ".bm";
}

uint8_t markCountRead(const String &path) {
    readerBusReady("mark_cnt");
    File f = readerFs().open(path.c_str(), "r");
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
    readerBusReady("mark_append");
    // 损坏自愈: 残缺尾部先重写为完整记录再追加
    File f = readerFs().open(path.c_str(), "r");
    uint32_t size = f ? f.size() : 0;
    if (f) f.close();
    if (size % 8 != 0 && size > 0) {
        uint32_t keep[MARK_MAX];
        uint8_t n = 0;
        f = readerFs().open(path.c_str(), "r");
        if (f) {
            while (n < MARK_MAX) {
                char rec[9];
                if (f.read((uint8_t *)rec, 8) != 8) break;
                rec[8] = '\0';
                keep[n++] = strtoul(rec, nullptr, 10);
            }
            f.close();
        }
        readerFs().remove(path.c_str());
        File wf = readerFs().open(path.c_str(), "w");
        if (!wf) return false;
        for (uint8_t i = 0; i < n; i++) {
            char rec[9];
            formatIndexNumber(keep[i], rec);
            wf.print(rec);
        }
        wf.close();
    }
    f = readerFs().open(path.c_str(), "a");   // 追加: LittleFS 语义必须用 "a"(FILE_WRITE 与 SD 不同, 会截断)
    if (!f) return false;
    f.seek(f.size());   // 定位到尾部追加
    char rec[9];
    formatIndexNumber(off, rec);
    size_t w = f.print(rec);
    f.close();
    return w == 8;
}

void markLoadPage(int page) {
    markCountLoaded = 0;
    String path = markPath();
    readerBusReady("mark_load");
    File f = readerFs().open(path.c_str(), "r");
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
        readerBusReady("mark_del");
        File f = readerFs().open(path.c_str(), "r");
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
    readerBusReady("mark_del");
    String tmp = path + "t";
    File tf = readerFs().open(tmp.c_str(), "w");
    if (tf) {
        bool ok = true;
        for (uint8_t i = 0; i < n && ok; i++) {
            char rec[9];
            formatIndexNumber(buf[i], rec);
            ok = tf.print(rec) == 8;
        }
        tf.close();
        File vf = readerFs().open(tmp.c_str(), "r");
        uint32_t vsize = vf ? vf.size() : 0;
        if (vf) vf.close();
        if (ok && vsize == (uint32_t)n * 8) {
            readerFs().remove(path.c_str());
            if (readerFs().rename(tmp.c_str(), path.c_str())) return true;
        }
        readerFs().remove(tmp.c_str());
    }

    // 阶段2 兜底: 总线恢复后按 RAM 缓冲直写 .bm + 回读校验
    readerBusReady("mark_del_retry");
    readerFs().remove(path.c_str());
    File df = readerFs().open(path.c_str(), "w");
    if (!df) return false;
    for (uint8_t i = 0; i < n; i++) {
        char rec[9];
        formatIndexNumber(buf[i], rec);
        df.print(rec);
    }
    df.close();
    File vf2 = readerFs().open(path.c_str(), "r");
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
        fillRect(0, y, SCR_W, 15, false);   // 恒白底 (空心框样式)
        int fg = true;   // 恒黑字
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
        if (selected) drawRect(0, y, SCR_W, 15, true);   // 选中画空心框
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
    uint32_t pageOff = 0;
    if (!parsePageRecordEx(page, &pageOff)) {   // 标记页首偏移读失败: 不静默当第 1 页
        showMsg("跳转失败", "读取失败");
        return;
    }
    if (!readTxtPage(pageOff)) {
        showMsg("读取失败", "请重试");
        return;
    }
    txtPage = page;
    txtPageStart = pageOff;
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
    statsInit();   // 阅读行为统计: 读 LittleFS /stats/ (翻页/会话/连续/排行)
    // 注册上传状态回调: file_api_fs 上传(START/END/ABORTED)时调 renderUploadStatus 显示
    // "上传中/上传完毕/上传失败"(墨水屏), 对齐官方 A7 web 上传状态显示。
    ofsUpSetPhaseCallback(renderUploadStatus);
    ofsDlSetPhaseCallback(renderDownloadStatus);   // 下载状态 → 墨水屏底行("下载中/下载完毕/下载失败")
    ofsOpSetPhaseCallback(renderFileOpStatus);     // 新建/删除/重命名/移动 → 墨水屏底行("操作中/成功/失败")
    // 注册 Web 设置修改回调: 配网页每次保存设置成功 → showMsg("修改成功", "选项=值") 局刷提示
    wifiManagerSetWebNotifyCb(webSettingsNotify);

    // 启动阶段不先绘制首页/启动画面: 先完成 SD 初始化, 再统一按 KEY3/最近阅读分流。
    // 这样 KEY1 复位后不会短暂跳首页, 默认只全刷恢复页一次。

    // ── 介质选择: 关闭 SD 启用(显式) → 只本地 flash; 否则尝试挂 SD, 检测不到也回退本地 flash ──
    bool wantSd = (settingsGetSdEnabled() != 0);   // 默认(v2/首次)=1 启用 SD; 0=显式关闭→本地
    // 仅 SD 挂载期间短暂停软 WDT (防挂载超时触发), 挂载后立即恢复
    ESP.wdtDisable();
    bool sdOk = wantSd ? SD.begin(5, SD_SCK_MHZ(20)) : false;
    ESP.wdtEnable(8000);   // 恢复软 WDT, 8s 超时; loop 里 delay(30) 会自动喂狗
    gBrowseLocal = !sdOk;                    // 关闭 SD 或检测不到 → 浏览本地 LittleFS
#if FORCE_LOCAL_MEDIUM_TEST
    gBrowseLocal = true;                     // 仅验收固件: 强制内部介质(免 SD 扫描/保证阅读走 LittleFS)
#endif
    if (sdOk) {
        sdAvailable = true;
        traceOpen();
        traceFmt("BOOT reason=%s info=%s", ESP.getResetReason().c_str(), ESP.getResetInfo().c_str());
        traceFmt("SD_READY cs=5 speed=20MHz");
    }
    debugFmt("SD begin=%d wantSd=%d local=%d", sdOk ? 1 : 0, wantSd ? 1 : 0, gBrowseLocal ? 1 : 0);

    // ---------- BOOT_AP_MODE: 重启默认进配网界面引导 (AP 热点管理页 192.168.4.1) ----------
    // 条件编译开启时, 跳过 KEY3 窗口/最近阅读分流, 直接启动 AP 配网。
    // ⚠️ 必须放在"本地介质提前 return"之前: 否则 sdEnabled=0 / 无卡(本地 LittleFS 介质)时
    //    永远进不了配网页(实测: 强制本地介质后 AP 不出现, 该分支被 5674 行 return 短路)。
    //    本地介质无需 SD 目录缓存(SD 不参与), 直接起 AP。
#if BOOT_AP_MODE
    {
        debugLine("BOOT route=ap-setup (BOOT_AP_MODE)");
        renderBootStage("正在初始化", "请稍候");
        progressSyncFreeReaderHeap();
        freeItemList();
        if (!gBrowseLocal) {
            // SD 介质: 进 AP 前把 SD 目录树扫进 LittleFS 缓存(避开 SD×AP 同时工作的崩溃)
            renderBootStage("正在加载储存卡", "构建索引缓存");
            fsCacheBuild();
        }
        wifiManagerBegin(renderNetworkPage, exitNetworkPage);
        appMode = APP_NETWORK;
        saveSleepRecord();
        // 配网开始后进入 loop (wifiManagerLoop 处理 AP), 不返回启动分流
        return;
    }
#endif

#if READER_AUTOTEST
    // P3/P4 验收: 自动开书 + 连续翻页, 必须在"本地介质提前 return"之前执行
    readerAutotestRun();
    return;
#endif

    if (gBrowseLocal) {
        // 本地 flash 介质: 不挂 SD 也无卡死循环, 直接进首页（跳过 SD 阅读恢复/引导）
        sdAvailable = false;
        appMode = APP_HOME;
        renderHome(true);
        epd.display(fb);
        saveSleepRecord();
        return;
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

    // ---------- BOOT_AP_MODE 分支已前移到"本地介质提前 return"之前 (见上) ----------

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
        File sf = readerFs().open(SLEEP_RECORD_PATH, "r");
        uint32_t ssize = sf ? sf.size() : 0;
        uint32_t smagic = 0;
        if (sf && ssize >= 4) {
            uint8_t m[4];
            sf.read(m, 4);
            smagic = ((uint32_t)m[0]) | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
        }
        if (sf) sf.close();
        debugFmt("BOOT_SLEEPFILE exist=%d size=%u magic=%08lX", readerFs().exists(SLEEP_RECORD_PATH) ? 1 : 0,
                 (unsigned)ssize, (unsigned long)smagic);
    }
    // 只读取睡眠记录
    SleepRecord bootRec;
    bool haveRec = readSleepRecord(bootRec);
    gBootFromSleep = haveRec && bootRec.fromSleep;   // 深睡唤醒: 面板留有右上角"休眠中", 恢复后需局刷擦除

    // 窗口补足: 扫描未覆盖满 1s 时继续轮询, 保证最短检测期
    while (millis() - keyWindow < 1000) {
        if (!gBootKey3Held) {
            if (readKey3() == 0) {
                gBootKey3Held = true;   // 任一 LOW 采样即计按下 (点按也能触发)
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
            gClockDisguiseMode = true;   // 伪装恢复: 强制简洁外观
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
            if (txtFile && readerFs().exists(txtChapterPath.c_str())) {
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

// ===== P3/P4 自动化验收钩子 (仅测试固件; 默认编译不参与) =====
// 编译: -DREADER_AUTOTEST=1 [-DREADER_AUTOTEST_PAGES=1000] [-DREADER_AUTOTEST_BOOK=\"/T1_300k.txt\"]
// 行为: 直接打开 LittleFS 上的测试书 → 连续 nextTxtPage() N 次 → 打印统计后停在阅读页。
// ⚠️ 必须由启动分流在"本地介质提前 return"之前调用(否则本地介质固件永远跑不到)。
#if READER_AUTOTEST
static void readerAutotestRun() {
    // 可选: 先把内部书复制一份到 SD(仅验收用), 用于验证 "SD→LittleFS 自动导入" 成功路径
#if IMPORT_TEST_SEED
    const char *bookPath = "/IMPORT_T2.txt";   // 种子: 从内部复制到 SD 的新名字(强制走导入拷贝路径)
    {
        if (LittleFS.begin()) {
            File a = LittleFS.open("/T1_300k.txt", "r");
            if (a) {
                if (!activeFsBusReady("seed")) { a.close(); }
                else {
                    File b = SD.open(bookPath, "w");
                    if (b) {
                        static uint8_t sbuf[1024];
                        uint32_t n = 0;
                        while (a.available()) {
                            size_t k = a.read(sbuf, sizeof(sbuf));
                            if (k == 0) break;
                            if (b.write(sbuf, k) != k) break;
                            n += k;
                            ESP.wdtFeed();
                        }
                        b.close();
                        Serial.printf_P(PSTR("IMPORT_SEED_OK bytes=%lu\n"), (unsigned long)n);
                    } else {
                        Serial.println(F("IMPORT_SEED_FAIL sd-open"));
                    }
                    a.close();
                }
            } else {
                Serial.println(F("IMPORT_SEED_SKIP no-lfs-file"));
            }
        }
    }
#else
    const char *bookPath = READER_AUTOTEST_BOOK;
#endif
#if IMPORT_TEST_OPEN
    // 仅测试: 种完即打开书并交回主循环 → 索引在真实 loop 中后台构建,
    // 用于测量"构建期按键采样间隔"(KEYLAG / IDX_KEYSTATS)。
    Serial.printf_P(PSTR("IMPORT_TEST_OPEN book=%s\n"), bookPath);
    startTxtReader(bookPath, false);
    return;
#endif
    Serial.printf_P(PSTR("AUTOTEST_BEGIN book=%s pages=%d rf=%d local=%d\n"),
                    bookPath, READER_AUTOTEST_PAGES,
                    (int)WiFi.getMode(), gBrowseLocal ? 1 : 0);
    startTxtReader(bookPath, false);
    delay(300);
    // ⚠️ 索引是"异步后台构建"(loop() 里 indexTaskStep 驱动)。自测阻塞在 setup, 必须自己驱动,
    //    否则 total=1/页码恒为 1, 翻页全部失败(实测 sent=50 ok=0 fail=50 page=1 total=1)。
    {
        uint32_t t0 = millis();
        while (txtIndexBuilding && (millis() - t0) < 300000UL) {
            indexTaskStep();
            ESP.wdtFeed();
            delay(5);
        }
        Serial.printf_P(PSTR("AUTOTEST_BUILD_DONE building=%d pages=%lu elapsed=%lu heap=%u\n"),
                        txtIndexBuilding ? 1 : 0, (unsigned long)txtTotalPages,
                        (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap());
    }
    // 归位到第 1 页, 保证正向验收从书首开始 (续读进度可能停在上次运行结束页)
    jumpPage = 1;
    jumpToPage();
    delay(200);
    Serial.printf_P(PSTR("AUTOTEST_AT_START page=%lu total=%lu\n"),
                    (unsigned long)txtPage, (unsigned long)txtTotalPages);
    uint32_t okPages = 0, fails = 0, lastPage = txtPage;
    uint32_t fwdOk = 0, fwdStop = 0, backOk = 0;
    for (int i = 0; i < READER_AUTOTEST_PAGES; i++) {
        uint32_t before = txtPage;
        nextTxtPage();
        ESP.wdtFeed();
        if (txtPage != before) { okPages++; lastPage = txtPage; fwdOk++; }
        else {
            fails++;
            if (txtTotalPages > 1 && txtPage >= txtTotalPages) { fwdStop++; break; }   // 末页: 正常停止
        }
        if ((i + 1) % 50 == 0) {
            Serial.printf_P(PSTR("AUTOTEST_PROGRESS sent=%d ok=%lu fail=%lu page=%lu total=%lu\n"),
                            i + 1, (unsigned long)okPages, (unsigned long)fails,
                            (unsigned long)txtPage, (unsigned long)txtTotalPages);
        }
        delay(30);
    }
    Serial.printf_P(PSTR("AUTOTEST_FWD_DONE fwdOk=%lu stoppedAtEnd=%lu page=%lu total=%lu\n"),
                    (unsigned long)fwdOk, (unsigned long)fwdStop,
                    (unsigned long)txtPage, (unsigned long)txtTotalPages);
    // 回翻校验: 从末页往回读 N/4 步, 验证 previousTxtPage 路径同样零异常
    {
        uint32_t backTarget = (uint32_t)(READER_AUTOTEST_PAGES / 4);
        for (uint32_t i = 0; i < backTarget; i++) {
            uint32_t before = txtPage;
            previousTxtPage();
            ESP.wdtFeed();
            if (txtPage != before) { okPages++; backOk++; }
            else { fails++; break; }   // 已到首页再往回 = 正常停止
            if ((i + 1) % 50 == 0) {
                Serial.printf_P(PSTR("AUTOTEST_BACK_PROGRESS sent=%lu ok=%lu page=%lu\n"),
                                (unsigned long)(i + 1), (unsigned long)backOk, (unsigned long)txtPage);
            }
            delay(30);
        }
        Serial.printf_P(PSTR("AUTOTEST_BACK_DONE backOk=%lu page=%lu\n"),
                        (unsigned long)backOk, (unsigned long)txtPage);
    }
    Serial.printf_P(PSTR("AUTOTEST_DONE sent=%d ok=%lu fail=%lu page=%lu total=%lu rf=%d\n"),
                    READER_AUTOTEST_PAGES, (unsigned long)okPages, (unsigned long)fails,
                    (unsigned long)lastPage, (unsigned long)txtTotalPages, (int)WiFi.getMode());
    appMode = APP_READER;   // 停留阅读页便于人工复核
}
#endif

void loop() {
    uint32_t loopStarted = millis();
    clockManagerCompTick();   // 时钟手动补偿结算 (内部按分钟闸门, 芯片在场改写芯片秒)
    progressTick();           // P6: 阅读进度节流落盘 (50 页 / 5 分钟)
    statsTick();              // P6b: 阅读统计节流落盘 (50 页 / 5 分钟)
    // 每天 23:30 静默联网校准 (仅空闲界面且非构建/非配网; 官方: 开=失败停机休眠 / 关=不睡次日再试)
    if (!txtIndexBuilding && (appMode == APP_HOME || appMode == APP_CLOCK)) {
        int sc = clockManagerSilentCalTick();
        if (sc == 2) {
            Serial.println(F("SILENT_CAL fail & force -> sleep"));
            enterSleepMode();
            return;
        }
    }
    if (diagLastLoopMs) {
        uint32_t gap = loopStarted - diagLastLoopMs;
        if (gap > diagMaxLoopGap) diagMaxLoopGap = gap;
    }
    diagFlushSd(false);
    // 按键采样间隔探针 (构建期灵敏度诊断): 记录两次扫描最大间隔, >80ms 打点
    {
        static uint32_t lastKeyScanMs = 0;
        uint32_t nowMs = millis();
        if (lastKeyScanMs && txtIndexBuilding) {
            uint32_t gap = nowMs - lastKeyScanMs;
            if (gap > gIndexStepMaxGapMs) gIndexStepMaxGapMs = gap;
            if (gap > 80) traceFmtLevel('W', "KEYLAG gap=%lu", (unsigned long)gap);
        }
        lastKeyScanMs = nowMs;
    }
    int raw2 = readKey2();
    int raw3 = readKey3();
    int r2 = scanKey(k2, raw2 == 0);
    int r3 = scanKey(k3, raw3 == 0);
#if SERIAL_REMOTE
    // 远程控制专用: 消费串口注入的按键事件, 覆盖物理扫描结果 → 走同一 appMode 分发。
    serialRemotePoll();
    if (gInjR2) { r2 = gInjR2; traceFmt("REMOTE_INJ key2=%d", gInjR2); gInjR2 = 0; }
    if (gInjR3) { r3 = gInjR3; traceFmt("REMOTE_INJ key3=%d", gInjR3); gInjR3 = 0; }
#endif
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
        // 诊断探针: 60s 电池采样点 + 当前阅读索引在 LittleFS 上是否可读 (P1 后阅读不再依赖 SD)
        int lfsOk = 1;
        if (appMode == APP_READER && txtIndexPath.length()) {
            lfsOk = readerFs().exists(txtIndexPath.c_str()) ? 1 : 0;
        }
        traceFmt("BATCHK mv=%d page=%lu mode=%d lfs=%d", lastBatteryMV, (unsigned long)txtPage, appMode, lfsOk);
    }

    if (millis() - lastPhysicalKeyMs >= AUTO_SLEEP_MS) {
#if SERIAL_REMOTE
        // 远程控制专用版: 深睡只能靠 KEY1 硬件复位唤醒, 摸不到设备时一旦睡着就再也醒不来,
        // 串口无法唤醒深睡 → 远程模式禁用自动休眠, 保证串口稳定在线。
        (void)0;
#else
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
#endif
    }

    if (r2 == 1) debugLine("KEY middle short");
    else if (r2 == 2) debugLine("KEY middle long");
    if (r3 == 1) debugLine("KEY right short");
    else if (r3 == 2) debugLine("KEY right long");

    // 组合键: 按 KEY2(中) 后 1 秒内按 KEY3(右) → 强制返回首页 (任何界面均可)
    // ⚠️ 需先读两键: KEY2 短按记时刻, KEY3 短按且距上次 KEY2 短按 <=1s → 触发
    static uint32_t lastK2ShortMs = 0;
    bool comboHome = false;
    if (r2 == 1) lastK2ShortMs = millis();
    if (r3 == 1 && lastK2ShortMs && (millis() - lastK2ShortMs) <= 1000) {
        comboHome = true;
        lastK2ShortMs = 0;
    }
    if (comboHome) {
        traceFmt("COMBO_HOME from mode=%d", appMode);
        // 关闭阅读器会话(若有)加统计收尾, 再回首页
        if (appMode == APP_READER) closeTxtReader();
        appMode = APP_HOME;
        renderHome(true);
        saveSleepRecord();
        delay(30);
        return;
    }

    if (appMode == APP_HOME) {
        if (r3 == 1) {
            homeSel = (homeSel + 1) % 7;
            renderHome(false);
        } else if (r2 == 1) {
            homeSel = (homeSel + 6) % 7;
            renderHome(false);
        } else if (r3 == 2) {
            enterHomeCard();
        }
        // 后台索引构建在主页也要喂步进: 否则退出阅读器停在主页时构建停摆,"构建中"永不结束
        if (txtIndexBuilding) indexTaskStep();
        delay(txtIndexBuilding ? 2 : 30);   // 构建期缩短延时: 提高按键采样率(A7 语义: 构建中一切可操作)
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
            wifiManagerRfOff("weather_exit");   // 退出天气页关 RF (P5: 阅读/待机不再继承 RF)
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

    if (appMode == APP_STATS) {
        // 阅读统计页: 中长返回首页 (只读页, 无编辑)
        if (r2 == 2) {
            appMode = APP_HOME;
            renderHome(true);
            saveSleepRecord();
        }
        delay(30);
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
        // P5 兜底断言: 阅读期间绝不允许 RF 处在非 OFF (任何流程漏关都会在此被纠正并留日志)
        if (WiFi.getMode() != WIFI_OFF) wifiManagerRfOff("reader_assert");
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
                readerMenuSel = (readerMenuSel + 1) % 12;
                readerMenuNote[0] = '\0';
                renderReaderMenu();
            } else if (r2 == 1) {
                readerMenuSel = (readerMenuSel + 10) % 12;   // +10 ≡ -2 (mod 12): 上移 2 个
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
        delay(txtIndexBuilding ? 2 : 30);
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
                    // 跳转到章节 (页首偏移读失败不静默当第 1 页)
                    uint32_t off = 0;
                    uint32_t wantPage = chapterRows[chapterSel].page;
                    if (!parsePageRecordEx(wantPage, &off)) {
                        showMsg("跳转失败", "读取失败");
                    } else if (!readTxtPage(off)) {
                        showMsg("读取失败", "请重试");
                    } else {
                        txtPage = wantPage;
                        txtPageStart = off;
                        writeProgress(txtPageStart);
                        appMode = APP_READER;
                        renderTxtPage(true);
                    }
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
        delay(txtIndexBuilding ? 2 : 30);
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
        delay(txtIndexBuilding ? 2 : 30);
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
    delay(txtIndexBuilding ? 2 : 30);
}

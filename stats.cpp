// stats.cpp — 阅读行为统计 V1（见 stats.h）
// 只统计: 翻页次数(pageTurns) / 会话次数(sessions) / 连续天数(streak) / 每书翻页排行。
// 不统计字数（txtFile.size()/3 是整本书总字数, 累计成"今日阅读字数"会失真, V1 不做）。
// 存储: LittleFS /stats/global.dat + /stats/books.dat, magic/version/crc32 校验, .tmp→替换防掉电。

#include "stats.h"
#include <LittleFS.h>
#include <string.h>
#include <time.h>

// 诊断日志 (定义在 ink-reader-esp.ino, 此处前置声明; 无则静默)
extern void traceFmt(const char *fmt, ...);

// ---- 静态状态 ----
static StatsGlobal gGlobal;
// P4 内存审计: 每书统计表(8×108B=864B)改会话级加载——阅读会话开始/统计页打开时从
// books.dat 载入, 会话结束/离开统计页释放; 极端低堆分配失败 → 书内统计跳过(全局仍计)。
static BookStat *gBooks = NULL;
static bool gStatsInited = false;

// 会话状态
static bool gInSession = false;
static int  gSessionBook = -1;   // 当前会话书槽位 index
static char gSessionPath[96] = "";

// ---- CRC32 (RFC 1952, 纯计算) ----
static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static const uint32_t STATS_MAGIC = 0x53544154UL;   // 'STAT'
static const char STATS_DIR[] = "/stats";
static const char STATS_GLOBAL[] = "/stats/global.dat";
static const char STATS_BOOKS[] = "/stats/books.dat";

// ---- 日期 key ----
uint32_t statsDayKey(const struct tm *tm) {
    return (uint32_t)((tm->tm_year + 1900) * 10000L + (tm->tm_mon + 1) * 100L + tm->tm_mday);
}
uint32_t statsWeekKey(const struct tm *tm) {
    // 年*100 + 年内第几周 (tm_yday 0-based, /7)
    return (uint32_t)((tm->tm_year + 1900) * 100L + tm->tm_yday / 7);
}

// ---- LittleFS 读写 ----
static bool ensureMounted() {
    static bool mounted = false;
    if (!mounted) { if (!LittleFS.begin()) { return false; } mounted = true; }
    return true;
}

// 读 payload 文件: 校验 header magic/version/size/crc32; 失败返回 false
static bool readPayload(const char *path, void *payload, size_t payloadSize) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    StatsHeader h;
    if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h)) { f.close(); return false; }
    if (h.magic != STATS_MAGIC || h.version != 1 || h.size != payloadSize) { f.close(); return false; }
    size_t got = f.read((uint8_t*)payload, payloadSize);
    f.close();
    if (got != payloadSize) return false;
    uint32_t crc = crc32_update(0, (const uint8_t*)payload, payloadSize);
    if (crc != h.crc32) return false;
    return true;
}

// 写 payload: .tmp → 替换, 防掉电损坏
static bool writePayload(const char *path, const void *payload, size_t payloadSize) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), PSTR("%s.tmp"), path);
    File f = LittleFS.open(tmp, "w");
    if (!f) return false;
    StatsHeader h;
    h.magic = STATS_MAGIC; h.version = 1; h.size = (uint16_t)payloadSize;
    h.crc32 = crc32_update(0, (const uint8_t*)payload, payloadSize);
    bool ok = (f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h)) &&
              (f.write((const uint8_t*)payload, payloadSize) == payloadSize);
    f.close();
    if (!ok) { LittleFS.remove(tmp); return false; }
    // LittleFS rename = POSIX 语义（原子替换已存在目标）: 直接 rename 消除 remove→rename 之间的
    // 掉电窗口（该窗口丢整个统计文件 → 下次 CRC 失败重置）。仅 rename 失败才走 remove+rename 兜底。
    if (!LittleFS.rename(tmp, path)) {
        LittleFS.remove(path);
        if (!LittleFS.rename(tmp, path)) { LittleFS.remove(tmp); return false; }
    }
    return true;
}

// ---- 每书表加载/释放 (P4) ----
static bool statsEnsureBooks() {
    if (gBooks) return true;
    if (!ensureMounted()) return false;
    gBooks = (BookStat *)malloc(sizeof(BookStat) * MAX_BOOK_STATS);
    if (!gBooks) { traceFmt(PSTR("STATS books alloc fail")); return false; }
    if (!readPayload(STATS_BOOKS, gBooks, sizeof(BookStat) * MAX_BOOK_STATS)) {
        memset(gBooks, 0, sizeof(BookStat) * MAX_BOOK_STATS);
    }
    return true;
}
void statsReleaseBooks() {
    if (!gBooks) return;
    free(gBooks);
    gBooks = NULL;
}

// ---- 重置默认 ----
static void statsReset() {
    memset(&gGlobal, 0, sizeof(gGlobal));
    gGlobal.lastDay = 0; gGlobal.lastWeek = 0;
    gGlobal.streak = 0;
    if (gBooks) memset(gBooks, 0, sizeof(BookStat) * MAX_BOOK_STATS);
}

// ---- 初始化 ----
void statsInit() {
    if (!ensureMounted()) { statsReset(); return; }
    bool okG = readPayload(STATS_GLOBAL, &gGlobal, sizeof(gGlobal));
    if (!okG) { memset(&gGlobal, 0, sizeof(gGlobal)); traceFmt(PSTR("STATS read global fail, reset")); }
    // books.dat 懒加载 (statsEnsureBooks): 会话开始/统计页时才读
    gStatsInited = true;
}

// ---- 每书槽位 ----
static int findBookByPath(const char *path) {
    if (!gBooks) return -1;
    for (int i = 0; i < MAX_BOOK_STATS; i++)
        if (gBooks[i].path[0] && strncmp(gBooks[i].path, path, 95) == 0) return i;
    return -1;
}
static int findEmptySlot() {
    for (int i = 0; i < MAX_BOOK_STATS; i++) if (!gBooks[i].path[0]) return i;
    return -1;
}
static int findLeastRecentSlot() {
    int best = 0;
    for (int i = 1; i < MAX_BOOK_STATS; i++)
        if (gBooks[i].lastReadTime < gBooks[best].lastReadTime) best = i;
    return best;
}
static int ensureBookSlot(const char *path) {
    if (!gBooks) return -1;
    int idx = findBookByPath(path);
    if (idx >= 0) return idx;
    idx = findEmptySlot();
    if (idx < 0) idx = findLeastRecentSlot();
    memset(&gBooks[idx], 0, sizeof(gBooks[idx]));
    strncpy(gBooks[idx].path, path, 95); gBooks[idx].path[95] = '\0';
    return idx;
}

// ---- 日期/连续性检查（RTC 无效则不更新 day/week/streak）----
static bool rtcValid = false;
static uint32_t curDayKey = 0, curWeekKey = 0;

// 采集时调用: 取当前 RTC 时间, 更新今日/本周重置 + streak
static void statsDateCheck(time_t nowT) {
    struct tm *tm = nowT > 1600000000UL ? localtime(&nowT) : nullptr;
    if (!tm) { rtcValid = false; return; }
    // RTC 有效性: 年份 < 2024 认为未校时, 不建连续天数
    if (tm->tm_year + 1900 < 2024) { rtcValid = false; return; }
    rtcValid = true;
    uint32_t dk = statsDayKey(tm), wk = statsWeekKey(tm);
    // 跨天: 今日重置
    if (curDayKey != dk) {
        gGlobal.dayPageTurns = 0; gGlobal.daySessions = 0;
        // 连续天数: 三态
        if (gGlobal.lastDay != 0 && gGlobal.lastDay != 0xFFFFFFFFUL) {
            if (dk == gGlobal.lastDay + 1) gGlobal.streak++;
            else if (dk > gGlobal.lastDay + 1) gGlobal.streak = 1;
            // dk == lastDay 不应发生（curDayKey != dk 已挡）
        } else {
            gGlobal.streak = 1;   // lastDay=0 (首次) → 视为新开始
        }
        gGlobal.lastDay = dk;
        curDayKey = dk;
    }
    // 跨周: 本周重置
    if (curWeekKey != wk) {
        gGlobal.weekPageTurns = 0; gGlobal.weekSessions = 0;
        gGlobal.lastWeek = wk;
        curWeekKey = wk;
    }
}

// ---- 采集接口 ----
void statsOnSessionStart(const char *path) {
    if (!gStatsInited) statsInit();
    if (!path || !path[0]) return;
    time_t nowT = time(nullptr);
    statsDateCheck(nowT);
    gInSession = true;
    strncpy(gSessionPath, path, 95); gSessionPath[95] = '\0';
    // P4: 书表懒加载; 分配失败 → gSessionBook=-1, 翻页只计全局不记书内
    gSessionBook = statsEnsureBooks() ? ensureBookSlot(path) : -1;
    if (gSessionBook >= 0) {
        gBooks[gSessionBook].lastReadTime = (uint32_t)nowT;
        gBooks[gSessionBook].lastReadDay = curDayKey;
    }
}

// ---- P6b (2026-09): 统计落盘节流 ----
// 原实现每翻一页写 /stats/global.dat + /stats/books.dat(各含 .tmp→rename) → 每页 4+ 次 flash 操作。
// 现改为 RAM 累计, 满足 ①每 50 页 ②每 5 分钟 ③会话结束 ④显式 statsSave() 才落盘。
// 硬复位(KEY1)最多丢 ≤50 页计数(验收 D 同口径); 会话计数仍在 statsOnSessionEnd 时写入。
static bool gStatsDirty = false;
static uint16_t gStatsTurnsSince = 0;
static uint32_t gStatsLastSaveMs = 0;

void statsOnPageTurn() {
    if (!gInSession) return;
    if (gBooks && gSessionBook >= 0) gBooks[gSessionBook].pageTurns++;
    gGlobal.totalPageTurns++;
    if (rtcValid) {
        gGlobal.dayPageTurns++;
        gGlobal.weekPageTurns++;
    }
    gStatsDirty = true;
    gStatsTurnsSince++;
    if (gStatsTurnsSince >= 50 || (millis() - gStatsLastSaveMs) >= 300000UL) statsSave();
}

// 主 loop 每圈调用: 5 分钟兜底落盘
void statsTick() {
    if (!gStatsDirty) return;
    if ((millis() - gStatsLastSaveMs) >= 300000UL) statsSave();
}

void statsOnSessionEnd() {
    if (!gInSession) return;
    gInSession = false;
    // sessions 计数（RTC 有效才计日/周；total 恒计）
    gGlobal.totalSessions++;
    if (rtcValid) {
        gGlobal.daySessions++;
        gGlobal.weekSessions++;
    }
    statsSave();
    statsReleaseBooks();   // P4: 会话结束, 书表归还堆 (下次会话/统计页再加载)
}

void statsSave() {
    if (!ensureMounted()) return;
    if (!LittleFS.exists(STATS_DIR)) LittleFS.mkdir(STATS_DIR);
    writePayload(STATS_GLOBAL, &gGlobal, sizeof(gGlobal));
    // P4: 书表未加载(会话从未开始)时不写, 防零表覆盖 books.dat
    if (gBooks) writePayload(STATS_BOOKS, gBooks, sizeof(BookStat) * MAX_BOOK_STATS);
    gStatsDirty = false;
    gStatsTurnsSince = 0;
    gStatsLastSaveMs = millis();
}

const StatsGlobal& statsGetGlobal() { return gGlobal; }
const BookStat* statsGetBooks() { statsEnsureBooks(); return gBooks; }   // P4: 可能返回 NULL (低堆)

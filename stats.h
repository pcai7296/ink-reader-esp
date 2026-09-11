#pragma once
// stats.h — 阅读行为统计 V1（翻页次数 / 会话次数 / 连续天数 / 每书翻页排行）
// V1 只统计 pageTurns / sessions / streak / 每书排行，不统计字数。
// 存储: LittleFS /stats/global.dat + /stats/books.dat（magic/version/crc32 校验, .tmp→替换 防掉电损坏）。
// 阅读器挂钩: startTxtReader/closeTxtReader/next-或prev翻页。不依赖精确计时。

#include <Arduino.h>

// ---- 全局统计（含本地校验字段）----
struct StatsHeader {
    uint32_t magic;       // 'STAT'
    uint16_t version;     // 1
    uint16_t size;        // payload 字节数
    uint32_t crc32;       // payload 的 CRC32
};

struct StatsGlobal {
    uint32_t lastDay;     // 上次统计 dayKey (年*10000+月*100+日), 0=未初始化
    uint32_t lastWeek;    // 上次统计 weekKey (年*100+年内周), 0=未初始化
    uint32_t dayPageTurns;    // 今日翻页数
    uint32_t weekPageTurns;   // 本周翻页数
    uint32_t totalPageTurns;  // 累计翻页数
    uint32_t daySessions;     // 今日会话次数
    uint32_t weekSessions;    // 本周会话次数
    uint32_t totalSessions;   // 累计会话次数
    uint16_t streak;          // 连续阅读天数
};

// ---- 每书统计（固定 8 槽位）----
#define MAX_BOOK_STATS 8
struct BookStat {
    char path[96];        // 书籍路径 (固定数组, 不塞动态 String 到 RAM)
    uint32_t pageTurns;   // 该书累计翻页数
    uint32_t lastReadTime;// 上次阅读时间戳 (RTC epoch)
    uint32_t lastReadDay; // 上次阅读 dayKey
};

// ---- 初始化: 读 LittleFS /stats/ 校验 crc32; 首次/损坏填默认 ----
void statsInit();

// ---- 采集接口 ----
// 进入阅读: 记当前书 path, 检查今日/本周/连续天数重置
void statsOnSessionStart(const char *path);
// 翻页一次 (next/prev): 当前书 pageTurns + 全局 day/week/total 翻页
// (P6b 节流: RAM 累计, 每 50 页或 5 分钟才落盘; 会话结束/显式 statsSave() 立即落盘)
void statsOnPageTurn();
// 主 loop 每圈调用: 5 分钟兜底落盘
void statsTick();
// 退出阅读: sessions +1, 写 books.dat + global.dat
void statsOnSessionEnd();

// ---- 读取接口 ----
const StatsGlobal& statsGetGlobal();
// P4: 每书表懒加载 (会话开始/统计页打开时读 books.dat); 极端低堆时可能返回 NULL, 调用方须判空
const BookStat* statsGetBooks();
// P4: 释放书表 (离开统计页调用; 会话结束内部已自动释放)
void statsReleaseBooks();

// ---- 强制写盘 ----
void statsSave();

// dayKey/weekKey 计算 (供测试/复用)
uint32_t statsDayKey(const struct tm *tm);
uint32_t statsWeekKey(const struct tm *tm);

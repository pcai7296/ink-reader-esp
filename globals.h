#pragma once
// globals.h — 跨模块全局变量统一声明
// 所有 extern 全局变量集中于此, 避免每个 .cpp 各自 extern 导致声明散落/不一致。
// 定义在 ink-reader-esp.ino (唯一定义点), 其他模块通过 #include "globals.h" 访问。

#include <Arduino.h>
#include <SD.h>
#include <SDFS.h>
#include "file_list.h"   // FileItem, winItems, winCount, itemCount, selIndex, topIndex
#include "fb_gfx.h"      // FramebufferGfx 类型 + epd/gfx/u8g2Fonts/textRendererReady extern

// ---------- 帧缓冲 / 屏幕 ----------
extern uint8_t fb[];
extern uint16_t fbRot;
extern bool textRendererReady;

// SCR_W/SCR_H 定义在 fb_gfx.h (296×128 横屏逻辑尺寸)

// ---------- 按键 ----------
#define KEY2_PIN 0
#define KEY3_PIN 3
int readKey2();
int readKey3();

// ---------- 诊断日志 ----------
void traceFmt(const char *fmt, ...);
void traceFmtLevel(char level, const char *fmt, ...);
void traceLine(const char *line);
void traceError(const char *line);
void traceWarn(const char *line);
void debugFmt(const char *fmt, ...);
void debugLine(const char *msg);

// ---------- 帧缓冲绘制原语 ----------
void setPix(int x, int y, bool black);
void fillRect(int x0, int y0, int w, int h, bool black);
void drawRect(int x0, int y0, int w, int h, bool black);

// ---------- U8g2 文本渲染 ----------
int utf8Width(const char *s);
void utf8Truncate(const char *src, char *dst, int maxW, int capacity);
int drawTextUTF8(int x, int y, const char *s, int maxW, bool black);
void drawChar16(int x, int y, char ch, bool black);
int textWidth(const char *s);
void drawText8(int x, int y, const char *s, int maxW, bool black);
void initTextRenderer();

// ---------- 屏幕刷新 ----------
void refresh(bool full);

// ---------- SD 总线管理 ----------
bool reinitSdBus(const char *reason);

// ---------- 文件管理器介质抽象 (2026-09 P3 前置: 管理器跟随介质选择) ----------
// activeFileFs(): 当前介质 (内部 LittleFS / SD SDFS), 由 gBrowseLocal 决定;
// activeFsIsLocal(): true=内部 LittleFS; activeFsBusReady(): SD 时=reinitSdBus, 本地=恒真。
fs::FS &activeFileFs();
bool activeFsIsLocal();
bool activeFsBusReady(const char *reason);

// ---------- 消息显示 ----------
void showMsg(const char *msg, const char *msg2);
void showMiniPrompt(const char *text);

// ---------- 应用模式 ----------
enum AppMode { APP_HOME = 0, APP_BROWSER = 1, APP_READER = 2, APP_CHAPTERS = 3,
               APP_NETWORK = 4, APP_CLOCK_CONNECT = 5, APP_CLOCK = 6,
               APP_WEATHER = 7, APP_SETTINGS = 8, APP_BMP = 9,
               APP_MARKS = 10, APP_CLOCK_DISGUISE = 11 };
extern int appMode;
extern bool sdAvailable;

// ---------- 电池 ----------
extern int lastBatteryMV;
int readBatteryMV();
uint8_t batPercent(int v_mV);
bool isCharging();

// ---------- 文件列表 / 浏览 ----------
extern String currentPath;
void listDir(const char *path);
bool listDir(const char *path);
void loadListWindow(const char *path, int offset);

// ---------- 天气缓存 ----------
extern char wCachedSummary[20];
void loadWeatherCache();
void saveWeatherCache();

// ---------- 最近阅读 ----------
extern String recentReadPath;
extern uint32_t recentReadPage;
extern uint32_t recentReadTotalPages;
extern bool recentReadValid;
void loadRecentReadSummary();
void saveRecentReadPath(const String &path);
void clearRecentReadPathIfMatches(const String &path);
void openRecentRead();

// ---------- 阅读器状态 ----------
extern File txtFile;
extern String txtPath;
extern String txtIndexPath;
extern String txtChapterPath;
extern uint32_t txtPage;
extern uint32_t txtTotalPages;
extern uint32_t txtPageStart;
extern uint32_t txtIndexedPages;
extern bool txtIndexBuilding;
extern uint16_t readerRot;
bool readerIsPortrait();
int txtLineCount();
int txtLineWidth();
uint16_t storedToRot(uint8_t v);
uint8_t rotToStored(uint16_t rot);
const char *rotLabel(uint16_t rot);
void startTxtReader(const char *path, bool forceRebuild);
void closeTxtReader();
void nextTxtPage();
void previousTxtPage();
void renderTxtPage(bool full);
void renderTxtPageNoRefresh();
void writeProgress(uint32_t offset);

// ---------- 阅读器菜单 ----------
extern bool readerMenuOpen;
extern int readerMenuSel;
extern char readerMenuNote[32];
extern bool readerJumpOpen;
extern bool readerRotSelOpen;
extern bool readerSyncOpen;
extern bool readerMarkMenuOpen;
extern uint8_t autoFlipSpeed;
extern uint8_t fixedRefreshEvery;
extern bool fontExternal;

// ---------- 章节 ----------
#define CHAPTER_ROWS 6
struct ChapterRow {
    char title[64];
    uint32_t page;
};
extern ChapterRow chapterRows[];
extern int chapterCountLoaded;
extern int chapterSel;
extern int chapterPage;
extern int chapterTotalPages;
extern uint32_t txtChapterCount;
extern uint8_t chapterSpeed;
extern bool chapterSpeedPopup;
extern int chapterSpeedSel;
uint32_t countTxtChapters();
uint32_t seekChapterOffset(uint32_t chapterIndex);
void enterChapterList();
void renderChapterList(bool full);
void renderChapterSpeedPopup();
void chapterNextPage();
void chapterPrevPage();
void chapterMoveSelection(int delta);

// ---------- 标签 ----------
#define MARK_MAX 50
extern uint8_t markCount;
extern int markPage;
extern int markTotalPages;
extern int markSel;
extern int markCountLoaded;
extern uint32_t markOffsets[];
extern bool markActionOpen;
extern uint8_t markActSel;
void renderMarkList(bool full);
void renderMarkMenuOverlay();
void markMoveSelection(int delta);
bool markAppend(uint32_t off);
void markLoadPage(int page);
bool markDeleteOne(uint8_t idx);
String markPath();
uint8_t markCountRead(const String &path);
void markEnsureTxtFile();
void jumpToMark(uint32_t off);
void enterMarksList();

// ---------- 启动状态 ----------
extern bool gBootPartialRefresh;
extern bool gBootKey3Window;
extern bool gBootKey3Held;
extern uint8_t gBootKey3PollLow;
extern uint32_t gBootHintPage;
extern uint32_t gRotateResumeOffset;
extern int homeSel;
extern int gNetworkReturnMode;
extern char yiyanText[64];

// ---------- 时钟 ----------
void renderClockPage(bool full);
extern time_t lastClockDisplayedMinute;

// ---------- 索引构建 ----------
extern File txtIndexScanFile;
extern File txtIndexBuildFile;
extern File txtChapterBuildFile;
extern String indexRows[];
extern int8_t indexLine;
extern uint16_t indexEnCount;
extern uint16_t indexChCount;
extern uint8_t indexLineOld;
extern bool indexHskgState;
extern uint32_t indexScanPos;
extern uint32_t indexPage;
extern bool indexPageStartPending;
extern uint32_t txtIndexedPages;
void beginTxtIndexBuild();
void beginResumeIndexBuildFromPartial();
void indexTaskStep();
void abortIndexBuild();

// ---------- 界面快照 / 休眠 ----------
void saveSleepRecord();
void enterSleepMode();

// ---------- 天气 ----------
void enterWeatherPage();
void fetchWeatherFlow(bool force);

// ---------- 配网 ----------
void renderNetworkPage(bool full);
void exitNetworkPage();
void renderBootStage(const char *line1, const char *line2);
void renderUploadStatus(int phase, const char *path);

// ---------- 其他页面 ----------
void renderHome(bool full);
void enterHomeCard();
void renderAll();
void renderSettingsPage(bool full);
void settingsHandleKeys(int r2, int r3);
void renderClockConnect(bool full);
void enterClockPage();

// fs_cache.cpp — SD 目录树 → LittleFS 缓存（见 fs_cache.h; 照抄官方"文件管理用 LittleFS"）
#include "fs_cache.h"
#include "sd_file_ops.h"
#include "wifi_manager.h"
#include <LittleFS.h>
#include <ESP8266WebServer.h>
#include <string.h>
#include <stdlib.h>

extern bool reinitSdBus(const char *reason);

// 前置声明（定义在文件后部, fsCacheScanOne 先调用）
static void fillEntryForCache(File &entry, SdEntry *e);

// ---- 显示过滤（与 file_api_fs.cpp ofsBlacklistedEntry/ofsWhitelistedFile 同规则; 本模块独立复制）----
static bool fsHasBlacklistSuffix(const char *dot) {
  static const char *const suffixes[] = {
    ".i1", ".z1", ".i2", ".z2", ".v1", ".vz1", ".i1p", ".v1p", ".bm", ".bmt", nullptr
  };
  for (size_t i = 0; suffixes[i]; i++) if (strcasecmp(dot, suffixes[i]) == 0) return true;
  return false;
}
static bool fsBlacklisted(const char *name) {
  if (!name || !name[0]) return true;
  if (name[0] == '.') return true;
  const char *dot = strrchr(name, '.');
  if (dot && dot[1] && fsHasBlacklistSuffix(dot)) return true;
  static const char *const blocked[] = {
    "android", "androud", "found.000", "foud.000", "lost.dir", "system volume information"
  };
  for (const char *item : blocked) if (strcasecmp(name, item) == 0) return true;
  return false;
}
static bool fsWhitelisted(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot || !dot[1]) return false;
  return strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".bmp") == 0;
}
static void fsJsonEscape(const char *s, char *out, size_t outSize) {
  size_t oi = 0;
  static const char hex[] = "0123456789abcdef";
  for (const char *p = s; *p && oi + 6 < outSize; p++) {
    unsigned char c = (unsigned char)*p;
    switch (c) {
      case '"':  out[oi++] = '\\'; out[oi++] = '"'; break;
      case '\\': out[oi++] = '\\'; out[oi++] = '\\'; break;
      case '\n': out[oi++] = '\\'; out[oi++] = 'n'; break;
      case '\r': out[oi++] = '\\'; out[oi++] = 'r'; break;
      case '\t': out[oi++] = '\\'; out[oi++] = 't'; break;
      default:
        if (c < 0x20) { out[oi++] = '\\'; out[oi++] = 'u'; out[oi++] = '0'; out[oi++] = '0';
                        out[oi++] = hex[(c >> 4) & 0xF]; out[oi++] = hex[c & 0xF]; }
        else out[oi++] = (char)c;
    }
  }
  out[oi] = '\0';
}

// djb2 hash → hex12（路径→缓存名; 碰撞由读回校验首行路径兜底）
void fsCacheNameFor(const char *dir, char *out, size_t outSize) {
  unsigned long h = 5381;
  for (const char *p = dir; *p; p++) h = ((h << 5) + h) + (unsigned char)*p;
  snprintf(out, outSize, "%08lx.txt", h & 0xFFFFFFFFul);
}

// 递归扫描一个目录, 写缓存文件（返回写缓存目录数; 递归累计; 超上限停止）
// ⚠️ 栈占用必须小（ESP8266 主循环栈仅 4KB, 递归 3 层）: 不用 SdEntry(680B), 不用大数组,
// 直接读 Dir::fileName()/fileSize()/isDirectory()（next() 缓存元数据, 不重开文件）, 每层栈 < 600B 才安全
//（否则栈溢出 Exception 9）。
// ⚠️★ 枚举必须用 SDFS.openDir + Dir::next()（官方 A7 同款）——File::openNextFile() 每项会
//    openFile("r") 重开文件（FatFile::open 完整路径解析 + 堆分配）, 大目录(600+) O(n²) + 低堆失败
//    → 枚举 0（实测 /网络小说II raw=0 的根因）; rewindDirectory() 是安慰剂, 已弃用。
static int fsCacheScanOne(const char *dir, int depth, int *dirCount,
                          int *fileCount, char *cachePath, size_t cachePathSize) {
  if (*dirCount >= FS_CACHE_MAX_DIRS || *fileCount >= FS_CACHE_MAX_FILES) return 0;
  if (depth > FS_CACHE_MAX_DEPTH) return 0;
  {
    File probe = SD.open(dir, FILE_READ);
    if (!probe || !probe.isDirectory()) { if (probe) probe.close(); Serial.printf("FS_OPEN dir=%s FAIL\n", dir); return 0; }
    probe.close();
  }
  Dir root = SDFS.openDir(dir);
  Serial.printf("FS_OPEN dir=%s isDir=1 raw(待枚举)\n", dir);

  // 1) 生成缓存文件路径 + 打开
  fsCacheNameFor(dir, cachePath, cachePathSize);
  char full[120];
  snprintf(full, sizeof(full), "%s/%s", FS_CACHE_DIR, cachePath);
  File cache = LittleFS.open(full, "w");
  if (!cache) return 0;
  // 首行 = 原始路径（校验碰撞）
  cache.printf("%s\n", dir);

  // 2) 逐项: 过滤 + 写 JSON 项（不建 SdEntry; Dir::fileName()/fileSize()/isDirectory() 直读）
  int n = 0, raw = 0;
  bool first = true;
  while (root.next()) {
    raw++;   // 原始枚举数（诊断: 区分"目录空" vs "被黑/白名单过滤"）
    if (n >= FS_CACHE_MAX_ITEMS) break;
    String nm = root.fileName();
    bool isDir = root.isDirectory();
    uint64_t sz = isDir ? 0 : (uint64_t)root.fileSize();
    if (fsBlacklisted(nm.c_str())) continue;              // 黑名单不显示（隐藏/索引sidecar/系统目录）
    // ✓ 网页文件管理显示所有用户文件（/字体 的 .ttf、任意扩展名）——只屏蔽黑名单, 不用设备 UI 的
    //    ".txt/.bmp 白名单"（那是屏幕浏览规则, 不适合网页 SD 管理; 否则 /字体/.test 全被过滤成空）
    char nameEsc[160];
    fsJsonEscape(nm.c_str(), nameEsc, sizeof(nameEsc));
    if (first) { cache.printf("["); first = false; } else { cache.printf(","); }
    if (isDir) cache.printf("{\"type\":\"dir\",\"name\":\"%s\"}", nameEsc);
    else       cache.printf("{\"type\":\"file\",\"size\":\"%llu\",\"name\":\"%s\"}",
                            (unsigned long long)sz, nameEsc);
    n++;
    (*fileCount)++;
    if (isDir) {
      // 递归子目录（构造子路径; 栈上短缓冲, 每层 <180B）
      char child[180];
      if (strcmp(dir, "/") == 0) snprintf(child, sizeof(child), "/%s", nm.c_str());
      else                       snprintf(child, sizeof(child), "%s/%s", dir, nm.c_str());
      fsCacheScanOne(child, depth + 1, dirCount, fileCount, cachePath, cachePathSize);
    }
  }
  // 3) 收尾
  if (first) cache.printf("[]");
  else       cache.printf("]");
  cache.close();
  if (raw > 0 && n == 0) Serial.printf("FS_SCAN dir=%s raw=%d kept=0(filtered)\n", dir, raw);
  if (n == 0 && raw == 0) Serial.printf("FS_SCAN dir=%s raw=0(empty)\n", dir);
  (*dirCount)++;
  return 1;
}

// 共享的 fillEntry 逻辑（fsCache 用; 本 TU 内 static）——不再使用（扫描直接读 entry）, 保留声明确保无编译告警
static void fillEntryForCache(File &entry, SdEntry *e) { (void)entry; (void)e; }

// 进 AP 前: 扫描 SD 目录树 → LittleFS 缓存
int fsCacheBuild() {
  // ⚠️ SD 与 EPD 共用 SPI; 进配网前屏幕刚刷新过（SPI 总线在 EPD 侧）→ 必须恢复 SD 总线再扫描,
  // 否则 SD.open 失败 → dirs=1 items=0（扫描空缓存, 浏览读不到 → 仍旧崩）。
  if (!reinitSdBus("fs_cache")) { Serial.println(F("FS_CACHE REINIT_FAIL")); return -1; }
  if (!LittleFS.begin()) {
    Serial.println(F("FS_CACHE LFS_FAIL"));
    return -1;
  }
  if (!LittleFS.exists(FS_CACHE_DIR)) LittleFS.mkdir(FS_CACHE_DIR);
  // 清旧缓存（重建）
  Dir d = LittleFS.openDir(FS_CACHE_DIR);
  while (d.next()) LittleFS.remove(String(FS_CACHE_DIR) + "/" + d.fileName());
  int dirCount = 0, fileCount = 0;
  char cachePath[32];
  fsCacheScanOne("/", 0, &dirCount, &fileCount, cachePath, sizeof(cachePath));
  Serial.printf("FS_CACHE_DONE dirs=%d items=%d\n", dirCount, fileCount);
  // 诊断: dump 根目录缓存文件前 120 字节（确认缓存格式/内容; 若空/畸形即写缓存或解析 bug）
  fsCacheNameFor("/", cachePath, sizeof(cachePath));
  char diag[140];
  snprintf(diag, sizeof(diag), "%s/%s", FS_CACHE_DIR, cachePath);
  File dc = LittleFS.open(diag, "r");
  if (dc) {
    Serial.printf("FS_CACHE_ROOT len=%u data=", (unsigned)dc.size());
    uint8_t db[120];
    int dn = dc.read(db, sizeof(db));
    for (int i = 0; i < dn; i++) Serial.write(db[i]);
    dc.close();
    Serial.println();
  } else {
    Serial.printf("FS_CACHE_ROOT OPEN_FAIL\n");
  }
  return dirCount;
}

// /fs/list: 读 LittleFS 缓存切片（照抄官方 handleFileList 分块流式精神）。
// ⚠️ 不用 malloc(4096)——配网会话堆仅 ~2.4KB, malloc 4KB 必失败 → 缓存永不命中 → 回退 SD → 崩（实测）。
// 改用 256B 小栈缓冲流式解析 JSON 数组, 按 start/count 切片; 全程读 LittleFS（flash）, 不占大堆、不碰 SD。
bool fsCacheServeList(const char *dir, size_t start, size_t count) {
  char name[32], full[300];
  fsCacheNameFor(dir, name, sizeof(name));
  snprintf(full, sizeof(full), "%s/%s", FS_CACHE_DIR, name);
  File c = LittleFS.open(full, "r");
  if (!c) { Serial.printf("FSLIST dir=%s OPEN_FAIL\n", dir); return false; }
  Serial.printf("FSLIST dir=%s len=%u\n", dir, (unsigned)c.size());   // 只打 size, 不位移文件指针

  ESP8266WebServer &s = wifiManagerServer();
  if (!s.chunkedResponseModeStart(200, "text/json")) { c.close(); s.send(505, "text/html", "HTTP1.1 required"); return true; }
  s.sendContent_P(PSTR("{\"items\":["));

  // 流式 JSON 项扫描器: 跳过首行路径(\n 前), 之后逐字节识别 {..} 项, 按 idx/start/count 发送。
  // ⚠️ '}' 检测必须优先于 itemLen 上限——item 满了(长文件名)也必须能 emit(内容截断但不丢项)。
  size_t idx = 0, emitted = 0;
  bool skipPathLine = true;            // 跳过首行（原始路径校验用）
  bool inItem = false;                 // 是否在 {...} 内
  int itemLen = 0;
  char item[400];                      // 单项缓冲（容纳长文件名 JSON; 超限截断但仍 emit）
  uint8_t rb[256];
  bool started = false;                // 是否已发出首个 '{'
  bool hasMore = false;
  while (true) {
    int n = c.read(rb, sizeof(rb));
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      char ch = (char)rb[i];
      if (skipPathLine) { if (ch == '\n') skipPathLine = false; continue; }
      if (inItem) {
        if (ch == '}') {
          // 必须先测 '}'——emit 一个完整项。item 累积到 '}' 前的字符, 这里补上 '}' 再发（否则 JSON 缺闭合 '}' 畸形 → 前端 parse 失败）。
          inItem = false;
          // 确保 item 以 '}' 结尾（item 内容 = { ...name 内容, 末尾补 '}'）
          if (itemLen < (int)sizeof(item) - 1) { item[itemLen++] = '}'; item[itemLen] = '\0'; }
          else { item[sizeof(item) - 2] = '}' ; item[sizeof(item) - 1] = '\0'; }
          if (idx >= start && emitted < count) {
            if (emitted) s.sendContent_P(PSTR(","));
            s.sendContent(item);
            emitted++;
          } else if (idx >= start + count) {
            hasMore = true;
          }
          idx++;
          itemLen = 0;
          (void)started;
        } else if (itemLen < (int)sizeof(item) - 1) {
          item[itemLen++] = ch;   // item 满则丢弃后续字符（内容截断, 但 '}' 仍会补上）
        }
      } else {
        if (ch == '{' && !started) { started = true; itemLen = 0; inItem = true; item[itemLen++] = ch; }
        else if (ch == '{') { itemLen = 0; inItem = true; item[itemLen++] = ch; }
      }
    }
  }
  c.close();
  Serial.printf("FSLIST emitted=%u more=%d\n", (unsigned)emitted, hasMore ? 1 : 0);
  bool more = hasMore || (emitted >= count);  // 已发满且可能还有 → 由前端"加载更多"触发
  s.sendContent_P(PSTR("],\"nextStart\":"));
  char num[16];
  snprintf(num, sizeof(num), "%u", (unsigned)(start + emitted));
  s.sendContent(num);
  s.sendContent_P(PSTR(",\"hasMore\":"));
  s.sendContent_P(more ? PSTR("true") : PSTR("false"));
  s.sendContent_P(PSTR("}"));
  s.chunkedResponseFinalize();
  return true;
}

// 使指定目录缓存失效: 删除 LittleFS 缓存文件（/fslist/<hash>.txt）。
// 上传/删除/重命名后调用, 让下一个 /fs/list 读不到缓存 → 回退读 SD → 显示最新目录。
void fsCacheInvalidateDir(const char *dir) {
  if (!dir || !dir[0]) dir = "/";   // 空 → 根目录（ofsParent 对根一级文件返回空）
  char name[32], full[300];
  fsCacheNameFor(dir, name, sizeof(name));
  snprintf(full, sizeof(full), "%s/%s", FS_CACHE_DIR, name);
  if (LittleFS.exists(full)) LittleFS.remove(full);
  Serial.printf("FSL_CACHE_INVAL dir=%s\n", dir);
}

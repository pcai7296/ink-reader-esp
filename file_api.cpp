// file_api.cpp — 文件管理 HTTP API（统一 API: Web / Android / Legado 都是客户端）
// 契约: docs/file-api.md（S7 落盘）; 错误码枚举见 file_api.h
//
// 约定:
// - 读端点开放, 变更端点必验 X-Admin-Pass（复用 requireAdminAuth）
// - 入参路径 = server.arg() 已 percent 解码 → normalizeApiPath 规范化 → 拒绝即 400
// - 大响应一律 chunked 流式（sendContent_P + (const char*)F() 驻留 flash,
//   配网会话低堆实测教训: 裸字面量进 .rodata 吞 5KB RAM）
// - 一次请求: reinitSdBus 一次 → sd_* 完成整个操作
#include "file_api.h"
#include "file_api_fs.h"
#include "wifi_manager.h"
#include "sd_path.h"
#include "sd_file_ops.h"
#include <LittleFS.h>
#include <ESP8266WebServer.h>

extern bool sdAvailable;   // file_manager.ino 全局: SD 挂载标志（/api/status O(1) 只读此标志）
extern bool reinitSdBus(const char *reason);

static bool lfsReady = false;   // 只挂载/注册一次（配网会话可重复进入）

// ---- 容量（仅缓存值; 绝不在此遍历 SD——配网会话堆硬约束, 遍历 4s+ 且需 >2KB 连续块）----
// 预算审计结论: 手机关联后稳定 heap ~3.3KB / maxblk ~2KB, 任何 handler 内遍历/清理必 OOM。
// /api/status 必须是 O(1): 只读简单变量。容量遍历/清理已从请求路径完全移除。
static uint64_t gCapTotal = 0, gCapUsed = 0;
static bool gCapReady = false;   // 仅由外部（未来状态机/独立端点）填充; 当前无缓存时返回 503

// O(1): 不触发任何 SD 操作。无缓存 → false（调用方回 503 feature_unavailable）
static bool apiGetCachedCapacity(uint64_t *total, uint64_t *used) {
  if (!gCapReady) return false;
  *total = gCapTotal;
  *used = gCapUsed;
  return true;
}

// 供 /fs/*（官方构造复刻）复用: O(1) 读缓存容量, 绝不遍历 SD（配网会话堆硬约束）
bool fileApiGetCachedCapacity(uint64_t *total, uint64_t *used) {
  return apiGetCachedCapacity(total, used);
}

// ---- JSON 工具 ----

// 发送 JSON 错误: {"ok":false,"error":"<枚举>"}
static void sendApiErr(int httpCode, const char *error) {
  String body = F("{\"ok\":false,\"error\":\"");
  body += error;
  body += F("\"}");
  wifiManagerServer().send(httpCode, "application/json; charset=utf-8", body);
}

// JSON 字符串转义（UTF-8 透传; 转义 " \ 与控制字符）
static void jsonStr(const char *s, char *out, size_t outSize) {
  size_t oi = 0;
  for (const char *p = s; *p && oi + 6 < outSize; p++) {
    unsigned char c = (unsigned char)*p;
    switch (c) {
      case '"':  out[oi++] = '\\'; out[oi++] = '"'; break;
      case '\\': out[oi++] = '\\'; out[oi++] = '\\'; break;
      case '\b': out[oi++] = '\\'; out[oi++] = 'b'; break;
      case '\f': out[oi++] = '\\'; out[oi++] = 'f'; break;
      case '\n': out[oi++] = '\\'; out[oi++] = 'n'; break;
      case '\r': out[oi++] = '\\'; out[oi++] = 'r'; break;
      case '\t': out[oi++] = '\\'; out[oi++] = 't'; break;
      default:
        if (c < 0x20) {
          out[oi++] = '\\'; out[oi++] = 'u'; out[oi++] = '0'; out[oi++] = '0';
          static const char hex[] = "0123456789abcdef";
          out[oi++] = hex[(c >> 4) & 0xF]; out[oi++] = hex[c & 0xF];
        } else {
          out[oi++] = (char)c;
        }
    }
  }
  out[oi] = '\0';
}

// ---- 目录列表流式输出上下文 ----
struct ListCtx {
  bool first;     // 是否首项（逗号放置）
  bool sdErr;     // 底层失败
  char *fmtBuf;   // 请求级格式化缓冲（搜索: 一次 malloc 全请求复用, 省每项 3 次 malloc——低堆关键）
};

static void listItemCb(const SdEntry *e, void *ctx) {
  ListCtx *lc = (ListCtx *)ctx;
  if (lc->sdErr) return;
  // 缓冲: 优先用请求级 fmtBuf（搜索预分配, 递归内安全）; 否则每项 malloc
  //（回调在搜索递归内时栈上大缓冲会爆栈 Exception 5, 绝不用栈缓冲）
  // fmtBuf 布局: tmp@0[920] name@920[256] path@1176[460] 总 1636（⚠️ 曾写 1410+错位重叠, 实测越界）
  char *tmp, *name, *path;
  if (lc->fmtBuf) {
    tmp = lc->fmtBuf; name = lc->fmtBuf + 920; path = lc->fmtBuf + 1176;
  } else {
    tmp = (char *)malloc(920);
    name = (char *)malloc(256);
    path = (char *)malloc(460);
    if (!tmp || !name || !path) {   // 低堆: 跳过该项不崩
      free(tmp); free(name); free(path);
      return;
    }
  }
  jsonStr(e->name, name, 256);
  jsonStr(e->path, path, 460);
  snprintf(tmp, 920,
           "%s{\"name\":\"%s\",\"path\":\"%s\",\"type\":\"%s\",\"size\":%llu,\"protected\":%s,\"pending\":%s}",
           lc->first ? "" : ",", name, path, e->isDir ? "dir" : "file",
           (unsigned long long)e->size,
           e->isProtected ? "true" : "false",
           e->isUploading ? "true" : "false");
  lc->first = false;
  wifiManagerServer().sendContent(tmp);
  if (!lc->fmtBuf) { free(tmp); free(name); free(path); }
}

// ---- /api/status: O(1) 健康检查（预算审计后: 只读简单变量, 绝不遍历 SD/清理/预热）----
// 硬指标: 本端点全程 <1KB 连续堆峰值（实测关联后 maxblk 仅 ~2KB, 遍历必 OOM）
static void handleApiStatus() {
  ESP8266WebServer &srv = wifiManagerServer();
  // sdMounted: 复用启动/会话期的 SD 挂载标志（不在此打开 SD 验证）
  char tmp[220];
  snprintf(tmp, sizeof(tmp),
           "{\"ok\":true,\"apiVersion\":1,\"sdMounted\":%s,\"sdTotal\":0,\"sdUsed\":0,\"sdFree\":0,\"apIp\":\"%s\",\"staIp\":\"%s\",\"heap\":%u,\"busy\":false}",
           sdAvailable ? "true" : "false",
           wifiManagerApIp() ? wifiManagerApIp() : "",
           wifiManagerStaIp() ? wifiManagerStaIp() : "",
           (unsigned)ESP.getFreeHeap());
  srv.send(200, "application/json; charset=utf-8", tmp);
}

// ---- /api/capacity: 只读缓存; 无缓存（未预热/资源不足）→ 503 feature_unavailable ----
// 绝不在此遍历 SD（4s+ 且需 >2KB 连续块, 配网会话必 OOM）
static void handleApiCapacity() {
  ESP8266WebServer &srv = wifiManagerServer();
  uint64_t total = 0, used = 0;
  if (!apiGetCachedCapacity(&total, &used)) {
    sendApiErr(503, "feature_unavailable");
    return;
  }
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "{\"ok\":true,\"total\":%llu,\"used\":%llu,\"free\":%llu}",
           (unsigned long long)total, (unsigned long long)used,
           (unsigned long long)(total > used ? total - used : 0));
  srv.send(200, "application/json; charset=utf-8", tmp);
}

// ---- /api/stat: 单文件信息 ----
static void handleApiStat() {
  ESP8266WebServer &srv = wifiManagerServer();
  String pathArg = srv.arg("path");
  if (pathArg.length() == 0) pathArg = "/";
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) {
    sendApiErr(400, "invalid_path");
    return;
  }
  if (!reinitSdBus("api_stat")) {
    sendApiErr(500, "sd_error");
    return;
  }
  SdEntry e;
  if (!sdStat(path, &e)) {
    sendApiErr(404, "not_found");
    return;
  }
  char tmp[600];
  char name[256];
  jsonStr(e.name, name, sizeof(name));
  snprintf(tmp, sizeof(tmp),
           "{\"ok\":true,\"name\":\"%s\",\"type\":\"%s\",\"size\":%llu,\"path\":\"%s\",\"protected\":%s,\"pending\":%s}",
           name, e.isDir ? "dir" : "file", (unsigned long long)e.size, path,
           e.isProtected ? "true" : "false", e.isUploading ? "true" : "false");
  srv.send(200, "application/json; charset=utf-8", tmp);
}

// ---- /api/files: 目录列表（chunked 流式）----
static void handleApiFiles() {
  ESP8266WebServer &srv = wifiManagerServer();
  String pathArg = srv.arg("path");
  if (pathArg.length() == 0) pathArg = "/";
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) {
    sendApiErr(400, "invalid_path");
    return;
  }
  if (!reinitSdBus("api_files")) {
    sendApiErr(500, "sd_error");
    return;
  }
  // 预检目录存在（流式头一旦发出就无法改状态码）
  {
    File probe = SD.open(path, FILE_READ);
    if (!probe || !probe.isDirectory()) {
      if (probe) probe.close();
      sendApiErr(404, "not_found");
      return;
    }
    probe.close();
  }
  bool truncated = false;
  char pathEsc[360];
  jsonStr(path, pathEsc, sizeof(pathEsc));
  srv.sendHeader("Connection", "close");   // 强制断开, 避免 keep-alive 残留连接挂起后续请求
  srv.chunkedResponseModeStart_P(200, PSTR("application/json; charset=utf-8"));
  srv.sendContent_P((const char *)F("{\"ok\":true,\"path\":\""));
  srv.sendContent(pathEsc);
  srv.sendContent_P((const char *)F("\",\"items\":["));
  // 请求级 fmtBuf: 一次分配全请求复用（每项 3 次 malloc × 200 项 = 600 次 malloc/free 是
  // 堆碎片化主因 → WiFi TX 回调 lmacRecycleMPDU 野指针 Exception 28, 实测）
  char *fmtBuf = (char *)malloc(1640);
  ListCtx lc = {true, false, fmtBuf};
  int count = sdListDir(path, listItemCb, &lc, 100, &truncated);   // maxItems 100: 降单请求峰值
  free(fmtBuf);
  srv.sendContent_P((const char *)F("],\"truncated\":"));
  srv.sendContent_P(truncated ? (const char *)F("true") : (const char *)F("false"));
  srv.sendContent_P((const char *)F(",\"count\":"));
  char cnt[16];
  snprintf(cnt, sizeof(cnt), "%d", count);
  srv.sendContent(cnt);
  srv.sendContent_P((const char *)F("}"));
  srv.chunkedResponseFinalize();
}

// ---- /api/search: 递归搜索（chunked 流式）----
static void handleApiSearch() {
  ESP8266WebServer &srv = wifiManagerServer();
  String qArg = srv.arg("q");
  String pathArg = srv.arg("path");
  String depthArg = srv.arg("depth");
  if (qArg.length() == 0) {
    sendApiErr(400, "invalid_query");
    return;
  }
  if (pathArg.length() == 0) pathArg = "/";
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) {
    sendApiErr(400, "invalid_path");
    return;
  }
  int depth = (depthArg.length() > 0) ? atoi(depthArg.c_str()) : 3;
  if (depth < 1) depth = 1;
  if (depth > 8) depth = 8;
  if (!reinitSdBus("api_search")) {
    sendApiErr(500, "sd_error");
    return;
  }
  // 预检根目录存在（流式头发出后无法改状态码）
  {
    File probe = SD.open(path, FILE_READ);
    if (!probe || !probe.isDirectory()) {
      if (probe) probe.close();
      sendApiErr(404, "not_found");
      return;
    }
    probe.close();
  }
  bool truncated = false;
  int scanned = 0;
  char qEsc[260];
  jsonStr(qArg.c_str(), qEsc, sizeof(qEsc));
  srv.sendHeader("Connection", "close");   // 强制断开, 避免 keep-alive 残留连接挂起后续请求
  srv.chunkedResponseModeStart_P(200, PSTR("application/json; charset=utf-8"));
  srv.sendContent_P((const char *)F("{\"ok\":true,\"query\":\""));
  srv.sendContent(qEsc);
  srv.sendContent_P((const char *)F("\",\"path\":\""));
  srv.sendContent(path);
  srv.sendContent_P((const char *)F("\",\"items\":["));
  // 请求级格式化缓冲（一次分配, 回调复用; 低堆时失败则退回每项 malloc）
  char *fmtBuf = (char *)malloc(1640);
  ListCtx lc = {true, false, fmtBuf};
  int found = sdSearch(path, qArg.c_str(), depth, listItemCb, &lc, 200, &scanned, &truncated);
  free(fmtBuf);
  srv.sendContent_P((const char *)F("],\"searched\":"));
  char tmp[24];
  snprintf(tmp, sizeof(tmp), "%d", scanned);
  srv.sendContent(tmp);
  srv.sendContent_P((const char *)F(",\"found\":"));
  snprintf(tmp, sizeof(tmp), "%d", found);
  srv.sendContent(tmp);
  srv.sendContent_P((const char *)F(",\"truncated\":"));
  srv.sendContent_P(truncated ? (const char *)F("true") : (const char *)F("false"));
  srv.sendContent_P((const char *)F("}"));
  srv.chunkedResponseFinalize();
}

// ---- /api/upload-status: 已存在 .uploading 大小（供上传恢复判断）----
static void handleApiUploadStatus() {
  ESP8266WebServer &srv = wifiManagerServer();
  String dirArg = srv.arg("path");
  String nameArg = srv.arg("name");
  if (dirArg.length() == 0) dirArg = "/";
  char dir[300];
  if (!normalizeApiPath(dirArg.c_str(), dir, sizeof(dir))) {
    sendApiErr(400, "invalid_path");
    return;
  }
  char name[256];
  if (!sanitizeUploadName(nameArg.c_str(), name, sizeof(name))) {
    sendApiErr(400, "invalid_name");
    return;
  }
  if (!reinitSdBus("api_upstatus")) {
    sendApiErr(500, "sd_error");
    return;
  }
  char tmpPath[560];
  snprintf(tmpPath, sizeof(tmpPath), "%s/%s.uploading", dir, name);
  SdEntry e;
  bool exists = sdStat(tmpPath, &e);
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "{\"ok\":true,\"exists\":%s,\"size\":%llu}",
           exists ? "true" : "false",
           exists ? (unsigned long long)e.size : 0ULL);
  srv.send(200, "application/json; charset=utf-8", tmp);
}

// ---- 变更端点（全部必验 X-Admin-Pass; 401 unauthorized）----
// 传输并发锁: v1 服务器单客户端, 阻塞式传输期间天然互斥;
// 该标志供后续协作式传输与跨请求防御（传输中其他变更端点 → 409 busy）
static volatile bool gTransferActive = false;

static bool apiBusy() {
  return gTransferActive;
}

static void sendApiOk() {
  wifiManagerServer().send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

// SdErr → HTTP 响应
static void sendSdErr(SdErr r) {
  switch (r) {
    case SD_NOT_FOUND:  sendApiErr(404, "not_found"); break;
    case SD_EXISTS:     sendApiErr(409, "exists"); break;
    case SD_NOT_EMPTY:  sendApiErr(409, "not_empty"); break;
    case SD_PROTECTED:  sendApiErr(403, "protected"); break;
    case SD_INVALID_MOVE: sendApiErr(409, "invalid_move"); break;
    default:            sendApiErr(500, "internal_error"); break;
  }
}

// POST /api/mkdir?path=
static void handleApiMkdir() {
  ESP8266WebServer &srv = wifiManagerServer();
  if (!wifiManagerAdminPassValid()) { sendApiErr(401, "unauthorized"); return; }
  if (apiBusy()) { sendApiErr(409, "busy"); return; }
  String pathArg = srv.arg("path");
  if (pathArg.length() == 0) { sendApiErr(400, "invalid_path"); return; }
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) { sendApiErr(400, "invalid_path"); return; }
  if (isProtectedPath(path)) { sendApiErr(403, "protected"); return; }
  if (!reinitSdBus("api_mkdir")) { sendApiErr(500, "internal_error"); return; }
  SdErr r = sdMkdir(path);
  if (r == SD_OK) sendApiOk(); else sendSdErr(r);
}

// POST /api/delete?path=
static void handleApiDelete() {
  ESP8266WebServer &srv = wifiManagerServer();
  if (!wifiManagerAdminPassValid()) { sendApiErr(401, "unauthorized"); return; }
  if (apiBusy()) { sendApiErr(409, "busy"); return; }
  String pathArg = srv.arg("path");
  if (pathArg.length() == 0) { sendApiErr(400, "invalid_path"); return; }
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) { sendApiErr(400, "invalid_path"); return; }
  if (isProtectedPath(path)) { sendApiErr(403, "protected"); return; }   // .uploading 可删（不在保护列表）
  if (!reinitSdBus("api_delete")) { sendApiErr(500, "internal_error"); return; }
  SdErr r = sdDelete(path);
  if (r == SD_OK) sendApiOk(); else sendSdErr(r);
}

// POST /api/rename?path=&name=  （同目录改名）
static void handleApiRename() {
  ESP8266WebServer &srv = wifiManagerServer();
  if (!wifiManagerAdminPassValid()) { sendApiErr(401, "unauthorized"); return; }
  if (apiBusy()) { sendApiErr(409, "busy"); return; }
  String pathArg = srv.arg("path");
  String nameArg = srv.arg("name");
  if (pathArg.length() == 0 || nameArg.length() == 0) { sendApiErr(400, "invalid_path"); return; }
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) { sendApiErr(400, "invalid_path"); return; }
  if (isProtectedPath(path)) { sendApiErr(403, "protected"); return; }
  char name[256];
  if (!sanitizeUploadName(nameArg.c_str(), name, sizeof(name))) { sendApiErr(400, "invalid_name"); return; }
  // 目标扩展名受保护（防改名生成索引类文件）→ 403
  char target[560];
  const char *slash = strrchr(path, '/');
  if (slash && slash != path) {
    snprintf(target, sizeof(target), "%.*s/%s", (int)(slash - path), path, name);
  } else {
    snprintf(target, sizeof(target), "/%s", name);
  }
  if (isProtectedPath(target)) { sendApiErr(403, "protected"); return; }
  if (!reinitSdBus("api_rename")) { sendApiErr(500, "internal_error"); return; }
  SdErr r = sdRename(path, name);
  if (r == SD_OK) sendApiOk(); else sendSdErr(r);
}

// POST /api/move?path=&dest=  （跨目录; 防环）
static void handleApiMove() {
  ESP8266WebServer &srv = wifiManagerServer();
  if (!wifiManagerAdminPassValid()) { sendApiErr(401, "unauthorized"); return; }
  if (apiBusy()) { sendApiErr(409, "busy"); return; }
  String pathArg = srv.arg("path");
  String destArg = srv.arg("dest");
  if (pathArg.length() == 0 || destArg.length() == 0) { sendApiErr(400, "invalid_path"); return; }
  char path[300], dest[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path)) ||
      !normalizeApiPath(destArg.c_str(), dest, sizeof(dest))) {
    sendApiErr(400, "invalid_path");
    return;
  }
  if (isProtectedPath(path)) { sendApiErr(403, "protected"); return; }
  if (isProtectedPath(dest)) { sendApiErr(403, "protected"); return; }
  if (!sdMoveDestAllowed(path, dest)) { sendApiErr(409, "invalid_move"); return; }
  // 目标文件名扩展名受保护 → 403
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  char target[560];
  snprintf(target, sizeof(target), "%s/%s", dest, base);
  if (isProtectedPath(target)) { sendApiErr(403, "protected"); return; }
  if (!reinitSdBus("api_move")) { sendApiErr(500, "internal_error"); return; }
  SdErr r = sdMove(path, dest);
  if (r == SD_OK) sendApiOk(); else sendSdErr(r);
}

// ---- S5: 下载 / 上传 ----

// RFC3986 percent-encode（Content-Disposition filename* 与响应头用）
static void percentEncode(const char *s, char *out, size_t outSize) {
  static const char hex[] = "0123456789ABCDEF";
  size_t oi = 0;
  for (const char *p = s; *p && oi + 3 < outSize; p++) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out[oi++] = (char)c;
    } else {
      out[oi++] = '%';
      out[oi++] = hex[c >> 4];
      out[oi++] = hex[c & 0xF];
    }
  }
  out[oi] = '\0';
}

// 本文件内取 basename（full 指向 API 层缓冲, 可直接解析）
static const char *baseNameOfLocal(const char *full) {
  const char *p = full;
  for (const char *q = full; *q; q++) {
    if (*q == '/') p = q + 1;
  }
  return p;
}

// Range 解析: 返回 -1=无/忽略, -2=非法(416), 1=有效(输出 start/end)
static int parseRange(const String &h, uint64_t size, uint64_t *start, uint64_t *end) {
  if (!h.startsWith("bytes=")) return -1;
  String spec = h.substring(6);
  if (spec.indexOf(',') >= 0) return -2;         // 多段 → v1 不支持 416
  int dash = spec.indexOf('-');
  if (dash < 0) return -2;
  String s = spec.substring(0, dash);
  String e = spec.substring(dash + 1);
  if (s.length() == 0) return -2;                // suffix range → 416 (v1 不支持)
  uint64_t st = strtoull(s.c_str(), NULL, 10);
  if (st >= size) return -2;
  uint64_t en = (e.length() > 0) ? strtoull(e.c_str(), NULL, 10) : size - 1;
  if (en >= size) en = size - 1;
  if (en < st) return -2;
  *start = st;
  *end = en;
  return 1;
}

// GET /api/download?path=  （支持 Range, 4KB 块流式 + 喂狗 + 断连检测）
static void handleApiDownload() {
  ESP8266WebServer &srv = wifiManagerServer();
  String pathArg = srv.arg("path");
  if (pathArg.length() == 0) { sendApiErr(400, "invalid_path"); return; }
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) { sendApiErr(400, "invalid_path"); return; }
  if (isUploadingTemp(path)) { sendApiErr(403, "upload_in_progress"); return; }  // .uploading 不可下载
  if (!reinitSdBus("api_download")) { sendApiErr(500, "internal_error"); return; }
  File f = SD.open(path, FILE_READ);
  if (!f) { sendApiErr(404, "not_found"); return; }
  if (f.isDirectory()) { f.close(); sendApiErr(400, "invalid_path"); return; }
  uint64_t size = (uint64_t)f.size();
  uint64_t start = 0, end = (size > 0 ? size - 1 : 0);
  int range = (size > 0 && srv.hasHeader("Range")) ? parseRange(srv.header("Range"), size, &start, &end) : -1;
  if (range == -2) {
    f.close();
    char cr[48];
    snprintf(cr, sizeof(cr), "bytes */%llu", (unsigned long long)size);
    srv.sendHeader("Content-Range", cr);
    sendApiErr(416, "invalid_range");
    return;
  }
  uint64_t len = (range == 1) ? (end - start + 1) : size;
  // 下载文件名: filename*=UTF-8''<encoded> 是标准 UTF-8 文件名 (RFC 5987),
  // filename="<ascii>" 是老浏览器兜底。此前 filename 硬编码 "download" 且无扩展名,
  // 浏览器优先用 filename → 下载文件恒叫 "download"。修复: filename 用原始文件名
  // (ASCII 安全), filename* 用 RFC3986 编码 (中文/空格正确)。
  const char *bname = baseNameOfLocal(path);
  char plain[256];
  snprintf(plain, sizeof(plain), "%s", bname);
  char enc[300];
  percentEncode(bname, enc, sizeof(enc));
  char disp[700];
  snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"; filename*=UTF-8''%s", plain, enc);
  srv.sendHeader("Accept-Ranges", "bytes");
  srv.sendHeader("Content-Disposition", disp);
  srv.setContentLength((size_t)len);
  if (range == 1) {
    char cr[80];
    snprintf(cr, sizeof(cr), "bytes %llu-%llu/%llu", (unsigned long long)start,
             (unsigned long long)end, (unsigned long long)size);
    srv.sendHeader("Content-Range", cr);
    srv.send(206, "application/octet-stream", "");
  } else {
    srv.send(200, "application/octet-stream", "");
  }
  gTransferActive = true;
  auditHeap("download_start");   // 审计: 下载开始（缓冲分配后）
  if (!f.seek(start)) { gTransferActive = false; f.close(); return; }
  // 2KB 动态缓冲: 配网会话堆 ~4.4KB（静态 RAM 86% + AP 结构）, 4KB malloc 会失败;
  // 吞吐由网络/TCP 决定(~0.2MB/s, S4.5 基准), 缓冲大小无影响
  uint8_t *buf = (uint8_t *)malloc(2048);
  if (!buf) { gTransferActive = false; f.close(); return; }
  srv.client().setNoDelay(true);   // 关 Nagle: 流式吞吐关键（实测 0.23→~1MB/s）
  uint64_t remaining = len;
  while (remaining > 0) {
    size_t want = (size_t)(remaining > 2048 ? 2048 : remaining);
    int n = f.read(buf, want);
    if (n <= 0) break;
    if (srv.client().write(buf, (size_t)n) != (size_t)n) break;   // 断连
    remaining -= (uint64_t)n;
    ESP.wdtFeed();
    // 注: 不加 yield()——实测 yield 把下载从 0.23 拖到 0.11MB/s;
    // TCP 吞吐 ~0.2MB/s 是栈/链路瓶颈(见 S4.5 基准), 非 SD 非缓冲
  }
  free(buf);
  gTransferActive = false;
  f.close();
}

// ---- 上传（multipart 流式写 .uploading + X-File-Size/X-Resume）----
enum UpErr {
  UP_NONE = 0,
  UP_PROTECTED,        // 403
  UP_EXISTS,           // 409 最终已存在（v1 禁止覆盖）
  UP_RESUME_MISMATCH,  // 409 续传偏移不一致
  UP_SIZE_MISMATCH,    // 400 实收 != X-File-Size
  UP_IO,               // 500/507 写失败/空间不足
  UP_INVALID,          // 400 参数非法
};
struct UpState {
  char tmpPath[600];
  char finalPath[600];
  uint64_t received;      // 已写字节（不含跳过的前缀）
  uint64_t expected;      // X-File-Size
  uint64_t skipRemaining; // 续传: 丢弃 multipart 前缀字节数（= 实际 .uploading 尺寸）
  uint64_t actualSize;    // 续传时实际 .uploading 尺寸（mismatch 响应带出）
  uint64_t lastFlush;     // 上次 flush 时的 received（断连后残留尺寸可见性）
  UpErr err;
  bool active;            // 传输进行中（START..END）
  bool done;              // 成功提交（rename 完成）
};
static UpState *gUp = NULL;   // 上传状态转堆上分配(静态 1.2KB 是配网会话堆负担)
static bool gUpAllocFail = false;   // gUp malloc 失败（低堆）标记, done handler 回 507
static File gUpFile;      // 传输句柄（单客户端, 回调间保持打开）
static uint8_t *gUpBuf = NULL;   // 4KB 写入缓冲（动态, START 分配 END 释放）: 攒满落盘提吞吐
static size_t gUpBufLen = 0;
static const size_t kUpBufSize = 2048;   // 2KB: 配网会话堆限制（4KB malloc 会失败; 网络才是吞吐瓶颈）

static void upReset() {
  if (gUp) { free(gUp); gUp = NULL; }
  if (gUpFile) gUpFile.close();
  if (gUpBuf) { free(gUpBuf); gUpBuf = NULL; }
  gUpBufLen = 0;
  gUpAllocFail = false;
}

// 刷新上传缓冲到 SD; 返回 false=写失败
static bool upFlush() {
  if (gUpBufLen == 0) return true;
  size_t n = gUpBufLen;
  gUpBufLen = 0;
  if (gUpFile.write(gUpBuf, n) != n) {
    gUp->err = UP_IO;   // 写失败（空间不足等）→ .uploading 保留, done 回 507
    return false;
  }
  gUp->received += n;
  // 每 ~64KB flush 一次: 断连时 .uploading 的目录项尺寸才可见（SdFat 只在 close/flush 更新
  // 目录项; 不 flush 的话中断后 upload-status 读到 size=0, 续传无法对齐——实测）
  if (gUp->received - gUp->lastFlush >= 65536) {
    gUpFile.flush();
    gUp->lastFlush = gUp->received;
  }
  return true;
}

// multipart 上传回调（流式写 .uploading; THandlerFunction 无参, 经 server.upload() 取数据）
static void handleApiUploadCb() {
  ESP8266WebServer &srv = wifiManagerServer();
  HTTPUpload &upload = srv.upload();
  if (upload.status == UPLOAD_FILE_START) {
    upReset();
    gUp = (UpState *)calloc(1, sizeof(UpState));
    if (!gUp) { gUpAllocFail = true; return; }   // 低堆: done 回 507, 不崩
    String pathArg = srv.arg("path");
    if (pathArg.length() == 0) pathArg = "/";
    char dir[300];
    if (!normalizeApiPath(pathArg.c_str(), dir, sizeof(dir))) { gUp->err = UP_INVALID; return; }
    char name[256];
    if (!sanitizeUploadName(upload.filename.c_str(), name, sizeof(name))) { gUp->err = UP_INVALID; return; }
    snprintf(gUp->tmpPath, sizeof(gUp->tmpPath), "%s/%s.uploading", dir, name);
    snprintf(gUp->finalPath, sizeof(gUp->finalPath), "%s/%s", dir, name);
    // 顺序固定: 受保护 → 已存在 → 续传校验 → 空间预检 → 创建 .uploading
    if (isProtectedPath(gUp->finalPath)) { gUp->err = UP_PROTECTED; return; }
    if (SD.exists(gUp->finalPath)) { gUp->err = UP_EXISTS; return; }
    gUp->expected = srv.hasHeader("X-File-Size") ? strtoull(srv.header("X-File-Size").c_str(), NULL, 10) : 0;
    bool resume = srv.hasHeader("X-Resume") && srv.header("X-Resume") == "1";
    if (resume) {
      // 客户端 X-Resume-Offset 是提示（断连时未 flush 的 FAT 缓存让实际尺寸比客户端读到的
      // 大几十 KB, 无法精确对齐）。服务端按 .uploading 实际尺寸跳过; 提示超出窗口(256KB)
      // → 409 resume_mismatch + serverOffset 供客户端重试。
      File old = SD.open(gUp->tmpPath, FILE_READ);
      uint64_t actual = old ? (uint64_t)old.size() : 0;
      if (old) old.close();
      gUp->actualSize = actual;
      uint64_t hint = 0;
      bool hasHint = srv.hasHeader("X-Resume-Offset");
      if (hasHint) hint = strtoull(srv.header("X-Resume-Offset").c_str(), NULL, 10);
      bool okHint = (!hasHint) || (hint <= actual && actual - hint <= 262144ULL);
      if (actual > 0 && okHint) {
        gUp->skipRemaining = actual;   // 丢弃 multipart 前 actual 字节
        gUp->received = actual;        // 已存在字节计入总量
      } else {
        gUp->err = UP_RESUME_MISMATCH;
        return;
      }
    }
    // 空间预检: 缓存容量仅 1.2s 前快照, 不精确; 写失败兜底（UP_IO → 507）
    if (SD.exists(gUp->tmpPath) && gUp->skipRemaining == 0) SD.remove(gUp->tmpPath);
    if (!reinitSdBus("api_upopen")) { gUp->err = UP_IO; return; }
    // 续传用 "a"（追加: 保留已存在字节 + 追加 body 跳过前缀后的部分）;
    // 全新用 "w"（截断）。用 "w"+跳过会丢掉已有字节（实测 size 差 = 偏移量）
    gUpFile = SD.open(gUp->tmpPath, gUp->skipRemaining > 0 ? "a" : "w");
    if (!gUpFile) { gUp->err = UP_IO; return; }
    gUpBuf = (uint8_t *)malloc(kUpBufSize);
    if (!gUpBuf) { gUp->err = UP_IO; return; }
    gUp->active = true;
    gTransferActive = true;
    auditHeap("upload_start");   // 审计: 上传初始化（gUp + gUpBuf 分配后）
    Serial.printf("UP_START %s expect=%llu skip=%llu\n", gUp->tmpPath,
                  (unsigned long long)gUp->expected, (unsigned long long)gUp->skipRemaining);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!gUp->active || gUp->err != UP_NONE) return;
    const uint8_t *p = upload.buf;
    size_t n = (size_t)upload.currentSize;
    if (gUp->skipRemaining > 0) {
      if (n <= gUp->skipRemaining) {
        gUp->skipRemaining -= n;
        return;
      }
      p += (size_t)gUp->skipRemaining;
      n -= (size_t)gUp->skipRemaining;
      gUp->skipRemaining = 0;
    }
    // 攒入 4KB 缓冲, 满则落盘（multipart 块 ~1.5KB 非对齐写 SD 极慢, 实测 0.18MB/s）
    while (n > 0) {
      size_t room = kUpBufSize - gUpBufLen;
      size_t take = n < room ? n : room;
      memcpy(gUpBuf + gUpBufLen, p, take);
      gUpBufLen += take;
      p += take;
      n -= take;
      if (gUpBufLen >= kUpBufSize) {
        if (!upFlush()) return;
      }
    }
    ESP.wdtFeed();
  } else if (upload.status == UPLOAD_FILE_END) {
    if (gUpFile && !upFlush()) { /* err 已置 */ }
    if (gUpFile) gUpFile.close();
    gTransferActive = false;
    if (!gUp->active) return;
    if (gUp->err != UP_NONE) return;   // done handler 回错误
    if (gUp->expected > 0 && gUp->received != gUp->expected) {
      gUp->err = UP_SIZE_MISMATCH;     // .uploading 保留供诊断/重试
      return;
    }
    // 校验通过 → 内部 rename 提交（不经过普通 rename API）
    if (!reinitSdBus("api_upcommit")) { gUp->err = UP_IO; return; }
    if (SD.rename(gUp->tmpPath, gUp->finalPath)) {
      Serial.printf("UP_DONE %s size=%llu\n", gUp->finalPath, (unsigned long long)gUp->received);
      gUp->done = true;   // 成功标志（active 会先被清理）
    } else {
      gUp->err = UP_IO;   // rename 失败 → .uploading 保留
    }
  }
}

// POST /api/upload 完成回调（发送最终响应）
static void handleApiUploadDone() {
  ESP8266WebServer &srv = wifiManagerServer();
  if (gUpAllocFail) { upReset(); sendApiErr(507, "insufficient_storage"); return; }
  if (!gUp) { sendApiErr(400, "invalid_path"); return; }   // 未开始
  if (gUp->done) {
    uint64_t size = gUp->received;
    upReset();
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "{\"ok\":true,\"size\":%llu}", (unsigned long long)size);
    srv.send(201, "application/json; charset=utf-8", tmp);
    return;
  }
  UpErr err = gUp->err;
  upReset();
  switch (err) {
    case UP_PROTECTED:      sendApiErr(403, "protected"); break;
    case UP_EXISTS:         sendApiErr(409, "exists"); break;
    case UP_RESUME_MISMATCH: {
      char tmp[180];
      snprintf(tmp, sizeof(tmp), "{\"ok\":false,\"error\":\"resume_mismatch\",\"serverOffset\":%llu}",
               (unsigned long long)gUp->actualSize);
      srv.send(409, "application/json; charset=utf-8", tmp);
      break;
    }
    case UP_SIZE_MISMATCH:  sendApiErr(400, "upload_size_mismatch"); break;
    case UP_IO:             sendApiErr(507, "insufficient_storage"); break;
    default:                sendApiErr(400, "invalid_path"); break;
  }
}

// ---- 路由分发 ----
// core 3.1.2 的 Uri 只支持精确匹配（无通配符, /api/* 是字面量→404 实测）;
// 且 12+ 个 API 路由对象吃 ~1.8KB 常驻堆（配网会话堆仅 ~1KB）。
// → API 不经路由注册, 走 onNotFound 天然入口（零路由堆）: wifi_manager 的 onNotFound
//   先调 fileApiTryDispatch(), /api/* 在此分发, 其余走默认 404。
static void handleApiDispatch() {
  ESP8266WebServer &srv = wifiManagerServer();
  String uri = srv.uri();
  HTTPMethod m = srv.method();
  auditHeap("api_req");   // 审计: 每个 /api 请求入口（含请求解析后的堆）
  if (uri == "/api/status" && m == HTTP_GET) { handleApiStatus(); return; }
  if (uri == "/api/capacity" && m == HTTP_GET) { handleApiCapacity(); return; }
  if (uri == "/api/stat" && m == HTTP_GET) { handleApiStat(); return; }
  if (uri == "/api/files" && m == HTTP_GET) { handleApiFiles(); return; }
  if (uri == "/api/search" && m == HTTP_GET) { handleApiSearch(); return; }
  if (uri == "/api/upload-status" && m == HTTP_GET) { handleApiUploadStatus(); return; }
  if (uri == "/api/download" && m == HTTP_GET) { handleApiDownload(); return; }
  if (uri == "/api/mkdir" && m == HTTP_POST) { handleApiMkdir(); return; }
  if (uri == "/api/delete" && m == HTTP_POST) { handleApiDelete(); return; }
  if (uri == "/api/rename" && m == HTTP_POST) { handleApiRename(); return; }
  if (uri == "/api/move" && m == HTTP_POST) { handleApiMove(); return; }
  sendApiErr(404, "not_found");
}

// ---- LittleFS Web UI 静态服务（懒挂载: 首次 /fm 请求才 LittleFS.begin, 省会话常驻堆）----
static void handleFmStatic(const String &uri);

// 共享懒挂载（/fm 与 /fs/edit 复用同一标志, 只 begin 一次）。
// 配网会话堆硬约束: 每个请求重复 LittleFS.begin() 会各分配 ~1KB 挂载结构, 在 3-4KB 基线上打穿堆
//（实测 /fs/edit 每次 begin → 异常重启）。返回 true 表示已挂载/已挂载成功, false=挂载失败。
bool fileApiEnsureLfsMount() {
  static bool mounted = false;
  if (mounted) return true;
  if (!LittleFS.begin()) return false;
  mounted = true;
  Serial.println(F("LFS_MOUNT_LAZY"));
  return true;
}

// 供 wifi_manager onNotFound 调用; 返回 true 表示已处理（/api/* /fs/* 或上传相关）
bool fileApiTryDispatch() {
  ESP8266WebServer &srv = wifiManagerServer();
  String uri = srv.uri();
  // 诊断: 记录所有到达 onNotFound 的请求（上传卡死排查: 确认 POST /fs/edit 是否漏到 onNotFound）
  {
    HTTPMethod m = srv.method();
    Serial.printf("DISPATCH uri=%s method=%d\n", uri.c_str(), (int)m);
  }
  if (uri.startsWith("/api/")) {
    handleApiDispatch();
    return true;
  }
  // 官方 A7 管理 Web 文件管理构造复刻 /fs/*（零路由对象堆, 前缀匹配分发）
  if (fileApiFsTryDispatch()) return true;
  if (uri == "/fm" || uri.startsWith("/fm/")) {
    handleFmStatic(uri);
    return true;
  }
  return false;
}

// ---- LittleFS Web UI 静态服务（懒挂载: 首次 /fm 请求才 LittleFS.begin, 省会话常驻堆）----
static void handleFmStatic(const String &uri) {
  ESP8266WebServer &srv = wifiManagerServer();
  // 旧 S6 UI（index.html/app.js/style.css）已移除（git 保留历史）; 入口改指官方构造 manager 页 /fs/edit
  if (uri == "/fm" || uri == "/fm/" || uri == "/fm/index.html") {
    srv.sendHeader("Location", "/fs/edit");
    srv.send(302);
    return;
  }
  static bool mounted = false;
  if (!mounted) {
    auditHeap("fm_before_mount");   // 审计: /fm 首请求挂载前
    if (!fileApiEnsureLfsMount()) {
      srv.send(500, "text/plain; charset=utf-8", "LittleFS mount failed");
      return;
    }
    mounted = true;
    auditHeap("fm_after_mount");   // 审计: LittleFS 挂载后
  }
  if (uri == "/fm") {   // 无尾斜杠 → 302
    srv.sendHeader("Location", "/fm/");
    srv.send(302);
    return;
  }
  // /fm/ → /index.html; /fm/<file> → /<file>
  // ⚠️ substring(3) 而非 (4): "/fm/" 是 4 字符, substring(4) 返回空串 → open("") 失败 404（实测）
  String fsPath = uri.substring(3);   // 去掉 "/fm"
  if (fsPath == "/" || fsPath == "/index.html" || fsPath.length() == 0) fsPath = "/index.html";
  const char *mime = "application/octet-stream";
  if (fsPath.endsWith(".html")) mime = "text/html; charset=utf-8";
  else if (fsPath.endsWith(".css")) mime = "text/css; charset=utf-8";
  else if (fsPath.endsWith(".js")) mime = "application/javascript; charset=utf-8";
  else if (fsPath.endsWith(".png")) mime = "image/png";
  File f = LittleFS.open(fsPath, "r");
  if (!f) {
    srv.send(404, "text/plain; charset=utf-8", "Not Found");
    return;
  }
  size_t sz = (size_t)f.size();
  srv.sendHeader("Connection", "close");
  srv.setContentLength(sz);
  srv.send(200, mime, "");
  // 分块流式发送（文件可达 10KB+, 一次性 String 会爆配网会话堆）
  static uint8_t buf[512];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (srv.client().write(buf, (size_t)n) != (size_t)n) break;
    ESP.wdtFeed();
  }
  f.close();
}
void fileApiInit() {
  if (lfsReady) return;
  // LittleFS 懒挂载: 配网会话堆是硬约束（AP 后台 5s 内 -2.9KB, 手机关联 OOM 实测）,
  // 挂载(~1KB)+serveStatic 改为首次 /fm 请求时执行（见 fileApiTryDispatch 的 handleFmStatic）
  lfsReady = true;
  // /fs/edit POST 上传是 4 参路由（onNotFound 收不到上传块）; 在 server.begin() 前注册。
  // ⚠️ 注意: 必须先于 server.begin(); fileApiInit 在 wifi_manager 的 server.begin() 前调用。
  fileApiFsRegisterUploadRoute();
}


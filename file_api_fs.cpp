// file_api_fs.cpp — 官方 A7 管理 Web "文件管理"高级层构造复刻（/fs/*, 移植自 web_test official_manager.cpp）
//
// 复刻对象: /system/manager.htm 前端 + /status /list /edit REST 后端
//   （V14 WEBServer.ino 路由表 + xz015_03A7_rebuild.ino §EDIT 文件管理器 + A7 二进制字符串证据）
// 差异（本项目/堆约束）:
//   - 管理对象 = SD 卡（官方 LittleFS; 构造不受 FS 类型影响）
//   - 无鉴权（官方同款）; 保留 isProtectedPath/.uploading 兜底（我方增强）
//   - 显示过滤: 黑名单(系统目录/隐藏/索引章节sidecar标签) + 文件白名单(.txt/.bmp),
//     与屏上文件管理器 listDir 一致（用户: "不该显示的不要显示"）
//   - /fs/status O(1): 只读 sdAvailable + fileApiGetCachedCapacity, 绝不遍历 SD（配网会话堆硬约束）
//   - 列表上限 200（sdListDir）; 递归删除有界（深度≤16+喂狗+先关句柄）; 上传 END 校验空文件;
//     JSON 字符串转义（官方不转义, 中文名含引号会破 JSON）
//   - 全部大缓冲用 malloc/static（ESP8266 循环栈仅 4KB, 栈缓冲必爆 Exception 5）
//
// 路由: 不经 server.on 注册（core 3.1.2 Uri 无通配符 + 路由对象吃堆）,
//   由 fileApiTryDispatch 前缀匹配 fileApiFsTryDispatch() 分发, 零路由对象堆。

#include "file_api_fs.h"
#include "file_api.h"
#include "wifi_manager.h"
#include "sd_path.h"
#include "sd_file_ops.h"
#include <SDFS.h>   // SDFS.openDir + Dir::next()（官方 A7 枚举同款; 大目录枚举修复）
#include "fs_cache.h"
#include <LittleFS.h>
#include <ESP8266WebServer.h>

extern bool sdAvailable;   // ink-reader-esp.ino 全局: SD 挂载标志（/fs/status O(1) 只读）
extern bool reinitSdBus(const char *reason);
bool fileApiGetCachedCapacity(uint64_t *total, uint64_t *used);   // file_api.cpp

static ESP8266WebServer &srv() { return wifiManagerServer(); }

// ---- 请求边界堆探针（诊断: 确认 /fs/list 每次请求后 heap 是否回升 vs 泄漏/碎片）----
// 打印 freeHeap + maxFreeBlock + stack（复用项目 traceFmt 同口径, 见 diagLog）。
// 编号: LIST_ENTER(进 handler) / HANDLER_RETURN(handler 返回) / LOOP_AFTER(handleClient 收尾后)
// 不带序号版; 序号由调用方经 ++fsListReqNo 传入以区分 NEXT_ENTER。
static uint32_t gFsListReqNo = 0;   // 递增请求序号, 区分连续请求
void fsListProbe(const char *tag) {
  Serial.printf("FSREQ #%lu %s heap=%u maxblk=%u stack=%u\n",
                (unsigned long)gFsListReqNo, tag,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
                (unsigned)ESP.getFreeContStack());
}
uint32_t fsListReqNext() { return ++gFsListReqNo; }   // 下一个请求序号（首次调用返回 1）

// 带分页明细的探针（FSREQ_DONE 用）: 额外显示 start/count/emitted/more
void fsListProbeDetail(const char *tag, size_t start, size_t count, size_t emitted, bool hasMore) {
  Serial.printf("FSREQ #%lu %s heap=%u maxblk=%u stack=%u start=%u count=%u emitted=%u more=%d\n",
                (unsigned long)gFsListReqNo, tag,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
                (unsigned)ESP.getFreeContStack(),
                (unsigned)start, (unsigned)count, (unsigned)emitted, hasMore ? 1 : 0);
}

// 供 wifi_managerLoop 在 server.handleClient() 后识别"本轮回调刚处理过一次 /fs/list"
// 以打印 LOOP_AFTER 边界探针（handler 在 handleClient 内执行, 返回后回到 loop）。
bool gFsListJustHandled = false;

// ---- 通用助手 ----

static bool endsWithIC(const char *s, const char *suffix) {
  size_t sl = strlen(s), fl = strlen(suffix);
  if (sl < fl) return false;
  return strcasecmp(s + sl - fl, suffix) == 0;
}

// 官方 getContentType 移植（按扩展名; 缺省 text/plain）
static const char *ofsContentType(const char *path) {
  if (endsWithIC(path, ".htm") || endsWithIC(path, ".html")) return "text/html";
  if (endsWithIC(path, ".css")) return "text/css";
  if (endsWithIC(path, ".js"))  return "application/javascript";
  if (endsWithIC(path, ".png")) return "image/png";
  if (endsWithIC(path, ".gif")) return "image/gif";
  if (endsWithIC(path, ".jpg")) return "image/jpeg";
  if (endsWithIC(path, ".ico")) return "image/x-icon";
  if (endsWithIC(path, ".xml")) return "text/xml";
  if (endsWithIC(path, ".pdf")) return "application/x-pdf";
  if (endsWithIC(path, ".zip")) return "application/x-zip";
  if (endsWithIC(path, ".gz"))  return "application/x-gzip";
  return "text/plain";
}

// JSON 字符串转义（官方未转义; 修正: 名称含 " \ 控制字符会破 JSON）
static void ofsJsonEscape(const char *s, char *out, size_t outSize) {
  size_t oi = 0;
  static const char hex[] = "0123456789abcdef";
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
          out[oi++] = hex[(c >> 4) & 0xF]; out[oi++] = hex[c & 0xF];
        } else {
          out[oi++] = (char)c;
        }
    }
  }
  out[oi] = '\0';
}

// ---- 显示过滤（搬运 ink-reader-esp.ino listDir 同款: 不该显示的不显示）----
// 纯栈/strcasecmp 版本（零 String 分配）: /fs/list 对每个 SD 项调用一次,
// 原 String lower = name + toLowerCase() 在 3-4KB 配网会话基线上每项分配/释放堆 → 200 项打穿堆（重启根因）。
static bool ofsHasBlacklistSuffix(const char *dot) {
  static const char *const suffixes[] = {
    ".i1", ".z1", ".i2", ".z2", ".v1", ".vz1", ".i1p", ".v1p", ".bm", ".bmt", nullptr
  };
  for (size_t i = 0; suffixes[i]; i++) {
    if (strcasecmp(dot, suffixes[i]) == 0) return true;
  }
  return false;
}

// 黑名单: 隐藏名(.开头)/索引章节sidecar标签扩展名/SD系统目录
static bool ofsBlacklistedEntry(const char *name) {
  if (!name || !name[0]) return true;
  if (name[0] == '.') return true;
  const char *dot = strrchr(name, '.');
  if (dot && dot[1] && ofsHasBlacklistSuffix(dot)) return true;
  const char *blocked[] = {
    "android", "androud", "found.000", "foud.000", "lost.dir", "system volume information"
  };
  // 逐项 strcasecmp 整名（目录名, 无扩展名依赖）; 不拷贝、不 lower String
  for (const char *item : blocked) {
    if (strcasecmp(name, item) == 0) return true;
  }
  return false;
}

// 白名单: 文件只显示 .txt/.bmp（目录始终显示, 由调用层判断）
static bool ofsWhitelistedFile(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot || !dot[1]) return false;
  return strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".bmp") == 0;
}

static void ofsReply(int code, const char *msg) { srv().send(code, "text/plain", msg); }
static void ofsReplyOKWithMsg(const char *msg)  { srv().send(200, "text/plain", msg); }
static void ofsReplyBadRequest(const char *msg) { srv().send(400, "text/plain", String(msg) + "\r\n"); }

// 取父路径（官方 handleFileCreate: substring(0, lastIndexOf('/')); 无父 → 空串）
static void ofsParent(const char *path, char *out, size_t outSize) {
  const char *slash = strrchr(path, '/');
  if (!slash || slash == path) { out[0] = '\0'; return; }
  size_t n = (size_t)(slash - path);
  if (n >= outSize) n = outSize - 1;
  memcpy(out, path, n);
  out[n] = '\0';
}

// 官方 lastExistingParent: 父链上溯到现存祖先（rename/delete 响应用）; 无现存 → 空串
static void ofsLastExistingParent(const char *path, char *out, size_t outSize) {
  ofsParent(path, out, outSize);
  while (out[0] && !SD.exists(out)) {
    char up[300];
    ofsParent(out, up, sizeof(up));
    snprintf(out, outSize, "%s", up);
  }
}

// ---- GET /fs/status: 官方 JSON 结构（数字为字符串; type=SD 对齐管理对象）----
// O(1): 只读 sdAvailable + 缓存容量, 绝不遍历 SD（配网会话堆硬约束）; 无缓存 → 容量 0
static void handleOfsStatus() {
  uint64_t total = 0, used = 0;
  bool capacity = fileApiGetCachedCapacity(&total, &used);
  String json;
  json.reserve(160);
  json = "{\"type\":\"SD\", \"isOk\":";
  bool ok = sdAvailable;
  if (ok) {
    json += "\"true\", \"totalBytes\":\"";
    json += capacity ? (unsigned long long)total : (unsigned long long)0;
    json += "\", \"usedBytes\":\"";
    json += capacity ? (unsigned long long)used : (unsigned long long)0;
    json += "\"";
  } else {
    json += "\"false\"";
  }
  json += ",\"unsupportedFiles\":\"\"}";
  srv().send(200, "application/json", json);
}

// ---- GET /fs/list?dir=&start=&count=: 分页流式列表（官方 text/json 项, 包在 items 里）----
// 用户拍板方案: 单次只取安全数量(默认50, 服务端 clamp 上限50), 前端翻页流式加载。
// 单次请求堆/栈峰值只与 count 相关; startOffset 仅增加 SD 遍历时间（不增加峰值）。
// 响应: {"items":[...],"nextStart":N,"hasMore":bool}（不返回 total, 避免整目录扫描）。

struct OfsListCtx { bool first; };
static void ofsListItemCb(const SdEntry *e, void *ctx) {
  OfsListCtx *lc = (OfsListCtx *)ctx;
  // 不该显示的不显示: 黑名单(系统目录/隐藏/索引章节sidecar标签) + 文件白名单(.txt/.bmp)
  // 目录始终显示; 过滤项不发内容, 不翻转 first（保持输出 JSON 连续）
  if (ofsBlacklistedEntry(e->name)) return;
  if (!e->isDir && !ofsWhitelistedFile(e->name)) return;
  static char buf[640];
  static char nameEsc[512];
  ofsJsonEscape(e->name, nameEsc, sizeof(nameEsc));
  if (e->isDir) {
    snprintf(buf, sizeof(buf), "%s{\"type\":\"dir\",\"name\":\"%s\"}",
             lc->first ? "" : ",", nameEsc);
  } else {
    snprintf(buf, sizeof(buf), "%s{\"type\":\"file\",\"size\":\"%llu\",\"name\":\"%s\"}",
             lc->first ? "" : ",", (unsigned long long)e->size, nameEsc);
  }
  lc->first = false;
  srv().sendContent(buf);   // mode=1 sendContent（A/B 实验已证伪 client.write, 保留 sendContent）
}

static void handleOfsList() {
  ESP8266WebServer &s = srv();
  // ---- 分页参数: start / count(count clamp 到 [1,50]) ----
  size_t start = 0;
  if (s.hasArg("start")) {
    long v = atol(s.arg("start").c_str());
    if (v > 0) start = (size_t)v;
  }
  size_t count = 50;   // 默认 50（协议硬上限, 见用户拍板）
  if (s.hasArg("count")) {
    long v = atol(s.arg("count").c_str());
    if (v > 0) count = (size_t)v;
  }
  if (count > 50) count = 50;   // 服务端强制上限（client 发 count=1000 也 clamp 到 50）

  fsListReqNext();
  fsListProbe("FSREQ_ENTER");   // 探针: 请求入口
  if (!s.hasArg("dir")) { ofsReplyBadRequest("DIR ARG MISSING "); return; }
  String dirArg = s.arg("dir");
  if (dirArg.length() == 0) dirArg = "/";
  char path[300];
  if (!normalizeApiPath(dirArg.c_str(), path, sizeof(path))) { ofsReplyBadRequest("BAD PATH"); return; }

  // ★ 浏览走 LittleFS 缓存（进 AP 前 fsCacheBuild 扫描好的 SD 目录树; 照抄官方"文件管理用 LittleFS"）。
  //   避开"配网会话实时遍历 SD × AP hostap_input"的 esf_buf_alloc 竞争（崩溃根因）。
  //   缓存不存在（扫描失败/超上限目录）→ 回退直接读 SD（保留原路径, 不静默 500）。
  if (fsCacheServeList(path, start, count)) {
    fsListProbeDetail("FSREQ_DONE(cached)", start, count, 0, false);
    gFsListJustHandled = true;
    return;
  }

  if (!reinitSdBus("fs_list")) { ofsReply(500, "FS INIT ERROR"); return; }
  if (strcmp(path, "/") != 0 && !SD.exists(path)) { ofsReplyBadRequest("BAD PATH"); return; }
  { File probe = SD.open(path, FILE_READ);
    if (!probe || !probe.isDirectory()) { if (probe) probe.close(); ofsReplyBadRequest("BAD PATH"); return; }
    probe.close(); }
  if (!s.chunkedResponseModeStart(200, "text/json")) {
    s.send(505, "text/html", "需要依赖 HTTP1.1");
    return;
  }
  // 响应前缀: {"items":[
  s.sendContent_P(PSTR("{\"items\":["));
  OfsListCtx lc = {true};
  size_t emitted = 0;
  bool hasMore = false;
  int n = sdListDirPaged(path, start, count, ofsListItemCb, &lc, &emitted, &hasMore);
  (void)n;
  // 响应后缀: ],"nextStart":<start+emitted>,"hasMore":<0|1>}
  s.sendContent_P(PSTR("],\"nextStart\":"));
  char num[16];
  snprintf(num, sizeof(num), "%u", (unsigned)(start + emitted));
  s.sendContent(num);
  s.sendContent_P(PSTR(",\"hasMore\":"));
  s.sendContent_P(hasMore ? PSTR("true") : PSTR("false"));
  s.sendContent_P(PSTR("}"));
  s.chunkedResponseFinalize();
  fsListProbeDetail("FSREQ_DONE", start, count, emitted, hasMore);   // 探针: 遍历+发送完成（含分页明细）
  gFsListJustHandled = true;    // 告知 loop: 本轮回调已处理 /fs/list（用于 LOOP_AFTER 探针）
}

// ---- PUT /fs/edit: 建文件/夹（无 src）或 重命名/移动（有 src）----
static void handleOfsEditPut() {
  ESP8266WebServer &s = srv();
  String pathArg = s.arg("path");
  if (pathArg.length() == 0) { ofsReplyBadRequest("PATH ARG MISSING"); return; }
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) { ofsReplyBadRequest("BAD PATH"); return; }
  if (strcmp(path, "/") == 0) { ofsReplyBadRequest("BAD PATH"); return; }
  if (!reinitSdBus("fs_put")) { ofsReply(500, "FS INIT ERROR"); return; }

  String srcArg = s.arg("src");
  if (srcArg.length() == 0) {
    if (SD.exists(path)) { ofsReplyBadRequest("PATH FILE EXISTS"); return; }
    if (isProtectedPath(path)) { ofsReply(403, "protected"); return; }
    size_t plen = strlen(path);
    bool isDir = (plen > 0 && path[plen - 1] == '/');
    if (isDir) {
      char d[300];
      snprintf(d, sizeof(d), "%s", path);
      d[strlen(d) - 1] = '\0';
      if (!SD.mkdir(d)) { ofsReply(500, "MKDIR FAILED"); return; }
      char parent[300];
      ofsParent(d, parent, sizeof(parent));
      ofsReplyOKWithMsg(parent);
      return;
    } else {
      File f = SD.open(path, "w");
      if (!f) { ofsReply(500, "CREATE FAILED"); return; }
      f.write((const char *)0);           // Print::write 有 NULL 保护, 等价建空文件
      f.close();
    }
    char parent[300];
    ofsParent(path, parent, sizeof(parent));
    ofsReplyOKWithMsg(parent);
    return;
  }

  char src[300];
  if (!normalizeApiPath(srcArg.c_str(), src, sizeof(src))) { ofsReplyBadRequest("BAD SRC"); return; }
  if (strcmp(src, "/") == 0) { ofsReplyBadRequest("BAD SRC"); return; }
  if (!SD.exists(src)) { ofsReply(404, "SRC FILE NOT FOUND"); return; }
  if (isProtectedPath(src) || isProtectedPath(path)) { ofsReply(403, "protected"); return; }
  char t[300], s2[300];
  snprintf(t, sizeof(t), "%s", path);
  size_t tl = strlen(t); if (tl > 1 && t[tl - 1] == '/') t[tl - 1] = '\0';
  snprintf(s2, sizeof(s2), "%s", src);
  size_t sl = strlen(s2); if (sl > 1 && s2[sl - 1] == '/') s2[sl - 1] = '\0';
  if (strcmp(s2, t) == 0) { ofsReplyBadRequest("PATH FILE EXISTS"); return; }
  if (SD.exists(t)) { ofsReplyBadRequest("PATH FILE EXISTS"); return; }
  if (!SD.rename(s2, t)) { ofsReply(500, "RENAME FAILED"); return; }
  char parent[300], srcParent[300];
  ofsLastExistingParent(s2, parent, sizeof(parent));
  ofsParent(t, srcParent, sizeof(srcParent));   // 目标目录（可能跨目录移动）
  fsCacheInvalidateDir(parent);       // 源目录刷新
  if (strcmp(srcParent, parent) != 0) fsCacheInvalidateDir(srcParent);   // 跨目录移动: 目标也刷新
  ofsReplyOKWithMsg(parent);
}

// ---- DELETE /fs/edit: 有界递归删除（官方 deleteRecursive + 深度上限 + 喂狗 + 先关句柄）----
// 瘦版（无静态路径缓冲, 对齐代码库 cleanupWalk/usedWalk 模式: 拼完整路径递归）:
// Dir API 枚举（官方 A7 同款）——File::openNextFile 每项重开文件, 大目录枚举 0 根因, 弃用。
#define OFS_MAX_DEPTH 16
static bool ofsDeleteRecursive(const char *path, int depth, int *count) {
  if (depth > OFS_MAX_DEPTH) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  bool isDir = f.isDirectory();
  f.close();
  if (!isDir) return SD.remove(path);
  {
    File p = SD.open(path, FILE_READ);
    bool okDir = p && p.isDirectory();
    if (p) p.close();
    if (!okDir) return false;
  }
  Dir d = SDFS.openDir(path);
  bool ok = true;
  int sinceFeed = 0;
  while (d.next()) {
    if (++sinceFeed >= 64) { sinceFeed = 0; ESP.wdtFeed(); }
    String nm = d.fileName();
    char full[420];
    if (nm.length() && nm[0] == '/') snprintf(full, sizeof(full), "%s%s", path, nm.c_str());
    else if (strcmp(path, "/") == 0) snprintf(full, sizeof(full), "/%s", nm.c_str());
    else snprintf(full, sizeof(full), "%s/%s", path, nm.c_str());
    if (!ofsDeleteRecursive(full, depth + 1, count)) { ok = false; break; }
    (*count)++;
  }
  if (!ok) return false;
  return SD.rmdir(path);
}

static void handleOfsEditDelete() {
  ESP8266WebServer &s = srv();
  String pathArg = s.arg(0);
  if (pathArg.length() == 0) { ofsReplyBadRequest("BAD PATH"); return; }
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) { ofsReplyBadRequest("BAD PATH"); return; }
  if (strcmp(path, "/") == 0) { ofsReplyBadRequest("BAD PATH"); return; }
  if (!reinitSdBus("fs_del")) { ofsReply(500, "FS INIT ERROR"); return; }
  if (isProtectedPath(path)) { ofsReply(403, "protected"); return; }
  if (!SD.exists(path)) { ofsReply(404, "FILE NOT FOUND"); return; }
  int count = 0;
  if (!ofsDeleteRecursive(path, 0, &count)) { ofsReply(500, "DELETE FAILED"); return; }
  char parent[300];
  ofsLastExistingParent(path, parent, sizeof(parent));
  if (parent[0] == '\0') strcpy(parent, "/");   // 根一级文件: ofsParent 返回空, 实际父目录是根
  fsCacheInvalidateDir(parent);   // 删除后刷新缓存, 防 /fs/list 读旧快照显示已删文件
  ofsReplyOKWithMsg(parent);
}

// ---- POST /fs/edit: 官方式简单上传（multipart, 4 参路由, 纯流式对齐官方 handleFileUpload）----
static File ofsUpFile;
static char ofsUpPath[300];
static uint64_t ofsUpSize = 0;
static int ofsUpErr = 0;
static int ofsUpErrCode = 0;
static char ofsUpErrMsg[64];

static void ofsUpReset() {
  if (ofsUpFile) ofsUpFile.close();
  ofsUpSize = 0;
  ofsUpErr = 0;
  ofsUpErrCode = 0;
  ofsUpErrMsg[0] = '\0';
  ofsUpPath[0] = '\0';
}

static void ofsUpFail(int code, const char *msg) {
  ofsUpErr = 1;
  ofsUpErrCode = code;
  snprintf(ofsUpErrMsg, sizeof(ofsUpErrMsg), "%s", msg);
}

// ---- 上传墨水屏状态（file_manager 注入渲染; 深回调内只设状态/调回调, 不直接刷屏）----
static int gOfsUpPhase = OFS_UP_PHASE_IDLE;
static char gOfsUpPhasePath[300] = {0};
static OfsUpPhaseCallback gOfsUpPhaseCb = NULL;

void ofsUpSetPhaseCallback(OfsUpPhaseCallback cb) { gOfsUpPhaseCb = cb; }
int  ofsUpGetPhase() { return gOfsUpPhase; }
const char *ofsUpGetPhasePath() { return gOfsUpPhasePath; }

// 内部: 上报 phase 给渲染层（去重; 完成/失败时带 path）
static void ofsUpReport(int phase) {
  if (gOfsUpPhase == phase && phase != OFS_UP_PHASE_UPLOADING) return;   // 除"上传中"外去重
  gOfsUpPhase = phase;
  const char *p = ofsUpPath[0] ? ofsUpPath : NULL;
  if (p) { size_t n = strlen(p); if (n >= sizeof(gOfsUpPhasePath)) n = sizeof(gOfsUpPhasePath) - 1; memcpy(gOfsUpPhasePath, p, n); gOfsUpPhasePath[n] = '\0'; }
  else gOfsUpPhasePath[0] = '\0';
  if (gOfsUpPhaseCb) gOfsUpPhaseCb(phase, p);
}

// 上传流回调（4 参路由: server.on("/fs/edit", HTTP_POST, done, cb) 才能逐块收到 upload.status;
// onNotFound 分发无 _ufn, canUpload=false, 收不到上传块——core 3.1.2 实测）
static void handleOfsEditUploadCb() {
  ESP8266WebServer &s = srv();
  HTTPUpload &upload = s.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OFS_UP_START filename=%s\n", upload.filename.c_str());
    ofsUpReset();
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    if (!normalizeApiPath(filename.c_str(), ofsUpPath, sizeof(ofsUpPath))) { ofsUpFail(400, "BAD PATH"); return; }
    if (strcmp(ofsUpPath, "/") == 0) { ofsUpFail(400, "BAD PATH"); return; }
    if (isProtectedPath(ofsUpPath)) { ofsUpFail(403, "protected"); return; }
    if (!reinitSdBus("fs_upopen")) { ofsUpFail(500, "FS INIT ERROR"); return; }
    ofsUpFile = SD.open(ofsUpPath, "w");
    if (!ofsUpFile) { ofsUpFail(500, "创建失败"); return; }
    Serial.printf("OFS_UP_START %s heap=%u\n", ofsUpPath, (unsigned)ESP.getFreeHeap());
    ofsUpReport(OFS_UP_PHASE_UPLOADING);   // ★ 上传开始 → 墨水屏"上传中"
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!ofsUpFile || ofsUpErr) return;
    // ★ 官方纯流式(WEBServer.ino:327): core 每块 upload.buf 直接 write, 零攒缓冲。
    // 对齐官方 handleFileUpload; 减少中间状态+堆。write 返回 != 写入长度 = 空间不足/写入失败。
    size_t bytesWritten = ofsUpFile.write(upload.buf, upload.currentSize);
    if (bytesWritten != upload.currentSize) {
      // 写入失败(空间不足等): 删半成品 + 标记错误(官方 LittleFS.remove + replyServerError)
      ofsUpFile.close(); ofsUpFile = File();
      if (ofsUpPath[0] && SD.exists(ofsUpPath)) SD.remove(ofsUpPath);
      ofsUpFail(507, "存储空间不足");
      Serial.printf("OFS_UP_WRITE_FAIL path=%s wrote=%u cur=%u\n",
                    ofsUpPath, (unsigned)bytesWritten, (unsigned)upload.currentSize);
      return;
    }
    ofsUpSize += bytesWritten;
    yield();
    ESP.wdtFeed();
  } else if (upload.status == UPLOAD_FILE_END) {
    // WRITE 已直接 write 落盘; END 只关文件 + 空文件校验(官方 WEBServer.ino:356-377)
    if (ofsUpFile && !ofsUpErr) {
      ofsUpFile.close();
    }
    if (ofsUpErr == 0 && ofsUpSize == 0) {
      SD.remove(ofsUpPath);
      ofsUpFail(500, "上传失败，空文件或存储空间不足");
    }
    Serial.printf("OFS_UP_END %s size=%llu err=%d\n", ofsUpPath,
                  (unsigned long long)ofsUpSize, ofsUpErr);
    // ★ 上传结束 → 墨水屏"上传完毕"(成功)  失败已在 done 报 FAIL
    if (ofsUpErr == 0) ofsUpReport(OFS_UP_PHASE_DONE);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    // ↑ 关键: 上传中断(AP 断连/超时)时 core 只调 upload(ABORTED), 不调 done!
    // 此前无 ABORTED 分支 → 无响应 → 前端一直转圈。必须在此清理 + 发响应。
    Serial.printf("OFS_UP_ABORTED path=%s size=%llu\n", ofsUpPath, (unsigned long long)ofsUpSize);
    ofsUpReport(OFS_UP_PHASE_FAIL);   // ★ 上传中止/失败 → 墨水屏"上传失败"（须在 reset 前, 拿 path）
    if (ofsUpFile) { ofsUpFile.close(); ofsUpFile = File(); }
    if (ofsUpPath[0] && SD.exists(ofsUpPath)) SD.remove(ofsUpPath);   // 清理半成品
    ofsUpReset();
    ofsReply(500, "upload_aborted");
  }
}

// 上传完成回调（发送最终响应）
static void handleOfsEditUploadDone() {
  Serial.printf("OFS_UP_DONE err=%d code=%d path=%s heap=%u\n", ofsUpErr, ofsUpErrCode, ofsUpPath, (unsigned)ESP.getFreeHeap());
  if (ofsUpErr == 0) {
    char parent[300];
    ofsParent(ofsUpPath, parent, sizeof(parent));
    fsCacheInvalidateDir(parent);   // 上传后刷新缓存, 防 /fs/list 读旧快照缺新文件
    ofsReplyOKWithMsg("上传成功");
    // 成功: 上传中已由 END 报 DONE; 但若空文件校验失败(END 里 ofsUpFail)或 START 就失败,
    // 需在此补报 FAIL（END 未报 DONE 时）。此处 ofsUpErr==0 → 已 DONE, 无需再报。
    return;
  }
  int code = ofsUpErrCode ? ofsUpErrCode : 500;
  ofsReply(code, ofsUpErrMsg);
  ofsUpReport(OFS_UP_PHASE_FAIL);   // ★ 上传失败(空文件/空间不足/路径错) → 墨水屏"上传失败"
  ofsUpReset();
}

// 4 参路由注册点（fileApiInit 调用）: /fs/edit POST 上传必须注册为真实路由（见上）
void fileApiFsRegisterUploadRoute() {
  srv().on("/fs/edit", HTTP_POST, handleOfsEditUploadDone, handleOfsEditUploadCb);
  Serial.println(F("OFS_UP_ROUTE_REGISTERED"));
}

// ---- GET /fs/edit: 官方 handleGetEdit 语义 → manager 页（LittleFS /manager.htm）----
static void handleOfsEditGet() {
  ESP8266WebServer &s = srv();
  // 复用 /fm 的共享懒挂载（只 begin 一次）; 每请求 LittleFS.begin() 各 ~1KB 峰值会打穿 3-4KB 配网堆
  if (!fileApiEnsureLfsMount()) { s.send(500, "text/plain; charset=utf-8", "LittleFS mount failed"); return; }
  File f = LittleFS.open("/manager.htm", "r");
  if (!f) { s.send(404, "text/plain; charset=utf-8", "manager page not found"); return; }
  s.streamFile(f, "text/html");
  f.close();
}

// ---- GET /fs/file?path=: 流式读 + ?download=true 下载（官方 GET <path> 语义）----
static void handleOfsFile() {
  ESP8266WebServer &s = srv();
  String pathArg = s.arg("path");
  if (pathArg.length() == 0) { ofsReplyBadRequest("BAD PATH"); return; }
  char path[300];
  if (!normalizeApiPath(pathArg.c_str(), path, sizeof(path))) { ofsReplyBadRequest("BAD PATH"); return; }
  if (isUploadingTemp(path)) { ofsReply(403, "upload_in_progress"); return; }
  if (!reinitSdBus("fs_file")) { ofsReply(500, "FS INIT ERROR"); return; }
  File f = SD.open(path, FILE_READ);
  if (!f) { ofsReply(404, "FILE NOT FOUND"); return; }
  if (f.isDirectory()) { f.close(); ofsReplyBadRequest("BAD PATH"); return; }
  uint64_t size = (uint64_t)f.size();
  const char *mime = s.hasArg("download") ? "application/octet-stream" : ofsContentType(path);
  // 下载文件名（manager.htm 走 /fs/file?download=true）: 此前无 Content-Disposition →
  // 浏览器用 URL 默认名 "file" 且无扩展名。加 filename=<basename> + filename*=UTF-8''<RFC3986>。
  if (s.hasArg("download")) {
    const char *bname = path;
    for (const char *p = path; *p; p++) { if (*p == '/') bname = p + 1; }
    char plain[256], enc[300], disp[700];
    snprintf(plain, sizeof(plain), "%s", bname);
    // RFC3986 percent-encode（中文/空格文件名）
    size_t oi = 0;
    for (const unsigned char *p = (const unsigned char*)bname; *p && oi + 3 < sizeof(enc); p++) {
      unsigned char c = *p;
      if (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~') enc[oi++] = (char)c;
      else { oi += snprintf(enc+oi, sizeof(enc)-oi, "%%%02X", c); }
    }
    enc[oi] = '\0';
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"; filename*=UTF-8''%s", plain, enc);
    s.sendHeader("Content-Disposition", disp);
  }
  s.sendHeader("Connection", "close");
  s.setContentLength((size_t)size);
  s.send(200, mime, "");
  s.client().setNoDelay(true);
  uint8_t *buf = (uint8_t *)malloc(4096);
  if (!buf) { f.close(); return; }
  uint64_t remaining = size;
  while (remaining > 0) {
    // ★★ 128MB 下载 WDT 复位根因修复(2026-08-29 二修):
    //  一修只加 yield() 放 write 后 — 无效: WiFiClient::write 在 TCP 发送缓冲满时会
    //  阻塞等待 ACK (AP 模式吞吐 ~0.19MB/s, 缓冲常满), 阻塞期间不喂狗 → 硬件 WDT 8s 复位。
    //  对齐官方 Stream::sendGeneric (StreamSend.cpp): 先查 availableForWrite(),
    //  缓冲满则 yield 让 TCP 栈处理 ACK 释放缓冲, write 永不阻塞; 每轮 yield+wdtFeed 双喂。
    // ★★ 四修(2026-08-29): 三修加的 zeroAvailCount>100 break 误伤大文件慢速下载 —
    //  AP 模式 0.19MB/s 下 availableForWrite() 可连续 100+ 轮为 0(缓冲满等 ACK), break 致
    //  传输中途中断(Chrome 12s / PowerShell 1MB 均如此)。只保留 connected() 断连检测
    //  (客户端真正断开才退出), avail==0 时继续 yield+wdtFeed 等缓冲, 不设轮数上限。
    if (!s.client().connected()) { break; }
    size_t avail = s.client().availableForWrite();
    if (avail == 0) {
      yield();
      ESP.wdtFeed();
      continue;
    }
    size_t want = (size_t)(remaining > 4096 ? 4096 : remaining);
    if (want > avail) want = avail;
    int n = f.read(buf, want);
    if (n <= 0) break;
    if (s.client().write(buf, (size_t)n) != (size_t)n) break;
    remaining -= (uint64_t)n;
    yield();
    ESP.wdtFeed();
  }
  free(buf);
  f.close();
}

// ---- 分发（前缀匹配 /fs/*; 零路由对象堆）----
bool fileApiFsTryDispatch() {
  ESP8266WebServer &s = srv();
  String uri = s.uri();
  if (!uri.startsWith("/fs/")) return false;
  HTTPMethod m = s.method();
  if (uri == "/fs/status" && m == HTTP_GET) { handleOfsStatus(); return true; }
  if (uri == "/fs/list" && m == HTTP_GET) { handleOfsList(); return true; }
  if (uri == "/fs/edit" && m == HTTP_GET) { handleOfsEditGet(); return true; }
  if (uri == "/fs/edit" && m == HTTP_PUT) { handleOfsEditPut(); return true; }
  if (uri == "/fs/edit" && m == HTTP_DELETE) { handleOfsEditDelete(); return true; }
  if (uri == "/fs/file" && m == HTTP_GET) { handleOfsFile(); return true; }
  // POST /fs/edit 上传由 fileApiFsRegisterUploadRoute 的 4 参路由处理（onNotFound 收不到上传块）
  ofsReply(404, "not_found");
  return true;
}

// ---- 对照实验探针（验证根因）-------------------------------------------------
// 与 /fs 调用链无关的最小反事实（用户拍板: 先做对照实验, 不直接接受"AP+SD 天生不可行"）。
// 每次在 AP_ONLY 定时区调用一次; 按编译期 OFS_ABTEST_MODE 选择动作, 全程只打
// ABSAM <kind> heap=<u> maxblk=<u> stack=<u> 一行（复用 fsListProbe 同口径）。
// 编译固件实验版时: build 命令行加 -DOFS_ABTEST_MODE=<0|1|2>; 默认 0(不碰 SD 的对照组)。
// 注意: 实验版仅供采集对照曲线; 数据采完必须去掉该宏（避免周期性 SD 访问常驻 AP 会话）。
#ifndef OFS_ABTEST_MODE
#define OFS_ABTEST_MODE 0
#endif

// mode 1 的极小 SD 读回调: 打开 /test.txt（或无则读根目录首个文件的前 16B）, 计数后丢弃
static void ofsAbReadTinyCb(const SdEntry *e, void *ctx) {
  (void)e; (void)ctx;
}

// 打印一行采样（kind: 0=no-sd 1=tiny-sd 2=lst-sd）
static void ofsAbSample(int kind) {
  Serial.printf("ABSAM kind=%d heap=%u maxblk=%u stack=%u\n", kind,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
                (unsigned)ESP.getFreeContStack());
}

void ofsAbTestTick() {
#if OFS_ABTEST_MODE == 1
  // 实验组 B: 极小 SD 读（严格单变量: 唯一新增 = "SD 访问"; 不复用 reinitSdBus, 因 reinit 是
  // "恢复 SPI 总线"机制而非"SD 访问"本身, 且 setup 已 SD.begin 挂载保持 sdAvailable=true）。
  // PHP: SD 若已卸载 SD.exists 也会触发底层探测——若返回 false 打 SD_NOMNT, 单凭这点即知 SD 状态。
  {
    File f = SD.open("/test.txt", FILE_READ);
    if (f) {
      uint8_t tmp[16];
      f.read(tmp, sizeof(tmp));
      f.close();
      ofsAbSample(1);
    } else {
      // /test.txt 不在: 读 SD 根目录首个文件前 16B（等价的极小 SD 访问, 单变量不变）
      Dir root = SDFS.openDir("/");
      if (root.next()) {
        File ef = root.openFile("r");
        if (ef) { uint8_t tmp[16]; ef.read(tmp, sizeof(tmp)); ef.close(); }
        ofsAbSample(1);
      } else {
        ofsAbSample(1);   // SD 目录打不开也采样, 记录此刻堆/块/栈
      }
    }
  }
#elif OFS_ABTEST_MODE == 2
  // 实验组 C: 最接近 /fs/list 的 SD 访问（sdListDirPaged 前 10 项, 无 HTTP/JSON/chunked/sendContent）
  if (reinitSdBus("abtest_lst")) {
    size_t emitted = 0; bool hasMore = false;
    sdListDirPaged("/", 0, 10, ofsAbReadTinyCb, NULL, &emitted, &hasMore);
    (void)hasMore;
    ofsAbSample(2);
  }
#else
  // 实验组 A: 对照组, 完全不碰 SD（只记录 AP + 手机连接下的基线）
  ofsAbSample(0);
#endif
}

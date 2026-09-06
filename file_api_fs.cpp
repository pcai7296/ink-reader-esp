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
  Serial.printf_P(PSTR("FSREQ #%lu %s heap=%u maxblk=%u stack=%u\n"),
                (unsigned long)gFsListReqNo, tag,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
                (unsigned)ESP.getFreeContStack());
}
uint32_t fsListReqNext() { return ++gFsListReqNo; }   // 下一个请求序号（首次调用返回 1）

// 带分页明细的探针（FSREQ_DONE 用）: 额外显示 start/count/emitted/more
void fsListProbeDetail(const char *tag, size_t start, size_t count, size_t emitted, bool hasMore) {
  Serial.printf_P(PSTR("FSREQ #%lu %s heap=%u maxblk=%u stack=%u start=%u count=%u emitted=%u more=%d\n"),
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

// ---- 显示过滤（Web 文件管理; 纯栈/strcasecmp 零 String 分配）----
// 2026-09 机制调整(用户拍板): 由"文件后缀白名单(.txt/.bmp)"改为"黑名单机制"——
//   Web 默认隐藏"设备自动生成"的后缀(索引/章节/标签/续建 sidecar), 其余用户文件(含 .bin/.ttf 等)
//   一律显示; Web 页可切换 hideAuto(0=连自动文件也显示, 默认 1=隐藏)。
//   硬隐藏(任何开关都不可见): 隐藏名(.开头) + SD 系统目录。
static bool ofsIsAutoGenSuffix(const char *dot) {
  static const char *const suffixes[] = {
    ".i1", ".z1", ".i2", ".z2", ".v1", ".vz1", ".i1p", ".v1p", ".bm", ".bmt", nullptr
  };
  for (size_t i = 0; suffixes[i]; i++) {
    if (strcasecmp(dot, suffixes[i]) == 0) return true;
  }
  return false;
}

// 硬隐藏（无论开关）: 隐藏名(.开头) + SD 系统目录
static bool ofsHardHidden(const char *name) {
  if (!name || !name[0]) return true;
  if (name[0] == '.') return true;
  const char *blocked[] = {
    "android", "androud", "found.000", "foud.000", "lost.dir", "system volume information"
  };
  for (const char *item : blocked) {
    if (strcasecmp(name, item) == 0) return true;
  }
  return false;
}

static bool ofsAutoGenFile(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot || !dot[1]) return false;
  return ofsIsAutoGenSuffix(dot);
}

// ---- 可见性单一入口（A 修复延续; 2026-09 黑名单机制 + hideAuto 开关）----
// hideAuto=true: 隐藏设备自动生成后缀文件(索引/章节/标签等); false: 全显示(除硬隐藏)。
// 调用方: /fs/list 实时列表、fsCacheBuild(存超集, hideAuto=false)、fsCacheServeList(按请求开关过滤)。
bool ofsEntryVisibleEx(const char *name, bool isDir, bool hideAuto) {
  if (ofsHardHidden(name)) return false;
  if (isDir) return true;
  if (hideAuto && ofsAutoGenFile(name)) return false;
  return true;
}

// 旧名保留(默认隐藏自动文件) —— 供既有调用点编译过渡
bool ofsEntryVisible(const char *name, bool isDir) {
  return ofsEntryVisibleEx(name, isDir, true);
}

// /fs/list 实时路径 scratch(深链实测 stack=0, 大局部一律 static; 单线程顺序安全)
static char gLsPath[300];

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

struct OfsListCtx { bool first; bool hideAuto; };
static void ofsListItemCb(const SdEntry *e, void *ctx) {
  OfsListCtx *lc = (OfsListCtx *)ctx;
  // 不该显示的不显示: 与缓存同一入口 ofsEntryVisibleEx(hideAuto=请求开关) —— 视图一致
  // 过滤项不发内容, 不翻转 first（保持输出 JSON 连续）
  if (!ofsEntryVisibleEx(e->name, e->isDir, lc->hideAuto)) return;
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
  // ---- 自动文件开关: auto=0 显示设备自动生成文件; 缺省/auto=1 隐藏(黑名单默认) ----
  bool hideAuto = !(s.hasArg("auto") && s.arg("auto") == "0");

  fsListReqNext();
  fsListProbe("FSREQ_ENTER");   // 探针: 请求入口
  if (!s.hasArg("dir")) { ofsReplyBadRequest("DIR ARG MISSING "); return; }
  String dirArg = s.arg("dir");
  if (dirArg.length() == 0) dirArg = "/";
  char *path = gLsPath;   // static: 删除/改动后自动刷新走实时 SD 列表时深链 stack=0, 栈局部必崩
  if (!normalizeApiPath(dirArg.c_str(), path, 300)) { ofsReplyBadRequest("BAD PATH"); return; }

  // ★ 浏览走 LittleFS 缓存（进 AP 前 fsCacheBuild 扫描好的 SD 目录树; 照抄官方"文件管理用 LittleFS"）。
  //   避开"配网会话实时遍历 SD × AP hostap_input"的 esf_buf_alloc 竞争（崩溃根因）。
  //   缓存不存在（扫描失败/超上限目录）→ 回退直接读 SD（保留原路径, 不静默 500）。
  if (fsCacheServeList(path, start, count, hideAuto)) {
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
  OfsListCtx lc = {true, hideAuto};
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

// ---- PUT/DELETE 改路径 handler 的栈瘦身(2026-09) ----
// 实测: 这些 handler 处于 WiFi/HTTP 深链时剩余连续栈仅 0~100B; 任何 ≥~150B 栈局部(原 path[300]+
// lab/d/t/s2 等)都会栈溢出 → Exception 2 / Soft WDT / 系统重启(串口多次实锤)。单线程顺序处理、
// handler 不可重入 → 用文件级 static 暂存(参照 handleOfsFile path 的做法), 零堆分配。
static char gOpPath[300];   // 主路径
static char gOpA[300];      // src / s2
static char gOpB[300];      // 目标 t / 临时
static char gOpC[300];      // 父目录等
static char gOpLab[220];    // 通知文案

// ---- PUT /fs/edit: 建文件/夹（无 src）或 重命名/移动（有 src）----
static void handleOfsEditPut() {
  ESP8266WebServer &s = srv();
  String pathArg = s.arg("path");
  if (pathArg.length() == 0) { ofsReplyBadRequest("PATH ARG MISSING"); return; }
  char *path = gOpPath;
  if (!normalizeApiPath(pathArg.c_str(), path, 300)) { ofsReplyBadRequest("BAD PATH"); return; }
  if (strcmp(path, "/") == 0) { ofsReplyBadRequest("BAD PATH"); return; }
  if (!reinitSdBus("fs_put")) { ofsReply(500, "FS INIT ERROR"); return; }

  String srcArg = s.arg("src");
  if (srcArg.length() == 0) {
    if (SD.exists(path)) { ofsReplyBadRequest("PATH FILE EXISTS"); return; }
    if (isProtectedPath(path)) { ofsReply(403, "protected"); return; }
    size_t plen = strlen(path);
    bool isDir = (plen > 0 && path[plen - 1] == '/');
    char *lab = gOpLab;
    if (isDir) {
      char *d = gOpB;
      snprintf(d, 300, "%s", path);
      d[strlen(d) - 1] = '\0';
      snprintf(lab, 220, "新建文件夹:%s", d);
      ofsOpReport(OFS_OP_PHASE_START, lab);
      ESP.wdtFeed();
      if (!SD.mkdir(d)) { ofsOpReport(OFS_OP_PHASE_FAIL, lab); ofsReply(500, "MKDIR FAILED"); return; }
      ESP.wdtFeed();
      ofsOpReport(OFS_OP_PHASE_DONE, lab);
      char *parent = gOpC;
      ofsParent(d, parent, 300);
      if (parent[0] == '\0') strcpy(parent, "/");
      fsCacheInvalidateDir(parent);   // ★ 新建夹后失效父缓存, /fs/list 立即可见(否则 stale 隐藏新项)
      ofsReplyOKWithMsg(parent);
      return;
    } else {
      snprintf(lab, 220, "新建:%s", path);
      ofsOpReport(OFS_OP_PHASE_START, lab);
      ESP.wdtFeed();
      File f = SD.open(path, "w");
      if (!f) { ofsOpReport(OFS_OP_PHASE_FAIL, lab); ofsReply(500, "CREATE FAILED"); return; }
      f.write((const char *)0);           // Print::write 有 NULL 保护, 等价建空文件
      f.close();
      ESP.wdtFeed();
      ofsOpReport(OFS_OP_PHASE_DONE, lab);
    }
    char *parent = gOpC;
    ofsParent(path, parent, 300);
    if (parent[0] == '\0') strcpy(parent, "/");
    fsCacheInvalidateDir(parent);   // ★ 新建后失效父缓存, /fs/list 立即可见
    ofsReplyOKWithMsg(parent);
    return;
  }

  char *src = gOpA;
  if (!normalizeApiPath(srcArg.c_str(), src, 300)) { ofsReplyBadRequest("BAD SRC"); return; }
  if (strcmp(src, "/") == 0) { ofsReplyBadRequest("BAD SRC"); return; }
  if (!SD.exists(src)) { ofsReply(404, "SRC FILE NOT FOUND"); return; }
  if (isProtectedPath(src) || isProtectedPath(path)) { ofsReply(403, "protected"); return; }
  char *t = gOpB;
  snprintf(t, 300, "%s", path);
  size_t tl = strlen(t); if (tl > 1 && t[tl - 1] == '/') t[tl - 1] = '\0';
  char *s2 = src;                          // src 用后即弃, 就地除尾斜杠作 s2
  size_t sl = strlen(s2); if (sl > 1 && s2[sl - 1] == '/') s2[sl - 1] = '\0';
  if (strcmp(s2, t) == 0) { ofsReplyBadRequest("PATH FILE EXISTS"); return; }
  if (SD.exists(t)) { ofsReplyBadRequest("PATH FILE EXISTS"); return; }
  char *lab = gOpLab;
  snprintf(lab, sizeof(gOpLab), "重命名/移动:%s → %s", s2, t);
  ofsOpReport(OFS_OP_PHASE_START, lab);
  ESP.wdtFeed();
  if (!SD.rename(s2, t)) { ofsOpReport(OFS_OP_PHASE_FAIL, lab); ofsReply(500, "RENAME FAILED"); return; }
  ESP.wdtFeed();
  ofsOpReport(OFS_OP_PHASE_DONE, lab);
  char *parent = gOpC, *srcParent = gOpPath;   // gOpPath 的 path 已用完
  ofsLastExistingParent(s2, parent, 300);
  ofsParent(t, srcParent, 300);                // 目标目录（可能跨目录移动）
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
  char *path = gOpPath;
  if (!normalizeApiPath(pathArg.c_str(), path, 300)) { ofsReplyBadRequest("BAD PATH"); return; }
  if (strcmp(path, "/") == 0) { ofsReplyBadRequest("BAD PATH"); return; }
  if (!reinitSdBus("fs_del")) { ofsReply(500, "FS INIT ERROR"); return; }
  if (isProtectedPath(path)) { ofsReply(403, "protected"); return; }
  if (!SD.exists(path)) { ofsReply(404, "FILE NOT FOUND"); return; }
  // ★ 通用操作墨水屏通知: 动手删除前 START, 成/败 DONE/FAIL（渲染在 loop, 此处只记录）
  char *lab = gOpLab;
  snprintf(lab, sizeof(gOpLab), "删除:%s", path);
  ofsOpReport(OFS_OP_PHASE_START, lab);
  ESP.wdtFeed();
  int count = 0;
  if (!ofsDeleteRecursive(path, 0, &count)) { ofsOpReport(OFS_OP_PHASE_FAIL, lab); ofsReply(500, "DELETE FAILED"); return; }
  ESP.wdtFeed();
  ofsOpReport(OFS_OP_PHASE_DONE, lab);
  char *parent = gOpC;
  ofsLastExistingParent(path, parent, 300);
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

// ---- 下载墨水屏状态（/fs/file?download=true 下载起止上报; 渲染层显示"下载中/下载完毕/下载失败"）----
// ⚠️ 静态瘦身(2026-09 Step B): 上报→渲染层回调是同步的, 渲染只用回调入参 path, 从不查存储的
// 路径 → 删 gOfsDlPhasePath[300] 及 getter（无任何消费者）。保留 phase int 供将来对称 upload 的
// "结束后回配网页"轮询。若以后需要断点续传 UI 查路径, 再按需加回, 勿常驻。
static OfsDlPhaseCallback gOfsDlPhaseCb = NULL;
static int gOfsDlPhase = OFS_DL_PHASE_IDLE;

void ofsDlSetPhaseCallback(OfsDlPhaseCallback cb) { gOfsDlPhaseCb = cb; }
int  ofsDlGetPhase() { return gOfsDlPhase; }

void ofsDlReport(int phase, const char *path) {
  if (phase == gOfsDlPhase && phase != OFS_DL_PHASE_START) return;   // 除"下载中"外去重
  gOfsDlPhase = phase;
  if (gOfsDlPhaseCb) gOfsDlPhaseCb(phase, path);
}

// ---- 通用文件管理操作墨水屏状态（/fs 新建/夹/删除/重命名/移动, 参照上传/下载同款上报）----
static OfsOpPhaseCallback gOfsOpPhaseCb = NULL;
static int gOfsOpPhase = OFS_OP_PHASE_IDLE;
void ofsOpSetPhaseCallback(OfsOpPhaseCallback cb) { gOfsOpPhaseCb = cb; }
int  ofsOpGetPhase() { return gOfsOpPhase; }
void ofsOpReport(int phase, const char *msg) {
  if (phase == gOfsOpPhase && phase != OFS_OP_PHASE_START) return;   // 除"操作中"外去重
  gOfsOpPhase = phase;
  if (gOfsOpPhaseCb) gOfsOpPhaseCb(phase, msg);
}

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
    Serial.printf_P(PSTR("OFS_UP_START filename=%s\n"), upload.filename.c_str());
    ofsUpReset();
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    if (!normalizeApiPath(filename.c_str(), ofsUpPath, sizeof(ofsUpPath))) { ofsUpFail(400, "BAD PATH"); return; }
    if (strcmp(ofsUpPath, "/") == 0) { ofsUpFail(400, "BAD PATH"); return; }
    if (isProtectedPath(ofsUpPath)) { ofsUpFail(403, "protected"); return; }
    if (!reinitSdBus("fs_upopen")) { ofsUpFail(500, "FS INIT ERROR"); return; }
    ofsUpFile = SD.open(ofsUpPath, "w");
    if (!ofsUpFile) { ofsUpFail(500, "创建失败"); return; }
    Serial.printf_P(PSTR("OFS_UP_START %s heap=%u\n"), ofsUpPath, (unsigned)ESP.getFreeHeap());
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
      Serial.printf_P(PSTR("OFS_UP_WRITE_FAIL path=%s wrote=%u cur=%u\n"),
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
    Serial.printf_P(PSTR("OFS_UP_END %s size=%llu err=%d\n"), ofsUpPath,
                  (unsigned long long)ofsUpSize, ofsUpErr);
    // ★ 上传结束 → 墨水屏"上传完毕"(成功)  失败已在 done 报 FAIL
    if (ofsUpErr == 0) ofsUpReport(OFS_UP_PHASE_DONE);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    // ↑ 关键: 上传中断(AP 断连/超时)时 core 只调 upload(ABORTED), 不调 done!
    // 此前无 ABORTED 分支 → 无响应 → 前端一直转圈。必须在此清理 + 发响应。
    Serial.printf_P(PSTR("OFS_UP_ABORTED path=%s size=%llu\n"), ofsUpPath, (unsigned long long)ofsUpSize);
    ofsUpReport(OFS_UP_PHASE_FAIL);   // ★ 上传中止/失败 → 墨水屏"上传失败"（须在 reset 前, 拿 path）
    if (ofsUpFile) { ofsUpFile.close(); ofsUpFile = File(); }
    if (ofsUpPath[0] && SD.exists(ofsUpPath)) SD.remove(ofsUpPath);   // 清理半成品
    ofsUpReset();
    ofsReply(500, "upload_aborted");
  }
}

// 上传完成回调（发送最终响应）
static void handleOfsEditUploadDone() {
  Serial.printf_P(PSTR("OFS_UP_DONE err=%d code=%d path=%s heap=%u\n"), ofsUpErr, ofsUpErrCode, ofsUpPath, (unsigned)ESP.getFreeHeap());
  if (ofsUpErr == 0) {
    char parent[300];
    ofsParent(ofsUpPath, parent, sizeof(parent));
    fsCacheInvalidateDir(parent);   // 上传后刷新缓存, 防 /fs/list 读旧快照缺新文件
    // ★ 修复(2026-09, 无头浏览器定性): 上传成功响应体 = "父目录路径"(与 PUT 创建/改名、DELETE
    //   同契约)。此前回人类文案 "上传成功" → manager.htm onOperationComplete 把响应文本当父目录
    //   路径去 httpList → GET /fs/list?dir=上传成功 → 400 BAD PATH —— 用户看到的"上传 400"
    //   (上传本身成功, 文件已落盘; 该 400 只是误列目录)。根级文件父目录为 "" → 归一 "/"。
    if (parent[0] == '\0') strcpy(parent, "/");
    ofsReplyOKWithMsg(parent);
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
  // ★ 栈/静态权衡(2026-09 演进): ①path+plain+enc+disp 原共 ~1556B 栈上局部 → 4KB loop 栈
  //   下载路径 FSDL 低水位被压到 80B(近溢出)→ 长传输偶发 Exception 29; ②Step B(2026-09):
  //   plain/enc/disp 1,256B 改为请求级 heap、sendHeader 后立即 free(主体传输阶段不存在);
  //   path[300] 需贯穿 handler(含 START/DONE 上报), 保留 static——单线程顺序处理、
  //   handler 不可重入/不递归, static 安全; ③绝不把这 1.5K 放回栈(实测 Ex29)。
  static char path[300];
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
    // ★ 生命周期受控(2026-09 Step B): plain/enc/disp 1,256B 仅构建 Content-Disposition 头时
    //   短暂需要 → 从 static(常驻 1,256B BSS)改请求级 malloc, sendHeader 后立即 free,
    //   下载主体传输阶段不存在(峰值仅 header 构建瞬间 +1.2K, 低于 512B 传输缓冲的常驻意义)。
    //   绝不回 4KB 栈(实测 80B 近溢出 Ex29)。malloc 失败 → 降级为不发 filename 头
    //   (浏览器用 URL 默认名, 传输不受影响), 记 FSDL_HDR_ALLOC_FAIL。
    char *hdr = (char *)malloc(256 + 300 + 700);
    if (!hdr) {
      Serial.printf_P(PSTR("FSDL_HDR_ALLOC_FAIL heap=%u maxblk=%u\n"),
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
    } else {
      char *plain = hdr, *enc = hdr + 256, *disp = hdr + 256 + 300;
      const char *bname = path;
      for (const char *p = path; *p; p++) { if (*p == '/') bname = p + 1; }
      snprintf(plain, 256, "%s", bname);
      // RFC3986 percent-encode（中文/空格文件名）
      size_t oi = 0;
      for (const unsigned char *p = (const unsigned char*)bname; *p && oi + 3 < 300; p++) {
        unsigned char c = *p;
        if (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~') enc[oi++] = (char)c;
        else { oi += snprintf(enc + oi, 300 - oi, "%%%02X", c); }
      }
      enc[oi] = '\0';
      snprintf(disp, 700, "attachment; filename=\"%s\"; filename*=UTF-8''%s", plain, enc);
      s.sendHeader("Content-Disposition", disp);
      free(hdr);   // ★ 头已发出, 立即释放
    }
  }
  s.sendHeader("Connection", "close");
  s.setContentLength((size_t)size);
  s.send(200, mime, "");
  s.client().setNoDelay(true);
  bool isDownload = s.hasArg("download");
  if (isDownload) ofsDlReport(OFS_DL_PHASE_START, path);   // ★ 下载开始 → 墨水屏"下载中"
  // ★ 2026-09 传输缓冲自适应: 原固定 malloc(4096) 在碎片堆(maxblk<4096, 实测 3848)下 START 即
  //   FSDL_MALLOC_FAIL → 0B 下载。勿用 static(4KB BSS 吃掉堆基线, AP 空闲仅 ~2K 连 status GET 都
  //   Exception 29 实测)。改为 4096→2048→1024 降级(仅 START 选一次尺寸, 传输循环语义不变;
  //   小缓冲还给 lwIP TX 留更多堆, 下载吞吐本就受 TCP 窗口 ~0.2MB/s 限制, 不受块大小影响)。
  static const size_t kBufCandidates[] = {512, 256, 128};
  size_t bufSize = 0;
  uint8_t *buf = NULL;
  for (size_t cand : kBufCandidates) {
    buf = (uint8_t *)malloc(cand);
    if (buf) { bufSize = cand; break; }
  }
  if (!buf) {
    Serial.printf_P(PSTR("FSDL_MALLOC_FAIL heap=%u maxblk=%u size=%llu\n"),
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
                  (unsigned long long)size);
    if (isDownload) ofsDlReport(OFS_DL_PHASE_FAIL, path);
    f.close();
    return;
  }
  Serial.printf_P(PSTR("FSDL_BUF buf=%u heap=%u maxblk=%u size=%llu\n"), (unsigned)bufSize,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
                (unsigned long long)size);
  uint64_t remaining = size;
  uint32_t lastProbe = millis();
  uint32_t lastByte = 0;
  uint32_t lastProbeBytes = 0;
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
    if (!s.client().connected()) {   // 客户端断开: 中止
      Serial.printf_P(PSTR("FSDL_DISCONNECTED sent=%llu remain=%llu\n"),
                    (unsigned long long)(size - remaining), (unsigned long long)remaining);
      break;
    }
    size_t avail = s.client().availableForWrite();
    if (avail == 0) {
      // 缓冲满等 ACK: 5s 无进展探针(定位 STA 下载无速度: 是否 avail 长期为 0 = TCP 不推进)
      if (millis() - lastProbe >= 5000) {
        Serial.printf_P(PSTR("FSDL_STALL sent=%llu heap=%u\n"),
                      (unsigned long long)(size - remaining), (unsigned)ESP.getFreeHeap());
        lastProbe = millis();
      }
      yield();
      ESP.wdtFeed();
      continue;
    }
    size_t want = (size_t)(remaining > bufSize ? bufSize : remaining);
    if (want > avail) want = avail;
    int n = f.read(buf, want);
    if (n <= 0) {
      Serial.printf_P(PSTR("FSDL_READ_FAIL at=%llu\n"), (unsigned long long)(size - remaining));
      break;
    }
    if (s.client().write(buf, (size_t)n) != (size_t)n) {
      Serial.printf_P(PSTR("FSDL_WRITE_FAIL at=%llu\n"), (unsigned long long)(size - remaining));
      break;
    }
    remaining -= (uint64_t)n;
    lastProbeBytes += (uint32_t)n;
    if (millis() - lastProbe >= 5000) {
      Serial.printf_P(PSTR("FSDL_PROGRESS rate=%uB/s sent=%llu heap=%u\n"), lastProbeBytes / 5,
                    (unsigned long long)(size - remaining), (unsigned)ESP.getFreeHeap());
      lastProbe = millis();
      lastProbeBytes = 0;
    }
    yield();
    ESP.wdtFeed();
  }
  Serial.printf_P(PSTR("FSDL_END sent=%llu size=%llu heap=%u\n"),
                (unsigned long long)(size - remaining), (unsigned long long)size,
                (unsigned)ESP.getFreeHeap());
  if (isDownload) ofsDlReport(remaining == 0 ? OFS_DL_PHASE_DONE : OFS_DL_PHASE_FAIL, path);   // ★ 下载完成/中断
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
  Serial.printf_P(PSTR("ABSAM kind=%d heap=%u maxblk=%u stack=%u\n"), kind,
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

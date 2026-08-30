// progress_sync.cpp — 直连手机 HTTP 阅读进度同步器
// 手机 = 进度服务器（监听 8384 端口，响应 LUMI1 文本，明文 HTTP）。
// 局域网：设备 STA 已连 → 直连 192.168.0.10；设备热点模式：纯 AP(192.168.0.1/24)，
// 手机连热点获固定 IP 192.168.0.100 → 直连。
// 协议：GET /progress?file=<RFC3986 编码文件名> → 200+LUMI1 / 404；
//       PUT /progress（body=LUMI1 + file= 行）→ 200/400。
// 本地进度读写与 UI 渲染通过宿主钩子 (progressSyncSnapshot/ApplyRemote/Render/Done, 在 file_manager.ino)。
// 设计: 非阻塞状态机, 每阶段阻塞 ≤6s, 循环内喂狗, 任何失败走 cleanup(WiFi OFF)。
// 旧 WebDAV 云同步已弃用（传输层整体删除；WebdavConfig/EEPROM 保留于 wifi_manager，UI 隐藏）。

#include "progress_sync.h"
#include "progress_lumi.h"
#include "wifi_manager.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <SD.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ---------- 调试 ----------
#define PROG_SYNC_DEBUG 1
#if PROG_SYNC_DEBUG
// 简短的同步调试输出 (Serial @115200, 不影响器件逻辑)
static void syncDbg(const char* fmt, ...) {
  char buf[220];
  va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf("[SYNC][%lu] %s\n", (unsigned long)millis(), buf);
}
static const char* syncStateName(int s) {
  switch (s) {
    case SYNC_IDLE:        return "IDLE";
    case SYNC_PREPARE:     return "PREPARE";
    case SYNC_WIFI:        return "WIFI";
    case SYNC_DISCOVER:    return "DISCOVER";
    case SYNC_WAIT_CLIENT: return "WAIT_CLIENT";
    case SYNC_CONNECT:     return "CONNECT";
    case SYNC_GET:         return "GET";
    case SYNC_PARSE:       return "PARSE";
    case SYNC_COMPARE:     return "COMPARE";
    case SYNC_APPLY:       return "APPLY";
    case SYNC_UPLOAD:      return "UPLOAD";
    case SYNC_FINISH:      return "FINISH";
    case SYNC_ERROR:       return "ERROR";
    default:               return "?";
  }
}
#else
#define syncDbg(...) ((void)0)
#endif

// ---------- 纯工具 ----------
// LUMI1 格式（规范 docs/progress-lumi1.md）：lumiMake / lumiParse / lumiUrlEncodeFilename
// 实现见 progress_lumi.cpp（纯 C++，PC 测试 pc_tests/progress_lumi_test.cpp 交叉核对 App 侧实现）。

// ---------- 同步控制器状态 ----------
static SyncState  gState = SYNC_IDLE;
static String     gTxtPath;          // 目标 TXT 完整路径（快照在 SYNC_PREPARE 执行）
static String     gFilename;         // 文件名（basename，UTF-8）
static String     gTarget;           // 目标手机 IP（wifiManagerSyncTarget()）
static String     gStatus;
static uint32_t   gLocalOffset = 0;
static uint32_t   gLocalSize = 0;
static float      gLocalPercent = 0;
static uint64_t   gSyncStartMs = 0;
static uint32_t   gRemoteOffset = 0;
static uint32_t   gRemoteSize = 0;
static float      gRemotePercent = 0;
static uint64_t   gRemoteTsMs = 0;
static bool       gCloudExists = false;
static int        gCompareSel = 0;        // 0=同步 1=覆盖
static bool       gConfirmUpload = false; // 覆盖二次确认 (防误触: 右长一次=提示, 两次=执行)
static bool       gErrWifiStopped = false;// ERROR 进入后已停 Wi-Fi (省电)
static bool       gStaAttempted = false; // SYNC_WIFI 是否已发起过 STA 连接尝试 (限时 20s 后切热点)
static uint32_t   gDeadline = 0;          // WAIT_CLIENT 15s 超时截止
static uint32_t   gLastPollMs = 0;        // WAIT_CLIENT 500ms 轮询
static uint8_t    gBuf[256];
static uint16_t   gBufLen = 0;
static int        gRespCL = -1;        // 最近一次响应头的 Content-Length, -1=未提供
static int        gConnectAttempt = 0; // SYNC_CONNECT 当前尝试号 0..2 (失败重试 2 次)
static uint32_t   gRetryAtMs = 0;      // 下次连接尝试时刻 (重试间隔 2s)
static bool       gConnectFreed = false; // 本生命周期是否已调用 progressSyncFreeReaderHeap()
static WiFiClient gWiFi;               // 直连手机 HTTP 客户端 (明文)

// ---------- /sync.cfg (SD 根, 可选行: ssid=/password=/ip=; 不触碰 EEPROM/配网页) ----------
static char gCfgSsid[33];
static char gCfgPass[65];
static char gCfgIp[16];

// ---------- UDP 手机发现 (仅 STA 模式: 局域网/手机热点; AP 模式仍用固定 192.168.0.100) ----------
static WiFiUDP  gDiscUdp;
static bool     gDiscStarted = false;
static uint32_t gDiscDeadline = 0;
static uint32_t gDiscPollMs = 0;
static const uint16_t  DISC_PORT = 8390;
static const uint32_t  DISC_TIMEOUT_MS = 2000UL;

// ---------- v3 文件指纹快照 (SyncSnapshot; 原子——任一步失败 gFpValid=false, 整组省略) ----------
enum FingerprintState { FINGERPRINT_UNKNOWN = 0, FINGERPRINT_MATCH = 1, FINGERPRINT_MISMATCH = 2 };
static bool     gFpValid = false;
static uint64_t gFpSize = 0;
static char     gFpH0[41], gFpH1[41], gFpH2[41];
static uint8_t  gFpBuf[1024];
static int      gFileFpState = FINGERPRINT_UNKNOWN;   // 最近一次 PARSE 三态
static uint8_t  gFileMismatch = 0;                    // bit0=size bit1=head bit2=middle bit3=tail

static uint16_t gTargetPort = 8384;  // 手机进度服务器端口 (默认 8384; /sync.cfg port= 可覆盖, 供联调/换端口)

// ---------- 手动 HTTP (单次 TCP 连接, Connection: close, 明文) ----------
// 每个请求独立连接: GET 完成后断开 (服务器按 Connection: close 关闭),
// PUT 前如已断开则重新连接 (带重试)。

static void syncDisconnect() {
  gWiFi.stop();
  syncDbg("DISCONNECT heap=%lu", (unsigned long)ESP.getFreeHeap());
}

// 连接阶段 (SYNC_CONNECT 与 SYNC_UPLOAD 断线重连共用):
// 阻塞 connect ≤5s (WiFiClient 默认 _timeout=5000);
// 失败重试 2 次、间隔 2s (跨 loop 迭代, 每迭代最多阻塞一次 connect)。
// 返回: 1=连接成功, 0=进行中(等重试间隔), -1=彻底失败 (gStatus 已设置)
static int syncConnectPhase() {
  if (!gConnectFreed) {           // 网络阶段前腾堆 (钩子, 只做一次)
    progressSyncFreeReaderHeap();
    gConnectFreed = true;
  }
  if ((int32_t)(millis() - gRetryAtMs) < 0) return 0;   // 重试间隔未到 (2s)
  gRetryAtMs = 0;
  IPAddress tip;
  if (!tip.fromString(gTarget.c_str())) {
    gStatus = "手机地址无效";
    return -1;
  }
  syncDbg("CONNECT attempt=%d target=[%s] port=%u", gConnectAttempt, gTarget.c_str(), (unsigned)gTargetPort);
  gStatus = "正在连接手机…";
  bool ok = gWiFi.connect(tip, gTargetPort);    // 阻塞 ≤5s (默认超时), 失败立即返回
  ESP.wdtFeed();
  if (ok) {
    gWiFi.setTimeout(3000);       // 读超时 3s
    syncDbg("CONNECT ok heap=%lu", (unsigned long)ESP.getFreeHeap());
    return 1;
  }
  syncDbg("CONNECT fail attempt=%d", gConnectAttempt);
  if (gConnectAttempt < 2) {
    gConnectAttempt++;
    gRetryAtMs = millis() + 2000UL;   // 2s 后重试
    return 0;
  }
  return -1;
}

static bool phoneSkipHeaders();   // 前置声明 (本文件下方定义)

// 读响应状态行 "HTTP/1.1 XXX"; 3s 超时; 成功后消费头部并解析 Content-Length → gRespCL
static int phoneReadStatus() {
  String line;
  gRespCL = -1;
  uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < 3000) {
    while (gWiFi.available()) {
      int c = gWiFi.read();
      if (c < 0) break;
      line += (char)c;
      if (c == '\n') {
        int sp1 = line.indexOf(' ');
        int sp2 = line.indexOf(' ', sp1 + 1);
        int code = (sp1 > 0 && sp2 > sp1) ? line.substring(sp1 + 1, sp2).toInt() : -1;
        syncDbg("RSP status=[%s] code=%d", line.c_str(), code);
        if (!phoneSkipHeaders()) return -1;   // 消费该响应剩余头部
        return code;
      }
      if (line.length() > 64) return -1;
    }
    if (!gWiFi.connected() && !gWiFi.available()) break;  // 服务器提前关闭 → 无响应
    delay(1);
    ESP.wdtFeed();
  }
  return -1;
}

// 跳过响应头 (读到 \r\n\r\n 或 \n\n), 解析 Content-Length 存入 gRespCL; 3s 超时
static bool phoneSkipHeaders() {
  String buf;
  uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < 3000) {
    while (gWiFi.available()) {
      int c = gWiFi.read();
      if (c < 0) break;
      buf += (char)c;
      if (buf.length() > 2048) return false;
      if (buf.endsWith("\r\n\r\n") || buf.endsWith("\n\n")) {
        gRespCL = -1;
        int pos = 0;
        while (pos < (int)buf.length()) {
          int eol = buf.indexOf('\n', pos);
          if (eol < 0) eol = buf.length();
          String h = buf.substring(pos, eol);
          int colon = h.indexOf(':');
          if (colon > 0) {
            String key = h.substring(0, colon);
            key.toLowerCase();
            if (key == "content-length") {
              gRespCL = h.substring(colon + 1).toInt();
            }
          }
          pos = eol + 1;
        }
        return true;
      }
    }
    if (!gWiFi.connected() && !gWiFi.available()) break;
    delay(1);
    ESP.wdtFeed();
  }
  return false;
}

// 读响应 body 到 gBuf (限 max)。前提: 头部已由 phoneReadStatus 消费完毕。
// 有 Content-Length → 精确读满; 无 CL → 读到连接关闭 (或 3s 超时)。
static bool phoneReadBody(uint16_t max) {
  gBufLen = 0;
  int want = (gRespCL >= 0) ? gRespCL : (int)max;
  if (want > (int)max) want = max;
  uint32_t t0 = millis();
  while (gBufLen < (uint16_t)want && (int32_t)(millis() - t0) < 3000) {
    while (gWiFi.available() && gBufLen < (uint16_t)want) {
      int c = gWiFi.read();
      if (c < 0) break;
      gBuf[gBufLen++] = (uint8_t)c;
    }
    if (!gWiFi.available()) {
      if (!gWiFi.connected()) break;   // 读到 close (无 CL 时按此结束)
      delay(1);
      ESP.wdtFeed();
    }
  }
  if (gRespCL >= 0) return gBufLen >= (uint16_t)want || gRespCL == 0;  // 读满 CL 即完成 (CL=0 无 body 也算完成)
  return gBufLen > 0;
}

static void setState(SyncState s) {
  syncDbg("STATE %s -> %s%s", syncStateName((int)gState), syncStateName((int)s),
          (s == SYNC_ERROR || s == SYNC_FINISH) ? " [status]" : "");
  if (s == SYNC_ERROR || s == SYNC_FINISH) syncDbg("STATUS %s", gStatus.c_str());
  gState = s;
  progressSyncRender((int)s);
}

static String basenameOf(const String& p) {
  int i = p.lastIndexOf('/');
  if (i >= 0) return p.substring(i + 1);
  int b = p.lastIndexOf('\\');
  if (b >= 0) return p.substring(b + 1);
  return p;
}

// ---------- /sync.cfg 读取 (SD 根, 纯文本 key=value) ----------
static void loadSyncCfg() {
  gCfgSsid[0] = gCfgPass[0] = gCfgIp[0] = '\0';
  gTargetPort = 8384;   // 默认端口; 有 port= 行则覆盖
  File f = SD.open("/sync.cfg", "r");
  if (!f) { syncDbg("SYNC_CFG none"); return; }
  char line[70];
  uint8_t li = 0;
  while (f.available()) {
    int c = f.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (li > 0) {
        line[li] = '\0';
        char* eq = strchr(line, '=');
        if (eq && eq > line && eq[1]) {
          *eq = '\0';
          const char* val = eq + 1;
          if      (strcmp(line, "ssid") == 0)     snprintf(gCfgSsid, sizeof(gCfgSsid), "%s", val);
          else if (strcmp(line, "password") == 0) snprintf(gCfgPass, sizeof(gCfgPass), "%s", val);
          else if (strcmp(line, "ip") == 0)       snprintf(gCfgIp,   sizeof(gCfgIp),   "%s", val);
          else if (strcmp(line, "port") == 0) {
            // port= 纯数字 1..65535 才生效
            bool digits = (val[0] != '\0');
            for (const char* q = val; *q; q++) if (*q < '0' || *q > '9') { digits = false; break; }
            if (digits) {
              unsigned long p = strtoul(val, 0, 10);
              if (p >= 1 && p <= 65535) gTargetPort = (uint16_t)p;
            }
          }
        }
      }
      li = 0;
    } else if (li < sizeof(line) - 1) {
      line[li++] = (char)c;
    }
  }
  f.close();
  syncDbg("SYNC_CFG ssid=[%s] ip=[%s] port=%u", gCfgSsid[0] ? gCfgSsid : "(无)", gCfgIp[0] ? gCfgIp : "(无)", (unsigned)gTargetPort);
}

// ---------- UDP 发现辅助 ----------
// 广播顺序: 先子网定向广播, 再 255.255.255.255 (部分热点/路由器对两类广播处理不同)
static void discoverySendPings() {
  if (!gDiscUdp.begin(DISC_PORT)) return;
  IPAddress staIp = WiFi.localIP();
  if (staIp.isSet() && staIp != IPAddress(0, 0, 0, 0)) {
    IPAddress bcast(staIp[0], staIp[1], staIp[2], 255);
    gDiscUdp.beginPacket(bcast, DISC_PORT);
    gDiscUdp.write((const uint8_t*)"LUMIDISC", 8);
    gDiscUdp.endPacket();
  }
  for (int i = 0; i < 2; i++) {
    gDiscUdp.beginPacket(IPAddress(255, 255, 255, 255), DISC_PORT);
    gDiscUdp.write((const uint8_t*)"LUMIDISC", 8);
    gDiscUdp.endPacket();
  }
  syncDbg("DISCOVER ping ip=%s port=%u", staIp.toString().c_str(), (unsigned)DISC_PORT);
}

// 返回 >0 且 ack 以 "LUMIACK " 开头 → 合法响应; 其余返回 0
static int discoveryPoll(char* out, size_t cap) {
  int sz = gDiscUdp.parsePacket();
  if (sz <= 0 || sz > (int)cap - 1) return 0;
  int n = gDiscUdp.read((char*)out, cap - 1);
  out[n] = '\0';
  if (memcmp(out, "LUMIACK ", 8) != 0) return 0;
  return n;
}

static void discoveryStop() { gDiscUdp.stop(); }

// ---------- v3 指纹快照生成 (复用已打开的 txtFile; 任一步失败 → 整组无效) ----------
extern File txtFile;   // file_manager.ino 的全局 (当前打开 TXT 句柄; PREPARE 后已打开)

static void fpRegion(uint64_t start, uint64_t end, char* out) {
  uint8_t d[20];
  if (end <= start) { lumiSha1(gFpBuf, 0, d); lumiHexEncode(d, 20, out); gFpValid = true; return; }
  uint32_t len = (uint32_t)(end - start);
  if (len > sizeof(gFpBuf)) len = sizeof(gFpBuf);
  bool sok = txtFile.seek(start);
  uint32_t got = 0;
  while (got < len) {                 // 循环读满: SdFat 块读可能短读, 循环兜底
    int r = txtFile.read(gFpBuf + got, len - got);
    if (r <= 0) break;
    got += (uint32_t)r;
  }
  if (got < len || !sok) {
    syncDbg("FP read-short start=%llu want=%lu got=%lu seek=%d pos=%lu size=%lu",
            (unsigned long long)start, (unsigned long)len, (unsigned long)got, (int)sok,
            (unsigned long)txtFile.position(), (unsigned long)txtFile.size());
    gFpValid = false;
    return;
  }
  gFpValid = true;   // 成功必须置 true —— 原代码漏置, 导致 computeFileFingerprint 恒失败(指纹从未生成)
  lumiSha1(gFpBuf, len, d);
  lumiHexEncode(d, 20, out);
}

static void computeFileFingerprint() {
  gFpValid = false;
  gFpSize = 0; gFpH0[0] = gFpH1[0] = gFpH2[0] = '\0';
  if (!txtFile) { syncDbg("FP FAIL no-txtfile"); return; }
  gFpSize = txtFile.size();
  if (gFpSize == 0) { syncDbg("FP FAIL size=0"); return; }
  uint64_t size = gFpSize;
  uint64_t h0e = (size < 1024) ? size : 1024;
  uint64_t cen = size / 2;
  uint64_t h1s = (cen > 512) ? (cen - 512) : 0;
  uint64_t h1e = (size < cen + 512) ? size : (cen + 512);
  uint64_t h2s = (size > 1024) ? (size - 1024) : 0;
  fpRegion(0, h0e, gFpH0);
  if (!gFpValid) { syncDbg("FP FAIL region0 start=0 len=%lu", (unsigned long)h0e); return; }
  fpRegion(h1s, h1e, gFpH1);
  if (!gFpValid) { syncDbg("FP FAIL region1 start=%llu len=%lu", (unsigned long long)h1s, (unsigned long)(h1e - h1s)); return; }
  fpRegion(h2s, size, gFpH2);
  if (!gFpValid) { syncDbg("FP FAIL region2 start=%llu len=%lu", (unsigned long long)h2s, (unsigned long)(size - h2s)); return; }
  gFpValid = true;
  syncDbg("FP size=%llu h0=[%s] h1=[%s] h2=[%s]",
          (unsigned long long)gFpSize, gFpH0, gFpH1, gFpH2);
}

void progressSyncBegin(const String& txtPath) {
  syncDbg("BEGIN path=[%s]", txtPath.c_str());
  gTxtPath = txtPath;
  gFilename = basenameOf(txtPath);
  gStatus = "";
  gCloudExists = false;
  gRemoteOffset = 0; gRemoteSize = 0; gRemotePercent = 0; gRemoteTsMs = 0;
  gCompareSel = 0;
  gConfirmUpload = false;
  gErrWifiStopped = false;
  gStaAttempted = false;
  gConnectAttempt = 0; gRetryAtMs = 0; gConnectFreed = false;
  gDiscStarted = false;
  gFpValid = false;               // v3: 指纹快照在 PREPARE 重建
  gFileFpState = FINGERPRINT_UNKNOWN;
  gFileMismatch = 0;
  gSyncStartMs = (clockManagerNow() > 0) ? ((uint64_t)clockManagerNow() * 1000ULL) : 0ULL;
  setState(SYNC_PREPARE);
}

void progressSyncLoop() {
  if (gState == SYNC_IDLE) return;
  ESP.wdtFeed();
  if (gState == SYNC_FINISH) { syncDisconnect(); progressSyncDone(true); gState = SYNC_IDLE; return; }
  if (gState == SYNC_ERROR) {
    // 错误停留等按键: 网络已无用, 立即停 Wi-Fi 省电 (只做一次)
    if (!gErrWifiStopped) { wifiManagerStopSta(); gErrWifiStopped = true; }
    return;
  }
  switch (gState) {
    case SYNC_PREPARE: {
      // 快照本地进度 (沿用 progressSyncSnapshot 宿主钩子)
      if (!progressSyncSnapshot(gTxtPath, gLocalOffset, gLocalSize, gLocalPercent)) {
        gStatus = "本地进度读取失败"; setState(SYNC_ERROR); break;
      }
      if (gLocalSize == 0) {
        gStatus = "进度同步失败"; setState(SYNC_ERROR); break;
      }
      syncDbg("PREPARE local offset=%lu size=%lu pct=%.2f",
              (unsigned long)gLocalOffset, (unsigned long)gLocalSize, (double)gLocalPercent);
      loadSyncCfg();          // SD 总线已由 progressSyncSnapshot 恢复, 读 /sync.cfg
      computeFileFingerprint();  // v3: 文件指纹快照 (同一 txtFile 句柄; 失败整组无效)
      setState(SYNC_WIFI);
      break;
    }
    case SYNC_WIFI: {
      if (wifiManagerIsStaUp()) {
        // 局域网模式: STA 已连 → 目标= /sync.cfg ip → UDP 发现 → TargetConfig 兜底
        syncDbg("WIFI sta-up ip=%s", WiFi.localIP().toString().c_str());
        gConnectAttempt = 0; gRetryAtMs = 0;
        if (gCfgIp[0]) { gTarget = String(gCfgIp); setState(SYNC_CONNECT); break; }
        setState(SYNC_DISCOVER);
        break;
      }
      // 未连 STA：优先 /sync.cfg 凭据直连, 否则 EEPROM 配网页凭据 (均限时 20s, 失败切热点)
      if (!gStaAttempted) {
        gStaAttempted = true;
        gStatus = "正在连接 Wi-Fi...";
        if (gCfgSsid[0]) {
          WiFi.persistent(false);        // 不把 /sync.cfg 凭据写进 flash 配网区
          WiFi.mode(WIFI_STA);
          WiFi.begin(gCfgSsid, gCfgPass);
          syncDbg("WIFI try-sta via /sync.cfg ssid=[%s]", gCfgSsid);
        } else {
          wifiManagerStartSta();
        }
        gDeadline = millis() + 20000UL;
        syncDbg("WIFI try-sta start");
        break;
      }
      if (wifiManagerIsStaUp()) {
        gStaAttempted = false;
        gConnectAttempt = 0; gRetryAtMs = 0;
        if (gCfgIp[0]) { gTarget = String(gCfgIp); setState(SYNC_CONNECT); break; }
        setState(SYNC_DISCOVER);
        break;
      }
      if ((int32_t)(millis() - gDeadline) >= 0) {
        // 热点模式: 纯 AP (STA 断开, 规避 AP/STA 同子网路由歧义), 等手机连上热点
        gStaAttempted = false;
        syncDbg("WIFI sta-timeout -> AP-only");
        if (!wifiManagerStartApOnly()) {
          gStatus = "热点启动失败"; setState(SYNC_ERROR); break;
        }
        gDeadline = millis() + 15000UL;   // 等手机接入超时 15s
        gLastPollMs = 0;
        gStatus = "热点已开，等待手机连接…";
        setState(SYNC_WAIT_CLIENT);
        break;
      }
      break;
    }
    case SYNC_DISCOVER: {
      // STA 模式手机发现: 广播 LUMIDISC → 收第一个合法 "LUMIACK <ip>" (≤2s), 失败回退
      if (!gDiscStarted) {
        gDiscStarted = true;
        gDiscDeadline = millis() + DISC_TIMEOUT_MS;
        gDiscPollMs = 0;
        gStatus = "正在发现手机…";
        discoverySendPings();
        break;
      }
      if ((int32_t)(millis() - gDiscPollMs) >= 100) {
        gDiscPollMs = millis();
        char ack[64];
        if (discoveryPoll(ack, sizeof(ack)) > 0) {
          char* sp = strchr(ack, ' ');
          if (sp) {
            // 容忍回包尾部空白/换行: IPAddress::fromString 遇非数字字符整串判失败,
            // 曾致 LUMIACK "192.168.0.10\n" 永远解析不出 → 发现必超时白耗 2s
            char* ips = sp + 1;
            while (*ips == ' ' || *ips == '\r' || *ips == '\n') ips++;
            size_t ilen = strlen(ips);
            while (ilen > 0 && (ips[ilen - 1] == '\r' || ips[ilen - 1] == '\n' || ips[ilen - 1] == ' '))
              ips[--ilen] = '\0';
            IPAddress tip;
            if (tip.fromString(ips)) {
              gTarget = String(ips);
              syncDbg("DISCOVER ok ip=%s", gTarget.c_str());
              discoveryStop();
              gConnectAttempt = 0; gRetryAtMs = 0;
              gStatus = "正在连接手机…";
              setState(SYNC_CONNECT);
              break;
            }
          }
        }
      }
      if ((int32_t)(millis() - gDiscDeadline) >= 0) {
        discoveryStop();
        syncDbg("DISCOVER timeout -> fallback");
        gTarget = String(wifiManagerSyncTarget());
        if (gTarget.length() == 0) { gStatus = "未配置手机地址"; setState(SYNC_ERROR); break; }
        gConnectAttempt = 0; gRetryAtMs = 0;
        gStatus = "正在连接手机…";
        setState(SYNC_CONNECT);
        break;
      }
      ESP.wdtFeed();
      break;
    }
    case SYNC_WAIT_CLIENT: {
      // 热点模式下每 500ms 查一次手机是否已连上热点
      if ((int32_t)(millis() - gLastPollMs) >= 500) {
        gLastPollMs = millis();
        int n = WiFi.softAPgetStationNum();
        syncDbg("WAIT_CLIENT stations=%d", n);
        if (n > 0) {
          gTarget = String(wifiManagerSyncTarget());   // STA 未连 → apIp (192.168.0.100)
          if (gTarget.length() == 0) { gStatus = "未配置手机地址"; setState(SYNC_ERROR); break; }
          gConnectAttempt = 0; gRetryAtMs = 0;
          gStatus = "正在连接手机…";
          setState(SYNC_CONNECT);
          break;
        }
      }
      if ((int32_t)(millis() - gDeadline) >= 0) {
        syncDbg("WAIT_CLIENT timeout");
        gStatus = "无法连接手机"; setState(SYNC_ERROR); break;
      }
      break;
    }
    case SYNC_CONNECT: {
      int r = syncConnectPhase();
      if (r == 0) break;                 // 等重试间隔 (保持本状态, 下一轮再试)
      if (r < 0) { syncDisconnect(); gStatus = "无法连接手机"; setState(SYNC_ERROR); break; }
      gStatus = "已连接，获取进度中…";
      setState(SYNC_GET);
      break;
    }
    case SYNC_GET: {
      // GET /progress?file=<RFC3986 编码文件名>
      char reqLine[300];
      lumiBuildProgressRequest(gFilename.c_str(), reqLine, sizeof(reqLine));
      String req = String(reqLine);
      req += "\r\nHost: ";
      req += gTarget;
      req += "\r\nConnection: close\r\n\r\n";
      syncDbg("REQ %s", reqLine);
      gWiFi.print(req);
      int code = phoneReadStatus();       // 读状态行 + 头部, 3s 超时
      syncDbg("GET code=%d heap=%lu", code, (unsigned long)ESP.getFreeHeap());
      if (code == 200) {
        if (phoneReadBody(sizeof(gBuf))) {
          syncDbg("GET body bytes=%u", (unsigned)gBufLen);
          setState(SYNC_PARSE);
        } else {
          syncDisconnect(); gStatus = "手机数据异常"; setState(SYNC_ERROR);
        }
      } else if (code == 404) {
        syncDisconnect(); gStatus = "手机无此书进度"; setState(SYNC_ERROR);
      } else {
        syncDisconnect(); gStatus = "连接失败 ✗"; setState(SYNC_ERROR);
      }
      break;
    }
    case SYNC_PARSE: {
      // lumiParse 按长度解析，不要求 null 结尾（规范 docs/progress-lumi1.md）
      syncDbg("PARSE raw bytes=%u", (unsigned)gBufLen);
      LumiProgress cp;
      bool parsed = (gBufLen > 0) && lumiParse((const char*)gBuf, gBufLen, cp);
      if (!parsed) { syncDisconnect(); gStatus = "手机进度无效"; setState(SYNC_ERROR); break; }
      if (cp.offset > gLocalSize) { syncDisconnect(); gStatus = "手机进度无效"; setState(SYNC_ERROR); break; }
      gCloudExists = true;
      gRemoteOffset = cp.offset;
      gRemoteSize = cp.size;
      gRemotePercent = cp.pct;
      gRemoteTsMs = cp.ts;
      // v3: 三态比较 (本地快照 vs 手机指纹; 双方完整指纹才比较, 任一方缺失 → UNKNOWN)
      gFileFpState = FINGERPRINT_UNKNOWN;
      gFileMismatch = 0;
      if (gFpValid && cp.hasFingerprint) {
        if (gFpSize != cp.fs) gFileMismatch |= 1;
        if (strcmp(gFpH0, cp.h0) != 0) gFileMismatch |= 2;
        if (strcmp(gFpH1, cp.h1) != 0) gFileMismatch |= 4;
        if (strcmp(gFpH2, cp.h2) != 0) gFileMismatch |= 8;
        gFileFpState = (gFileMismatch == 0) ? FINGERPRINT_MATCH : FINGERPRINT_MISMATCH;
        syncDbg("FP state=%d mask=%u", gFileFpState, (unsigned)gFileMismatch);
      }
      syncDbg("PARSE ok ts=%llu size=%lu offset=%lu pct=%.2f", (unsigned long long)cp.ts,
              (unsigned long)cp.size, (unsigned long)cp.offset, (double)cp.pct);
      syncDisconnect();                 // 比较页等待按键, 连接不再复用 (GET 已 Connection: close)
      gStatus = "请选择同步方向";
      setState(SYNC_COMPARE);
      break;
    }
    case SYNC_COMPARE:
      // 等待按键 (progressSyncHandleKeys); 无按键不前进
      break;
    case SYNC_APPLY: {
      // 跨设备文件可能不同版本(大小不同): 手机进度的 offset 基于其源设备文件,
      // percent 也基于源文件大小。本机直接套 offset 会导致百分比错位。策略:
      //   1) offset 换算出的百分比与 pct 一致(相差<=1%) -> 视为同一文件,
      //      直接用精确字节偏移(误差 <1 页)。
      //   2) 相差明显 -> 源文件与本机不同版本, 按 pct 换算(两位小数精度,
      //      14 万页规模误差约 ±14 页, 已是跨版本文件下的最优近似)。
      uint32_t target = gRemoteOffset;
      double offsetPct = (gLocalSize > 0)
          ? (double)gRemoteOffset * 100.0 / (double)gLocalSize : 0.0;
      double diff = offsetPct - (double)gRemotePercent;
      if (diff < 0.0) diff = -diff;
      if (gLocalSize > 0 && gRemotePercent >= 0.0f && gRemotePercent <= 100.0f && diff > 1.0) {
        target = (uint32_t)((double)gRemotePercent / 100.0 * (double)gLocalSize);
        if (target > gLocalSize) target = gLocalSize;
        syncDbg("APPLY pct-convert offset=%lu -> %lu (offPct=%.2f poPct=%.2f)",
                (unsigned long)gRemoteOffset, (unsigned long)target,
                offsetPct, (double)gRemotePercent);
      } else {
        syncDbg("APPLY use-offset %lu (offPct=%.2f poPct=%.2f)",
                (unsigned long)target, offsetPct, (double)gRemotePercent);
      }
      if (progressSyncApplyRemote(target)) {
        gStatus = "同步成功 ✓";
        setState(SYNC_FINISH);   // loop 顶部 disconnect + done
      } else { syncDisconnect(); gStatus = "本地写入失败"; setState(SYNC_ERROR); }
      break;
    }
    case SYNC_UPLOAD: {
      // PUT 推送本地进度 (覆盖): 比较页期间连接已断开 → 需要时重新连接(带重试)
      if (!gWiFi.connected()) {
        gStatus = "正在连接手机…";
        int r = syncConnectPhase();
        if (r == 0) break;
        if (r < 0) { syncDisconnect(); gStatus = "无法连接手机"; setState(SYNC_ERROR); break; }
      }
      gStatus = "正在推送进度…";
      char lumi[320];
      bool made = lumiMakeEx(lumi, sizeof(lumi), gSyncStartMs, gLocalSize, gLocalOffset, gLocalPercent,
                             gFpValid ? gFpSize : 0,
                             gFpValid ? gFpH0 : NULL,
                             gFpValid ? gFpH1 : NULL,
                             gFpValid ? gFpH2 : NULL);
      if (!made) { syncDisconnect(); gStatus = "进度数据过大"; setState(SYNC_ERROR); break; }
      String body = String(lumi);
      body += "file=";
      body += gFilename;            // 文件名原始 UTF-8, body 内不需要编码
      body += "\n";
      String req = "PUT /progress HTTP/1.1\r\nHost: ";
      req += gTarget;
      req += "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: ";
      req += String(body.length());
      req += "\r\nConnection: close\r\n\r\n";
      req += body;
      syncDbg("REQ PUT /progress body=[%s]", body.c_str());
      gWiFi.print(req);
      int code = phoneReadStatus();
      syncDbg("PUT code=%d heap=%lu", code, (unsigned long)ESP.getFreeHeap());
      syncDisconnect();
      if (code == 200) { gStatus = "同步成功 ✓"; setState(SYNC_FINISH); }
      else if (code == 400) { gStatus = "手机拒绝进度"; setState(SYNC_ERROR); }
      else { gStatus = "连接失败 ✗"; setState(SYNC_ERROR); }
      break;
    }
    default: break;
  }
}

void progressSyncHandleKeys(int middleEvent, int rightEvent) {
  if (gState == SYNC_IDLE) return;
  if (gState == SYNC_ERROR) {
    // 错误页: 中长/右长 返回
    if (middleEvent == 2 || rightEvent == 2) progressSyncCancel();
    return;
  }
  if (gState != SYNC_COMPARE) return;
  // 两键: 中短=同步方向左移, 右短=右移; 中长=取消; 右长=执行
  if (middleEvent == 1) { gCompareSel = (gCompareSel == 0) ? 1 : 0; gConfirmUpload = false; progressSyncRender((int)gState); }
  else if (rightEvent == 1) { gCompareSel = (gCompareSel == 0) ? 1 : 0; gConfirmUpload = false; progressSyncRender((int)gState); }
  else if (middleEvent == 2) { progressSyncCancel(); }
  else if (rightEvent == 2) {
    if (gCompareSel == 0) { gConfirmUpload = false; setState(SYNC_APPLY); }       // 同步=手机→本地
    else if (!gConfirmUpload) { gConfirmUpload = true; progressSyncRender((int)gState); }  // 覆盖=防误触: 首次右长提示, 再按执行
    else { gConfirmUpload = false; gConnectAttempt = 0; gRetryAtMs = 0; setState(SYNC_UPLOAD); }  // 二次右长确认 → 本地→手机
  }
}

bool progressSyncConfirmUploadPending() { return gConfirmUpload; }

void progressSyncCancel() {
  syncDisconnect();   // 归还连接
  gStatus = "已取消";
  progressSyncDone(false);
  gState = SYNC_IDLE;
}

bool progressSyncActive() { return gState != SYNC_IDLE; }
int  progressSyncFileFingerprintState() { return gFileFpState; }
uint8_t progressSyncFileMismatch() { return gFileMismatch; }
int  progressSyncState() { return (int)gState; }
const char* progressSyncStatusText() { return gStatus.c_str(); }
uint32_t progressSyncLocalOffset() { return gLocalOffset; }
float    progressSyncLocalPercent() { return gLocalPercent; }
uint32_t progressSyncLocalSize() { return gLocalSize; }
uint32_t progressSyncRemoteOffset() { return gRemoteOffset; }
float    progressSyncRemotePercent() { return gRemotePercent; }
uint64_t progressSyncRemoteTsMs() { return gRemoteTsMs; }
bool     progressSyncRemoteExists() { return gCloudExists; }
int      progressSyncCompareSel() { return gCompareSel; }

// progress_sync.cpp — 直连手机 HTTP 阅读进度同步器
// 手机 = 进度服务器（监听 8384 端口，响应 LUMI1 文本，明文 HTTP）。
// 局域网：设备 STA 已连 → 直连 192.168.0.10；设备热点模式：纯 AP(192.168.0.1/24)，
// 手机连热点获固定 IP 192.168.0.100 → 直连。
// 协议：GET /progress?file=<RFC3986 编码文件名> → 200+LUMI1 / 404；
//       PUT /progress（body=LUMI1 + file= 行）→ 200/400。
// 本地进度读写与 UI 渲染通过宿主钩子 (progressSyncSnapshot/ApplyRemote/Render/Done, 在 ink-reader-esp.ino)。
// 设计: 非阻塞状态机, 每阶段阻塞 ≤6s, 循环内喂狗, 任何失败走 cleanup(WiFi OFF)。
// 旧 WebDAV 云同步已弃用（传输层整体删除；WebdavConfig/EEPROM 保留于 wifi_manager，UI 隐藏）。

#include "progress_sync.h"
#include "progress_lumi.h"
#include "wifi_manager.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <SD.h>
#include <LittleFS.h>   // /sync.cfg LittleFS 优先(P1/P2: 阅读期间 SD 已卸载)
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <pgmspace.h>

// ---------- 调试 ----------
#define PROG_SYNC_DEBUG 1
#if PROG_SYNC_DEBUG
// 简短的同步调试输出 (Serial @115200, 不影响器件逻辑)
// (2026-09 Step D): 格式串进 flash (调用点 PSTR), vsnprintf_P 读 flash fmt; %s 实参仍须 RAM
static void syncDbg(PGM_P fmt, ...) {
  char buf[220];
  va_list args; va_start(args, fmt);
  vsnprintf_P(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf_P(PSTR("[SYNC][%lu] %s\n"), (unsigned long)millis(), buf);
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
static bool       gStaAttempted = false; // SYNC_WIFI 是否已发起过 STA 连接尝试
static uint8_t    gStaStage = 0;         // STA 阶段: 0=未开始 1=/sync.cfg 凭据 2=EEPROM 配网凭据
static uint32_t   gDeadline = 0;          // WAIT_CLIENT 15s 超时截止
static uint32_t   gLastPollMs = 0;        // WAIT_CLIENT 500ms 轮询
// P3 内存审计: 响应 body 缓冲原常驻 BSS(256B) → SYNC_GET 内 malloc、PARSE 消费完即 free
static uint8_t    *gBodyBuf = NULL;
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
// ★★ 2026-09-12 两轮实机定位（最后定稿方案）：
//   ① 手机日志证明它**收到了** LUMIDISC 并回了 3 次 LUMIACK，设备却恒 `DISCOVER timeout`；
//      钩子对照（`-DUDP_DISC_TEST=1`）显示**新建** WiFiUDP 收包 50/50 全中 ⇒ 收包链路没问题，
//      **病根是"同一个对象 stop() 之后再 begin()"能发不能收**（lwip 收包回调没重新挂上）。
//   ② 第一版修法"每次发现 new/delete 对象"虽能收包，但实测**会把设备卡死在 DISCOVER**
//      （无任何日志、看门狗也不复位）⇒ 弃用动态分配。
//   **定稿**：常驻一个静态对象，**每个同步会话只 begin() 一次并全程复用**（既不做 stop→begin 循环，
//   也不 new/delete）；会话结束走 `discoveryReset()`，下次会话重新绑一次。
static WiFiUDP  gDiscUdp;
static bool     gDiscBound = false;    // 本会话是否已绑定 8390
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
// (P3 内存审计: 原 gFpBuf[1024] 常驻 BSS 已改 computeFileFingerprint 内即用即还)
static int      gFileFpState = FINGERPRINT_UNKNOWN;   // 最近一次 PARSE 三态
static uint8_t  gFileMismatch = 0;                    // bit0=size bit1=head bit2=middle bit3=tail

static uint16_t gTargetPort = 8384;  // 手机进度服务器端口 (默认 8384; /sync.cfg port= 可覆盖, 供联调/换端口)

// ---------- 手动 HTTP (单次 TCP 连接, Connection: close, 明文) ----------
// 每个请求独立连接: GET 完成后断开 (服务器按 Connection: close 关闭),
// PUT 前如已断开则重新连接 (带重试)。

static void syncDisconnect() {
  gWiFi.stop();
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);   // 恢复默认省电（同步期间临时关掉，见 syncWifiNoSleep 注释）
  syncDbg(PSTR("DISCONNECT heap=%lu"), (unsigned long)ESP.getFreeHeap());
}

// ★ 2026-09-12（用户报"设备发广播、手机收不到"）：ESP8266 默认 `WIFI_MODEM_SLEEP`（DTIM 省电），
//   在省电态会**漏收广播/组播帧**；实测手机对 PC 的广播秒回、对设备的广播连日志都没有。
//   同步期间临时关掉省电（同步结束 in syncDisconnect 恢复），提升收发可靠性。
static void syncWifiNoSleep() {
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  syncDbg(PSTR("WIFI nosleep set (mode=%d)"), (int)WiFi.getSleepMode());
}

// 把发现到的手机 IP 记进 TargetConfig.staIp —— 下次即使广播仍被 AP 过滤，也能直接/单播命中
static void rememberPhoneIp(const String &ip) {
  if (ip.length() == 0) return;
  TargetConfig t;
  if (!loadTargetConfig(t)) return;
  if (strcmp(t.staIp, ip.c_str()) == 0) return;
  snprintf(t.staIp, sizeof(t.staIp), PSTR("%s"), ip.c_str());
  if (saveTargetConfig(t)) syncDbg(PSTR("DISCOVER 记住手机 IP=%s"), t.staIp);
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
    gStatus = F("手机地址无效");
    return -1;
  }
  syncDbg(PSTR("CONNECT attempt=%d target=[%s] port=%u"), gConnectAttempt, gTarget.c_str(), (unsigned)gTargetPort);
  gStatus = F("正在连接手机…");
  bool ok = gWiFi.connect(tip, gTargetPort);    // 阻塞 ≤5s (默认超时), 失败立即返回
  ESP.wdtFeed();
  if (ok) {
    gWiFi.setTimeout(3000);       // 读超时 3s
    syncDbg(PSTR("CONNECT ok heap=%lu"), (unsigned long)ESP.getFreeHeap());
    return 1;
  }
  syncDbg(PSTR("CONNECT fail attempt=%d"), gConnectAttempt);
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
  line.reserve(64);   // 一次到位: 阅读期低堆, 逐字节 += 的多次 realloc 碎片化
  gRespCL = -1;
  uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < 8000) {   // 8s: 手机侧 NanoHTTPD 首个响应偶发偏慢(实测 ~3s), 原 3s 会误判成"连接失败"
    while (gWiFi.available()) {
      int c = gWiFi.read();
      if (c < 0) break;
      line += (char)c;
      if (c == '\n') {
        int sp1 = line.indexOf(' ');
        int sp2 = line.indexOf(' ', sp1 + 1);
        int code = (sp1 > 0 && sp2 > sp1) ? line.substring(sp1 + 1, sp2).toInt() : -1;
        syncDbg(PSTR("RSP status=[%s] code=%d"), line.c_str(), code);
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
  buf.reserve(2048);   // 一次到位: 阅读期低堆, 逐字节 += 的多次 realloc 碎片化
  uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < 8000) {
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

// 读响应 body 到 body (限 max)。前提: 头部已由 phoneReadStatus 消费完毕。
// 有 Content-Length → 精确读满; 无 CL → 读到连接关闭 (或 3s 超时)。
static bool phoneReadBody(uint8_t *body, uint16_t max) {
  gBufLen = 0;
  int want = (gRespCL >= 0) ? gRespCL : (int)max;
  if (want > (int)max) want = max;
  uint32_t t0 = millis();
  while (gBufLen < (uint16_t)want && (int32_t)(millis() - t0) < 8000) {
    while (gWiFi.available() && gBufLen < (uint16_t)want) {
      int c = gWiFi.read();
      if (c < 0) break;
      body[gBufLen++] = (uint8_t)c;
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
  syncDbg(PSTR("STATE %s -> %s%s"), syncStateName((int)gState), syncStateName((int)s),
          (s == SYNC_ERROR || s == SYNC_FINISH) ? " [status]" : "");
  if (s == SYNC_ERROR || s == SYNC_FINISH) syncDbg(PSTR("STATUS %s"), gStatus.c_str());
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

// ---------- /sync.cfg 读取 (LittleFS 优先, SD 兼容回退; 纯文本 key=value) ----------
// 架构: 阅读期间 SD 已卸载(P2), 而同步是从阅读菜单发起的网络动作 → 配置改放 LittleFS。
// SD 上旧的 /sync.cfg 仍兼容(同步前会重新挂载 SD); 两者都不属"阅读实时链路"。
static void loadSyncCfg() {
  gCfgSsid[0] = gCfgPass[0] = gCfgIp[0] = '\0';
  gTargetPort = 8384;   // 默认端口; 有 port= 行则覆盖
  File f;
  bool fromSd = false;
  if (LittleFS.begin()) f = LittleFS.open("/sync.cfg", "r");
  if (!f) {
    // 兼容回退: SD 根 /sync.cfg (阅读期间 SD 已 end(), 这里按需重挂载)
    SD.begin(5, SD_SCK_MHZ(20));
    f = SD.open("/sync.cfg", "r");
    fromSd = (bool)f;
  }
  if (!f) { syncDbg(PSTR("SYNC_CFG none")); return; }
  syncDbg(fromSd ? PSTR("SYNC_CFG from=SD") : PSTR("SYNC_CFG from=LittleFS"));
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
          if      (strcmp(line, "ssid") == 0)     snprintf(gCfgSsid, sizeof(gCfgSsid), PSTR("%s"), val);
          else if (strcmp(line, "password") == 0) snprintf(gCfgPass, sizeof(gCfgPass), PSTR("%s"), val);
          else if (strcmp(line, "ip") == 0)       snprintf(gCfgIp,   sizeof(gCfgIp),   PSTR("%s"), val);
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
  syncDbg(PSTR("SYNC_CFG ssid=[%s] ip=[%s] port=%u"), gCfgSsid[0] ? gCfgSsid : "(无)", gCfgIp[0] ? gCfgIp : "(无)", (unsigned)gTargetPort);
}

// ---------- UDP 发现辅助 ----------
// ★ 全程**常驻绑定**：会话之间不 stop()（stop 后再 begin 同对象会"能发不能收"），
//   仅在"一发都没发出去"时 discoveryRebind() 重绑一次。
static bool discoveryBegin();
static void discoveryRebind();
static int  discoveryPoll(char* out, size_t cap);   // 定义在下面（收包解析）

// 逐 IP 单播扫描（AP 常过滤"无线→无线"广播；单播与 TCP 同一通路、实测可达）：
// 对 /24 内 254 个地址各发一个 LUMIDISC，手机收到任一即回 LUMIACK。254 个小包 ≈1s。
static void discoveryUnicastSweep() {
  IPAddress sta = WiFi.localIP();
  if (!sta.isSet() || sta == IPAddress(0, 0, 0, 0)) return;
  int sent = 0;
  for (int h = 1; h <= 254; h++) {
    if (h == (int)sta[3]) continue;
    IPAddress dst(sta[0], sta[1], sta[2], (uint8_t)h);
    gDiscUdp.beginPacket(dst, DISC_PORT);
    gDiscUdp.write((const uint8_t*)"LUMIDISC", 8);
    if (gDiscUdp.endPacket() > 0) sent++;
    delay(1);                       // 别一次性灌满 lwip 发送队列
    if ((h & 31) == 0) ESP.wdtFeed();
  }
  syncDbg(PSTR("DISCOVER sweep sent=%d"), sent);
}

// 轮询 ACK（成功时 gTarget 已设好并已记入 TargetConfig）
static bool discoveryPollAck(uint32_t windowMs) {
  char ack[64];
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < windowMs) {
    if (discoveryPoll(ack, sizeof(ack)) > 0) {
      char* sp = strchr(ack, ' ');
      if (sp) {
        // 容忍回包尾部空白/换行: IPAddress::fromString 遇非数字字符整串判失败
        char* ips = sp + 1;
        while (*ips == ' ' || *ips == '\r' || *ips == '\n') ips++;
        size_t ilen = strlen(ips);
        while (ilen > 0 && (ips[ilen - 1] == '\r' || ips[ilen - 1] == '\n' || ips[ilen - 1] == ' '))
          ips[--ilen] = '\0';
        IPAddress tip;
        if (tip.fromString(ips)) {
          gTarget = String(ips);
          syncDbg(PSTR("DISCOVER ok ip=%s"), gTarget.c_str());
          rememberPhoneIp(gTarget);
          return true;
        }
      }
    }
    delay(20);
    ESP.wdtFeed();
  }
  return false;
}

static void discoverySendPings() {
  // 仅 STA(局域网/手机热点)模式进入本状态: 目标地址用 STA 子网广播; 热点模式走 SYNC_WAIT_CLIENT
  // (热点固定给手机分配 192.168.0.100 且客户端上限 1, 见 wifi_managerStartApOnly) —— 不需要发现。
  if (!discoveryBegin()) return;
  IPAddress staIp = WiFi.localIP();
  int sent = 0;
  if (staIp.isSet() && staIp != IPAddress(0, 0, 0, 0)) {
    IPAddress bcast(staIp[0], staIp[1], staIp[2], 255);
    gDiscUdp.beginPacket(bcast, DISC_PORT);
    gDiscUdp.write((const uint8_t*)"LUMIDISC", 8);
    if (gDiscUdp.endPacket() > 0) sent++;
  }
  for (int i = 0; i < 2; i++) {
    gDiscUdp.beginPacket(IPAddress(255, 255, 255, 255), DISC_PORT);
    gDiscUdp.write((const uint8_t*)"LUMIDISC", 8);
    if (gDiscUdp.endPacket() > 0) sent++;
  }
  // ★ 同时**单播**探测已配置的目标 IP（广播可能被 AP 过滤；单播与 TCP 走同一条通路，已被证明可达）
  const char *cfgTarget = wifiManagerSyncTarget();
  IPAddress uni;
  if (cfgTarget && cfgTarget[0] && uni.fromString(cfgTarget)) {
    gDiscUdp.beginPacket(uni, DISC_PORT);
    gDiscUdp.write((const uint8_t*)"LUMIDISC", 8);
    if (gDiscUdp.endPacket() > 0) sent++;
    syncDbg(PSTR("DISCOVER unicast probe -> %s"), cfgTarget);
  }
  syncDbg(PSTR("DISCOVER ping ip=%s port=%u sent=%d"), staIp.toString().c_str(), (unsigned)DISC_PORT, sent);
  if (sent == 0) {                       // 一发都没出去 → 旧 socket 失效(换网/重连), 重绑后再发一次
    discoveryRebind();
    gDiscUdp.beginPacket(IPAddress(255, 255, 255, 255), DISC_PORT);
    gDiscUdp.write((const uint8_t*)"LUMIDISC", 8);
    gDiscUdp.endPacket();
  }
}

// 返回 >0 且 ack 以 "LUMIACK " 开头 → 合法响应; 其余返回 0
static int discoveryPoll(char* out, size_t cap) {
  if (!gDiscBound) return 0;
  int sz = gDiscUdp.parsePacket();
  if (sz <= 0) return 0;
  if (sz > (int)cap - 1) sz = (int)cap - 1;   // 超长包按上限读掉, 不留在缓冲里
  int n = gDiscUdp.read((char*)out, sz);
  if (n < 0) n = 0;
  out[n] = '\0';
  if (n < 8 || memcmp(out, "LUMIACK ", 8) != 0) {
    syncDbg(PSTR("DISCOVER rx-nonack len=%d [%s]"), n, out);
    return 0;
  }
  return n;
}


// 常驻绑定：全局只 begin() 一次，之后全程复用（不做 stop→begin 循环，也不用 new/delete）
static bool discoveryBegin() {
  if (gDiscBound) return true;
  if (!gDiscUdp.begin(DISC_PORT)) {
    syncDbg(PSTR("DISCOVER begin-fail heap=%u"), (unsigned)ESP.getFreeHeap());
    return false;
  }
  gDiscBound = true;
  syncDbg(PSTR("DISCOVER bound localPort=%u"), (unsigned)gDiscUdp.localPort());
  return true;
}

// 仅"发包全失败"(换网/重连后旧 socket 失效)时重绑一次
static void discoveryRebind() {
  gDiscUdp.stop();
  gDiscBound = false;
  if (discoveryBegin()) syncDbg(PSTR("DISCOVER rebound"));
}

// 会话结束 = **什么都不做**：故意不 stop()。
// (stop() 后再 begin() 同一对象会"能发不能收" —— 2026-09-12 DISCOVER 恒超时的真正原因；
//  而上一版"每次 new/delete"会把设备卡死在 DISCOVER，已弃用)
static void discoveryStop() { /* 保持绑定，勿 stop */ }

// ---------- v3 指纹快照生成 (复用已打开的 txtFile; 任一步失败 → 整组无效) ----------
extern File txtFile;   // ink-reader-esp.ino 的全局 (当前打开 TXT 句柄; PREPARE 后已打开)

static void fpRegion(uint8_t *fpBuf, size_t fpCap, uint64_t start, uint64_t end, char* out) {
  uint8_t d[20];
  if (end <= start) { lumiSha1(fpBuf, 0, d); lumiHexEncode(d, 20, out); gFpValid = true; return; }
  uint32_t len = (uint32_t)(end - start);
  if (len > fpCap) len = fpCap;
  bool sok = txtFile.seek(start);
  uint32_t got = 0;
  while (got < len) {                 // 循环读满: SdFat 块读可能短读, 循环兜底
    int r = txtFile.read(fpBuf + got, len - got);
    if (r <= 0) break;
    got += (uint32_t)r;
  }
  if (got < len || !sok) {
    syncDbg(PSTR("FP read-short start=%llu want=%lu got=%lu seek=%d pos=%lu size=%lu"),
            (unsigned long long)start, (unsigned long)len, (unsigned long)got, (int)sok,
            (unsigned long)txtFile.position(), (unsigned long)txtFile.size());
    gFpValid = false;
    return;
  }
  gFpValid = true;   // 成功必须置 true —— 原代码漏置, 导致 computeFileFingerprint 恒失败(指纹从未生成)
  lumiSha1(fpBuf, len, d);
  lumiHexEncode(d, 20, out);
}

static void computeFileFingerprint() {
  gFpValid = false;
  gFpSize = 0; gFpH0[0] = gFpH1[0] = gFpH2[0] = '\0';
  if (!txtFile) { syncDbg(PSTR("FP FAIL no-txtfile")); return; }
  gFpSize = txtFile.size();
  if (gFpSize == 0) { syncDbg(PSTR("FP FAIL size=0")); return; }
  // P3 内存审计: 1KB 读块只在指纹计算瞬间存活, 算完即还堆 (原常驻 BSS)
  static const size_t kFpBufCap = 1024;
  uint8_t *fpBuf = (uint8_t *)malloc(kFpBufCap);
  if (!fpBuf) { syncDbg(PSTR("FP FAIL alloc")); return; }
  uint64_t size = gFpSize;
  uint64_t h0e = (size < 1024) ? size : 1024;
  uint64_t cen = size / 2;
  uint64_t h1s = (cen > 512) ? (cen - 512) : 0;
  uint64_t h1e = (size < cen + 512) ? size : (cen + 512);
  uint64_t h2s = (size > 1024) ? (size - 1024) : 0;
  fpRegion(fpBuf, kFpBufCap, 0, h0e, gFpH0);
  if (!gFpValid) { free(fpBuf); syncDbg(PSTR("FP FAIL region0 start=0 len=%lu"), (unsigned long)h0e); return; }
  fpRegion(fpBuf, kFpBufCap, h1s, h1e, gFpH1);
  if (!gFpValid) { free(fpBuf); syncDbg(PSTR("FP FAIL region1 start=%llu len=%lu"), (unsigned long long)h1s, (unsigned long)(h1e - h1s)); return; }
  fpRegion(fpBuf, kFpBufCap, h2s, size, gFpH2);
  if (!gFpValid) { free(fpBuf); syncDbg(PSTR("FP FAIL region2 start=%llu len=%lu"), (unsigned long long)h2s, (unsigned long)(size - h2s)); return; }
  free(fpBuf);
  gFpValid = true;
  syncDbg(PSTR("FP size=%llu h0=[%s] h1=[%s] h2=[%s]"),
          (unsigned long long)gFpSize, gFpH0, gFpH1, gFpH2);
}

void progressSyncBegin(const String& txtPath) {
  syncDbg(PSTR("BEGIN path=[%s]"), txtPath.c_str());
  gTxtPath = txtPath;
  gFilename = basenameOf(txtPath);
  gStatus = F("");
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
        gStatus = F("本地进度读取失败"); setState(SYNC_ERROR); break;
      }
      if (gLocalSize == 0) {
        gStatus = F("进度同步失败"); setState(SYNC_ERROR); break;
      }
      syncDbg(PSTR("PREPARE local offset=%lu size=%lu pct=%.2f"),
              (unsigned long)gLocalOffset, (unsigned long)gLocalSize, (double)gLocalPercent);
      loadSyncCfg();          // SD 总线已由 progressSyncSnapshot 恢复, 读 /sync.cfg
      computeFileFingerprint();  // v3: 文件指纹快照 (同一 txtFile 句柄; 失败整组无效)
      setState(SYNC_WIFI);
      break;
    }
    case SYNC_WIFI: {
      if (wifiManagerIsStaUp()) {
        // 局域网模式: STA 已连 → 目标= /sync.cfg ip → UDP 发现 → TargetConfig 兜底
        syncDbg(PSTR("WIFI sta-up ip=%s"), WiFi.localIP().toString().c_str());
        syncWifiNoSleep();
        gConnectAttempt = 0; gRetryAtMs = 0;
        if (gCfgIp[0]) { gTarget = String(gCfgIp); setState(SYNC_CONNECT); break; }
        setState(SYNC_DISCOVER);
        break;
      }
      // 未连 STA：**两阶段**尝试 —— ① /sync.cfg 凭据 ② EEPROM 配网页凭据（各 20s）。
      // 用户 2026-09-12："进度同步不应该先尝试连接 WiFi 而不是开热点" → 两条都试过仍失败，
      // 才进 SYNC_STA_FAILED 停在提示页**等你按键决定**是否开热点（不再静默切热点）。
      if (!gStaAttempted) {
        gStaAttempted = true;
        gStaStage = 1;
        // 先腾堆 (关 txtFile + 清阅读行缓冲): 同步是从**阅读器**里发起的, 阅读会话占着几 KB,
        // 而 WiFi 栈建立连接也吃堆 —— 时钟页能连上、同步连不上, 差距可能就在这。
        // (connect 阶段本来也会调, 这里提前 + 用 gConnectFreed 去重, 不重复释放)
        progressSyncFreeReaderHeap();
        gConnectFreed = true;
        syncDbg(PSTR("WIFI_PRE heap=%u mode=%d cred=%d eeprom_ssid=[%s] syncCfg_ssid=[%s]"),
                (unsigned)ESP.getFreeHeap(), (int)WiFi.getMode(),
                wifiManagerHasCredentials() ? 1 : 0,
                wifiManagerCfgSsid() ? wifiManagerCfgSsid() : "(无)",
                gCfgSsid[0] ? gCfgSsid : "(无)");
        gStatus = F("正在连接 Wi-Fi...");
        if (gCfgSsid[0]) {
          WiFi.persistent(false);        // 不把 /sync.cfg 凭据写进 flash 配网区
          WiFi.mode(WIFI_STA);
          WiFi.begin(gCfgSsid, gCfgPass);
          syncDbg(PSTR("WIFI try-sta[1] via /sync.cfg ssid=[%s]"), gCfgSsid);
        } else {
          bool ok = wifiManagerStartSta();
          syncDbg(PSTR("WIFI try-sta[1] via EEPROM ok=%d ssid=[%s]"), ok ? 1 : 0, wifiManagerCfgSsid());
        }
        gDeadline = millis() + 20000UL;
        break;
      }
      if (wifiManagerIsStaUp()) {
        gStaAttempted = false;
        gStaStage = 0;
        gConnectAttempt = 0; gRetryAtMs = 0;
        syncDbg(PSTR("WIFI sta-up stage=%d ip=%s"), gStaStage, WiFi.localIP().toString().c_str());
        syncWifiNoSleep();
        if (gCfgIp[0]) { gTarget = String(gCfgIp); setState(SYNC_CONNECT); break; }
        setState(SYNC_DISCOVER);
        break;
      }
      if ((int32_t)(millis() - gDeadline) >= 0) {
        if (gStaStage == 1 && gCfgSsid[0]) {
          // ① /sync.cfg 失败 → ② 再试"你配网页里配好的网络"（而不是直接开热点）
          gStaStage = 2;
          bool ok = wifiManagerStartSta();
          syncDbg(PSTR("WIFI try-sta[1] FAIL -> try-sta[2] EEPROM ok=%d"), ok ? 1 : 0);
          gStatus = F("正在连接 Wi-Fi (2/2)...");
          gDeadline = millis() + 20000UL;
          break;
        }
        // 两种都没连上 → 交给你决定（右长开热点）
        gStaAttempted = false;
        gStaStage = 0;
        syncDbg(PSTR("WIFI sta-fail -> 等用户决定(右长=开热点)"));
        gStatus = F("未连上 Wi-Fi");
        setState(SYNC_STA_FAILED);
        break;
      }
      break;
    }
    case SYNC_STA_FAILED: {
      break;   // 等按键: 见 progressSyncHandleKeys (右长=开热点, 短按/中长=退出)
    }
    case SYNC_DISCOVER: {
      // STA 模式手机发现，两段式（2026-09-12 实机定稿）：
      //   ① 广播 + 已配置 IP 的单播探测 → 紧循环轮询 1s；
      //   ② 失败则**逐 IP 单播扫描**（254 个小包 ≈1s）→ 再轮询 1.5s。
      // 为什么要②：用户的路由器**过滤"无线→无线"广播**（实测：手机对 PC 的广播秒回、
      // 对设备的广播连日志都没有），而单播与 TCP 同一通路、完全可达。
      // 为什么必须紧循环轮询：早期"主 loop 每 100ms 轮询"的形态收不到包（同 socket 同网络，
      // 仅轮询形态不同），`-DUDP_DISC_TEST=1` 钩子的紧循环 50/50 全中。
      if (gDiscStarted) break;     // 本状态一次性完成，不再分帧
      gDiscStarted = true;
      gStatus = F("正在发现手机…");
      progressSyncRender((int)gState);
      discoverySendPings();
      bool found = discoveryPollAck(1000);
      if (!found) {
        discoveryUnicastSweep();
        found = discoveryPollAck(1500);
      }
      gConnectAttempt = 0; gRetryAtMs = 0;
      if (found) {
        gStatus = F("正在连接手机…");
        setState(SYNC_CONNECT);
      } else {
        syncDbg(PSTR("DISCOVER timeout -> fallback"));
        gTarget = String(wifiManagerSyncTarget());
        if (gTarget.length() == 0) { gStatus = F("未配置手机地址"); setState(SYNC_ERROR); break; }
        gStatus = F("正在连接手机…");
        setState(SYNC_CONNECT);
      }
      break;
    }
    case SYNC_WAIT_CLIENT: {
      // 热点模式下每 500ms 查一次手机是否已连上热点
      if ((int32_t)(millis() - gLastPollMs) >= 500) {
        gLastPollMs = millis();
        int n = WiFi.softAPgetStationNum();
        syncDbg(PSTR("WAIT_CLIENT stations=%d"), n);
        if (n > 0) {
          gTarget = String(wifiManagerSyncTarget());   // STA 未连 → apIp (192.168.0.100)
          if (gTarget.length() == 0) { gStatus = F("未配置手机地址"); setState(SYNC_ERROR); break; }
          gConnectAttempt = 0; gRetryAtMs = 0;
          gStatus = F("正在连接手机…");
          setState(SYNC_CONNECT);
          break;
        }
      }
      if ((int32_t)(millis() - gDeadline) >= 0) {
        syncDbg(PSTR("WAIT_CLIENT timeout"));
        gStatus = F("无法连接手机"); setState(SYNC_ERROR); break;
      }
      break;
    }
    case SYNC_CONNECT: {
      int r = syncConnectPhase();
      if (r == 0) break;                 // 等重试间隔 (保持本状态, 下一轮再试)
      if (r < 0) { syncDisconnect(); gStatus = F("无法连接手机"); setState(SYNC_ERROR); break; }
      gStatus = F("已连接，获取进度中…");
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
      syncDbg(PSTR("REQ %s"), reqLine);
      gWiFi.print(req);
      int code = phoneReadStatus();       // 读状态行 + 头部, 3s 超时
      syncDbg(PSTR("GET code=%d heap=%lu"), code, (unsigned long)ESP.getFreeHeap());
      if (code == 200) {
        gBodyBuf = (uint8_t *)malloc(256);   // P3: 只在 GET→PARSE 两态间存活
        if (gBodyBuf && phoneReadBody(gBodyBuf, 256)) {
          syncDbg(PSTR("GET body bytes=%u"), (unsigned)gBufLen);
          setState(SYNC_PARSE);
        } else {
          free(gBodyBuf); gBodyBuf = NULL;
          syncDisconnect(); gStatus = F("手机数据异常"); setState(SYNC_ERROR);
        }
      } else if (code == 404) {
        // 404 细分（2026-09-12 用户拍板："书架上没有这书，就告诉 ESP 手机上没有这本书"）：
        //   手机端 body: no-book=书架上没这本书; no-progress=书在架上但手机侧还没进度
        char b[24] = "";
        gBodyBuf = (uint8_t *)malloc(24);
        if (gBodyBuf && phoneReadBody(gBodyBuf, 24)) {
          snprintf(b, sizeof(b), "%s", (const char *)gBodyBuf);
        }
        free(gBodyBuf); gBodyBuf = NULL;
        syncDbg(PSTR("GET 404 body=[%s]"), b);
        syncDisconnect();
        gStatus = (strstr(b, "no-book")) ? F("手机上没有这本书") : F("手机无此书进度");
        setState(SYNC_ERROR);
      } else {
        syncDisconnect(); gStatus = F("连接失败 ✗"); setState(SYNC_ERROR);
      }
      break;
    }
    case SYNC_PARSE: {
      // lumiParse 按长度解析，不要求 null 结尾（规范 docs/progress-lumi1.md）
      uint8_t *body = gBodyBuf; gBodyBuf = NULL;   // 取走所有权, 解析后立即归还堆
      syncDbg(PSTR("PARSE raw bytes=%u"), (unsigned)gBufLen);
      LumiProgress cp;
      bool parsed = (gBufLen > 0 && body) && lumiParse((const char*)body, gBufLen, cp);
      free(body);
      if (!parsed) { syncDisconnect(); gStatus = F("手机进度无效"); setState(SYNC_ERROR); break; }
      if (cp.offset > gLocalSize) { syncDisconnect(); gStatus = F("手机进度无效"); setState(SYNC_ERROR); break; }
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
        syncDbg(PSTR("FP state=%d mask=%u"), gFileFpState, (unsigned)gFileMismatch);
      }
      syncDbg(PSTR("PARSE ok ts=%llu size=%lu offset=%lu pct=%.2f"), (unsigned long long)cp.ts,
              (unsigned long)cp.size, (unsigned long)cp.offset, (double)cp.pct);
      syncDisconnect();                 // 比较页等待按键, 连接不再复用 (GET 已 Connection: close)
      gStatus = F("请选择同步方向");
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
        syncDbg(PSTR("APPLY pct-convert offset=%lu -> %lu (offPct=%.2f poPct=%.2f)"),
                (unsigned long)gRemoteOffset, (unsigned long)target,
                offsetPct, (double)gRemotePercent);
      } else {
        syncDbg(PSTR("APPLY use-offset %lu (offPct=%.2f poPct=%.2f)"),
                (unsigned long)target, offsetPct, (double)gRemotePercent);
      }
      if (progressSyncApplyRemote(target)) {
        gStatus = F("同步成功 ✓");
        setState(SYNC_FINISH);   // loop 顶部 disconnect + done
      } else { syncDisconnect(); gStatus = F("本地写入失败"); setState(SYNC_ERROR); }
      break;
    }
    case SYNC_UPLOAD: {
      // PUT 推送本地进度 (覆盖): 比较页期间连接已断开 → 需要时重新连接(带重试)
      if (!gWiFi.connected()) {
        gStatus = F("正在连接手机…");
        int r = syncConnectPhase();
        if (r == 0) break;
        if (r < 0) { syncDisconnect(); gStatus = F("无法连接手机"); setState(SYNC_ERROR); break; }
      }
      gStatus = F("正在推送进度…");
      char lumi[320];
      bool made = lumiMakeEx(lumi, sizeof(lumi), gSyncStartMs, gLocalSize, gLocalOffset, gLocalPercent,
                             gFpValid ? gFpSize : 0,
                             gFpValid ? gFpH0 : NULL,
                             gFpValid ? gFpH1 : NULL,
                             gFpValid ? gFpH2 : NULL);
      if (!made) { syncDisconnect(); gStatus = F("进度数据过大"); setState(SYNC_ERROR); break; }
      String body = String(lumi);
      body += "file=";
      body += gFilename;            // 文件名原始 UTF-8, body 内不需要编码
      body += "\n";
      String req = F("PUT /progress HTTP/1.1\r\nHost: ");
      req += gTarget;
      req += "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: ";
      req += String(body.length());
      req += "\r\nConnection: close\r\n\r\n";
      req += body;
      syncDbg(PSTR("REQ PUT /progress body=[%s]"), body.c_str());
      gWiFi.print(req);
      int code = phoneReadStatus();
      syncDbg(PSTR("PUT code=%d heap=%lu"), code, (unsigned long)ESP.getFreeHeap());
      // 404 细分同上：no-book=手机书架上没有这本书（2026-09-12 用户拍板：这种情况要明确告知）
      char b404[24] = "";
      if (code == 404) {
        gBodyBuf = (uint8_t *)malloc(24);
        if (gBodyBuf && phoneReadBody(gBodyBuf, 24)) {
          snprintf(b404, sizeof(b404), "%s", (const char *)gBodyBuf);
        }
        free(gBodyBuf); gBodyBuf = NULL;
        syncDbg(PSTR("PUT 404 body=[%s]"), b404);
      }
      syncDisconnect();
      if (code == 200) { gStatus = F("同步成功 ✓"); setState(SYNC_FINISH); }
      else if (code == 400) { gStatus = F("手机拒绝进度"); setState(SYNC_ERROR); }
      else if (code == 404) {
        gStatus = (strstr(b404, "no-book")) ? F("手机上没有这本书") : F("手机无此书进度");
        setState(SYNC_ERROR);
      }
      else { gStatus = F("连接失败 ✗"); setState(SYNC_ERROR); }
      break;
    }
    default: break;
  }
}

void progressSyncHandleKeys(int middleEvent, int rightEvent) {
  if (gState == SYNC_IDLE) return;
  if (gState == SYNC_STA_FAILED) {
    // STA 两阶段都失败 → **不再自动开热点**, 交给用户: 右长=开热点等手机接, 短按/中长=退出
    if (rightEvent == 2) {
      syncDbg(PSTR("STA_FAILED -> user start AP-only"));
      if (!wifiManagerStartApOnly()) { gStatus = F("热点启动失败"); setState(SYNC_ERROR); return; }
      gDeadline = millis() + 15000UL;   // 等手机接入超时 15s (热点固定给手机 192.168.0.100)
      gLastPollMs = 0;
      gStatus = F("热点已开，等待手机连接…");
      setState(SYNC_WAIT_CLIENT);
      progressSyncRender((int)gState);
    } else if (middleEvent == 2 || middleEvent == 1 || rightEvent == 1) {
      progressSyncCancel();
    }
    return;
  }
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
  free(gBodyBuf); gBodyBuf = NULL;   // P3: 防御释放 (取消可能发生在 GET/PARSE 态)
  gStatus = F("已取消");
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

#pragma once
#include <Arduino.h>
#include "progress_lumi.h"   // LUMI1 格式（规范 docs/progress-lumi1.md），LumiProgress 结构

// 进度同步 = 直连手机 HTTP（旧 WebDAV 云同步已弃用：代码保留于 wifi_manager 的
// WebdavConfig/EEPROM，传输层在 progress_sync.cpp 已删除，UI 已隐藏）。
// 手机 = 进度服务器，监听 8384 端口，响应 LUMI1 文本（明文 HTTP）。
enum SyncState {
  SYNC_IDLE = 0,
  SYNC_PREPARE,     // 快照本地进度
  SYNC_WIFI,        // STA 两阶段尝试: ① /sync.cfg 凭据 ② EEPROM 配网凭据 (各 20s)
  SYNC_STA_FAILED,  // 两种网络都没连上 → 停在提示页**等用户决定**(右长=开热点, 短按/中长=退出)
  SYNC_DISCOVER,    // UDP 发现手机 (LUMIDISC/LUMIACK, ≤2s; STA 与热点模式都用)
  SYNC_WAIT_CLIENT, // 热点模式：500ms 轮询 softAP station，15s 超时
  SYNC_CONNECT,     // TCP 连接手机:8384（5s 超时，失败重试 2 次间隔 2s）
  SYNC_GET,         // GET /progress?file=<RFC3986> 拉取手机进度
  SYNC_PARSE,       // lumiParse + 校验
  SYNC_COMPARE,     // 比较页 [同步=从手机拉取][覆盖=推送到手机]
  SYNC_APPLY,       // 手机→本地
  SYNC_UPLOAD,      // 本地→手机（PUT 推送）
  SYNC_FINISH,
  SYNC_ERROR
};

// ---- 同步控制器 (宿主通过钩子交互, 本模块不碰 .i1) ----
void progressSyncBegin(const String& txtPath);
void progressSyncLoop();
void progressSyncHandleKeys(int middleEvent, int rightEvent);
void progressSyncCancel();               // 取消/错误返回 → cleanup(WiFi OFF)
bool progressSyncActive();
int  progressSyncState();
const char* progressSyncStatusText();    // 当前状态/错误文案
// 比较/展示用 getter
uint32_t progressSyncLocalOffset();
float    progressSyncLocalPercent();
uint32_t progressSyncLocalSize();
uint32_t progressSyncRemoteOffset();
float    progressSyncRemotePercent();
uint64_t progressSyncRemoteTsMs();
bool     progressSyncRemoteExists();
int      progressSyncCompareSel();   // 比较页选中: 0=同步 1=覆盖
bool     progressSyncConfirmUploadPending();  // 覆盖是否已进入二次确认 (防误触)
// v3 文件指纹三态: 0=UNKNOWN 1=MATCH 2=MISMATCH; mismatch 掩码 bit0=size bit1=head bit2=middle bit3=tail (仅 MISMATCH 有诊断意义)
int      progressSyncFileFingerprintState();
uint8_t  progressSyncFileMismatch();

// ---- 手机绑定 (方案 D-1: 常驻轻量 UDP 监听 + 绑定/上次成功 IP 记忆) ----
// 用户 2026-09-12 批准：常驻监听**只**处理 LUMIWHO / LUMIBIND / LUMIPING，
// 非阻塞、不做文件/SD/扫描/同步/刷屏；绑定写入延后到主循环执行。
void espBindTick();                                  // 主 loop 每圈调用 (极轻量, 微秒级)
bool syncBindSet(const char *ip, uint16_t port);     // 写绑定目标 (手机 LUMIBIND / 配网页)
bool syncBindClear();                                // 清除绑定 (配网页)
bool syncBindGet(char *ipOut, size_t cap, uint16_t &portOut);   // 读绑定目标 ("" = 未绑定)
const char *syncBindLastOk();                        // 上次成功 IP (" = 无)
const char *progressSyncTargetIp();                  // 当前同步目标 IP (只读访问器, 供 UI 显示)
const char *syncStaticDevIp();                       // 设备静态 IP 设定("" = 用 DHCP; 默认 192.168.0.100)
const char *syncLocalIpText();                       // 设备当前局域网 IP 文本(供屏幕显示)

// ---- 纯工具（LUMI1，实现见 progress_lumi.h/cpp，PC 可测）----

// ---- 宿主钩子 (ink-reader-esp.ino 实现) ----
bool   progressSyncSnapshot(const String& txtPath, uint32_t& localOffset, uint32_t& txtSize, float& localPercent);
void   progressSyncRender(int state);            // 绘制当前同步界面 (状态/比较/错误)
bool   progressSyncApplyRemote(uint32_t offset); // 手机→本地: 写 .i1[0] + seek + 重渲
void   progressSyncDone(bool ok);                // 同步结束/取消 (恢复阅读器)
void   progressSyncFreeReaderHeap();             // 网络阶段前: 清空阅读行缓冲, 腾堆给网络 (构建中安全)
void   progressSyncRestoreReaderHeap();          // 网络结束后: 重读当前页填充行缓冲

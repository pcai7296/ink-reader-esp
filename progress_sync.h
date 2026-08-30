#pragma once
#include <Arduino.h>
#include "progress_lumi.h"   // LUMI1 格式（规范 docs/progress-lumi1.md），LumiProgress 结构

// 进度同步 = 直连手机 HTTP（旧 WebDAV 云同步已弃用：代码保留于 wifi_manager 的
// WebdavConfig/EEPROM，传输层在 progress_sync.cpp 已删除，UI 已隐藏）。
// 手机 = 进度服务器，监听 8384 端口，响应 LUMI1 文本（明文 HTTP）。
enum SyncState {
  SYNC_IDLE = 0,
  SYNC_PREPARE,     // 快照本地进度
  SYNC_WIFI,        // STA 已连→局域网直用；未连→wifiManagerStartApOnly() 纯 AP
  SYNC_DISCOVER,    // STA 模式 UDP 发现手机 (LUMIDISC/LUMIACK, ≤2s, 失败回退 TargetConfig)
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

// ---- 纯工具（LUMI1，实现见 progress_lumi.h/cpp，PC 可测）----

// ---- 宿主钩子 (ink-reader-esp.ino 实现) ----
bool   progressSyncSnapshot(const String& txtPath, uint32_t& localOffset, uint32_t& txtSize, float& localPercent);
void   progressSyncRender(int state);            // 绘制当前同步界面 (状态/比较/错误)
bool   progressSyncApplyRemote(uint32_t offset); // 手机→本地: 写 .i1[0] + seek + 重渲
void   progressSyncDone(bool ok);                // 同步结束/取消 (恢复阅读器)
void   progressSyncFreeReaderHeap();             // 网络阶段前: 清空阅读行缓冲, 腾堆给网络 (构建中安全)
void   progressSyncRestoreReaderHeap();          // 网络结束后: 重读当前页填充行缓冲

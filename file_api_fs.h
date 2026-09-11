#pragma once
#include <Arduino.h>

// 官方 A7 管理 Web "文件管理"高级层构造复刻（/fs/*, 移植自 web_test official_manager.cpp）
// - 走 onNotFound 分发（fileApiTryDispatch 前缀匹配, 零路由对象堆）: 见 fileApiFsTryDispatch
// - 管理对象 = SD 卡（官方 LittleFS; 构造不变）; 无鉴权（官方同款）; 显示过滤对齐屏上文件管理器
// - /fs/status O(1): 只读 sdAvailable + 缓存容量, 绝不遍历 SD（配网会话堆硬约束）
// 差异取舍见 docs/官方A7文件管理构造移植.md 与 docs/file-api.md
// A 修复(2026-09, 用户拍板): 可见性单一规则入口——/fs/list 实时列表与 fs_cache 缓存构建/出站共用
// 同一过滤, 杜绝"缓存视图显示 .bin / 实时视图隐藏 .bin"的视图不一致(删除后同目录文件"集体消失"假象)。
// 规则(2026-09 黑名单机制, 用户拍板): 目录=硬隐藏(隐藏名/系统目录)之外全显示;
//   文件=硬隐藏之外显示, 当 hideAuto=true 时隐藏"设备自动生成"后缀(.i1/.z1/.i2/.z2/.v1/.vz1/.i1p/.v1p/.bm/.bmt)。
//   缓存存超集(hideAuto=false), 出站按请求开关过滤 —— 两个视图永远一致。勿复制规则成两份。
bool ofsEntryVisibleEx(const char *name, bool isDir, bool hideAuto);
bool ofsEntryVisible(const char *name, bool isDir);   // 兼容旧名(等价 hideAuto=true)
bool fileApiFsTryDispatch();
// POST /fs/edit 上传是 4 参路由（onNotFound 收不到上传块）: fileApiInit 里调用一次
void fileApiFsRegisterUploadRoute();
// 上传墨水屏状态: 0=空闲 1=上传中 2=上传完毕 3=上传失败/中止
#define OFS_UP_PHASE_IDLE 0
#define OFS_UP_PHASE_UPLOADING 1
#define OFS_UP_PHASE_DONE 2
#define OFS_UP_PHASE_FAIL 3
// 上传状态回调（file_api_fs 上传回调里调用; 由 ink-reader-esp.ino 注入渲染函数实现"上传中/上传完毕"墨水屏）
// 传 phase + 最终文件名(完成/失败时非空, 中途中止为空)
typedef void (*OfsUpPhaseCallback)(int phase, const char *path);
void ofsUpSetPhaseCallback(OfsUpPhaseCallback cb);
int  ofsUpGetPhase();          // 供主循环/渲染查询当前上传阶段
// 下载墨水屏状态: 0=空闲 1=下载中 2=下载完毕 3=下载失败/中止
#define OFS_DL_PHASE_IDLE 0
#define OFS_DL_PHASE_START 1
#define OFS_DL_PHASE_DONE 2
#define OFS_DL_PHASE_FAIL 3
typedef void (*OfsDlPhaseCallback)(int phase, const char *path);
void ofsDlSetPhaseCallback(OfsDlPhaseCallback cb);   // 下载状态回调(下载 handler 起止触发)
// 通用文件管理操作墨水屏通知(2026-09, 参照上传/下载): /fs 新建文件/夹/删除/重命名/移动 起止上报。
// msg = 短文案(如 "新建:_x.txt" / "删除:/dir"); 渲染层参照 renderUploadStatus(配网页底行局刷)。
#define OFS_OP_PHASE_IDLE 0
#define OFS_OP_PHASE_START 1
#define OFS_OP_PHASE_DONE 2
#define OFS_OP_PHASE_FAIL 3
typedef void (*OfsOpPhaseCallback)(int phase, const char *msg);
void ofsOpSetPhaseCallback(OfsOpPhaseCallback cb);
void ofsOpReport(int phase, const char *msg);
int  ofsOpGetPhase();   // 供主循环在 handleClient 后浅栈渲染操作通知
// 请求边界堆探针（诊断）: file_api_fs.cpp 实现; wifi_managerLoop 在 handleClient() 后
// 借 gFsListJustHandled 打印 LOOP_AFTER（确认 /fs/list 每次请求后 heap 是否回升）
void fsListProbe(const char *tag);
uint32_t fsListReqNext();
void fsListProbeDetail(const char *tag, size_t start, size_t count, size_t emitted, bool hasMore);
extern bool gFsListJustHandled;

// ---- 对照实验探针（验证根因: "纯 AP 下 SD 访问 × AP hostap_input 竞争" 是否成立）----
// 与 /fs 调用链无关的最小反事实实验。编译期开关:
//   OFS_ABTEST_MODE = 0 → 完全不碰 SD（对照组 A: 只记录 AP+手机连接下的 heap/maxblk/stack 基线）
//   OFS_ABTEST_MODE = 1 → 极小 SD 读（实验组 B: reinitSdBus + SD.exists("/test.txt") + read 16B）
//   OFS_ABTEST_MODE = 2 → sdListDirPaged(root,0,10)（实验组 C: 最接近 /fs/list 的 SD 访问, 无 HTTP/JSON）
// 用途: 若 AP+无SD 与 AP+极小SD 都稳定、仅 /fs/list 崩 → 问题在 /fs 调用链而非"SD×AP 竞争";
//       若 AP+极小SD 明显退化 → C5(压低 SD 单次占用) 有值。wifiManagerLoop 的 AP_ONLY 定时区调用。
void ofsAbTestTick();

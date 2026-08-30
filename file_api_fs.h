#pragma once
#include <Arduino.h>

// 官方 A7 管理 Web "文件管理"高级层构造复刻（/fs/*, 移植自 web_test official_manager.cpp）
// - 走 onNotFound 分发（fileApiTryDispatch 前缀匹配, 零路由对象堆）: 见 fileApiFsTryDispatch
// - 管理对象 = SD 卡（官方 LittleFS; 构造不变）; 无鉴权（官方同款）; 显示过滤对齐屏上文件管理器
// - /fs/status O(1): 只读 sdAvailable + 缓存容量, 绝不遍历 SD（配网会话堆硬约束）
// 差异取舍见 docs/官方A7文件管理构造移植.md 与 docs/file-api.md
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
const char *ofsUpGetPhasePath();// 本次上传的文件路径（完成/失败时可用）
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

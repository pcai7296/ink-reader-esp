#pragma once
// sd_file_ops.h — SD 文件操作层（文件管理 API 的底层）
// 调用约定:
// - 所有函数假定入参为已规范化路径（normalizeApiPath 输出）, 且调用方已按需 reinitSdBus
// - 一次请求 = 调用方 reinitSdBus 一次 → 调本层函数完成整个操作（本层不重复 reinit）
// - 本层只做文件操作, 不涉及 HTTP/认证（那些在 file_api.cpp）
#include <Arduino.h>
#include <SD.h>

// reinitSdBus 定义于 file_manager.ino（局刷后 SPI 总线在 EPD 侧, SD 访问前必须恢复）
bool reinitSdBus(const char *reason);

// 目录项（回调/查询输出; name 为 basename, path 为完整路径, UTF-8）
struct SdEntry {
  char name[248];
  char path[400];       // 完整路径（搜索/客户端定位用）
  bool isDir;
  uint64_t size;        // 文件字节数; 目录=0
  bool isProtected;     // isProtectedPath(fullpath) 结果
  bool isUploading;     // isUploadingTemp(fullpath) 结果
};

// 操作错误码
enum SdErr {
  SD_OK = 0,
  SD_NOT_FOUND,      // 路径不存在
  SD_EXISTS,         // 目标已存在
  SD_NOT_EMPTY,      // 目录非空
  SD_PROTECTED,      // 受保护路径（由调用层先查 isProtectedPath, 本层兜底）
  SD_INVALID_MOVE,   // 移动防环
  SD_IO_FAIL,        // 底层失败（读/写/改名/挂载）
};

typedef void (*SdListCb)(const SdEntry *e, void *ctx);

// 列出目录（SD 枚举顺序, 未排序——客户端自行排序; 与设备 UI 的排序逻辑无关）。
// 返回枚举项数; 达到 maxItems 上限时 *truncated=true 并提前停止; 路径非目录返回 -1。
int sdListDir(const char *path, SdListCb cb, void *ctx, int maxItems, bool *truncated);

// 分页列出目录（/fs/list 用; 不破坏上方的 sdListDir 旧契约——/api/files 仍在用）。
// 先 Dir::next() 跳过 startOffset 项, 再喂 cb 直到 count+1 项:
//   - 前 count 项送 cb, 计入 *emitted;
//   - 第 count+1 项存在（但不再送 cb）→ *hasMore=true。
// 遍历为 O(startOffset+count), 但回调/编码/发送峰值只与 count 相关（ESP8266 约束）。
// 返回实际回调项数（≤count）; 目录不存在返回 -1; startOffset 超出目录 → *emitted=0*hasMore=false。
// ⚠️ 枚举一律用 SDFS.openDir + Dir::next()（官方 A7 同款）——File::openNextFile 每项重开文件,
// 大目录(600+)枚举 0 根因（见 REVERSE_NOTES.md §10）。
int sdListDirPaged(const char *path, size_t startOffset, size_t count,
                   SdListCb cb, void *ctx, size_t *emitted, bool *hasMore);

// 单文件/目录信息; 不存在返回 false
bool sdStat(const char *path, SdEntry *out);

// 容量（FSInfo64, 大卡精确）; SD 未挂载返回 false
bool sdCapacity(uint64_t *total, uint64_t *used);

// 删除: 文件直接删; 目录仅空目录可删（非空 → SD_NOT_EMPTY）
SdErr sdDelete(const char *path);

// 新建目录（父目录须存在; 已存在 → SD_EXISTS）
SdErr sdMkdir(const char *path);

// 同目录改名（newName 为净化后的 basename; 目标已存在 → SD_EXISTS）
SdErr sdRename(const char *oldPath, const char *newName);

// 跨目录移动（rename 同卷; 防环由调用层先查 sdMoveDestAllowed, 本层兜底;
// 目标目录须存在; 目标已存在 → SD_EXISTS）
SdErr sdMove(const char *path, const char *destDir);

// 递归搜索: 名称包含 query（忽略大小写）。
// 上限: maxDepth≤8, 结果 maxItems≤200, 扫描 ≤10000（*scanned 累计）;
// 达到结果/扫描上限即停, *truncated=true。
// 返回匹配数（≤maxItems）; 根目录不存在返回 -1。
int sdSearch(const char *root, const char *query, int maxDepth,
             SdListCb cb, void *ctx, int maxItems, int *scanned, bool *truncated);

// 清理残留 .uploading（有界递归, 删除所有临时文件; 扫描 ≤10000 项）
// 会话模型: 新会话开始即清——上一会话遗留的 .uploading 必然过期
// （本会话内断连产生的 .uploading 保留供 X-Resume 恢复, 本函数不会在会话中重复调用）。
// 返回删除数。
int sdCleanupUploading();

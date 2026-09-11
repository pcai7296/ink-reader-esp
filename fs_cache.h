#pragma once
// fs_cache.h — SD 目录树 → LittleFS 缓存（照抄官方: 文件管理用 LittleFS, 浏览不碰 SD 卡）
//
// 背景: 纯 AP 配网会话堆 ~5KB, /fs/list 实时遍历 SD 卡会与 AP hostap_input 争资源 → esf_buf_alloc 失败崩溃。
// 官方固件 `fileSystem = &LittleFS`（flash）, 文件管理全程 LittleFS 不碰 SD, 因此稳定。
// 本模块把 SD 目录树在"进 AP 前（堆充足）"扫描成 LittleFS 缓存文件, /fs/list 读缓存（LittleFS）→ 避开 SD×AP 竞争。
//
// 缓存格式: LittleFS `/fslist/<hash12>.txt`, 首行 = 原始目录路径, 其余 = 该目录 items 的 JSON 数组
//   [{"type":"dir|file","name":"..","size":N},...]（仅过"硬隐藏", 存超集: 含自动生成后缀与 .bin 等;
//   hideAuto 过滤在出站 fsCacheServeList 按请求开关执行——与实时列表同规则)。
// 路径 hash: djb2 → hex12; 读回校验首行路径, 防碰撞失真。
//
// 扫描上限: FS_CACHE_MAX_DEPTH 层 / FS_CACHE_MAX_DIRS 目录 / FS_CACHE_MAX_ITEMS 每目录项。
// 触及上限即"截断构建": 超限目录无缓存（/fs/list 自动回退实时列表）, 并落 /fslist/_TRUNCATED 标记文件（诊断用）。
#include <Arduino.h>

#define FS_CACHE_DIR       "/fslist"
#define FS_CACHE_MAX_DEPTH 3      // 扫描层级上限
#define FS_CACHE_MAX_DIRS  128    // 缓存目录数上限
#define FS_CACHE_MAX_ITEMS 100    // 每目录条目上限(超集: 需容纳自动文件; 出站再按 count/开关过滤)
#define FS_CACHE_MAX_FILES 4096   // 总条目上限（防大书库扫爆）

// 路径 → 缓存文件名（返回值写入 out, 如 "f1e2d3c4b5a67890.txt"）; 不访问 FS。
void fsCacheNameFor(const char *dir, char *out, size_t outSize);

// 进 AP 前调用: 递归扫描 SD 目录树 → 写 LittleFS 缓存（每目录一个文件）。
// 需已 reinitSdBus + SD 挂载; 先确保 LittleFS 挂载。返回写缓存目录数; 失败 -1。
// 线程安全: 仅进 AP 前调用一次（无并发）。
int fsCacheBuild();

// /fs/list 用: 读指定目录的 LittleFS 缓存, 按 hideAuto 过滤后按 start/count 切片; 缓存不存在 →
// 返回 false（调用方回退 SD）。hideAuto 与 /fs/list 实时同规则(ofsEntryVisibleEx)。
bool fsCacheServeList(const char *dir, size_t start, size_t count, bool hideAuto);

// 使指定目录的 LittleFS 缓存失效（删除对应缓存文件）。上传/删除/重命名后调用:
// 下次 /fs/list 读不到缓存 → 回退读 SD → 显示最新列表（否则缓存旧快照含已删文件/缺新文件）。
void fsCacheInvalidateDir(const char *dir);

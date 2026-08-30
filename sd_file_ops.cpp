// sd_file_ops.cpp — SD 文件操作层实现（见 sd_file_ops.h 契约）
// 注意: 先于 sd_file_ops.h 用宏绕开 SDFSImpl 的 protected 成员（仅本 TU, 类布局不变,
// 其他 TU 正常包含; 用于直读卡容量 sectorCount, info64 的 clusterCount 在 29GB 卡上失真）
#define protected public
#define private public
#include <SDFS.h>
#undef protected
#undef private
#include "sd_file_ops.h"
#include "sd_path.h"
#include <string.h>

// 从 File::name() 的完整路径提取 basename
static const char *baseNameOf(const char *full) {
  const char *p = full;
  for (const char *q = full; *q; q++) {
    if (*q == '/') p = q + 1;
  }
  return p;
}

// 填充 SdEntry（fullPath 用于保护/临时判定, name 取 basename, path 存完整路径）
static void fillEntry(const char *fullPath, bool isDir, uint64_t size, SdEntry *e) {
  // File::fullName() 返回根相对路径（无前导 /）; 规范化补上, 否则 isProtectedPath 判不了
  if (fullPath[0] == '/') {
    snprintf(e->path, sizeof(e->path), "%s", fullPath);
  } else {
    snprintf(e->path, sizeof(e->path), "/%s", fullPath);
  }
  snprintf(e->name, sizeof(e->name), "%s", baseNameOf(e->path));
  e->isDir = isDir;
  e->size = size;
  e->isProtected = isProtectedPath(e->path);
  e->isUploading = isUploadingTemp(e->path);
}

// 搜索回调用堆上 scratch（单线程同步消费; SdEntry 含 path 后 650B, 静态/栈上每层都放会
// 挤占配网会话堆——配网入口 AP 后堆仅 ~240B, 静态 RAM 90% 是主因）
static SdEntry *gScratchEntry = NULL;

// ⚠️★ 枚举根因（2026-09 确认, 见 docs/ 与 reverse_a7_a01/reports/CONCLUSION_BIG_DIR_ROOTCAUSE.md）:
// 官方 A7 枚举 = SDFS.openDir + Dir::next()（纯目录项扫描: File32::openNext + getName(_lfn,64) + close,
// 零堆分配、零路径解析）。File::openNextFile()（core FS.cpp）内部每项都会 _fakeDir->openFile("r")
// → SDFSImpl::open → FatFile::open(完整路径) 完整路径解析 + LFN 匹配 + 堆分配(FsName/File32/shared_ptr)
// → 大目录(600+)每次 O(n) 遍历父目录 = O(n²), 纯 AP 低堆下 malloc 失败 → openFile 返回空 File
// → if(!entry) break → 枚举 0 条。rewindDirectory() 只是"首次调用创建 _fakeDir"(与 openNextFile 相同),
// 是安慰剂修复, 从未真正生效。修复 = 全部枚举改用 Dir API, 绝不 openFile。
// Dir::fileName() 返回 basename（可能带前导 /, fillEntry 会归一化）; fileSize()/isDirectory() 为
// next() 缓存元数据, 不重开文件。

// 枚举前的目录有效性检查（只 open 目录本身一次, 不枚举项; Dir 无 operator bool, 用 File 探测）
static bool dirProbe(const char *path) {
  File p = SD.open(path, FILE_READ);
  if (!p || !p.isDirectory()) { if (p) p.close(); return false; }
  p.close();
  return true;
}

// Dir 无 fullName(): 由目录路径 + basename 拼完整路径（同官方 SDFSDirImpl::openFile 的拼法）
static void joinFull(const char *dir, const String &name, char *out, size_t outSize) {
  if (name.length() && name[0] == '/') {
    snprintf(out, outSize, "%s%s", dir, name.c_str());
  } else if (strcmp(dir, "/") == 0) {
    snprintf(out, outSize, "/%s", name.c_str());
  } else {
    snprintf(out, outSize, "%s/%s", dir, name.c_str());
  }
}

int sdListDir(const char *path, SdListCb cb, void *ctx, int maxItems, bool *truncated) {
  if (truncated) *truncated = false;
  if (!dirProbe(path)) return -1;
  Dir root = SDFS.openDir(path);
  // 局部 SdEntry（sdListDir 非递归, 650B 栈可承受; 搜索递归才用堆 scratch）
  SdEntry localEntry;
  int count = 0;
  while (root.next()) {
    String nm = root.fileName();
    bool isDir = root.isDirectory();
    char full[420];
    joinFull(path, nm, full, sizeof(full));
    // 保护/临时判定需要完整路径——Dir 无 fullName, joinFull 已拼
    fillEntry(full, isDir, isDir ? 0 : (uint64_t)root.fileSize(), &localEntry);
    bool stop = false;
    if (cb) cb(&localEntry, ctx);   // 写出这一项（ofsListItemCb → snprintf + sendContent）
    count++;
    // 每项让出（顺序: 写出下一项前先 yield→wdtFeed）:
    // 缩短两次 SDK/loop 调度之间的最长连续执行时间, 防大目录(大量 txt/bmp 全过白名单)
    // 在 handleClient 内同步遍历+sendContent 触发 Soft WDT reset（实测 stack=336）。
    // 全程每项让出比"每64项一次"更平稳, 代价是轻微吞吐下降（可接受: 网络 ~0.2MB/s 才是瓶颈）。
    yield();
    ESP.wdtFeed();
    if (count >= maxItems) { if (truncated) *truncated = true; stop = true; }
    if (stop) break;
  }
  return count;
}

// ---- 分页列表（/fs/list; 见 sd_file_ops.h 注释）----
int sdListDirPaged(const char *path, size_t startOffset, size_t count,
                   SdListCb cb, void *ctx, size_t *emitted, bool *hasMore) {
  if (emitted) *emitted = 0;
  if (hasMore) *hasMore = false;
  if (!dirProbe(path)) return -1;
  Dir root = SDFS.openDir(path);
  SdEntry localEntry;
  size_t skipped = 0;
  size_t e = 0;
  // 阶段1: 跳过 startOffset 项（SD 目录无随机跳转, 只能顺序 next()）
  while (skipped < startOffset) {
    if (!root.next()) return (int)e;   // 已到目录尾, 无更多
    skipped++;
    if ((skipped & 0x3F) == 0) { yield(); ESP.wdtFeed(); }   // 遍历喂狗（与 sdListDir 同策略）
  }
  // 阶段2: 读 count+1 项 —— 前 count 项送 cb, 第 count+1 项存在 ⇒ hasMore=true
  while (e <= count) {
    if (!root.next()) break;
    String nm = root.fileName();
    bool isDir = root.isDirectory();
    char full[420];
    joinFull(path, nm, full, sizeof(full));
    fillEntry(full, isDir, isDir ? 0 : (uint64_t)root.fileSize(), &localEntry);
    if (e < count) {
      if (cb) cb(&localEntry, ctx);
      e++;
    } else {
      // 第 count+1 项: 存在但不再送 cb → 有更多
      if (hasMore) *hasMore = true;
    }
    yield();
    ESP.wdtFeed();   // 每项让出（防 Soft WDT; 低堆 + 网络发送节奏）
  }
  if (emitted) *emitted = e;
  return (int)e;
}

bool sdStat(const char *path, SdEntry *out) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  fillEntry(path, f.isDirectory(), f.isDirectory() ? 0 : (uint64_t)f.size(), out);
  f.close();
  return true;
}

// 直读卡总容量（sectorCount×512, CSD 直读, 秒回且可靠;
// 实测 info64 的 clusterCount×bytesPerCluster 在 29GB 卡上报 901MB 失真, 弃用）
static uint64_t sdCardTotalBytes() {
  auto *impl = static_cast<sdfs::SDFSImpl *>(SDFS.getImpl().get());
  if (!impl || !impl->_fs.card()) return 0;
  return (uint64_t)impl->_fs.card()->sectorCount() * 512ULL;
}

// 目录遍历求和已用字节（递归累加所有文件真实字节; FAT 簇开销未计入, 近似值）
// scanned 上限 20000 防恶意大目录拖死; 截断时返回 false
static bool usedWalk(const char *dir, uint64_t *acc, int *scanned) {
  if (!dirProbe(dir)) return false;
  Dir d = SDFS.openDir(dir);
  while (d.next()) {
    if (*scanned > 20000) break;   // 上限: 防恶意大目录拖死（used 为近似值）
    (*scanned)++;
    if ((*scanned) % 128 == 0) {
      ESP.wdtFeed();
      yield();   // 软狗需让出主循环: 在 HTTP 处理器内冷启动遍历时防止 Soft WDT reset（实测）
    }
    if (d.isDirectory()) {
      // 递归传拼好的完整路径（Dir 无 fullName; File::name() 只给 basename, 嵌套路径会丢上下文）;
      // 不得复制大 char 缓冲: 每层栈缓冲超限撑爆 ESP8266 小栈（实测 Exception 5 Alloca 栈溢出）
      char child[420];
      joinFull(dir, d.fileName(), child, sizeof(child));
      usedWalk(child, acc, scanned);
      continue;
    }
    *acc += (uint64_t)d.fileSize();
  }
  return true;
}

bool sdCapacity(uint64_t *total, uint64_t *used) {
  uint64_t tot = sdCardTotalBytes();
  if (tot == 0) return false;
  uint64_t usedBytes = 0;
  int scanned = 0;
  usedWalk("/", &usedBytes, &scanned);
  if (usedBytes > tot) usedBytes = tot;   // 防御
  if (total) *total = tot;
  if (used) *used = usedBytes;
  return true;
}

SdErr sdDelete(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return SD_NOT_FOUND;
  bool isDir = f.isDirectory();
  f.close();
  if (isDir) {
    // 目录仅空可删
    if (!dirProbe(path)) return SD_NOT_FOUND;
    Dir d = SDFS.openDir(path);
    bool empty = !d.next();
    if (!empty) return SD_NOT_EMPTY;
    return SD.rmdir(path) ? SD_OK : SD_IO_FAIL;
  }
  return SD.remove(path) ? SD_OK : SD_IO_FAIL;
}

SdErr sdMkdir(const char *path) {
  if (SD.exists(path)) return SD_EXISTS;
  return SD.mkdir(path) ? SD_OK : SD_IO_FAIL;
}

SdErr sdRename(const char *oldPath, const char *newName) {
  // 同目录: 目标 = oldPath 的目录 + "/" + newName
  char dir[300];
  snprintf(dir, sizeof(dir), "%s", oldPath);
  char *slash = strrchr(dir, '/');
  if (!slash || slash == dir) {
    snprintf(dir, sizeof(dir), "/%s", newName);   // 根目录下文件
  } else {
    slash[1] = '\0';
    size_t dlen = strlen(dir);
    snprintf(dir + dlen, sizeof(dir) - dlen, "%s", newName);
  }
  if (!SD.exists(oldPath)) return SD_NOT_FOUND;
  if (SD.exists(dir)) return SD_EXISTS;
  return SD.rename(oldPath, dir) ? SD_OK : SD_IO_FAIL;
}

SdErr sdMove(const char *path, const char *destDir) {
  if (!sdMoveDestAllowed(path, destDir)) return SD_INVALID_MOVE;
  File d = SD.open(destDir, FILE_READ);
  if (!d || !d.isDirectory()) {
    if (d) d.close();
    return SD_NOT_FOUND;
  }
  d.close();
  if (!SD.exists(path)) return SD_NOT_FOUND;
  char target[320];
  const char *base = baseNameOf(path);
  snprintf(target, sizeof(target), "%s/%s", destDir, base);
  if (SD.exists(target)) return SD_EXISTS;
  return SD.rename(path, target) ? SD_OK : SD_IO_FAIL;
}

// 搜索内部递归: 返回匹配数; scanned 累计扫描项
static int searchWalk(const char *dirPath, const char *query, int depth, int maxDepth,
                      SdListCb cb, void *ctx, int maxItems, int *scanned, bool *truncated) {
  if (depth > maxDepth || *scanned >= 10000 || (*truncated)) return 0;
  if (!dirProbe(dirPath)) return 0;
  Dir dir = SDFS.openDir(dirPath);
  int found = 0;
  int sinceFeed = 0;
  while (dir.next()) {
    (*scanned)++;
    if (++sinceFeed >= 64) {
      sinceFeed = 0;
      ESP.wdtFeed();
      yield();   // 软狗需让出: HTTP 处理器内递归遍历(AP 慢网络)防 Soft WDT reset
    }
    String nm = dir.fileName();
    bool match = (strcasestr(nm.c_str(), query) != NULL);
    char full[420];
    if (match) {
      // 保护/临时判定用拼好的完整路径（Dir 无 fullName; 嵌套路径）; path 字段带完整路径供客户端定位
      joinFull(dirPath, nm, full, sizeof(full));
      fillEntry(full, dir.isDirectory(), dir.isDirectory() ? 0 : (uint64_t)dir.fileSize(), gScratchEntry);
      if (cb) cb(gScratchEntry, ctx);
      found++;
      if (found >= maxItems) { *truncated = true; break; }
    }
    if (dir.isDirectory()) {
      // 递归传拼好的完整路径（Dir 无 fullName）; 不复制大缓冲防栈溢出
      joinFull(dirPath, nm, full, sizeof(full));
      int sub = searchWalk(full, query, depth + 1, maxDepth, cb, ctx, maxItems, scanned, truncated);
      found += sub;
      if (found >= maxItems || *truncated) break;
    }
  }
  return found;
}

int sdSearch(const char *root, const char *query, int maxDepth,
             SdListCb cb, void *ctx, int maxItems, int *scanned, bool *truncated) {
  if (truncated) *truncated = false;
  if (scanned) *scanned = 0;
  if (maxDepth <= 0) maxDepth = 1;
  if (maxDepth > 8) maxDepth = 8;
  if (maxItems > 200) maxItems = 200;
  if (!query || query[0] == '\0') return -2;   // 空查询
  File r = SD.open(root, FILE_READ);
  if (!r || !r.isDirectory()) {
    if (r) r.close();
    return -1;
  }
  r.close();
  // 堆上 scratch（递归内调用; 用完即释放, 不常驻）
  gScratchEntry = (SdEntry *)malloc(sizeof(SdEntry));
  if (!gScratchEntry) return -3;   // 低堆: 搜索不可用, 不崩
  int ret = searchWalk(root, query, 1, maxDepth, cb, ctx, maxItems, scanned, truncated);
  free(gScratchEntry);
  gScratchEntry = NULL;
  return ret;
}

// .uploading 清理递归: 删除所有临时文件, 返回删除数
static int cleanupWalk(const char *dir, int *scanned) {
  if (*scanned > 10000) return 0;
  if (!dirProbe(dir)) return 0;
  Dir d = SDFS.openDir(dir);
  int removed = 0;
  while (d.next()) {
    (*scanned)++;
    if ((*scanned) % 64 == 0) {
      ESP.wdtFeed();
      yield();   // 软狗需让出（懒清理在 HTTP 处理器内执行）
    }
    String nm = d.fileName();
    if (d.isDirectory()) {
      char child[420];
      joinFull(dir, nm, child, sizeof(child));
      removed += cleanupWalk(child, scanned);   // 递归用拼好的完整路径（嵌套路径）
      continue;
    }
    if (isUploadingTemp(nm.c_str())) {
      // 先关句柄再 remove（FAT 对打开文件的删除会失败）; 用拼好的完整路径
      char full[420];
      joinFull(dir, nm, full, sizeof(full));
      if (SD.remove(full)) removed++;
      else Serial.printf("CLN_FAIL %s\n", full);
      continue;
    }
  }
  return removed;
}

int sdCleanupUploading() {
  int scanned = 0;
  int removed = cleanupWalk("/", &scanned);
  Serial.printf("CLN_UPLOADING removed=%d scanned=%d\n", removed, scanned);
  return removed;
}

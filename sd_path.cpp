// sd_path.cpp — 文件 API 路径安全 + 保护规则纯函数实现（见 sd_path.h 契约）
#include "sd_path.h"
#include <string.h>

static bool isControlChar(unsigned char c) {
  return c < 0x20 || c == 0x7f;
}

bool normalizeApiPath(const char *raw, char *out, size_t outSize) {
  if (!raw || !out || outSize == 0 || raw[0] != '/') return false;
  size_t oi = 0;
  const char *p = raw;
  bool prevSlash = false;
  while (*p) {
    unsigned char c = (unsigned char)*p;
    if (isControlChar(c)) return false;      // 控制字符
    if (c == '\\') return false;             // 反斜杠（Windows 分隔符穿越形态）
    if (c == '/') {
      if (!prevSlash) {                      // 折叠连续 '/'
        if (oi + 1 >= outSize) return false;
        out[oi++] = '/';
      }
      prevSlash = true;
      p++;
      continue;
    }
    prevSlash = false;
    // 段边界检查: '.' 或 '..' 独占一段 → 拒绝（不做 POSIX 归一化, 直接拒绝）
    if (c == '.') {
      const char *q = p;
      while (*q == '.') q++;
      if (q > p && (*q == '/' || *q == '\0')) return false;
    }
    if (oi + 1 >= outSize) return false;
    out[oi++] = (char)c;
    p++;
  }
  if (oi + 1 >= outSize) return false;
  out[oi] = '\0';
  return true;
}

bool sanitizeUploadName(const char *raw, char *out, size_t outSize) {
  if (!raw || !out || outSize == 0) return false;
  // 取 basename: 剥 'C:\fakepath\...' 与任何路径前缀
  const char *base = raw;
  for (const char *p = raw; *p; p++) {
    if (*p == '/' || *p == '\\') base = p + 1;
  }
  if (*base == '\0') return false;
  size_t len = strlen(base);
  if (len > 240) return false;               // FAT 长文件名安全上限
  if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)base[i];
    if (c == '/' || c == '\\') return false; // basename 内不应再有分隔符
    if (isControlChar(c)) return false;
  }
  if (len + 1 > outSize) return false;
  memcpy(out, base, len + 1);
  return true;
}

bool pathExtension(const char *path, char *extBuf, size_t extSize) {
  if (!path || !extBuf || extSize == 0) return false;
  const char *base = path;
  for (const char *p = path; *p; p++) {
    if (*p == '/') base = p + 1;
  }
  const char *dot = NULL;
  for (const char *p = base; *p; p++) {
    if (*p == '.') dot = p;                  // 最后一个点
  }
  // 点必须在 basename 首字符之后才算扩展名（隐藏文件 ".foo" 不算）
  if (!dot || dot == base) return false;
  size_t elen = strlen(dot);
  if (elen + 1 > extSize) return false;
  memcpy(extBuf, dot, elen + 1);
  return true;
}

// 受保护扩展名表（唯一权威）: ext 形如 ".i1", 精确匹配大小写不敏感
static bool extIsProtected(const char *ext) {
  static const char *const kProt[SD_PATH_PROTECTED_EXTS] = {
      ".i1", ".z1", ".i1p", ".v1", ".vz1", ".v1p", ".bm", ".bmt", ".i2", ".z2"};
  for (int i = 0; i < SD_PATH_PROTECTED_EXTS; i++) {
    if (strcasecmp(ext, kProt[i]) == 0) return true;
  }
  return false;
}

bool isProtectedPath(const char *path) {
  if (!path || path[0] != '/') return false;
  // 系统目录: 目录前缀边界（"/.tiemereader" 本身与 "/.tiemereader/..." 受保护,
  // "/.tiemereader_backup" 不误伤）
  static const char kSys[] = "/.tiemereader";
  size_t slen = sizeof(kSys) - 1;
  if (strncmp(path, kSys, slen) == 0) {
    char c = path[slen];
    if (c == '\0' || c == '/') return true;
  }
  // 索引/数据扩展名: 精确匹配（"test.i1.bak" 扩展名 .bak 不受保护）
  char ext[16];
  if (!pathExtension(path, ext, sizeof(ext))) return false;
  return extIsProtected(ext);
}

bool isProtectedBaseName(const char *baseName) {
  if (!baseName || !baseName[0]) return false;
  if (strchr(baseName, '/')) return false;   // 契约: 只接受 basename（含分隔符即非本函数用途）
  // 系统目录同名: 原实现拼全路径再查, 根目录改名为 ".tiemereader" 会被拒。basename 级保守保留
  // （任何目录下都不允许改成这个名字——否则 API 立刻造出一个不可删/不可改的项）。
  if (strcmp(baseName, ".tiemereader") == 0) return true;
  char ext[16];
  if (!pathExtension(baseName, ext, sizeof(ext))) return false;
  return extIsProtected(ext);
}

bool isUploadingTemp(const char *path) {
  if (!path) return false;
  size_t len = strlen(path);
  static const char kSuf[] = ".uploading";
  size_t slen = sizeof(kSuf) - 1;
  return len > slen && strcmp(path + len - slen, kSuf) == 0;
}

bool sdMoveDestAllowed(const char *path, const char *destDir) {
  if (!path || !destDir || path[0] != '/' || destDir[0] != '/') return false;
  if (strcmp(path, destDir) == 0) return false;          // 移到自身
  size_t plen = strlen(path);
  // destDir 是 path 的子孙: destDir 以 path + "/" 开头
  if (strncmp(destDir, path, plen) == 0 && destDir[plen] == '/') return false;
  return true;
}

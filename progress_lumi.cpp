// progress_lumi.cpp — LUMI1 阅读进度格式实现（纯 C++）
// 规范: docs/progress-lumi1.md；接口: progress_lumi.h
// 风格约束：无动态分配、无 Arduino String（与固件 AGENTS.md 一致，减少堆碎片）。

#include "progress_lumi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ---------- 生成 ----------

bool lumiMakeEx(char* out, size_t cap, uint64_t ts, uint32_t size, uint32_t offset, float pct,
                uint64_t fs, const char* h0, const char* h1, const char* h2) {
  if (cap < 64) return false;
  int n = snprintf(out, cap, "LUMI1\nts=%llu\nsize=%lu\noffset=%lu\npct=%.2f\n",
                   (unsigned long long)ts, (unsigned long)size, (unsigned long)offset, (double)pct);
  if (n < 0 || (size_t)n >= cap) return false;   // 必填放不下 → 整包失败（调用方处理）
  // v3 指纹组: 原子——h0/h1/h2 全给才附加; 放不下则整组省略（绝不截断成半组）
  if (h0 && h1 && h2) {
    int need = snprintf(NULL, 0, "fs=%llu\nh0=%s\nh1=%s\nh2=%s\n",
                        (unsigned long long)fs, h0, h1, h2);
    if (need > 0 && (size_t)(n + need) < cap) {
      snprintf(out + n, cap - n, "fs=%llu\nh0=%s\nh1=%s\nh2=%s\n",
               (unsigned long long)fs, h0, h1, h2);
      n += need;
    }
  }
  return true;
}

void lumiMake(char* out, size_t cap, uint64_t ts, uint32_t size, uint32_t offset, float pct) {
  lumiMakeEx(out, cap, ts, size, offset, pct, 0, NULL, NULL, NULL);
}

// ---------- 解析 ----------

namespace {

// 一行内取 key/value（不含行尾 '\n'）。line 长度可能含 '\r' → 由调用方按规范判无效。
// 返回: 找到 '=' 且 key 非空且 value 非空 → true
bool splitKeyValue(const char* line, size_t len, size_t& keyLen, const char*& value, size_t& valueLen) {
  const char* eq = (const char*)memchr(line, '=', len);
  if (!eq) return false;
  keyLen = (size_t)(eq - line);
  value = eq + 1;
  valueLen = len - keyLen - 1;
  return keyLen > 0 && valueLen > 0;
}

bool allDigits(const char* s, size_t len) {
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++)
    if (s[i] < '0' || s[i] > '9') return false;
  return true;
}

// pct: 数字 + 最多一个 '.'；返回 true 且解析成功
bool parsePct(const char* s, size_t len, float& out) {
  if (len == 0) return false;
  int dots = 0;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c == '.') { if (++dots > 1) return false; }
    else if (c < '0' || c > '9') return false;
  }
  char buf[40];
  if (len >= sizeof(buf)) return false;
  memcpy(buf, s, len); buf[len] = '\0';
  out = (float)atof(buf);
  return true;
}

// v3 指纹: 恰好 40 个小写 hex
bool isHex40(const char* s, size_t len) {
  if (len != 40) return false;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

} // namespace

bool lumiParse(const char* data, size_t len, LumiProgress& out) {
  out.valid = false;
  out.hasFile = false;
  out.file[0] = '\0';
  out.ts = 0; out.size = 0; out.offset = 0; out.pct = 0.0f;
  // v3 指纹组
  out.fs = 0;
  out.h0[0] = out.h1[0] = out.h2[0] = '\0';
  out.hasFingerprint = false;
  if (!data || len == 0) return false;

  bool gotTs = false, gotSize = false, gotOffset = false, gotPct = false;
  // v3: 原子组——任一字段缺失/重复/非法 → 整组无效 (hasFingerprint=false)
  bool gotFs = false, gotH0 = false, gotH1 = false, gotH2 = false;
  bool fpBad = false;
  uint64_t fpFs = 0;
  char fpH0[41] = {0}, fpH1[41] = {0}, fpH2[41] = {0};
  size_t pos = 0;
  int lineNo = 0;

  while (pos < len) {
    // 找本行结尾（'\n'）
    const char* nl = (const char*)memchr(data + pos, '\n', len - pos);
    size_t lineLen = nl ? (size_t)(nl - (data + pos)) : (len - pos);
    const char* line = data + pos;

    // 规则：任何 \r → 整包无效；单行 >128B → 无效
    if (memchr(line, '\r', lineLen)) return false;
    if (lineLen > LUMI_MAX_LINE) return false;

    if (lineNo == 0) {
      // 首行必须恰为版本行
      if (lineLen != strlen(LUMI_VERSION_LINE)) return false;
      if (memcmp(line, LUMI_VERSION_LINE, lineLen) != 0) return false;
    } else {
      size_t keyLen; const char* value; size_t valueLen;
      if (!splitKeyValue(line, lineLen, keyLen, value, valueLen)) return false;

      // 必填 key（重复 → 无效）
      if (keyLen == 2 && memcmp(line, "ts", 2) == 0) {
        if (gotTs || !allDigits(value, valueLen) || valueLen > 19) return false;
        char buf[24]; memcpy(buf, value, valueLen); buf[valueLen] = '\0';
        out.ts = strtoull(buf, 0, 10); gotTs = true;
      } else if (keyLen == 4 && memcmp(line, "size", 4) == 0) {
        if (gotSize || !allDigits(value, valueLen) || valueLen > 10) return false;
        char buf[16]; memcpy(buf, value, valueLen); buf[valueLen] = '\0';
        out.size = (uint32_t)strtoul(buf, 0, 10); gotSize = true;
      } else if (keyLen == 6 && memcmp(line, "offset", 6) == 0) {
        if (gotOffset || !allDigits(value, valueLen) || valueLen > 10) return false;
        char buf[16]; memcpy(buf, value, valueLen); buf[valueLen] = '\0';
        out.offset = (uint32_t)strtoul(buf, 0, 10); gotOffset = true;
      } else if (keyLen == 3 && memcmp(line, "pct", 3) == 0) {
        if (gotPct || !parsePct(value, valueLen, out.pct)) return false;
        gotPct = true;
      } else if (keyLen == 4 && memcmp(line, "file", 4) == 0) {
        // 可选：首个生效，>64B 忽略该字段（不判整包无效）
        if (!out.hasFile && valueLen <= LUMI_MAX_FILE_NAME) {
          memcpy(out.file, value, valueLen); out.file[valueLen] = '\0';
          out.hasFile = true;
        }
      } else if (keyLen == 2 && memcmp(line, "fs", 2) == 0) {
        // v3 指纹组: fs=uint64 十进制 (≤20 位); 重复/非法 → 整组无效
        if (gotFs || !allDigits(value, valueLen) || valueLen > 20) fpBad = true;
        else {
          char buf[24]; memcpy(buf, value, valueLen); buf[valueLen] = '\0';
          fpFs = strtoull(buf, 0, 10); gotFs = true;
        }
      } else if (keyLen == 2 && memcmp(line, "h0", 2) == 0) {
        if (gotH0 || !isHex40(value, valueLen)) fpBad = true;
        else { memcpy(fpH0, value, valueLen); fpH0[valueLen] = '\0'; gotH0 = true; }
      } else if (keyLen == 2 && memcmp(line, "h1", 2) == 0) {
        if (gotH1 || !isHex40(value, valueLen)) fpBad = true;
        else { memcpy(fpH1, value, valueLen); fpH1[valueLen] = '\0'; gotH1 = true; }
      } else if (keyLen == 2 && memcmp(line, "h2", 2) == 0) {
        if (gotH2 || !isHex40(value, valueLen)) fpBad = true;
        else { memcpy(fpH2, value, valueLen); fpH2[valueLen] = '\0'; gotH2 = true; }
      }
      // 其他 key（未知/扩展：chapterIndex、charOffset…）忽略
    }

    pos += lineLen;
    if (nl) pos += 1;       // 跳过 '\n'
    else break;             // 末尾无换行：接受最后一个字段行
    lineNo++;
  }

  if (lineNo == 0) return false;          // 空包
  if (!gotTs || !gotSize || !gotOffset || !gotPct) return false;  // 缺必填
  if (out.offset > out.size) return false;  // offset 超自身 size
  if (out.pct < 0.0f || out.pct > 100.0f) return false;  // pct 越界
  // v3 指纹组原子判定: 四字段全部存在且合法 → 完整指纹; 否则整组视为不存在
  if (gotFs && gotH0 && gotH1 && gotH2 && !fpBad) {
    out.fs = fpFs;
    memcpy(out.h0, fpH0, 41); memcpy(out.h1, fpH1, 41); memcpy(out.h2, fpH2, 41);
    out.hasFingerprint = true;
  }
  out.valid = true;
  return true;
}

// ---------- hex 编码 (v3 指纹) ----------

void lumiHexEncode(const uint8_t* src, size_t n, char* out) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hex[src[i] >> 4];
    out[i * 2 + 1] = hex[src[i] & 15];
  }
  out[n * 2] = '\0';
}

// ---------- SHA-1 (RFC 3174, 纯 C, 无外部依赖; 与 Android MessageDigest 输出一致) ----------

static uint32_t lumiRotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void lumiSha1Block(uint32_t h[5], const uint8_t* p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (int i = 16; i < 80; i++) w[i] = lumiRotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999u; }
    else if (i < 40) { f = b ^ c ^ d;          k = 0x6ED9EBA1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
    else             { f = b ^ c ^ d;          k = 0xCA62C1D6u; }
    uint32_t t = lumiRotl(a, 5) + f + e + k + w[i];
    e = d; d = c; c = lumiRotl(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void lumiSha1(const uint8_t* data, size_t len, uint8_t out[20]) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  size_t i = 0;
  while (len - i >= 64) { lumiSha1Block(h, data + i); i += 64; }
  // 尾部填充: [剩余字节] 0x80 0x00... 长度(bit, big-endian)
  uint8_t tail[128];
  size_t rem = len - i;
  memcpy(tail, data + i, rem);
  tail[rem] = 0x80;
  size_t padLen = (rem < 56) ? 64 : 128;
  memset(tail + rem + 1, 0, padLen - rem - 9);
  uint64_t bitLen = (uint64_t)len * 8ULL;
  for (int k = 0; k < 8; k++) tail[padLen - 1 - k] = (uint8_t)(bitLen >> (8 * k));
  lumiSha1Block(h, tail);
  if (padLen == 128) lumiSha1Block(h, tail + 64);
  for (int k = 0; k < 5; k++) {
    out[k * 4] = (uint8_t)(h[k] >> 24);
    out[k * 4 + 1] = (uint8_t)(h[k] >> 16);
    out[k * 4 + 2] = (uint8_t)(h[k] >> 8);
    out[k * 4 + 3] = (uint8_t)(h[k]);
  }
}

void lumiFingerprintRegions(const uint8_t* data, size_t size,
                            char* out0, char* out1, char* out2) {
  // §3.4 区域公式（与固件 progress_sync 分段切片、Android 实现一致）
  size_t h0s = 0, h0e = (size < 1024) ? size : 1024;
  size_t cen = size / 2;
  size_t h1s = (cen > 512) ? (cen - 512) : 0;
  size_t h1e = (size < cen + 512) ? size : (cen + 512);
  size_t h2s = (size > 1024) ? (size - 1024) : 0;
  size_t h2e = size;
  uint8_t d[20];
  lumiSha1(data + h0s, h0e - h0s, d); lumiHexEncode(d, 20, out0);
  lumiSha1(data + h1s, h1e - h1s, d); lumiHexEncode(d, 20, out1);
  lumiSha1(data + h2s, h2e - h2s, d); lumiHexEncode(d, 20, out2);
}

// ---------- URL 编码 ----------

void lumiUrlEncodeFilename(const char* s, char* out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p && o + 3 < cap; p++) {
    unsigned char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '.' || c == '_' || c == '~') {
      out[o++] = (char)c;
    } else {
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 15];
    }
  }
  out[o] = '\0';
}

// 直连手机协议 GET 请求行: "GET /progress?file=<RFC3986 编码文件名> HTTP/1.1"
// 复用 lumiUrlEncodeFilename（保留字符集/大写十六进制规则单一来源）。
void lumiBuildProgressRequest(const char* filename, char* out, size_t cap) {
  char enc[LUMI_MAX_FILE_NAME * 4 + 1];   // 64B 文件名 → 编码 ≤192B + 安全余量
  lumiUrlEncodeFilename(filename, enc, sizeof(enc));
  snprintf(out, cap, "GET /progress?file=%s HTTP/1.1", enc);
}

void lumiBuildCloudPath(const char* endpoint, const char* filename, char* out, size_t cap) {
  const char* PREFIX = "/Apps/Books/.LumiBooks/Cache/";
  const char* SUFFIX = ".lumi";
  size_t o = 0;
  size_t ep = strlen(endpoint);
  while (ep > 0 && endpoint[ep - 1] == '/') ep--;   // 去尾斜杠
  if (ep + 1 < cap) { memcpy(out, endpoint, ep); o = ep; }
  // PREFIX 自带前导 '/'，此处不再补 '/'（否则产生 "dav//Apps" 双斜杠，与规范 §2 不符）
  size_t pl = strlen(PREFIX);
  if (o + pl < cap) { memcpy(out + o, PREFIX, pl); o += pl; }
  // 编码文件名
  static const char hex[] = "0123456789ABCDEF";
  for (const unsigned char* p = (const unsigned char*)filename; *p && o + 3 + 5 < cap; p++) {
    unsigned char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '.' || c == '_' || c == '~') {
      out[o++] = (char)c;
    } else {
      out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15];
    }
  }
  size_t sl = strlen(SUFFIX);
  if (o + sl < cap) { memcpy(out + o, SUFFIX, sl); o += sl; }
  out[o] = '\0';
}

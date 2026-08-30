// progress_lumi.h — LUMI1 阅读进度格式（纯 C++，无 Arduino 依赖）
// 规范文档: docs/progress-lumi1.md
// 本模块可同时编译进固件 (progress_sync.cpp 调用) 与 PC 测试 (pc_tests/progress_lumi_test.cpp)。
// 与 App 侧 LumiProgressCodec.kt 逐规则一致（fixture 交叉核对见两端测试）。
#pragma once
#include <stdint.h>
#include <stddef.h>

#define LUMI_VERSION_LINE     "LUMI1"
#define LUMI_MAX_LINE         128          // 单行上限（不含 '\n'），超长整包无效
#define LUMI_MAX_FILE_NAME    64           // 可选 file= 字段上限（不含结尾 '\0'）

struct LumiProgress {
  uint64_t ts;        // epoch 毫秒（无时钟时可为 0）
  uint32_t size;      // 源文件字节数
  uint32_t offset;    // 原始 TXT 字节偏移
  float    pct;       // 0.00..100.00
  bool     valid;     // 解析成功
  char     file[LUMI_MAX_FILE_NAME + 1];  // 可选 file= 字段（UTF-8），无则空串
  bool     hasFile;   // 是否携带 file=
  // ---- v3 文件指纹组（原子：四字段全部存在且合法才 hasFingerprint=true）----
  uint64_t fs;        // 发送方文件字节数（十进制）
  char     h0[41];    // 文件头 [0, min(size,1024)) 的 SHA-1 hex（40 小写）
  char     h1[41];    // 中部 [max(0,size/2-512), min(size,size/2+512))
  char     h2[41];    // 尾部 [max(0,size-1024), size)
  bool     hasFingerprint;   // 完整指纹组（fs+h0+h1+h2）存在且合法
};

// 生成 LUMI1 文本（含结尾 '\n'，保证 null 结尾；cap 至少 96）
void lumiMake(char* out, size_t cap, uint64_t ts, uint32_t size, uint32_t offset, float pct);

// v3: 带可选文件指纹组的生成。fs/h0/h1/h2 原子组——任一指纹参数为 NULL/fs 缺失则整组省略；
// 容量预算: 必填放不下 → 返回 false (调用方整包失败); 指纹组放不下 → 整组省略 (不截断)。
bool lumiMakeEx(char* out, size_t cap, uint64_t ts, uint32_t size, uint32_t offset, float pct,
                uint64_t fs, const char* h0, const char* h1, const char* h2);

// 解析 LUMI1。data 可为非 null 结尾块，len 为其长度。失败 out.valid=false。
// 规则见 docs/progress-lumi1.md §3：LF-only、单行 ≤128B、必填校验、未知 key 忽略。
bool lumiParse(const char* data, size_t len, LumiProgress& out);

// RFC3986 文件名 percent-encoding：保留 A-Za-z0-9-._~，其余 UTF-8 字节转 %XX（大写十六进制）
void lumiUrlEncodeFilename(const char* s, char* out, size_t cap);

// 字节 → 小写 hex（out 至少 2n+1；n 字节 → 2n hex + '\0'），v3 指纹用
void lumiHexEncode(const uint8_t* src, size_t n, char* out);

// ---- v3 文件指纹（纯 C SHA-1，RFC 3174，无外部依赖；pc_test/固件共用）----
void lumiSha1(const uint8_t* data, size_t len, uint8_t out[20]);

// 按 docs/progress-lumi1.md §3.4 区域算法对完整文件缓冲计算三处 SHA-1 hex：
//   h0 = [0, min(size,1024));  h1 = [max(0,size/2-512), min(size,size/2+512));  h2 = [max(0,size-1024), size)
// out0/out1/out2 各至少 41B。跨平台 fixture 用（固件分段调用 lumiSha1 + lumiHexEncode，切片公式与此一致）。
void lumiFingerprintRegions(const uint8_t* data, size_t size,
                            char* out0, char* out1, char* out2);

// 直连手机协议 GET 请求行: "GET /progress?file=<RFC3986 编码文件名> HTTP/1.1"（不含行尾 CRLF）
void lumiBuildProgressRequest(const char* filename, char* out, size_t cap);

// endpoint(去尾斜杠) + /Apps/Books/.LumiBooks/Cache/<编码文件名>.lumi (已弃用, 直连取代)
void lumiBuildCloudPath(const char* endpoint, const char* filename, char* out, size_t cap);

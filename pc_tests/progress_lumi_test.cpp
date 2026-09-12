// progress_lumi_test.cpp — LUMI1 阅读进度格式 PC 测试（固件侧，逐字节钦定 fixture）
// 规范: docs/progress-lumi1.md；被测实现: progress_lumi.h/.cpp（纯 C++，无 Arduino 依赖）
// 编译(MinGW-W64, 项目根): g++ -std=c++11 -Wall -Wextra pc_tests\progress_lumi_test.cpp progress_lumi.cpp -I. -o pc_tests\progress_lumi_test.exe
// 编译(MSVC):     cl /nologo /W3 /EHsc progress_lumi_test.cpp ..\progress_lumi.cpp /I.. /Fe:progress_lumi_test.exe
// 运行: .\progress_lumi_test.exe   （全部断言通过输出 ALL PASS，退出码 0）
// fixture 与 App 侧 LumiProgressCodecTest.kt 逐字节一致（docs/progress-lumi1.md §6 交叉核对）。

#include "progress_lumi.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name) do { \
    if (cond) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s\n", name); } \
} while (0)

#define CHECK_STR(actual, expected, name) do { \
    if (actual && strcmp(actual, expected) == 0) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s (got=%s, want=%s)\n", name, actual ? actual : "(null)", expected); } \
} while (0)

// 武炼巅峰 的 UTF-8 字节（\x 转义避免 MSVC 源码编码问题）
#define WULIAN_DINGFENG "\xE6\xAD\xA6\xE7\x82\xBC\xE5\xB7\x85\xE5\xB3\xB0"

// ---------- 钦定 fixture（与 App 侧 Kotlin 测试逐字节一致） ----------

// F1: 基础正例（含结尾 LF）
static const char* F1 = "LUMI1\nts=1724400000000\nsize=12345678\noffset=123456\npct=73.10\n";

// F2: 携带可选字段（file 为 UTF-8 中文名；未知 key 应被忽略）
static const char* F2 =
    "LUMI1\nts=1724400000000\nsize=12345678\noffset=123456\npct=73.10\n"
    "file=" WULIAN_DINGFENG ".txt\n"
    "chapterIndex=123\n"
    "charOffset=456\n"
    "foo=bar\n";

// F3: 末行无换行（允许）
static const char* F3 = "LUMI1\nts=1\nsize=100\noffset=10\npct=10.00";

// ---------- 反例（全部必须 valid=false） ----------
static const char* I1 = "LUMI2\nts=1\nsize=100\noffset=10\npct=10.00\n";   // 版本错
static const char* I2 = "LUMI1\nts=1\nsize=100\noffset=10\n";               // 缺 pct
static const char* I3 = "LUMI1\nts=1\nsize=100\noffset=200\npct=50.00\n";   // offset>size
static const char* I4 = "LUMI1\nts=1\nsize=100\noffset=10\npct=abc\n";      // pct 非数字
static const char* I5 = "LUMI1\nts=1\nsize=100\noffset=10\npct=101.00\n";   // pct>100
static const char* I6 = "LUMI1\r\nts=1\r\nsize=100\r\noffset=10\r\npct=10.00\r\n";  // 含 \r
static const char* I8 = "";                                                 // 空
static const char* I9 = "hello world";                                      // 首行非版本行
static const char* I10 = "LUMI1\nts=1\nts=2\nsize=100\noffset=10\npct=10.00\n";  // 重复必填 ts
static const char* I11 = "LUMI1\n\nts=1\nsize=100\noffset=10\npct=10.00\n";      // 空行

// I7: 行超 128B（"x=" + 129 个 'a'）——运行时构造
static char i7[200];
static void buildI7() {
    const char* head = "LUMI1\nts=1\nsize=100\noffset=10\npct=10.00\nx=";
    size_t h = strlen(head);
    memcpy(i7, head, h);
    memset(i7 + h, 'a', 129);
    i7[h + 129] = '\n';
    i7[h + 130] = '\0';
}

int main() {
    buildI7();

    // ===== M1: lumiMake 输出（逐字节） =====
    {
        char out[256];
        lumiMake(out, sizeof(out), 1724400000000ULL, 12345678UL, 123456UL, 73.10f);
        CHECK_STR(out, "LUMI1\nts=1724400000000\nsize=12345678\noffset=123456\npct=73.10\n",
                  "M1: lumiMake 输出逐字节一致 (LF 结尾, 无 % 号)");
    }

    // ===== F1: 基础正例 =====
    {
        LumiProgress p;
        bool ok = lumiParse(F1, strlen(F1), p);
        CHECK(ok, "F1: 解析返回 true");
        CHECK(p.valid, "F1: valid==true");
        CHECK(p.ts == 1724400000000ULL, "F1: ts==1724400000000");
        CHECK(p.size == 12345678UL, "F1: size==12345678");
        CHECK(p.offset == 123456UL, "F1: offset==123456");
        CHECK(fabs((double)p.pct - 73.10) < 0.001, "F1: pct≈73.10 (误差<0.001)");
        CHECK(!p.hasFile, "F1: hasFile==false");
        CHECK(strlen(F1) < 256, "F1: 编码输出总字节数 < 256");
    }

    // ===== F2: 可选字段 + 未知 key 忽略 =====
    {
        LumiProgress p;
        bool ok = lumiParse(F2, strlen(F2), p);
        CHECK(ok, "F2: 解析返回 true");
        CHECK(p.valid, "F2: valid==true");
        CHECK(p.hasFile, "F2: hasFile==true");
        CHECK_STR(p.file, WULIAN_DINGFENG ".txt", "F2: file 恰为 武炼巅峰.txt (UTF-8 字节)");
        CHECK(p.ts == 1724400000000ULL, "F2: ts 仍解析 (未知 key 不干扰)");
        CHECK(p.size == 12345678UL, "F2: size 仍解析");
        CHECK(p.offset == 123456UL, "F2: offset 仍解析");
        CHECK(fabs((double)p.pct - 73.10) < 0.001, "F2: pct 仍解析");
    }

    // ===== F3: 末行无换行 =====
    {
        LumiProgress p;
        bool ok = lumiParse(F3, strlen(F3), p);
        CHECK(ok, "F3: 末行无换行 → 解析成功");
        CHECK(p.valid, "F3: valid==true");
        CHECK(p.pct > 9.99f && p.pct < 10.01f, "F3: pct≈10.00");
    }

    // ===== I1-I11: 反例（全部必须 valid=false） =====
    {
        struct { const char* s; const char* name; } inv[] = {
            { I1, "I1: 版本错 (LUMI2) → 无效" },
            { I2, "I2: 缺必填 pct → 无效" },
            { I3, "I3: offset>size → 无效" },
            { I4, "I4: pct 非数字 → 无效" },
            { I5, "I5: pct>100 → 无效" },
            { I6, "I6: 含 \\r → 无效" },
            { i7, "I7: 行超 128B → 无效" },
            { I8, "I8: 空包 → 无效" },
            { I9, "I9: 首行非版本行 → 无效" },
            { I10, "I10: 重复必填 ts → 无效" },
            { I11, "I11: 空行 → 无效" },
        };
        for (size_t i = 0; i < sizeof(inv) / sizeof(inv[0]); i++) {
            LumiProgress p;
            bool ok = lumiParse(inv[i].s, strlen(inv[i].s), p);
            CHECK(!ok && !p.valid, inv[i].name);
        }
    }

    // ===== E1-E3: lumiUrlEncodeFilename =====
    {
        char out[256];
        lumiUrlEncodeFilename(WULIAN_DINGFENG ".txt", out, sizeof(out));
        CHECK_STR(out, "%E6%AD%A6%E7%82%BC%E5%B7%85%E5%B3%B0.txt", "E1: 中文文件名 RFC3986 编码");
    }
    {
        char out[256];
        lumiUrlEncodeFilename("a b%c#d?.txt", out, sizeof(out));
        CHECK_STR(out, "a%20b%25c%23d%3F.txt", "E2: 空格/%/#/? 编码");
    }
    {
        char out[256];
        lumiUrlEncodeFilename("A-Z0-9-._~", out, sizeof(out));
        CHECK_STR(out, "A-Z0-9-._~", "E3: 保留字符 A-Za-z0-9-._~ 原样");
    }

    // ===== C1-C2: lumiBuildCloudPath =====
    {
        char out[512];
        lumiBuildCloudPath("https://dav.jianguoyun.com/dav/", WULIAN_DINGFENG ".txt", out, sizeof(out));
        CHECK_STR(out, "https://dav.jianguoyun.com/dav/Apps/Books/.LumiBooks/Cache/%E6%AD%A6%E7%82%BC%E5%B7%85%E5%B3%B0.txt.lumi",
                  "C1: 带尾斜杠 endpoint + 中文文件名");
    }
    {
        char out[512];
        lumiBuildCloudPath("http://192.168.0.10:8080/dav", "a b.txt", out, sizeof(out));
        CHECK_STR(out, "http://192.168.0.10:8080/dav/Apps/Books/.LumiBooks/Cache/a%20b.txt.lumi",
                  "C2: 无尾斜杠 endpoint + 空格文件名");
    }

    // ===== D0 直连手机协议 (docs/progress-lumi1.md §2) =====
    // P1: PUT body = lumiMake 输出 + file=<原始 UTF-8 文件名>\n (逐字节)
    {
        char lumi[256];
        lumiMake(lumi, sizeof(lumi), 1724400000000ULL, 12345678UL, 123456UL, 73.10f);
        char putBody[320];
        snprintf(putBody, sizeof(putBody), "%sfile=%s\n", lumi, WULIAN_DINGFENG ".txt");
        CHECK_STR(putBody,
                  "LUMI1\nts=1724400000000\nsize=12345678\noffset=123456\npct=73.10\n"
                  "file=" WULIAN_DINGFENG ".txt\n",
                  "P1: PUT body = lumiMake + file=<原始 UTF-8 文件名> 行 (逐字节)");
    }
    // P2: GET 请求行 (lumiBuildProgressRequest 钦定字面量)
    {
        char req[300];
        lumiBuildProgressRequest(WULIAN_DINGFENG ".txt", req, sizeof(req));
        CHECK_STR(req, "GET /progress?file=%E6%AD%A6%E7%82%BC%E5%B7%85%E5%B3%B0.txt HTTP/1.1",
                  "P2: GET 请求行 (中文文件名 RFC3986 编码)");
    }
    // P3: GET 请求行与 lumiUrlEncodeFilename 拼接结果一致 (双实现交叉核对)
    {
        char enc[256];
        lumiUrlEncodeFilename(WULIAN_DINGFENG ".txt", enc, sizeof(enc));
        char want[320];
        snprintf(want, sizeof(want), "GET /progress?file=%s HTTP/1.1", enc);
        char req[300];
        lumiBuildProgressRequest(WULIAN_DINGFENG ".txt", req, sizeof(req));
        CHECK_STR(req, want, "P3: GET 请求行 == 手拼 lumiUrlEncodeFilename");
    }

    // ===== v3 文件指纹组 (docs/progress-lumi1.md §3.4) =====
    // H1-H3: SHA-1 标准向量 (与 App 侧 Kotlin 同预期)
    {
        uint8_t d[20]; char hex[41];
        lumiSha1((const uint8_t*)"", 0, d); lumiHexEncode(d, 20, hex);
        CHECK_STR(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "H1: SHA-1(\"\") 标准向量");
        lumiSha1((const uint8_t*)"abc", 3, d); lumiHexEncode(d, 20, hex);
        CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d", "H2: SHA-1(\"abc\") 标准向量");
        uint8_t a64[64]; memset(a64, 'a', 64);
        lumiSha1(a64, 64, d); lumiHexEncode(d, 20, hex);
        CHECK_STR(hex, "0098ba824b5c16427bd7a1122a5a442a25ec644d", "H3: SHA-1('a'×64) 标准向量");
    }
    // FP1: 区域算法 fixture —— 与 App 侧 LumiProgressCodecTest 同一输入/同预期 (跨平台一致性锚点)
    {
        uint8_t data[2048];
        for (int i = 0; i < 2048; i++) {
            data[i] = (uint8_t)(((uint32_t)i * 2654435761u) >> 24);
        }
        char h0[41], h1[41], h2[41];
        lumiFingerprintRegions(data, sizeof(data), h0, h1, h2);
        CHECK_STR(h0, "27f5437d5dd60a0b696464ec473256dda66f0059", "FP1: h0 == 跨平台预期 (Knuth 序列 [0,1024))");
        CHECK_STR(h1, "489325aa1da1805e9a9b7af8a1f1b41f9d8415a9", "FP1: h1 == 跨平台预期 ([512,1536))");
        CHECK_STR(h2, "5c9d5e64fb59659a2f7467cb0486207d8e9b3085", "FP1: h2 == 跨平台预期 ([1024,2048))");
        // 空文件: 三处均 SHA-1("")
        char e0[41], e1[41], e2[41];
        lumiFingerprintRegions(data, 0, e0, e1, e2);
        CHECK_STR(e0, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "FP2: size=0 → h0=SHA-1(\"\")");
        CHECK_STR(e1, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "FP2: size=0 → h1=SHA-1(\"\")");
        CHECK_STR(e2, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "FP2: size=0 → h2=SHA-1(\"\")");
        // 小文件边界: size=1 → 三处都取 [0,1)
        lumiFingerprintRegions(data, 1, e0, e1, e2);
        uint8_t d[20]; char want[41];
        lumiSha1(data, 1, d); lumiHexEncode(d, 20, want);
        CHECK_STR(e0, want, "FP3: size=1 → 三处同取 [0,1)");
        CHECK_STR(e1, want, "FP3: size=1 → 三处同取 [0,1)");
        CHECK_STR(e2, want, "FP3: size=1 → 三处同取 [0,1)");
    }
    // M2: lumiMakeEx 带指纹组输出逐字节
    {
        char out[320];
        bool ok = lumiMakeEx(out, sizeof(out), 1724400000000ULL, 12345678UL, 123456UL, 73.10f,
                             2048ULL, "27f5437d5dd60a0b696464ec473256dda66f0059",
                             "489325aa1da1805e9a9b7af8a1f1b41f9d8415a9",
                             "5c9d5e64fb59659a2f7467cb0486207d8e9b3085");
        CHECK(ok, "M2: lumiMakeEx 返回 true");
        CHECK_STR(out,
                  "LUMI1\nts=1724400000000\nsize=12345678\noffset=123456\npct=73.10\n"
                  "fs=2048\nh0=27f5437d5dd60a0b696464ec473256dda66f0059\n"
                  "h1=489325aa1da1805e9a9b7af8a1f1b41f9d8415a9\n"
                  "h2=5c9d5e64fb59659a2f7467cb0486207d8e9b3085\n",
                  "M2: lumiMakeEx 指纹组逐字节一致 (原子全量)");
    }
    // M3: 容量预算——必填放不下 → false; 指纹组放不下 → 整组省略 (不截断)
    {
        char out[128];
        bool ok = lumiMakeEx(out, sizeof(out), 1724400000000ULL, 12345678UL, 123456UL, 73.10f,
                             2048ULL, "27f5437d5dd60a0b696464ec473256dda66f0059",
                             "489325aa1da1805e9a9b7af8a1f1b41f9d8415a9",
                             "5c9d5e64fb59659a2f7467cb0486207d8e9b3085");
        CHECK(ok, "M3: cap 仅够必填 → true (指纹组省略)");
        CHECK(strstr(out, "fs=") == NULL && strstr(out, "h0=") == NULL, "M3: 指纹组整组省略, 无半组");
        CHECK(strlen(out) < 128, "M3: 输出不越界");
        char tiny[60];
        ok = lumiMakeEx(tiny, sizeof(tiny), 1724400000000ULL, 12345678UL, 123456UL, 73.10f,
                        2048ULL, "27f5437d5dd60a0b696464ec473256dda66f0059",
                        "489325aa1da1805e9a9b7af8a1f1b41f9d8415a9",
                        "5c9d5e64fb59659a2f7467cb0486207d8e9b3085");
        CHECK(!ok, "M3: cap 连必填都放不下 → false (整包失败)");
    }
    // F4-F8: 指纹组解析 (原子性)
    {
        const char* F4 = "LUMI1\nts=1\nsize=100\noffset=10\npct=10.00\n"
            "fs=2048\nh0=27f5437d5dd60a0b696464ec473256dda66f0059\n"
            "h1=489325aa1da1805e9a9b7af8a1f1b41f9d8415a9\n"
            "h2=5c9d5e64fb59659a2f7467cb0486207d8e9b3085\n";
        LumiProgress p;
        bool ok = lumiParse(F4, strlen(F4), p);
        CHECK(ok && p.valid, "F4: 完整指纹组 → 解析成功");
        CHECK(p.hasFingerprint, "F4: hasFingerprint==true");
        CHECK(p.fs == 2048ULL, "F4: fs==2048");
        CHECK_STR(p.h0, "27f5437d5dd60a0b696464ec473256dda66f0059", "F4: h0 正确");
        CHECK_STR(p.h2, "5c9d5e64fb59659a2f7467cb0486207d8e9b3085", "F4: h2 正确");
    }
    {
        // 半组: 缺 h2
        const char* F5 = "LUMI1\nts=1\nsize=100\noffset=10\npct=10.00\n"
            "fs=2048\nh0=27f5437d5dd60a0b696464ec473256dda66f0059\n"
            "h1=489325aa1da1805e9a9b7af8a1f1b41f9d8415a9\n";
        LumiProgress p;
        bool ok = lumiParse(F5, strlen(F5), p);
        CHECK(ok && p.valid, "F5: 半组 (缺 h2) → 整包仍有效 (指纹是可选)");
        CHECK(!p.hasFingerprint, "F5: 半组 → hasFingerprint==false (整组视为不存在)");
    }
    {
        // 非法 h0 (含大写 hex)
        const char* F6 = "LUMI1\nts=1\nsize=100\noffset=10\npct=10.00\n"
            "fs=2048\nh0=27F5437D5DD60A0B696464EC473256DDA66F0059\n"
            "h1=489325aa1da1805e9a9b7af8a1f1b41f9d8415a9\n"
            "h2=5c9d5e64fb59659a2f7467cb0486207d8e9b3085\n";
        LumiProgress p;
        bool ok = lumiParse(F6, strlen(F6), p);
        CHECK(ok && p.valid, "F6: 非法 h0 (大写) → 整包仍有效");
        CHECK(!p.hasFingerprint, "F6: 非法 h0 → hasFingerprint==false");
    }
    {
        // 重复 fs
        const char* F7 = "LUMI1\nts=1\nsize=100\noffset=10\npct=10.00\n"
            "fs=100\nfs=200\nh0=27f5437d5dd60a0b696464ec473256dda66f0059\n"
            "h1=489325aa1da1805e9a9b7af8a1f1b41f9d8415a9\n"
            "h2=5c9d5e64fb59659a2f7467cb0486207d8e9b3085\n";
        LumiProgress p;
        bool ok = lumiParse(F7, strlen(F7), p);
        CHECK(ok && p.valid, "F7: 重复 fs → 整包仍有效");
        CHECK(!p.hasFingerprint, "F7: 重复 fs → hasFingerprint==false");
    }
    {
        // fs 超 20 位
        const char* F8 = "LUMI1\nts=1\nsize=100\noffset=10\npct=10.00\n"
            "fs=184467440737095516150\nh0=27f5437d5dd60a0b696464ec473256dda66f0059\n"
            "h1=489325aa1da1805e9a9b7af8a1f1b41f9d8415a9\n"
            "h2=5c9d5e64fb59659a2f7467cb0486207d8e9b3085\n";
        LumiProgress p;
        bool ok = lumiParse(F8, strlen(F8), p);
        CHECK(ok && p.valid, "F8: fs 超 20 位 → 整包仍有效");
        CHECK(!p.hasFingerprint, "F8: fs 溢出 → hasFingerprint==false");
    }

    printf("----\nPASS=%d FAIL=%d\n", g_pass, g_fail);
    if (g_fail > 0) {
        printf("HAS FAILURES\n");
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}

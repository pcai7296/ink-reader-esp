// hitokoto_parse_test.cpp — PC 端解析器测试（真实一言响应样本）
// 编译: g++ -std=c++11 -Wall -Wextra hitokoto_parse_test.cpp hitokoto.cpp -o hitokoto_parse_test.exe
// 运行: .\hitokoto_parse_test.exe   （全部断言通过输出 ALL PASS，退出码 0）

#include "hitokoto.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int g_pass = 0;

// 简单 UTF-8 完整性校验：字符串无孤立首字节/续字节（截断残留检测）
static bool validUtf8(const char* s) {
    if (!s) return false;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p < 0x80) { p++; }
        else if ((*p & 0xE0) == 0xC0) {
            if (!p[1] || (p[1] & 0xC0) != 0x80) return false;
            p += 2;
        }
        else if ((*p & 0xF0) == 0xE0) {
            if (!p[1] || !p[2] || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return false;
            p += 3;
        }
        else return false;
    }
    return true;
}

#define CHECK(cond, name) do { \
    if (cond) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s\n", name); } \
} while (0)

#define CHECK_STR(actual, expected, name) do { \
    if (actual && strcmp(actual, expected) == 0) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s (got=%s, want=%s)\n", name, actual ? actual : "(null)", expected); } \
} while (0)

// ---------- 样本 ----------

// 标准响应（中文一言 + 出处）
static const char* SAMPLE_OK =
    "{\"id\":12345,\"hitokoto\":\"人总要有梦想，万一实现了呢。\",\"type\":\"a\","
    "\"from\":\"网络\",\"from_who\":null,\"creator\":\"xxx\",\"created_at\":\"2020-01-01 00:00:00\"}";

// 含转义引号与反斜杠
static const char* SAMPLE_ESCAPE =
    "{\"hitokoto\":\"他说：\\\"你好\\\"，然后\\\\笑了。\"}";

// 控制字符转义（\n）
static const char* SAMPLE_CTRL =
    "{\"hitokoto\":\"第一行\\n第二行\"}";

// 无 hitokoto 字段
static const char* SAMPLE_NO_FIELD =
    "{\"from\":\"网络\",\"status_code\":\"ok\"}";

// 空值
static const char* SAMPLE_EMPTY =
    "{\"hitokoto\":\"\"}";

// 未闭合字符串
static const char* SAMPLE_UNCLOSED =
    "{\"hitokoto\":\"abc}";

// 完全不是 JSON
static const char* SAMPLE_GARBAGE =
    "not json at all <html>";

// 超长内容（200 字节）→ 截断到 cap-1
static const char* SAMPLE_LONG =
    "{\"hitokoto\":\"这是一个非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常"
    "非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常"
    "非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常"
    "非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常"
    "非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常"
    "非常长的句子用来测试截断逻辑。\"}";

// 截断落在 UTF-8 汉字中间（63 字节处恰好是汉字中间）→ 回退到字符边界
static const char* SAMPLE_UTF8_EDGE =
    "{\"hitokoto\":\"123456789012345678901234567890123456789012345678901234567890汉汉字字字字字\"}";

int main() {
    char out[64];

    CHECK(parseHitokoto(SAMPLE_OK, out, sizeof(out)), "standard ok");
    CHECK_STR(out, "人总要有梦想，万一实现了呢。", "standard content");

    CHECK(parseHitokoto(SAMPLE_ESCAPE, out, sizeof(out)), "escape ok");
    CHECK_STR(out, "他说：\"你好\"，然后\\笑了。", "escape content");

    CHECK(parseHitokoto(SAMPLE_CTRL, out, sizeof(out)), "ctrl ok");
    CHECK_STR(out, "第一行 第二行", "ctrl \\n -> space");

    CHECK(!parseHitokoto(SAMPLE_NO_FIELD, out, sizeof(out)), "no field -> false");
    CHECK(!parseHitokoto(SAMPLE_EMPTY, out, sizeof(out)), "empty -> false");
    CHECK(!parseHitokoto(SAMPLE_UNCLOSED, out, sizeof(out)), "unclosed -> false");
    CHECK(!parseHitokoto(SAMPLE_GARBAGE, out, sizeof(out)), "garbage -> false");
    CHECK(!parseHitokoto(NULL, out, sizeof(out)), "null buf -> false");
    CHECK(!parseHitokoto(SAMPLE_OK, NULL, 0), "null out -> false");

    CHECK(parseHitokoto(SAMPLE_LONG, out, sizeof(out)), "long ok");
    CHECK(strlen(out) <= 63, "long truncated <= 63");
    CHECK(validUtf8(out), "long valid utf8");

    CHECK(parseHitokoto(SAMPLE_UTF8_EDGE, out, sizeof(out)), "utf8 edge ok");
    CHECK(strlen(out) <= 63, "utf8 edge truncated <= 63");
    CHECK(validUtf8(out), "utf8 edge valid utf8");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail > 0) return 1;
    printf("ALL PASS\n");
    return 0;
}
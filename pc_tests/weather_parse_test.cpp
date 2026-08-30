// weather_parse_test.cpp — PC 端解析器测试（真实心知天气响应样本）
// 编译: g++ -std=c++11 -Wall -Wextra weather_parse_test.cpp weather_data.cpp -o weather_parse_test.exe
// 运行: .\weather_parse_test.exe   （全部断言通过输出 ALL PASS，退出码 0）

#include "weather_data.h"
#include <stdio.h>
#include <string.h>

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

// ---------- 真实样本 ----------

// now.json（中文城市/天气现象）
static const char* SAMPLE_NOW =
    "{\"results\":[{\"location\":{\"id\":\"WX4FBXXFKE4F\",\"name\":\"深圳\",\"country\":\"CN\","
    "\"path\":\"深圳,深圳,广东,中国\",\"timezone\":\"Asia/Shanghai\",\"timezone_offset\":\"+08:00\"},"
    "\"now\":{\"text\":\"晴\",\"code\":\"0\",\"temperature\":\"26\"},"
    "\"last_update\":\"2026-08-15T08:30:00+08:00\"}]}";

// daily.json（3 天）
static const char* SAMPLE_DAILY =
    "{\"results\":[{\"location\":{\"id\":\"WX4FBXXFKE4F\",\"name\":\"深圳\",\"country\":\"CN\"},"
    "\"daily\":["
    "{\"date\":\"2026-08-15\",\"text_day\":\"晴\",\"code_day\":\"0\",\"text_night\":\"晴\",\"code_night\":\"0\","
    "\"high\":\"33\",\"low\":\"26\",\"rainfall\":\"0.0\",\"precip\":\"0.0\",\"wind_direction\":\"东南\","
    "\"wind_direction_degree\":\"135\",\"wind_speed\":\"12.6\",\"wind_scale\":\"2\",\"humidity\":\"65\"},"
    "{\"date\":\"2026-08-16\",\"text_day\":\"多云\",\"code_day\":\"1\",\"text_night\":\"多云\",\"code_night\":\"1\","
    "\"high\":\"32\",\"low\":\"27\",\"rainfall\":\"0.0\",\"precip\":\"0.0\",\"wind_direction\":\"南\","
    "\"wind_direction_degree\":\"180\",\"wind_speed\":\"10.8\",\"wind_scale\":\"2\"},"
    "{\"date\":\"2026-08-17\",\"text_day\":\"阵雨\",\"code_day\":\"3\",\"text_night\":\"阵雨\",\"code_night\":\"3\","
    "\"high\":\"31\",\"low\":\"26\",\"rainfall\":\"5.2\",\"precip\":\"5.2\",\"wind_direction\":\"东南\","
    "\"wind_direction_degree\":\"135\",\"wind_speed\":\"15.3\",\"wind_scale\":\"3\"}"
    "]}]}";

// life/suggestion.json
static const char* SAMPLE_LIFE =
    "{\"results\":[{\"date\":\"2026-08-15\",\"suggestion\":{\"uv\":{\"brief\":\"中等\"}}}]}";

// 错误样本
static const char* SAMPLE_ERR =
    "{\"status_code\":\"AP010001\",\"status\":\"\",\"results\":[]}";

// 坏 JSON 样本
static const char* SAMPLE_MALFORMED_1 = "{\"results\":[{\"now\":{\"text\":\"晴\"}]";   // 缺括号/键
static const char* SAMPLE_MALFORMED_2 = "{\"results\":[{\"now\":{\"text\":晴}}]}";     // 值无引号
static const char* SAMPLE_MALFORMED_3 = "not json at all";

// 超长值样本（200 字符 city）
static const char* SAMPLE_LONG_CITY =
    "{\"results\":[{\"location\":{\"id\":\"X\",\"name\":\"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\",\"country\":\"CN\"},"
    "\"now\":{\"text\":\"晴\",\"code\":\"0\",\"temperature\":\"26\"},"
    "\"last_update\":\"2026-08-15T08:30:00+08:00\"}]}";

// 温度非数字样本
static const char* SAMPLE_BAD_TEMP =
    "{\"results\":[{\"location\":{\"id\":\"X\",\"name\":\"北京\",\"country\":\"CN\"},"
    "\"now\":{\"text\":\"晴\",\"code\":\"0\",\"temperature\":\"abc\"},"
    "\"last_update\":\"2026-08-15T08:30:00+08:00\"}]}";

int main() {
    // ===== parseActual =====
    {
        ActualWeather w;
        bool ok = parseActual(SAMPLE_NOW, &w);
        CHECK(ok, "parseActual: 真实样本解析成功");
        CHECK_STR(w.city, "深圳", "parseActual: city");
        CHECK_STR(w.weatherName, "晴", "parseActual: weatherName");
        CHECK_STR(w.weatherCode, "0", "parseActual: weatherCode");
        CHECK_STR(w.temp, "26", "parseActual: temp");
        CHECK_STR(w.lastUpdate, "2026-08-15T08:30:00+08:00", "parseActual: lastUpdate");
    }
    // 超长 city 截断
    {
        ActualWeather w;
        bool ok = parseActual(SAMPLE_LONG_CITY, &w);
        CHECK(ok, "parseActual: 超长 city 样本解析成功");
        CHECK(strlen(w.city) <= 15, "parseActual: 超长 city 截断不溢出");
    }
    // 温度非数字 → 失败
    {
        ActualWeather w;
        CHECK(!parseActual(SAMPLE_BAD_TEMP, &w), "parseActual: 温度非数字返回 false");
    }

    // ===== parseFuture =====
    {
        FutureWeather f;
        bool ok = parseFuture(SAMPLE_DAILY, &f);
        CHECK(ok, "parseFuture: 真实样本解析成功");
        CHECK_STR(f.date[0], "2026-08-15", "parseFuture: date[0]");
        CHECK_STR(f.textDay[0], "晴", "parseFuture: textDay[0]");
        CHECK_STR(f.textNight[0], "晴", "parseFuture: textNight[0]");
        CHECK_STR(f.high[0], "33", "parseFuture: high[0]");
        CHECK_STR(f.low[0], "26", "parseFuture: low[0]");
        CHECK_STR(f.date[1], "2026-08-16", "parseFuture: date[1]");
        CHECK_STR(f.textDay[1], "多云", "parseFuture: textDay[1]");
        CHECK_STR(f.high[1], "32", "parseFuture: high[1]");
        CHECK_STR(f.date[2], "2026-08-17", "parseFuture: date[2]");
        CHECK_STR(f.textDay[2], "阵雨", "parseFuture: textDay[2]");
        CHECK_STR(f.high[2], "31", "parseFuture: high[2]");
        CHECK_STR(f.low[2], "26", "parseFuture: low[2]");
        CHECK_STR(f.humidity, "65", "parseFuture: humidity (daily[0])");
        CHECK_STR(f.windScale, "2", "parseFuture: windScale (daily[0])");
    }

    // ===== parseLife =====
    {
        LifeIndex l;
        bool ok = parseLife(SAMPLE_LIFE, &l);
        CHECK(ok, "parseLife: 真实样本解析成功");
        CHECK_STR(l.uvi, "中等", "parseLife: uvi brief");
    }

    // ===== parseErrCode =====
    {
        char code[16];
        bool ok = parseErrCode(SAMPLE_ERR, code, sizeof(code));
        CHECK(ok, "parseErrCode: 提取成功");
        CHECK_STR(code, "AP010001", "parseErrCode: code 值");
        CHECK_STR(weatherErrorText(code), "无此城市", "weatherErrorText: AP010001");
        CHECK_STR(weatherErrorText("AP010004"), "无此用户", "weatherErrorText: AP010004");
        CHECK_STR(weatherErrorText("ZZZZ"), "网络错误", "weatherErrorText: 未知码");
        CHECK_STR(weatherErrorText(NULL), "网络错误", "weatherErrorText: NULL");
    }

    // ===== 坏 JSON =====
    {
        ActualWeather w;
        FutureWeather f;
        LifeIndex l;
        CHECK(!parseActual(SAMPLE_MALFORMED_1, &w), "parseActual: 缺键坏 JSON 返回 false");
        CHECK(!parseActual(SAMPLE_MALFORMED_2, &w), "parseActual: 值无引号返回 false");
        CHECK(!parseActual(SAMPLE_MALFORMED_3, &w), "parseActual: 非 JSON 返回 false");
        CHECK(!parseFuture(SAMPLE_MALFORMED_1, &f), "parseFuture: 坏 JSON 返回 false");
        CHECK(!parseFuture(SAMPLE_MALFORMED_3, &f), "parseFuture: 非 JSON 返回 false");
        CHECK(!parseLife(SAMPLE_MALFORMED_3, &l), "parseLife: 非 JSON 返回 false");
        char code[16];
        CHECK(!parseErrCode(SAMPLE_MALFORMED_3, code, sizeof(code)), "parseErrCode: 非 JSON 返回 false");
    }

    printf("----\nPASS=%d FAIL=%d\n", g_pass, g_fail);
    if (g_fail > 0) {
        printf("HAS FAILURES\n");
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
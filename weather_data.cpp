// weather_data.cpp — 天气数据层实现（心知天气 API）
// 解析器为纯 C（无 String/ArduinoJson），PC 测试与固件共用
// HTTP 获取仅在 ARDUINO 编译时启用

#include "weather_data.h"
#include <string.h>
#if defined(ARDUINO)
#include <stdio.h>      // snprintf_P (flash 格式串, Step D)
#include <pgmspace.h>   // PSTR
#endif

// ---------- 辅助函数 ----------

// 在 buf 中从 pos 起查找键 "key":，返回指向值起始（跳过空白与引号）的指针；找不到返回 NULL
static const char* findKeyValue(const char* pos, const char* key, size_t keyLen) {
    while (pos && *pos) {
        const char* hit = strstr(pos, key);
        if (!hit) return NULL;
        const char* p = hit + keyLen;
        // 跳过空白
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == ':') {
            p++;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p == '"') return p + 1;  // 指向字符串值首字符
            return NULL;                  // 值不是字符串
        }
        pos = hit + 1;  // 继续向后找
    }
    return NULL;
}

// 从 src（指向字符串值首字符）拷贝到下一个未转义引号为止；截断到 cap-1 字节并补 '\0'
static bool copyStringValue(const char* src, char* dst, size_t cap) {
    if (!src || cap == 0) return false;
    size_t i = 0;
    while (*src && *src != '"' && i + 1 < cap) {
        if (*src == '\\') {  // 跳过转义符（值内少见，保守处理）
            src++;
            if (!*src) break;
        }
        dst[i++] = *src++;
    }
    dst[i] = '\0';
    return i > 0;
}

// 查找 "key" 并提取字符串值到 dst
static bool extractString(const char* buf, const char* key, char* dst, size_t cap) {
    size_t keyLen = strlen(key);
    const char* v = findKeyValue(buf, key, keyLen);
    if (!v) return false;
    return copyStringValue(v, dst, cap);
}

// ---------- 解析器 ----------

bool parseActual(const char* buf, ActualWeather* out) {
    if (!buf || !out) return false;

    // 城市: location 对象内的 name
    const char* loc = strstr(buf, "\"location\"");
    if (!loc) return false;
    if (!extractString(loc, "\"name\"", out->city, sizeof(out->city))) return false;

    // 实况: now 对象
    const char* now = strstr(buf, "\"now\"");
    if (!now) return false;
    if (!extractString(now, "\"text\"", out->weatherName, sizeof(out->weatherName))) return false;
    if (!extractString(now, "\"code\"", out->weatherCode, sizeof(out->weatherCode))) return false;
    if (!extractString(now, "\"temperature\"", out->temp, sizeof(out->temp))) return false;
    extractString(now, "\"humidity\"", out->humidity, sizeof(out->humidity));   // 可选(缺省空串)

    if (!extractString(buf, "\"last_update\"", out->lastUpdate, sizeof(out->lastUpdate))) return false;

    // 简单合理性校验：温度应为数字（可含负号）
    for (size_t i = 0; out->temp[i]; i++) {
        char c = out->temp[i];
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+')) return false;
    }
    return true;
}

bool parseFuture(const char* buf, FutureWeather* out) {
    if (!buf || !out) return false;

    // 定位 daily 数组
    const char* arr = strstr(buf, "\"daily\"");
    if (!arr) return false;
    arr = strchr(arr, '[');
    if (!arr) return false;
    arr++;  // 越过 '['
    // 跳过第一个 '{' 前的空白
    while (*arr && (*arr == ' ' || *arr == '\t' || *arr == '\r' || *arr == '\n')) arr++;
    if (*arr != '{') return false;

    const char* objStart = arr;
    for (int i = 0; i < 3; i++) {
        // 每个对象: 从 '{' 到下一个 '}'（不嵌套的简单对象）
        if (!objStart || *objStart != '{') return false;
        const char* objEnd = strchr(objStart, '}');
        if (!objEnd) return false;

        if (!extractString(objStart, "\"date\"", out->date[i], sizeof(out->date[i]))) return false;
        if (!extractString(objStart, "\"text_day\"", out->textDay[i], sizeof(out->textDay[i]))) return false;
        if (!extractString(objStart, "\"text_night\"", out->textNight[i], sizeof(out->textNight[i]))) return false;
        if (!extractString(objStart, "\"high\"", out->high[i], sizeof(out->high[i]))) return false;
        if (!extractString(objStart, "\"low\"", out->low[i], sizeof(out->low[i]))) return false;

        if (i == 0) {  // 湿度/风力取 daily[0]
            extractString(objStart, "\"humidity\"", out->humidity, sizeof(out->humidity));
            extractString(objStart, "\"wind_scale\"", out->windScale, sizeof(out->windScale));
        }

        // 下一对象: 从 '}' 后找下一个 '{'
        objStart = strchr(objEnd + 1, '{');
    }
    return true;
}

bool parseLife(const char* buf, LifeIndex* out) {
    if (!buf || !out) return false;
    const char* uv = strstr(buf, "\"uv\"");
    if (!uv) return false;
    return extractString(uv, "\"brief\"", out->uvi, sizeof(out->uvi));
}

bool parseErrCode(const char* buf, char* code, size_t cap) {
    if (!buf || !code || cap == 0) return false;
    return extractString(buf, "\"status_code\"", code, cap);
}

const char* weatherErrorText(const char* code) {
    if (!code) return "网络错误";
    // 心知 API 业务错误码
    if (strcmp(code, "AP010001") == 0) return "无此城市";
    if (strcmp(code, "AP010002") == 0) return "无此权限";
    if (strcmp(code, "AP010003") == 0) return "访问超限";
    if (strcmp(code, "AP010004") == 0) return "无此用户";
    // 固件本地错误码
    if (strcmp(code, "NOKEY") == 0) return "未配置天气KEY";
    if (strcmp(code, "NOCFG") == 0) return "未配置WiFi";
    if (strcmp(code, "NOWIFI") == 0) return "WiFi连接失败";
    if (strcmp(code, "PARSE") == 0) return "数据解析失败";
    // ESP8266HTTPClient 负值错误码（httpGet 存为 "HTTP-<n>"）
    if (strcmp(code, "HTTP-1") == 0) return "API连接被拒";
    if (strcmp(code, "HTTP-4") == 0) return "API未连接";
    if (strcmp(code, "HTTP-5") == 0) return "API连接丢失";
    if (strcmp(code, "HTTP-7") == 0) return "API服务器无响应";
    if (strcmp(code, "HTTP-8") == 0) return "API内存不足";
    if (strcmp(code, "HTTP-11") == 0) return "API请求超时";
    // HTTP 状态码
    if (strcmp(code, "HTTP401") == 0) return "API密钥无效";
    if (strcmp(code, "HTTP403") == 0) return "API无权限";
    if (strcmp(code, "HTTP404") == 0) return "API地址错误";
    if (strncmp(code, "HTTP", 4) == 0) return "API请求失败";
    return "网络错误";
}

#ifdef ARDUINO
// ---------- HTTP 获取 ----------
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

// 单个请求（失败重试 1 次）：成功返回 true 并填充 body；
// 失败填 errCode（优先 status_code，其次 HTTP<code>）
static bool httpGet(HTTPClient& http, const char* url, String& outBody,
                    char* errCode, size_t errCap) {
    WiFiClient client;
    for (int attempt = 0; attempt < 2; attempt++) {
        http.begin(client, url);
        http.setTimeout(2000);  // 单请求最坏 4s（含重试）< WDT 8s
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            outBody = http.getString();
            http.end();
            return true;
        }
        String b = http.getString();
        char tmp[16];
        if (parseErrCode(b.c_str(), tmp, sizeof(tmp))) {
            strncpy(errCode, tmp, errCap - 1);
            errCode[errCap - 1] = '\0';
        } else {
            snprintf_P(errCode, errCap, PSTR("HTTP%d"), code);
        }
        http.end();
    }
    return false;
}

bool fetchWeather(ActualWeather* a, FutureWeather* f, LifeIndex* l,
                  const char* key, const char* city,
                  char* errCode, size_t errCap,
                  void (*onStep)(int step)) {
    if (!a || !f || !l || !key || !city || !errCode || errCap == 0) return false;
    errCode[0] = '\0';

    char url[256];
    String body;
    HTTPClient http;

    // 1. 实况 now.json（对齐 A7 步骤提示: 获取天气实况数据）
    snprintf_P(url, sizeof(url),
             PSTR("http://api.seniverse.com/v3/weather/now.json?key=%s&location=%s&language=zh-Hans&unit=c"),
             key, city);
    if (!httpGet(http, url, body, errCode, errCap)) return false;
    if (!parseActual(body.c_str(), a)) { snprintf_P(errCode, errCap, PSTR("PARSE")); return false; }
    if (onStep) onStep(0);
    ESP.wdtFeed();  // 端点间喂狗

    // 2. 未来 daily.json（获取未来天气数据）
    snprintf_P(url, sizeof(url),
             PSTR("http://api.seniverse.com/v3/weather/daily.json?key=%s&location=%s&language=zh-Hans&unit=c&start=0&days=3"),
             key, city);
    if (!httpGet(http, url, body, errCode, errCap)) return false;
    if (!parseFuture(body.c_str(), f)) { snprintf_P(errCode, errCap, PSTR("PARSE")); return false; }
    if (onStep) onStep(1);
    ESP.wdtFeed();  // 端点间喂狗

    // 3. 紫外线 life/suggestion.json（获取生活指数）
    snprintf_P(url, sizeof(url),
             PSTR("http://api.seniverse.com/v3/life/suggestion.json?key=%s&location=%s&language=zh-Hans"),
             key, city);
    if (!httpGet(http, url, body, errCode, errCap)) return false;
    if (!parseLife(body.c_str(), l)) { snprintf_P(errCode, errCap, PSTR("PARSE")); return false; }
    if (onStep) onStep(2);

    return true;
}
#endif // ARDUINO
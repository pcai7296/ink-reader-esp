// bili_fans.cpp — B站粉丝数 (api.bilibili.com/x/relation/stat?vmid=<uid>&jsonp=jsonp)
// 参考 hitokoto.cpp: 纯 C 解析 + ARDUINO 段 HTTP 获取; 阻塞 ≤2.5s, 失败静默

#include "bili_fans.h"
#include <string.h>
#include <stdio.h>

// 提取 "follower": 后的整数 (可能带小数 → 只取整数部分)
bool parseBiliFollower(const char *buf, uint32_t *out) {
    if (!buf || !out) return false;
    const char *key = "\"follower\"";
    const char *p = strstr(buf, key);
    if (!p) return false;
    const char *c = p + strlen(key);
    while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') c++;
    if (*c != ':') return false;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') c++;
    if (*c < '0' || *c > '9') return false;
    uint64_t v = 0;
    while (*c >= '0' && *c <= '9') {
        v = v * 10 + (uint64_t)(*c - '0');
        if (v > 0xFFFFFFFFull) return false;   // 溢出防御
        c++;
    }
    *out = (uint32_t)v;
    return true;
}

#ifdef ARDUINO
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

bool fetchBiliFollower(const char *uid, uint32_t *out, char *errCode, size_t errCap) {
    if (!uid || !uid[0] || !out || !errCode || errCap == 0) return false;
    out[0] = 0;
    errCode[0] = '\0';
    char url[96];
    // uid 只允许数字, 防注入 URL
    size_t ui = 0;
    while (uid[ui] >= '0' && uid[ui] <= '9' && ui < 32) ui++;
    if (ui == 0) { snprintf(errCode, errCap, "UID"); return false; }
    snprintf(url, sizeof(url), "http://api.bilibili.com/x/relation/stat?vmid=%.*s&jsonp=jsonp",
             (int)ui, uid);

    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, url)) { snprintf(errCode, errCap, "BEGIN"); return false; }
    http.setTimeout(2500);
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        String body = http.getString();
        http.end();
        if (parseBiliFollower(body.c_str(), out)) return true;
        snprintf(errCode, errCap, "PARSE");
        return false;
    }
    snprintf(errCode, errCap, "HTTP%d", code);
    http.end();
    return false;
}
#endif // ARDUINO

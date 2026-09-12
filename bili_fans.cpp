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
    // uid 只允许数字, 防注入 URL
    size_t ui = 0;
    while (uid[ui] >= '0' && uid[ui] <= '9' && ui < 32) ui++;
    if (ui == 0) { snprintf(errCode, errCap, PSTR("UID")); return false; }

    // ⚠️ 2026-09-12 实测修正: 原用 `api.bilibili.com` 走**明文 HTTP** → 该域名现在强制 HTTPS,
    //   对 HTTP 请求返回 **307**（PC 实测 http://api.bilibili.com/... → 307, 加 jsonp/换 UA 都一样）
    //   → ESP8266 HTTPClient 不跨协议跟随重定向 → 只能拿到 HTTP307 失败, B粉一直不可用。
    //   改用 B 站**接口镜像** `api.biliapi.net`（PC 实测明文 HTTP 200, JSON 与原域名逐字节同形:
    //   `{"code":0,...,"data":{...,"follower":1428204,...}}`）→ 无需 TLS, 省掉 ESP8266 上约 20KB 的
    //   BearSSL 握手堆开销。`.net` 失败再试 `.com`（同为镜像, 同样支持明文 HTTP）。
    static const char *const hosts[2] = {"api.biliapi.net", "api.biliapi.com"};
    char url[96];
    for (uint8_t h = 0; h < 2; h++) {
        snprintf(url, sizeof(url), PSTR("http://%s/x/relation/stat?vmid=%.*s&jsonp=jsonp"),
                 hosts[h], (int)ui, uid);
        WiFiClient client;
        HTTPClient http;
        if (!http.begin(client, url)) { snprintf(errCode, errCap, PSTR("BEGIN")); continue; }
        http.setTimeout(2500);
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            String body = http.getString();
            http.end();
            if (parseBiliFollower(body.c_str(), out)) { errCode[0] = '\0'; return true; }
            snprintf(errCode, errCap, PSTR("PARSE"));
            continue;
        }
        snprintf(errCode, errCap, PSTR("HTTP%d"), code);
        http.end();
    }
    return false;
}
#endif // ARDUINO

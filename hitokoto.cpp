// hitokoto.cpp — 一言数据层（v1.hitokoto.cn）
// 纯 C 解析器（无 Arduino 依赖）+ HTTP 获取（ARDUINO 段）
// 显示一行 296px / 16px 中文约 18 字 → 内容截断 63 字节足够

#include "hitokoto.h"
#include <string.h>
#include <stdio.h>

// ---------- 解析器（纯 C） ----------

// 定位 JSON 键 "key" 的值字符串起始；返回指向值首字符（跳过引号）的指针，失败 nullptr
// 只处理扁平对象（hitokoto API 无嵌套）；键必须带引号（"hitokoto"）
static const char* findJsonString(const char* buf, const char* key) {
    if (!buf || !key) return nullptr;
    size_t klen = strlen(key);
    char quoted[80];
    if (klen + 2 >= sizeof(quoted)) return nullptr;
    quoted[0] = '"';
    memcpy(quoted + 1, key, klen);
    quoted[klen + 1] = '"';
    quoted[klen + 2] = '\0';
    // 查找 "\"key\""（键前非空白字符必须是 { 或 ,）
    const char* p = buf;
    while ((p = strstr(p, quoted)) != nullptr) {
        const char* pre = p - 1;
        while (pre >= buf && (*pre == ' ' || *pre == '\t' || *pre == '\n' || *pre == '\r')) pre--;
        if (pre >= buf && (*pre == '{' || *pre == ',')) {
            const char* q = p + klen + 2;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
                if (*q == '"') return q + 1;
            }
        }
        p += klen + 2;
    }
    return nullptr;
}

// 从 JSON 字符串值（已跳过开引号）读取到闭合引号，处理转义；写入 out 直至 cap-1
// 返回实际写入字节数（不含 '\0'）；字符串未闭合返回 -1
// truncated 输出：因容量不足提前停止（仍有内容未写入）为 true
static int copyJsonString(const char* src, char* out, size_t cap, bool* truncated) {
    size_t n = 0;
    *truncated = false;
    while (*src && *src != '"') {
        if (*src == '\\') {
            src++;
            if (*src == 'u') {          // \uXXXX：丢弃（API 实际返回 UTF-8 明文）
                src += 4;
                continue;
            }
            if (*src == 'n' || *src == 'r' || *src == 't') {   // 控制字符 → 空格
                if (n + 1 < cap) out[n++] = ' ';
                else *truncated = true;
                src++;
                continue;
            }
            if (*src == '"' || *src == '\\' || *src == '/') {
                if (n + 1 < cap) out[n++] = *src;
                else *truncated = true;
                src++;
                continue;
            }
            // 未知转义：丢弃反斜杠，原样输出
            if (*src && n + 1 < cap) out[n++] = *src;
            else if (*src) *truncated = true;
            if (*src) src++;
            continue;
        }
        if (n + 1 < cap) out[n++] = *src;
        else *truncated = true;
        src++;
    }
    if (*src != '"') return -1;   // 未闭合
    out[n] = '\0';
    return (int)n;
}

bool parseHitokoto(const char* buf, char* out, size_t cap) {
    if (!buf || !out || cap == 0) return false;
    out[0] = '\0';
    const char* v = findJsonString(buf, "hitokoto");
    if (!v) return false;
    bool truncated = false;
    int n = copyJsonString(v, out, cap, &truncated);
    if (n <= 0) return false;   // 空值或未闭合
    if (truncated) {
        // 截断可能停在 UTF-8 字符中间。回退规则：
        // 末字节是续字节 → 找到所属字符首字节，校验字节数是否完整；
        // 不完整则删除整个残缺字符（宁可少一字，不显示乱码）
        int s = n;
        while (s > 0 && ((unsigned char)out[s - 1] & 0xC0) == 0x80) s--;
        if (s > 0) {
            unsigned char b0 = (unsigned char)out[s - 1];
            int need = (b0 & 0xE0) == 0xC0 ? 2 : ((b0 & 0xF0) == 0xE0 ? 3 : 1);
            if (n - (s - 1) < need) n = s - 1;   // 残缺 → 删除整个字符
        } else if (n > 0 && ((unsigned char)out[n - 1] & 0xC0) != 0x80) {
            n = 0;   // 首字节开头即残缺（out[0] 是首字节）
        }
    }
    out[n] = '\0';
    return true;
}

#ifdef ARDUINO
// ---------- HTTP 获取 ----------
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

bool fetchHitokoto(char* out, size_t cap, char* errCode, size_t errCap) {
    if (!out || cap == 0 || !errCode || errCap == 0) return false;
    out[0] = '\0';
    errCode[0] = '\0';

    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, "http://v1.hitokoto.cn/")) {
        snprintf(errCode, errCap, PSTR("BEGIN"));
        return false;
    }
    http.setTimeout(2000);   // 进入时钟页总阻塞 = 全刷 1.5s + 获取 ≤2s
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        String body = http.getString();
        http.end();
        if (parseHitokoto(body.c_str(), out, cap)) return true;
        snprintf(errCode, errCap, PSTR("PARSE"));
        return false;
    }
    snprintf(errCode, errCap, PSTR("HTTP%d"), code);
    http.end();
    return false;
}
#endif // ARDUINO
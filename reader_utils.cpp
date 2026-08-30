// reader_utils.cpp — 阅读器自包含工具函数实现
// 从 ink-reader-esp.ino 提取, 零全局依赖, 纯逻辑。

#include "reader_utils.h"
#include <string.h>
#include <stdio.h>

uint32_t decodeUtf8(const char *p, int &length) {
    uint8_t c = (uint8_t)p[0];
    if (c < 0x80) { length = 1; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) {
        length = 2;
        return ((uint32_t)(c & 0x1F) << 6) | ((uint8_t)p[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
        length = 3;
        return ((uint32_t)(c & 0x0F) << 12) |
               ((uint32_t)((uint8_t)p[1] & 0x3F) << 6) |
               ((uint8_t)p[2] & 0x3F);
    }
    length = 1;
    return 0xFFFD;
}

bool isUtf8Space(const char *p, int &length) {
    if ((uint8_t)p[0] == 0x20) { length = 1; return true; }
    if ((uint8_t)p[0] == 0xE3 && (uint8_t)p[1] == 0x80 && (uint8_t)p[2] == 0x80) {
        length = 3; return true;
    }
    length = 0;
    return false;
}

bool isChapterNumber(uint32_t cp) {
    return (cp >= '0' && cp <= '9') ||
           cp == 0x3007 || cp == 0x4E00 || cp == 0x4E8C || cp == 0x4E09 ||
           cp == 0x56DB || cp == 0x4E94 || cp == 0x516D || cp == 0x4E07 ||
           cp == 0x516B || cp == 0x4E5D || cp == 0x5341 || cp == 0x767E ||
           cp == 0x5343 || cp == 0x5343 || cp == 0x4E24;
}

bool isChapterTitle(const char *line, char *title, size_t titleSize) {
    const uint8_t *p = (const uint8_t *)line;
    while (*p == 0x20) p++;
    // "第" UTF-8 = E7 AC AC
    if (p[0] != 0xE7 || p[1] != 0xAC || p[2] != 0xAC) return false;
    p += 3;
    const uint8_t *numberStart = p;
    bool hasNumber = false;
    while (*p) {
        if (*p >= '0' && *p <= '9') { hasNumber = true; p++; continue; }
        int len = 0;
        uint32_t cp = decodeUtf8((const char *)p, len);
        if (len == 3 && isChapterNumber(cp)) { hasNumber = true; p += 3; continue; }
        break;
    }
    if (!hasNumber) return false;
    int len = 0;
    uint32_t cp = decodeUtf8((const char *)p, len);
    if (len != 3) return false;
    const uint32_t words[] = {0x5377, 0x7AE0, 0x56DE, 0x8282, 0x7BC7, 0x90E8, 0x96C6};  // 卷章回节篇部集
    bool hasWord = false;
    for (uint8_t i = 0; i < 7; i++)
        if (cp == words[i]) { hasWord = true; break; }
    if (!hasWord) return false;
    size_t length = 0;
    const char *start = (const char *)numberStart - 3;  // "第" 3 字节
    while (start > line && (uint8_t)start[-1] == 0x20) start--;
    while (start[length] && length + 1 < titleSize) { title[length] = start[length]; length++; }
    title[length] = '\0';
    while (length > 0 && (title[length - 1] == ' ' || title[length - 1] == '\r')) title[--length] = '\0';
    return true;
}

int txtCharWidth(uint8_t c) {
    if (c <= 31 || c == 127) return -1;  // 控制字符: 与模拟器 getCharLength 一致 (+1 后 0)
    if (c == 0x20 || c == '!') return 4;
    if (c == '"' || c == '#' || c == '(' || c == ')' || c == '[' || c == ']' || c == '`') return 5;
    if (c == '$') return 6;
    if (c == '%' || c == '&' || c == '*' || c == '+') return 7;
    if (c == '\'') return 3;
    if (c == ',' || c == '.') return 3;
    if (c == '1') return 5;
    if (c == ':' || c == ';' || c == '|') return 4;
    if (c == '@') return 9;
    if (c == 'A') return 8;
    if (c == 'D' || c == 'G' || c == 'H' || c == 'N' || c == 'O' || c == 'Q' ||
        c == 'T' || c == 'U' || c == 'V' || c == 'X' || c == 'Y' || c == 'Z') return 7;
    if (c == 'I' || c == 'J') return 3;
    if (c == 'M') return 8;
    if (c == 'W') return 11;
    if (c == 'c' || c == 'f' || c == 'k' || c == 's' || c == 'x' || c == 'z') return 5;
    if (c == 'i') return 1;
    if (c == 'l') return 2;
    if (c == 'j') return 2;
    if (c == 'm' || c == 'w') return 9;
    if (c == 'o' || c == 'v' || c == 'y') return 7;
    if (c == 'r' || c == 't') return 4;
    return 6;
}

void formatIndexNumber(uint32_t value, char *out) {
    snprintf(out, 9, "%08lu", (unsigned long)value);
}

const char* weekdayCn(int wday) {
    static const char* const names[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    return names[((wday % 7) + 7) % 7];
}

void formatProgressPercent(uint64_t page, uint64_t total, char *out) {
    if (total == 0) { snprintf(out, 12, "0%%"); return; }
    uint32_t p10k = (uint32_t)((page * 10000ULL) / total);   // 万分位 0-10000 (0-100%)
    if (p10k > 10000) p10k = 10000;
    if (p10k < 10) {   // < 0.1%: 两位小数 (0.01~0.09)
        snprintf(out, 12, "0.%02lu%%", (unsigned long)p10k);
    } else {           // >= 0.1%: 一位小数
        snprintf(out, 12, "%lu.%lu%%", (unsigned long)(p10k / 100), (unsigned long)((p10k % 100) / 10));
    }
}

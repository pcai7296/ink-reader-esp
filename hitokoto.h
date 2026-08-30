// hitokoto.h — 一言数据层（v1.hitokoto.cn）
// 解析器为纯 C 实现（无 String/ArduinoJson），PC 测试可编译
// 参照官方 A7 JsonHitokoto.ino 功能，适配 file_manager

#ifndef HITOKOTO_H
#define HITOKOTO_H

#include <stddef.h>
#include <stdbool.h>

// 解析一言响应 {"hitokoto":"...","from":"..."} → out（UTF-8，截断至 cap-1 字节含 '\0'）
// 成功返回 true；找不到 hitokoto 字段 / 空值 / 坏 JSON 返回 false
bool parseHitokoto(const char* buf, char* out, size_t cap);

#ifdef ARDUINO
// ---- HTTP 获取（仅 Arduino 编译）----
// GET http://v1.hitokoto.cn/ 一次（2s 超时）→ out；成功返回 true
// 失败返回 false 并填 errCode（"HTTP<code>"）
bool fetchHitokoto(char* out, size_t cap, char* errCode, size_t errCap);
#endif

#endif // HITOKOTO_H
// bili_fans.h — B站粉丝数数据层（api.bilibili.com/x/relation/stat）
// 纯 C 解析 + Arduino HTTP 获取（镜像 hitokoto.cpp 模式）

#ifndef BILI_FANS_H
#define BILI_FANS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// 从 JSON 提取 "follower": <整数> (data.follower); 成功返回 true
bool parseBiliFollower(const char *buf, uint32_t *out);

#ifdef ARDUINO
// 请求 <uid> 的粉丝数; 成功 out=数值并返回 true; 失败填 errCode(如 "HTTP<code>"/"BEGIN"/"PARSE")
bool fetchBiliFollower(const char *uid, uint32_t *out, char *errCode, size_t errCap);
#endif

#endif // BILI_FANS_H

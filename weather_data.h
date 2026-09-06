// weather_data.h — 天气数据层（心知天气 API）
// 参照官方 V14 (xz014_02A3) JsonWeather.ino/GetData.ino 实现，适配 file_manager
// 解析器为纯 C 实现（无 String/ArduinoJson），PC 测试可编译

#ifndef WEATHER_DATA_H
#define WEATHER_DATA_H

#include <stddef.h>
#include <stdbool.h>

// 实况天气
struct ActualWeather {
    char city[16];        // 城市名 (UTF-8)
    char weatherName[16]; // 天气现象 (UTF-8, 如"晴")
    char weatherCode[4];  // 天气代码 (如 "0")
    char temp[5];         // 温度 (如 "26", 含负号)
    char humidity[4];     // 实况相对湿度 % (now.humidity, 如 "62"); 缺失为空串
    char lastUpdate[25];  // 更新时间 (ISO8601, 取 HH:MM 用 [11..14])
};

// 未来 3 天预报
struct FutureWeather {
    char date[3][6];      // 日期 "08-15"
    char textDay[3][8];   // 白天天气
    char textNight[3][8]; // 夜间天气
    char high[3][4];      // 最高温
    char low[3][4];       // 最低温
    char humidity[4];     // 湿度 % (daily[0])
    char windScale[4];    // 风力等级 (daily[0], 如 "2")
};

// 生活指数（紫外线）
struct LifeIndex {
    char uvi[8]; // 紫外线 brief (UTF-8, 如 "中等")
};

// ---- 解析器（纯 C，无 Arduino 依赖）----
// 解析 now.json 响应；成功返回 true 并填充 out
bool parseActual(const char* buf, ActualWeather* out);
// 解析 daily.json 响应；成功返回 true 并填充 out
bool parseFuture(const char* buf, FutureWeather* out);
// 解析 life/suggestion.json 响应；成功返回 true 并填充 out
bool parseLife(const char* buf, LifeIndex* out);
// 从任意响应提取 status_code 到 code（cap 含 '\0'）；找不到返回 false
bool parseErrCode(const char* buf, char* code, size_t cap);

// 错误码 → 中文文案（线程无关，返回静态字符串）
const char* weatherErrorText(const char* code);

#ifdef ARDUINO
// ---- HTTP 获取（仅 Arduino 编译）----
// 顺序请求 now → daily → life；每端点间喂狗；失败重试 1 次
// 全部成功返回 true；失败返回 false 并填 errCode（"HTTP<code>" 或 status_code）
// onStep: 可选进度回调 (0=实况 1=未来 2=生活指数)，每个端点 HTTP 成功后调用
bool fetchWeather(ActualWeather* a, FutureWeather* f, LifeIndex* l,
                  const char* key, const char* city,
                  char* errCode, size_t errCap,
                  void (*onStep)(int step) = nullptr);
#endif

#endif // WEATHER_DATA_H
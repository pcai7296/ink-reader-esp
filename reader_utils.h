#pragma once
// reader_utils.h — 阅读器自包含工具函数
// 纯逻辑函数, 无全局变量依赖, 无副作用, 可安全提取到独立模块。
// 包含: UTF-8 解码、章节标题检测、ASCII 字符宽度、格式化工具。

#include <stddef.h>
#include <stdint.h>

// UTF-8 解码: 返回 Unicode codepoint, length = 消费字节数 (1-3)
uint32_t decodeUtf8(const char *p, int &length);

// 空格检测: 返回是否为空格 (ASCII 0x20 或全角空格 U+3000), length = 消费字节数
bool isUtf8Space(const char *p, int &length);

// 章节数字检测: 阿拉伯数字 0-9 或中文数字 (一二三四五六七八九十百千万两) 或 〇
bool isChapterNumber(uint32_t cp);

// 章节标题检测: 匹配 "第" + 数字 + "卷/章/回/节/篇/部/集"
// title: 输出标题文本 (去前后空格/回车), titleSize: 缓冲大小
// 返回 true 表示是章节标题
bool isChapterTitle(const char *line, char *title, size_t titleSize);

// ASCII 字符像素宽度 (Tahoma 16px 查表, 返回纯宽度不含 +1 间距)
int txtCharWidth(uint8_t c);

// 格式化索引编号: 8 位十进制 ASCII (左补零)
// out: 至少 9 字节缓冲
void formatIndexNumber(uint32_t value, char *out);

// tm_wday (0=周日) → 中文星期
const char* weekdayCn(int wday);

// 阅读进度百分比 (分级精度): 总进度 <0.1% 显示两位小数(如 0.05%), >=0.1% 显示一位小数(如 46.2%)
// page/total → "XX.X%" 或 "0.XX%"; out 至少 12 字节缓冲
void formatProgressPercent(uint64_t page, uint64_t total, char *out);

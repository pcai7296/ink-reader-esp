#pragma once
// sd_path.h — 文件 API 路径安全 + 保护规则纯函数（固件侧, 无 Arduino 依赖, PC 可测）
//
// 统一约定（docs/file-api.md）:
// - 输入路径必须是 server.arg() 已 percent 解码后的字符串（+→空格由客户端 %2B 规避）
// - 本层只做纯校验/规范化, 不触碰 SD/EEPROM/网络
// - 所有返回 bool 的函数失败时输出缓冲保持未定义内容（调用方应视为无效）

#include <stdbool.h>
#include <stddef.h>

// 受保护扩展名（精确匹配, 大小写不敏感）: 索引/章节/sidecar/标签
#define SD_PATH_PROTECTED_EXTS 10

// 规范化 API 路径:
// - 必须以 '/' 开头; 折叠连续 '/'（"//a///b" → "/a/b"）
// - 拒绝: 空串、任何 "." / ".." 段（不归一化, 直接拒绝）、'\\'、控制字符(<0x20 或 0x7f)
// - 尾斜杠保留（目录形态）
// 成功返回 true 且 out 为规范化结果; 失败返回 false
bool normalizeApiPath(const char *raw, char *out, size_t outSize);

// 净化上传文件名（multipart filename）:
// - 取 basename（剥 'C:\fakepath\...' 与任何路径前缀）
// - 拒绝: 空、"."、".."、basename 后仍含 '/' 或 '\\'、控制字符、长度 > 240
// 成功返回 true 且 out 为净化结果; 失败返回 false
bool sanitizeUploadName(const char *raw, char *out, size_t outSize);

// 受保护路径判定（禁删/禁改/禁移动/禁覆盖上传; 读取不限）:
// - 系统目录: path == "/.tiemereader" 或 starts_with("/.tiemereader/")
//   （目录前缀边界: "/.tiemereader_backup" 不误伤）
// - 扩展名精确匹配 SD_PATH_PROTECTED_EXTS 列表（"test.i1.bak" 扩展名是 .bak, 不保护）
// 入参应为已规范化路径
bool isProtectedPath(const char *path);

// basename 保护判定（改名/移动的目标名用）: 只查扩展名 + ".tiemereader" 同名, 不看目录部分。
// 用途 = 调用方已对目录部分单独调用 isProtectedPath 时, 免去"拼 560B 目标全路径只为看扩展名"
// 的深链栈开销（ESP8266 循环栈 4KB; 原来 /api/rename,/api/move 各压一个 char[560]）。
// 含 '/' 的入参返回 false（契约: 只接受 basename）。
bool isProtectedBaseName(const char *baseName);

// 上传临时文件识别: 以 ".uploading" 结尾
// （.uploading 可删除、不可 rename/move/download, 由调用层按操作区分）
bool isUploadingTemp(const char *path);

// 取路径的最后一个扩展名（含点, 如 ".i1"）; 无扩展名返回 false
// 供 isProtectedPath 内部使用, 也暴露给调用层（如 Web UI 类型判断）
bool pathExtension(const char *path, char *extBuf, size_t extSize);

// 移动防环: destDir 不能等于 path 本身、也不能是 path 的子孙目录
// （防 /foo → /foo/bar 自嵌套）; path 必须以 '/' 开头; 返回 true=允许移动
bool sdMoveDestAllowed(const char *path, const char *destDir);

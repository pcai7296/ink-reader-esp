#pragma once
#include <Arduino.h>

// 文件管理 HTTP API（统一 API: Web / Android / Legado 都是客户端）
// 路由全部挂在现有 ESP8266WebServer 实例上（wifi_manager.cpp 的 server），
// 服务器生命周期与现有配网/同步会话一致（不新增常驻网络模式）。
//
// S0: LittleFS Web UI 挂载 + serveStatic("/fm/") 骨架
// S3+: /api/* 走 onNotFound 分发（fileApiTryDispatch, 省路由对象堆）
void fileApiInit();
bool fileApiTryDispatch();
// 共享懒挂载 LittleFS（/fm 与 /fs/edit 复用同一标志, 只 begin 一次）; 返回 true=已挂载成功
bool fileApiEnsureLfsMount();

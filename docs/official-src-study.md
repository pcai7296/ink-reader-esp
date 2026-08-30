# 官方源码研究（weather-ink-screen Gitee 仓库）—— 关键对照发现

> 来源：`https://gitee.com/Lichengjiez/weather-ink-screen`（master1, V015-03 系列，作者持续维护）
> 已克隆到 `J:\code\esp8266\_research_fileserver\weather-ink-screen-gitee\`
> 源码目录 `源码&SD例程/V14源码/xz014_02A3/`（V14 完整源码，31 个 .ino/.c）

## 一、权威对照：官方从没在"纯 AP + SD 文件管理"场景下工作过

- 官方文件管理（`WEBServer.ino` 的 `webRead_fileManagement`/`webFileUpload`）**全部用 `LittleFS`（flash）**，
  **从不访问 SD 卡**。`SDFS` 只出现在系统信息/存储查询（`webPut_sdInit` 的 `SDFS.info`），非文件管理。
- 官方配网 `peiwang_mod()`（`DisplayPeiWang.ino`）**不碰 SD**，只处理 WiFi + LittleFS 文件管理。
- 官方把 SD 卡做成**可开关**（`eepUserSet.sdState`，`webPut_sdInit` 挂载/卸载），与配网文件管理**彻底分离**。

→ **结论：我们的场景（纯 AP 配网 + SD 卡大型文件库管理）是官方源码从未覆盖的路径。**
   官方 LittleFS 文件库是小目录（表盘/天气缓存/几 KB），而我们是 SD 上 142417 页小说 + 大量 .i1/.z1 索引。

## 二、官方 AP 参数的关键差异（最可能有价值）

`STA_AT_OTA.ino`:
```cpp
void initAp() {  // 纯 AP
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(ap_ssid, ap_password, random(1,14), 0, 1);  // ← 关键！
  WiFi.mode(WIFI_AP);
}
```
- `softAP(ssid, psk, channel=random(1,14), hidden=0, max_connection=1)`
- **`max_connection=1`**！官方显著减少 AP 的连接缓冲/结构预留（SDK 按 max_connection 预留每连接缓冲）。
- 我们项目 `WiFi.softAP(apSsid.c_str(), AP_PASSWORD)` 用默认 **`max_connection=4`**。

→ **高价值候选改动：把 `softAP` 改成 `max_connection=1`**，可能大幅减少 AP 会话的 DRAM 缓冲占用，
   给中断里的 `esf_buf_alloc` 留出空间。有官方实测背书，改动极小。

## 三、官方网络模式与我们不同（我们当初为规避才改）

- 官方配网成功用 **`WIFI_AP_STA`**（`initApSTA`，`ap_state==4`）；无配置用**纯 AP**（`initAp`）。
- 官方 `WIFI_AP_STA`（AP+STA 共存）。我们项目为避免 AP+STA 的 SDK phy Exception 29，
  **改成纯 AP（`WIFI_AP` / `AP_ONLY`）**——这是我们当初主动的取舍。

## 四、官方 SD 挂载用临时禁用看门狗

`SdInit.ino`：
```cpp
boolean sdBeginCheck() {
  ESP.wdtDisable();          // 停用看门狗
  SDFS.end();
  if (SD.begin(SD_CS, SPI_SPEED)) { ... ESP.wdtEnable(8000); ... }
  else { ... ESP.wdtEnable(8000); ... }
}
```
官方注释明说："SD卡挂载失败会导致软看门狗重启"——官方深知 SD 挂载的 WDT 风险，用 `wdtDisable/wdtEnable` 包裹。

## 五、官方文件列表实现（V14）

`webRead_fileManagement()`（WEBServer.ino:914）用 **`chunkedResponseModeStart` + `sendContent` 分块响应**，
注释"使用HTTP/1.1分块响应以避免生成巨大的临时字符串"。但它仍：先 `LittleFS.openDir("")` 遍历一次
统计 count，再 `String fileManagement_name[count]` 数组收集 → 适合 LittleFS 小目录，**对 SD 大目录不可用**。

## 结论 / 下一步（有依据、改动最小）

1. **首选：`softAP` 加 `max_connection=1`**（官方实测参数）。低风险、直接减少 AP 内存，可能让
   esf_buf_alloc 在中断里拿到足够 DRAM。→ 值得立即试。
2. 参考官方 **`SdInit.ino` 的 `wdtDisable/wdtEnable` 包裹 SD 挂载**——检查我们的 SD 挂载是否可能触发 WDT。
3. 官方从不配网碰 SD——**若 max_connection=1 仍不够，需接受"纯 AP 下 SD 文件管理天然受限"或改网络形态。**

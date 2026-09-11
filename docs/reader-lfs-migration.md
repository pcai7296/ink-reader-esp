# reader-lfs-migration.md — 阅读架构迁移：SD → LittleFS（官方路线）定稿与基线

> 用户 2026-09 定稿。**核心一句话**：阅读时（正文、索引、章节）全部走 LittleFS；**SD 与 WiFi 都退出阅读实时链路**。
> 先把稳定性恢复到官方级别，再恢复已有功能。禁止在同一版本里做“SD/LittleFS 自动 fallback”（状态组合爆炸）。
> 规则：`SD → LittleFS → 阅读` 允许；`SD → 阅读` 不允许。

## 1. 最终架构

```
                    ┌────────────────────┐
                    │     LittleFS       │  书籍正文 .txt / 页表 .i1 / 章节 .z1 / 阅读状态
                    └─────────┬──────────┘
                              ▼  RAM / Parser
                              ▼  EPD SSD1680
阅读期间固定： SD = OFF（不参与）   WiFi = OFF   LittleFS = ON   EPD = 按官方生命周期
SD 保留职责：书库管理 / Web 文件管理 / 导入导出 / 大文件中转
```

阅读数据源 = **LittleFS**（V14/A7 官方实现即如此：`DisplayTxt.ino:50-52` 用 `LittleFS.open()` 打开 `.i` 索引与正文；见 `REVERSE_NOTES.md §13`）。
SD→阅读 的旧路径（每页 `SD.open` 索引、SD 正文、SD 枚举章节）是“随机显示第 1 页”与续航差的共同根源，本次迁移连根去除。

## 2. 冻结基线（P0）

- 基线提交（tag）：`sd-reader-baseline-20260911`（= `23f25f0`，含页表读取失败修复 `b001468` 与 `BATCHK`/`PAGE_REC_*` 探针）。
- 基线验收清单（迁移后逐项 A/B 对照）：
  ① 正常启动 ② 进入书库 ③ 打开书 ④ 翻页 ⑤ 跳转 ⑥ 退出 ⑦ 已知问题 ⑧ 电流
- 基线已知问题（作为 A/B 对照项）：
  1. **随机显示第 1 页**：`parsePageRecord` SD.open 偶发失败返回 0 →（已修 `b001468`，根因仍属 SD 在阅读链路 → 本次迁移根除）；
  2. **SD 与 EPD/GPIO12/AP 争抢**（枚举异常、电池采样后首次 SD 访问失败）；
  3. **WiFi 常开**：用过时钟/天气/配网后 RF 未关（全仓仅 1 处 `wifiManagerStopSta()`，见 `ink-reader-esp.ino:4238`）→ 阅读时 ~70–100mA；
  4. **EPD 局刷后保持上电**（`epd_290a.cpp:249-288`）→ 面板 booster 持续带电。
- 电流基线：**未测**。设备已内置探针 `BATCHK mv=… page=… mode=… sd=…`（每 60s），可用 `-DDIAG_SD=1` 版把日志写入 SD 的 `debug_trace.log`，**拔掉 USB** 跑 3–6h 后按 mV 斜率换算，作为 P5/P7 的 A/B 基准。

## 3. 容量边界（必须明确接受，不做绕过）

- LittleFS 分区：`_FS_start=0x40400000`(物理 0x200000) → `_FS_end=0x405FA000`，大小 **0x1FA000 = 2,072,576 B ≈ 1.98 MiB**（block 8192 / page 256）。
- 页表开销（沿用官方 8 字节/页格式，不重设计）：以实测样本《武炼巅峰》55MB → 142,417 页 ⇒ **约 386 B/页**，即索引 ≈ 正文 × (8/386) ≈ **2.07%**。
- 因此单本可读上限（估算，实机以 `/fs` 本地介质模式的 LittleFS 容量显示为准）：
  **正文 ≈ 1.8–1.9 MB**（≈ 60–70 万汉字，≈ 4.5–5 千页），再加 `.z1` 与 FS 元数据开销。
- 拒绝规则（P10 导入前置校验）：**>100MB 一律拒绝（官方规则）**；同时 `需要 = txt + i1 + z1 + 余量` 超过 LittleFS 可用空间 → **导入前拒绝**，绝不复制到一半报 full。

## 4. 验收顺序（用户定稿）

- **A 官方阅读核心**：LittleFS + 正文 + i1 + z1 → **连续翻页 5000 次**：0 次 SD 访问、0 次“第 1 页异常”、0 次句柄异常、0 次重启。
- **B 跳转**：首页→100→1000→10000→尾页→返回。
- **C 长时间阅读**：连续 5h，记录 mV / 平均电流 / 内存 / LittleFS / WiFi 状态 / 翻页错误。
- **D 断电恢复**：随机断电重启 → 恢复到最近保存点（允许最多丢 5 分钟 / 50 页）。
- **E SD 独立**：Reader 不使用 SD；File Manager 可正常使用 SD。

## 5. 实施顺序（P0–P11）

```
P0 冻结当前版本 + 基线（本文档 + tag）                    ✅
P1 阅读数据源统一到 LittleFS（reader 不再直接依赖 SdFat）
P2 SD 从阅读路径彻底移除（进阅读 SD.end；退出再挂载）
P3 最小阅读闭环（开书 → 第1页 → 上/下页 → 退出）
P4 连续翻页稳定性验收（A）
P5 WiFi 生命周期修复（时钟/天气/配网/同步/进阅读强制 OFF）—— 独立提交   ✅（代码完成，待实机看 RF_OFF 日志）
P6 进度批量写入（5min / 50 页 / 退出 / 换书；RAM 实时，落盘节流）
P7 EPD powerOff A/B（不改存储；A=LittleFS+WiFiOFF，B=再加每页 powerOff）
P8 恢复阅读功能（字体/自动翻页/刷新间隔/旋转/跳转/标签/休眠）
P9 恢复网络功能（续读进度的网络同步/天气/Web 配置/配网）—— 不改阅读数据源
P10 SD → LittleFS 导入系统（容量与 100MB 前置校验 + 校验回读）
P11 全功能验收（5h / 1000+ 页 / 断电）
```

## 6. 迁移期间的功能冻结（P1–P4）

暂时不参与最小闭环（迁移完成后再按 P8/P9 恢复）：进度同步、天气、Web 控制、阅读统计、标签、自动翻页、旋转扩展、复杂恢复。
EPD 的 `powerOff()` 改动**不与存储迁移混提**（留到 P7 单独 A/B）。

## 7. P5 实现明细（WiFi 生命周期，2026-09）

- 新增统一出口 `wifiManagerRfOff(reason)`（`wifi_manager.cpp`）：仅当 `WiFi.getMode()!=WIFI_OFF` 时 `WiFi.disconnect(true) + WiFi.mode(WIFI_OFF)`，并打印 `RF_OFF reason=… was=…`。
- 调用点：时钟校准终态（校时成功 `clock_ntp_ok` / 天气时间成功 `clock_weather_ok` / 无凭据 `clock_no_creds` / WiFi 超时 `clock_wifi_timeout` / 天气失败 `clock_weather_fail` / 跳过 `clock_skip`）；天气页退出 `weather_exit`；配网页退出 `network_exit`；进入阅读 `reader_enter`（`startTxtReader`）；阅读循环兜底断言 `reader_assert`（任何漏关都会在此纠正并留日志）。
- 依据：官方 `WifiShutdown()`（`Other.ino:25-29`）= `WiFi.mode(WIFI_OFF)`，在 `Clock_8025T.ino:93`、`DisplaySetup.ino:194-195`、`DisplayTxt.ino:854` 处调用（见 `REVERSE_NOTES.md §13`）。
- 验收：串口日志出现对应 `RF_OFF reason=…`；阅读页不再出现 `reader_assert`（出现即说明仍有流程漏关）。

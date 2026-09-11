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
P1 阅读数据源统一到 LittleFS（reader 不再直接依赖 SdFat）      ✅ 代码完成（编译过；待实机 P3 验收）
P2 SD 从阅读路径彻底移除（进阅读 SD.end；退出再挂载）           ✅ 代码完成（进阅读 `SD.end()`+CS 高+日志 `SD_OFF`; 文件管理器按需 `SD.begin`）
P3 最小阅读闭环（开书 → 第1页 → 上/下页 → 退出）              ⏳ 待用户上传 T1 到 LittleFS 后实机验证
P4 连续翻页稳定性验收（A）                                    ⏳ 待 P3
P5 WiFi 生命周期修复（时钟/天气/配网/同步/进阅读强制 OFF）—— 独立提交   ✅（代码 + 已烧录，待看 RF_OFF 日志）
P6 进度批量写入（5min / 50 页 / 退出 / 换书；RAM 实时，落盘节流）  ✅ 代码完成（写盘含构建期 sidecar）
P7 EPD powerOff A/B（不改存储；A=LittleFS+WiFiOFF，B=再加每页 powerOff）  ⏳ 已预置编译开关 `-DREADER_EPD_PAGEOFF=1`（默认 0=保持上电），待 A 版稳定后 A/B 测试
P8 恢复阅读功能（字体/自动翻页/刷新间隔/旋转/跳转/标签/休眠）
P9 恢复网络功能（续读进度的网络同步/天气/Web 配置/配网）—— 不改阅读数据源
P10 SD → LittleFS 导入系统（容量与 100MB 前置校验 + 校验回读）
P11 全功能验收（5h / 1000+ 页 / 断电）
```

## 6. 迁移期间的功能冻结（P1–P4）

暂时不参与最小闭环（迁移完成后再按 P8/P9 恢复）：进度同步、天气、Web 控制、阅读统计、标签、自动翻页、旋转扩展、复杂恢复。
EPD 的 `powerOff()` 改动**不与存储迁移混提**（留到 P7 单独 A/B）。

## 8. P1 详细方案（阅读数据源统一 LittleFS）

**执行结果（2026-09，代码完成）**：
- 新增 `readerFs()`（=LittleFS）与占位 `readerBusReady()`（阅读链路不再需要 SD 总线仲裁；原 `reinitSdBus(...)` 调用点全部改为它，恒 true）。
- 共替换 **126 处** `SD.*`（含首轮 90 + 补充 18 + 单参 open 补 `"r"`），覆盖：最近阅读摘要/进度文件、索引构建与续建（含 sidecar `…p`）、页表读取（`parsePageRecordEx/offsetToPage`）、进度读写、正文读取（`readTxtPageCore/readTxtPage`）、章节（`loadChapterRows/chapterBuildPageTable/countTxtChapters/seekChapterOffset`）、标签（`markEnsureTxtFile/markCountRead/markAppend/markLoadPage/markDeleteOne`）、`buildTxtIndex`/`abortIndexBuild`/`removeLegacyIndexFiles`/`findPageByOffset/findPageCeil`、进度同步宿主钩子（`progressSyncSnapshot/RestoreReaderHeap/ApplyRemote`）。
- 文件模式按 `fs::FS` 语义逐个校正：读 `"r"`；新建/重写 `"w"`；**追加必用 `"a"`**（`beginResumeIndexBuildFromPartial` 的 `.i1/.z1` 追加、`markAppend` 的 `.bm` 追加）——SD 的 `FILE_WRITE` 是追加语义，直接照搬会截断文件。
- 阅读状态改址：`RECENT_READ_PATH = "/recentread.dat"`、`SLEEP_RECORD_PATH = "/sleepmode.dat"`、标签 `.bm/.bmt` 均在 LittleFS；不再创建 SD 的 `/.tiemereader`。
- 入口规则：`startTxtReader` 仅在 `gBrowseLocal`（内部介质浏览）下工作；SD 介质下点开 TXT → 提示 **“SD 仅文件管理 / 请切到内部介质或先导入”**，不做自动 fallback。
- 仍在 SD（非阅读实时链路，允许）：`debug_trace.log`（DIAG_SD 诊断）、天气缓存 `/.tiemereader/weather.dat`（首页/天气域）、文件管理/介质切换的 `SD.begin`。

### P3 实机验证步骤（用户操作）
1. 设置页把 **SD 卡设为“未启用”**（或拔卡）→ 重启 → 设备进入 `gBrowseLocal` 内部介质模式；
2. 进配网页，用 **现有文件管理 Web**（`/fs/edit`）把测试书 **T1（100–300KB .txt）上传到 LittleFS 根目录**（无需上传 .i1/.z1，首次打开由设备构建）；
3. 设备文件管理器打开该 txt → 应正常进入阅读：首页/下一页/上一页/跳转/退出；
4. 反例验证：切回 SD 介质后点开 SD 上的 txt → 必须提示“SD 仅文件管理”。

**关键有利条件（已核实）**：本固件已有介质抽象——`ink-reader-esp.ino:2350 static fs::FS &browseFs(){ return gBrowseLocal ? LittleFS : SDFS; }`，文件管理器/列表都走 `browseFs()`，且 `File` 就是 `fs::File`。
阅读路径是唯一绕开它、直接用 **SdFat 的 `SD.` 对象**（127 处 `SD.*` 调用），这就是 bug/续航问题的边界所在。

实施：
1. 新增 `static fs::FS &readerFs() { return LittleFS; }`（P1 固定 LittleFS；单点可切，便于将来对比实验）。
2. 机械替换阅读链路内的 `SD.open/exists/remove/mkdir` → `readerFs().…`（类型不变）：
   - 页表/进度：`parsePageRecordEx`、`writeProgress`、`readProgressOffset`、`findPageByOffset/Ceil`
   - 索引构建/续建：`beginTxtIndexBuild`、`beginResumeIndexBuildFromPartial`、`indexTaskStep`、`finishTxtIndexBuild`、`abortIndexBuild`（含 sidecar `…p` 逻辑；LittleFS 稳定后 P6 简化为批量写记录[0]）
   - 正文读取：`readTxtPageCore/readTxtPage`（重试由 `reinitSdBus` 改为“重新 open 一次”，不再动 SD 总线）
   - 章节：`countTxtChapters`、`loadChapterRows`、`chapterBuildPageTable`、`.z1` 构建
   - 阅读状态：`saveRecentReadPath`/`loadRecentReadSummary`、`.bm/.bmt` 标签（阅读状态按架构归 LittleFS）
3. 阅读链路内**移除** `reinitSdBus(...)` / `SD.begin(...)` 调用（LittleFS 不共享 EPD/电池引脚，不再需要总线仲裁）；`parsePageRecordEx` 保留一次重试但只做 reopen。
4. `startTxtReader` 入口规则改为：
   - 当前介质 = 本地（`gBrowseLocal`，LittleFS）→ 正常阅读；
   - 当前介质 = SD → 提示 **“SD 仅文件管理：请先用导入（P10）或切到内部介质浏览”**，不再从 SD 打开书（**杜绝 SD 参与阅读**、不做自动 fallback）。
5. 删除/停用 `startTxtReader` 里针对 `gBrowseLocal` 的“仅浏览不支持阅读”旧守卫。

## 9. P2 方案（SD 退出阅读）
- 进入阅读前：`SDFS.end()`（并确保 `SD.end()`/CS 高），置 `sdPowered=false`；阅读期间任何 `browseFs()` 调用都视为违规（加断言/日志 `READER_SD_TOUCH`）。
- 退出阅读回文件管理器/首页时：按需重新挂载（`SD.begin(5, SD_SCK_MHZ(20))` / `SDFS.begin()`），并刷新目录缓存。
- 阅读期间 `gBrowseLocal` 语义统一为“内部介质”，与 SD 状态解耦，避免状态组合。

## 10. 测试书部署（P3/P4 前置；用户定稿）
- **用现有 Web 文件管理直接上传进 LittleFS**：设备切内部介质模式（设置里关闭 SD / 无卡）→ 配网页 `/fs/edit` 上传 `T1`(100–300KB) / `T2`(1–1.5MB) / `T3`(贴近容量上限) 到 LittleFS 根目录。
- **不做导入功能、不经 SD 中转**（P10 再实现 SD→LittleFS 正式导入 + 容量/100MB 前置校验）。
- `.i1/.z1` 不必上传：首次打开由设备在 LittleFS 上构建（顺带验证构建器）。
- 设备内浏览路径已有：`gBrowseLocal` 本地介质模式，可在文件管理器里浏览 LittleFS 并打开 txt（P1 后即可进入阅读）。

## 11. P6b / P7 明细（2026-09）
**P6b 统计节流**（`stats.cpp`）：原 `statsOnPageTurn()` 每翻一页写 `/stats/global.dat` + `/stats/books.dat`（各含 `.tmp`→rename，≈4+ 次 flash 操作/页）。现改为 RAM 累计，**每 50 页 / 5 分钟 / 会话结束 / 显式 `statsSave()`** 才落盘；`statsTick()` 由主 loop 每圈调用兜底。硬复位最多丢 ≤50 页计数（与验收 D 同口径）；会话计数仍在 `statsOnSessionEnd` 写入。

**P7 EPD powerOff A/B**：
- 预置编译开关 `READER_EPD_PAGEOFF`（默认 0）：`refresh(false)` 末尾，若 `appMode==APP_READER` 则 `epd.powerOff()`（对齐官方 `DisplayTxt.ino:433/847/1076` 每页断电）。
- 编译 B 版：`arduino-cli compile --fqbn esp8266:esp8266:d1_mini --libraries libraries --build-property "compiler.cpp.extra_flags=-DREADER_EPD_PAGEOFF=1" --build-path build_p7_ab ink-reader-esp.ino`
- A/B 对比项：平均电流（由 `BATCHK mv=` 每分钟日志换算）、翻页时间、连续翻页 1000 次、黑屏/残影、刷新失败、电池下降速度。
- **判定原则**：若 B 版明显增加刷新异常，则保留 A（不改）。

## 12. P3/P4 工具（2026-09 已就绪）
- **测试书生成**：`python make_test_books.py --out build/test_books` → `T1_300k.txt`(307KB) / `T2_1m2.txt`(1.20MB) / `T3_1m8.txt`(1.84MB，贴近 LittleFS 上限，用于验证容量拒绝)。内容为 UTF-8 中文正文 + `第N章 …` 标准章节标题（同时验证 .i1 页表与 .z1 章节识别）。
- **T3 容量校验**：1.84MB 正文 + 索引(≈2.07%) ≈ **1.88MB**，接近 LittleFS 可用上限(~1.9MB) → 上传/阅读可能因空间不足失败，这是**预期边界**而非 bug。
- **P4 千次翻页验收**：`python p4_flip_test.py --port COM20 --count 1000 --interval 0.35`
  - 前置固件：`-DSERIAL_REMOTE=1` 编译（`build_remote/`，已验证可编译）；该版把 GPIO3/RX 让给串口，按键由脚本注入 `K3S`（协议见 `ink-reader-esp.ino` §serialRemoteInject：`K2S/K2L/K3S/K3L/B/?`）。
  - 判定：`PAGE_NEXT` 递增无跳变；**零** `SD_REINIT`（阅读期间无 SD 访问）；**零** `PAGE_REC_*`/`PAGE_NEXT_RECFAIL`/`PAGE_READ_FAIL`/`PROGRESS_ZERO_SKIP`；**零** `Fatal exception`/`Soft WDT`/中途 `BOOT reason`；日志出现 `RF_OFF reason=reader_enter` 且**不出现** `reader_assert`。
  - 结果与日志：`serial_logs/p4_flip_*.log`。

## 13. P9 预置：消除“阅读期间 SD 已卸载”带来的连带读 SD（2026-09）
P2 之后阅读期间 SD 处于 `end()` 状态，因此以下**非阅读实时链路**的 SD 依赖一并改为 **LittleFS 优先 + SD 兼容回退**（回退时按需 `SD.begin`，都发生在显式联网/UI 动作中，不属翻页实时路径）：
- `/sync.cfg`（进度同步凭据/目标，`progress_sync.cpp::loadSyncCfg`）：LittleFS `/sync.cfg` 优先，日志 `SYNC_CFG from=LittleFS|SD`；SD 旧文件仍可用。
- 天气壁纸 BMP（`bmp_show.cpp::bmpShowFromSd`）：LittleFS 优先，SD 回退（壁纸可由 Web 直接上传到 LittleFS）。
- 主页天气摘要缓存：`/.tiemereader/weather.dat`(SD) → **LittleFS `/weather.dat`**（设备状态归 LittleFS；同时修掉“阅读后回首页读不到缓存”的回归）。
- 仍在 SD 的仅剩：`debug_trace.log`（DIAG_SD 诊断，默认不写）、介质切换/文件管理器的 `SD.begin`、`startTxtReader` 的 `SD.end()`（P2 本体）。

## 7. P5 实现明细（WiFi 生命周期，2026-09）
- 新增统一出口 `wifiManagerRfOff(reason)`（`wifi_manager.cpp`）：仅当 `WiFi.getMode()!=WIFI_OFF` 时 `WiFi.disconnect(true) + WiFi.mode(WIFI_OFF)`，并打印 `RF_OFF reason=… was=…`。
- 调用点：时钟校准终态（校时成功 `clock_ntp_ok` / 天气时间成功 `clock_weather_ok` / 无凭据 `clock_no_creds` / WiFi 超时 `clock_wifi_timeout` / 天气失败 `clock_weather_fail` / 跳过 `clock_skip`）；天气页退出 `weather_exit`；配网页退出 `network_exit`；进入阅读 `reader_enter`（`startTxtReader`）；阅读循环兜底断言 `reader_assert`（任何漏关都会在此纠正并留日志）。
- 依据：官方 `WifiShutdown()`（`Other.ino:25-29`）= `WiFi.mode(WIFI_OFF)`，在 `Clock_8025T.ino:93`、`DisplaySetup.ino:194-195`、`DisplayTxt.ino:854` 处调用（见 `REVERSE_NOTES.md §13`）。
- 验收：串口日志出现对应 `RF_OFF reason=…`；阅读页不再出现 `reader_assert`（出现即说明仍有流程漏关）。

---

## 14. P3/P4 实机验收记录（2026-09，T1 测试书；无按键自动化）

**部署方式（用户定稿路径）**：设备切内部介质 → Web 文件管理 `POST /fs/edit` 上传 `T1_300k.txt` → 校验 `GET /fs/file?path=/T1_300k.txt` 返回 **200 / 307,356 字节**（与源文件一致 ⇒ 确认落在 LittleFS）。
> ⚠️ 前置修复：原 `/fs/edit` 上传**硬编码写 SD**（管理器不跟随介质）；已改为 `activeFileFs()` 介质抽象（见 §8 P3 前置提交 `079ed6b`）。

**索引（在 LittleFS 上构建，官方同款 8B/页格式）**：`IDX async done pages=681 chapters=53 indexSize=5456 z1Size=1095`（307KB 正文，构建 ≈90s；`indexTaskStep` 分片驱动；自测固件需自行驱动该步进，否则页码恒为 1）。

**验收 A（正向 1 → 末页，最终干净运行）**：`AUTOTEST_FWD_DONE fwdOk=680 stoppedAtEnd=1 page=681 total=681`
- 从第 1 页连续翻到末页（680 次翻页覆盖全部 681 页），**零失败**；随后 1 次"末页无法再前进"为**预期停止**（`AUTOTEST_DONE sent=900 ok=905 fail=1 page=681 total=681 rf=0`）。
- 异常扫描全为 0：`PAGE_REC_*` / `PAGE_NEXT_RECFAIL` / `PAGE_READ_FAIL` / `PROGRESS_ZERO_SKIP` / `Fatal exception` / `Soft WDT` / 中途 `BOOT reason`；阅读期间 `SD_REINIT`(SD 访问) = **0**；`rf=0` = WiFi OFF。
- `PROG_FLUSH reason=threshold off=…` 每 50 页出现一次 ⇒ P6 进度节流按设计工作。

**验收 A'（反向）**：`AUTOTEST_BACK_DONE backOk=225 page=456` —— 末页回翻 225 次全部成功，同样零异常。

**判定**：P3（开书/首页/上下翻/末页停止）✅；P4（连续翻页零 SD 访问）✅。

**验收用编译开关（默认全关，仅测试固件）**：`-DREADER_AUTOTEST=1 -DREADER_AUTOTEST_PAGES=N -DFORCE_LOCAL_MEDIUM_TEST=1`；需要配网页部署时再加 `-DBOOT_AP_MODE=1 -DWIFI_TEST_FORCE_AP=1`（强制热点、不连已保存 STA，便于 PC 上传）。

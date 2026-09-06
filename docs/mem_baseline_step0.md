# BASELINE v2026-09-05 — Step 0 静态 RAM 审计

> 目的：锁死当前工作区（HEAD `1b37d97` + 未提交修改）的精确内存账本，作为后续
> 「腾静态堆」每步「基线→改一文件→编译→audit→ΔRAM→烧录回归」的对照零点。
> 审计工具：`rodata_audit.py`（v2，ELF `readelf -S` 权威总量 + 逐 `.o` 段名分文件 +
> `nm -S` 具名符号；替代已丢失的旧 map-解析版）。

## 0. 编译条件（本次与以后回归必须一致）

| 项 | 值 |
|---|---|
| arduino-cli | 1.4.1 |
| core | esp8266 3.1.2（J:\Arduino15\packages\esp8266\hardware\esp8266\3.1.2） |
| FQBN | esp8266:esp8266:d1_mini |
| libraries | `J:\code\esp8266\ink-reader-esp\libraries` |
| 源码 | ink-reader-esp 工作区（git HEAD 1b37d97 + 未提交修改；M: file_api_fs.cpp/.h, file_api.cpp, wifi_manager.cpp/.h, ink-reader-esp.ino, data/*.htm …） |
| 正常版 flags | `-DBOOT_AP_MODE=0 -DSERIAL_REMOTE=0` |
| AP 引导版 flags | `-DBOOT_AP_MODE=1 -DSERIAL_REMOTE=0` |

构建目录（均被 .gitignore 忽略）：`build_bsln_norm/`、`build_bsln_bootap/`；
审计 JSON：`build_bsln_norm/audit_norm.json`、`build_bsln_bootap/audit_bootap.json`。
对照（Q3 差分用）：HEAD `1b37d97` 干净源码树 `J:\code\esp8266\_headref_1b37d97\`（git archive，
正常版 `build_head_norm/`，审计 `audit_head.json`）——**HEAD 与工作区差 = 未提交批次净效果**。

## 1. RAM 总账（readelf -S，DRAM 0x3FFE8000 起 / 0x13940=80,192B）

| 区段 | 正常版 (BOOT_AP_MODE=0) | AP 引导版 (BOOT_AP_MODE=1) |
|---|---:|---:|
| RAM 总量 | 80,192 | 80,192 |
| .data | 1,708 | 1,708 |
| .noinit | 56 | 56 |
| **RAM .rodata** | **19,572** | **19,216** |
| .bss | 46,528 | 46,520 |
| **静态已用（全部，含 noinit）** | **67,864 (84.6%)** | **67,500 (84.2%)** |
| 静态已用（arduino-cli 口径 data+rodata+bss） | 67,808 (84.5%) | 67,444 (84.1%) |
| 静态结束偏移（=heap_start−0x3FFE8000） | 0x10928 | 0x107B8 |
| **堆顶 ≈ 80,192−静态结束** | **≈ 12,312** | **≈ 12,680** |
| IRAM | 61,535/65,536 (93.9%) | 61,535/65,536 (93.9%) |
| Flash code (.irom0.text) | 933,916/1,048,576 (89.1%) | 932,684/1,048,576 (88.9%) |

> 口径说明：`max free block`/开机 `freeHeap` 是实机值（现有 `auditHeap`/`FSREQ`/`FSDL` 探针
> 已打印），编译产物给不出，烧录回归时从串口取。heap 顶 = 上限，实际 freeHeap 再减栈 + lwIP 运行分配。

## 2. 与历史快照对比（口径必须逐项对齐再比）

| 快照 | 源 | .bss | RAM .rodata | .data | 静态合计 | 堆顶≈ |
|---|---:|---:|---:|---:|---:|---:|
| 8/30 build（旧 build/，7 天前产物） | readelf 同口径 | 43,352 | 19,412 | 1,704 | 64,524 | ~15.7K |
| mem_profile_2026-09 快照（build_fsdlfix 等） | 该文 §1 | 46,520 | 20,988 | 1,708 | 69,216 | ~11.0K |
| HEAD 1b37d97 干净树（对照，正常版） | 本次 readelf | 44,656 | 20,476 | 1,708 | 66,896 | ~13.3K |
| **BASELINE 正常版（工作区=HEAD+未提交）** | 同上 | 46,528 | 19,572 | 1,708 | **67,864** | **12,312** |

结论：当前工作区静态比 mem_profile 快照**低 ~1.35K**，但比干净 HEAD **高 ~968B**（见 §6，
未提交批次里 rodata 转换的回血被下载修复的 static 化对冲后仍为净增）；比 8/30 build 高 ~3.3K
（9/04-05 的 /fs 管理 web、下载状态、UI 重构新增）。**“8/30 的 69,216B”确实不是当前值。**

## 3. 逐源文件（sketch 目录 .o；正常版/引导版除 ino 外相同）

表内为 [.rodata, .bss, .data]，单位 B：

| 源文件 | rodata | bss | data | static | 备注 |
|---|---:|---:|---:|---:|---|
| ink-reader-esp.ino | 8,932 (引导 8,574) | 12,135 | 63 | 21,130 | fb 4,736 在此；正文/页面串大头 |
| wifi_manager.cpp | 4,986 | 771 | 12 | 5,769 | 配网页面/状态串 + TRY_STA/STA_ONLY 状态字 |
| file_api_fs.cpp | 2,350 | 3,753 | 0 | 6,103 | /fs/*；bss=上传/下载静态态 1.5K+路径/缓冲 |
| progress_sync.cpp | 1,864 | 1,715 | 6 | 3,585 | gFpBuf 1,024 在 bss |
| file_api.cpp | 2,258 | 560 | 0 | 2,818 | /api/* 错误串 |
| fs_cache.cpp | 1,548 | 0 | 0 | 1,548 | 目录树→LittleFS 缓存层 |
| stats.cpp | 116 | 1,008 | 4 | 1,128 | 阅读统计 |
| weather_data.cpp | 885 | 0 | 0 | 885 | 天气串 |
| progress_lumi.cpp | 278 | 0 | 0 | 278 | LUMI1 编解码 |
| epd_290a.cpp | 253 | 0 | 0 | 253 | LUT（flash？见 nm RODATA 榜） |
| reader_utils / sd_path / sd_file_ops / hitokoto / bmp_show | 135/114/81/55/6 | 0/0/4/0/40 | … | 小 |
| gb2312.c | 0 | 0 | 0 | 0 | 死代码未编译 |

> sketch 文件合计 ≈ 静态 ~48.5K（67.9K 总量减去 SDK 网栈/core/库）。RAM .rodata 的
> 绝大多数是匿名串字面量（nm 具名 R 符号合计仅几百 B，见下），即「可 flash 化」主目标仍成立。

## 4. BSS/具名大符号 Top（正常版；'b'=文件内 static）

| size | 符号 | 备注 |
|---|---:|---|
| 4,736 | fb | EPD 帧缓冲，不可动 |
| **4,096** | chapterPageOffsets | 章节页偏移表，仅 APP_CHAPTERS 用 → 按需 malloc 候选 ① |
| 1,704 | g_ic | SdFat 卷对象，不可动 |
| **1,024** | gFpBuf | 进度同步指纹缓冲，仅同步期用 → 候选 ② |
| 864 | winItems | UI 窗口项数组 |
| 408 | chapterRows | 章节行缓冲 |
| 256×3 | pmc / event_TaskQueue / diagRing | 后两者 SDK/core 静态度 |
| 228 | indexRows / Update | indexRows 阅读器；Update=OTA |
| 216 | txtLines | 阅读行 String 数组（网络期已清内容） |
| 160 | gWebNotifyLine | 配网页底行消息 |
| … | | 其余为 SDK 网栈/lwIP/core 数百小对象（~25K，不可动） |

DATA 具名符号均为小项（heap_context 52 …）；RODATA 具名符号合计很小
（memp_pools 44、epd LUT 30×2 …），证实 RAM .rodata ≈ 匿名串 + 小常量表。

## 5. Q1–Q4 直接回答

1. **当前静态总量**：正常版 **67,864B（84.6%）**；AP 引导版 67,500B（84.2%）。
2. **当前 RAM .rodata**：正常版 **19,572B**；引导版 19,216B（两版差 = BOOT_AP_MODE #if 分支串）。
3. **wifi_manager / file_api_fs / file_api 已省多少**：见 §6（HEAD 对照差分）。摘要：转换净省
   rodata wifi_manager 932B / file_api_fs 355B / file_api 27B，但 file_api_fs 同批把下载修复的
   1,864B 搬进 static BSS → 三个文件静态净额 wifi_manager −932 / file_api_fs +1,509 / file_api −27。
4. **chapterPageOffsets 等静态数组实际占用**：chapterPageOffsets=4,096、gFpBuf=1,024、
   fb=4,736、winItems=864、chapterRows=408、diagRing=256、txtLines=216、gWebNotifyLine=160
   （其余见 §4 表）。

## 6. HEAD(1b37d97) vs 工作区差分（Q3，未提交批次净效果，正常版同 flags 双编译）

| 源文件 | Δrodata | Δbss | Δstatic | 归因 |
|---|---:|---:|---:|---|
| wifi_manager.cpp | **−932** | 0 | **−932** | printf_P 批次（净省，全进 flash） |
| file_api_fs.cpp | −355 | **+1,864** | **+1,509** | 转换省 355；但下载修复把 path/plain/enc/disp 1,556 + 下载状态 gOfsDlPhasePath 300 + 相位 8 从栈搬 static |
| file_api.cpp | −27 | 0 | −27 | 3 处 printf_P |
| ink-reader-esp.ino | **+468** | +2 | +470 | 新增 下载/上传状态与 STA_ONLY 界面文案（这批还没做 ino 转换） |
| progress_sync / fs_cache / stats / weather_data / … | 0 | 0 | 0 | 未动 |
| **合计** | **−904** | **+1,872** | **+968** | 堆顶 13,280 → 12,312（−968） |

**⚠️ 重要结论（对后续规划的影响）**：本未提交批次的「字符串 flash 化」确实回血了
**−1,314B rodata**（932+355+27），但被两件事对冲为**净 +968B 静态**：
1. 下载修复的 1,864B「栈→static」（当时为解决 4KB loop 栈 80B 近溢出 Exception 29，见
   file_api_fs.cpp:549-551 注释）——这是**审查里提醒过的静态化负债**；
2. 新功能文案 +468B 落在 ino（未转换）。
⇒ **下一步别急着开新转换批次**：先把 ino 新增文案与剩余串按 §6.4 批序转换（抵掉 +468），
再决定 file_api_fs 那 1,864B 静态怎么还（收缩/按需 heap，绝不能搬回栈）；之后静态才真正开始下降。

## 7. 回归协议（后续每步执行）

1. 改一个文件 → `arduino-cli compile ... --build-path build_bsln_norm`（flags 同 §0）
2. `python rodata_audit.py build_bsln_norm` → 记 ΔRAM
3. 烧录回归按需：AP 列表 / 64MB 下载×3 / `/api/download` Range / BearSSL 同步（不必每步全跑）

---

## 8. Step A 结果（2026-09-05 已执行）— .ino 配网页/上传下载状态文案 flash 化

改动（`ink-reader-esp.ino`）：
- `drawTextUTF8` 新增 `const __FlashStringHelper*` 重载：PSTR → ≤64B RAM 副本 → 原 RAM 版渲染
  （搬运用 `snprintf_P`，flash 4B 对齐安全读；纯文本无 `%`）
- `renderUploadStatus` / `renderDownloadStatus`：标题/后缀/通知行全部 PSTR 化
  （`snprintf_P` 纯文本无参 + `%s` 一律 RAM 实参）
- `renderNetworkPage`：全部字面量 `F()` 化；`IP:/管理: http://热点: /STA:` 格式串 `snprintf_P`
- `webSettingsNotify`：分隔 `：`/格式串 PSTR 化；`Serial.printf` → `printf_P`

结果（同 flags 双编译；audit json 已更新）：

| 指标 | 正常版 BASELINE | StepA 后 | Δ | AP 引导 BASELINE | StepA 后 | Δ |
|---|---:|---:|---:|---:|---:|---:|
| RAM .rodata | 19,572 | 19,156 | **−416** | 19,216 | 18,800 | **−416** |
| .bss | 46,528 | 46,528 | 0 | 46,520 | 46,520 | 0 |
| .data / .noinit | 1,708 / 56 | 同 | 0 | 同 | 同 | 0 |
| 静态已用（全部） | 67,864 | 67,448 | **−416** | 67,500 | 67,084 | **−416** |
| 堆顶 ≈ | 12,312 | 12,728 | **+416** | 12,680 | 13,096 | +416 |
| ink-reader-esp rodata | 8,932 | 8,467 | −465 | 8,574 | 8,109 | −465 |

**验收：静态 −416B ≥ ~400B ✔**。文件级：`ink-reader-esp.ino` rodata 8,467 ≈ HEAD 的 8,464 → **+468 新增负债清零**。
代价：flash 增加 ~0.5–0.6KB（PSTR 文案 + 重载代码）；渲染路径新增 ≤64B 栈副本（配网页/状态渲染时才出现，不在 FSDL 流式循环内）。

---

## 9. Step B 结果（2026-09-05 已执行）— file_api_fs.cpp 1,864B 拆解

拆解（1,864B ≠ 整体，按消费域分别处理；**均未回 4KB 栈**）：
- **下载状态 308B** → `gOfsDlPhasePath[300]` + `ofsDlGetPhasePath()` getter **删除**（审计：无任何消费者——
  上报→渲染回调同步、渲染只用回调入参 path）；保留 `gOfsDlPhase`(4B)+`gOfsDlPhaseCb`(4B) 供将来对称 upload
  的"结束后回配网页"轮询。→ **−300B 常驻**。
- **header 三缓冲 1,556B 中的 plain/enc/disp 1,256B** → 从 static 改**请求级 malloc**（需要时创建→
  sendHeader 后立即 free→主体传输阶段不存在）；`path[300]` 需贯穿 handler（START/DONE 上报）保留 static。
  malloc 失败 → 降级为不发 Content-Disposition（URL 默认名，传输不受影响）+ `FSDL_HDR_ALLOC_FAIL` 日志。
  → **−1,256B 常驻**（峰值仅 header 构建瞬间 +1.2K，低于 512B 传输缓冲的意义）。
- 净 −1,556B BSS（文件级 file_api_fs bss 3,753 → 2,197）。

结果（同 flags 双编译；audit json 已更新）：

| 指标 | 正常版 StepA 后 | StepB 后 | Δ | AP 引导 StepA 后 | StepB 后 | Δ |
|---|---:|---:|---:|---:|---:|---:|
| RAM .rodata | 19,156 | 19,156 | 0 | 18,800 | 18,800 | 0 |
| .bss | 46,528 | 44,976 | **−1,552** | 46,520 | 44,968 | −1,552 |
| 静态已用（全部） | 67,448 | 65,896 | **−1,552** | 67,084 | 65,532 | −1,552 |
| 堆顶 ≈ | 12,728 | 14,280 | +1,552 | 13,096 | 14,648 | +1,552 |
| file_api_fs bss | 3,753 | 2,197 | −1,556 | 同 | 同 | −1,556 |

**A+B 累计（正常版）：静态 67,864 → 65,896（−1,968B），堆顶 12,312 → 14,280。**
待实机回归项（Step B 引入了下载路径运行时变化）：AP/STA 下载文件名仍正确（header 正常路径）、
状态行正常、观察 `FSDL_HDR_ALLOC_FAIL` 不应出现（低堆时降级可接受）。

---

## 10. Step C 结果（2026-09-05 已执行）— wifi_manager / file_api web 层字面量 flash 化

改动：
- `wifi_manager.cpp`（21 处）/ `file_api.cpp`（5 处）：`server.send(code, "ct", "body")` 全字面量 →
  `server.send_P(code, PSTR(ct), PSTR(body))`（ESP8266WebServer 原生 PGM 版，含多行实参跨行合并）
- `file_api.cpp`：`sendApiErr` 形参 `const char*` → `const __FlashStringHelper*`，61 处错误 token 调用点
  `"xxx"` → `F("xxx")`（String::concat 支持 flash）
- 注意：per-`.o` 差异 ≠ 链接后总量（跨 TU 合并/丢弃），以 ELF readelf 总量为准。

结果（同 flags 双编译）：

| 指标 | 正常版 StepB 后 | StepC 后 | Δ | AP 引导 StepB 后 | StepC 后 | Δ |
|---|---:|---:|---:|---:|---:|---:|
| RAM .rodata | 19,156 | 18,080 | **−1,076** | 18,800 | 17,724 | −1,076 |
| .bss | 44,976 | 44,968 | −8 | 44,968 | 44,968 | 0 |
| 静态已用（全部） | 65,896 | 64,812 | **−1,084** | 65,532 | 64,456 | −1,076 |
| 堆顶 ≈ | 14,280 | 15,368 | +1,088 | 14,648 | 15,720 | +1,072 |

源文件级参考（.o 内 rodata，未含链接合并）：wifi_manager 4,986→3,978（−1,008）；
file_api 2,258→2,016（−242）；ink-reader-esp 8,467 / file_api_fs 2,350 / progress_sync 1,864 未动。

**A+B+C 累计（正常版）：静态 67,864 → 64,812（−3,052B），堆顶 12,312 → 15,368。**
距阶段目标 63–64KB 还差 ~0.8–1.8KB（D: progress_sync/weather 及其余状态串可到）。

---

## 11. Step D 结果（2026-09-05 已执行）— progress_sync / weather_data 字符串 → 阶段目标达成 🎯

改动：
- `progress_sync.cpp`：`syncDbg` 形参 `const char*` → `PGM_P`，37 处日志格式串 `PSTR(...)` +
  `vsnprintf_P`（newlib flash 格式支持）；33 处 `gStatus = "…"` → `F("…")`（String flash 赋值）；
  1 处 `String req = "…"` → F 构造；补 `<stdio.h>/<pgmspace.h>`
- `weather_data.cpp`：3 条心知 API URL 格式串 → `snprintf_P(PSTR(...))`；`"PARSE"`×3 / `"HTTP%d"`×1
  同转；补 `#if ARDUINO` 保护的 `<stdio.h>/<pgmspace.h>`（PC 测试宿主编译不受影响）
- 说明：`weatherErrorText` 中文标签（~20 处 return 串）与 JSON 键 token 因消费方为 `strstr`/`%s`
  RAM 实参暂缓（小、需 consumer 改造，列入后续可选项）

结果（同 flags 双编译）：

| 指标 | 正常版 StepC 后 | StepD 后 | Δ | AP 引导 StepC 后 | StepD 后 | Δ |
|---|---:|---:|---:|---:|---:|---:|
| RAM .rodata | 18,080 | 16,396 | **−1,684** | 17,724 | 16,040 | −1,684 |
| .bss | 44,968 | 44,968 | 0 | 44,968 | 44,976 | +8 |
| 静态已用（全部） | 64,812 | **63,128** | **−1,684** | 64,456 | 62,780 | −1,676 |
| 堆顶 ≈ | 15,368 | **17,048** | +1,680 | 15,720 | 17,400 | +1,680 |

**A–D 全程累计（正常版）：静态 67,864 → 63,128（−4,736B，78.7%），堆顶 12,312 → 17,048（+4,736B）。
→ 阶段目标「约 63–64KB」达成 ✔（AP 引导版 62,780）。**

后续可选（未做，按用户排序留给后续评估）：E `chapterPageOffsets` 4KB 按需 malloc、
F `gFpBuf`/`winItems` 等、G LittleFS.end()、H SDK/lwIP 裁剪，以及
weatherErrorText/JSON 键/stateText 等"需 consumer 改造"的串。FSDL 512/256/128 自适应未动。

---

## 12. /fs/list 视图一致性修复 A（2026-09-05 已实施, 用户拍板）

问题：缓存视图(不过滤, 显示 .bin) vs 实时视图(白名单 .txt/.bmp) 不一致 → 删除使缓存失效后
切到实时视图 → 同目录非白名单文件"集体消失"假象（Playwright 复现; 物理文件未丢, 复位后证实）。
修法（严格限 /fs/list 可见性, **未动 DELETE 的 SD 删除逻辑**）：
- `file_api_fs.{h,cpp}`：新增**单一入口** `bool ofsEntryVisible(name, isDir)`
  （目录=黑名单; 文件=黑名单+白名单 .txt/.bmp），实时列表 `ofsListItemCb` 改走它；
- `fs_cache.cpp`：删除本模块复制的那套过滤（fsHasBlacklistSuffix/fsBlacklisted/fsWhitelisted），
  `fsCacheScanOne` 改调 `ofsEntryVisible`——**规则全库只有这一份**。
- 编译：正常版 RODATA 16,328 / 静态 ≈63,068B（−60 vs §11）；BOOT_AP RODATA 15,968
  （顺带省掉重复过滤表）。
- 实测（BOOT_AP 复位重建缓存）：`/fs/list` = 仅 4 目录 + 武炼巅峰.txt（bin 不再入缓存;
  `/字体` raw=34 kept=0(filtered); 全树 items 194→155）→ 与实时视图同规则 ✔。
- 遗留 SD 测试残留（隐藏, 0B, 列表不可见）：`_va.bin`（必要时经 /api 或改名清理）。

**验收中发现的两个存量"栈炸弹"（超 A 范围, 待用户决策是否修）**：
1. `/api/stat`：栈上 SdEntry(~672B)+path[300]+tmp[600]+name[256] ≈1.8KB → 4KB 循环栈深链溢出
   → 设备 "Software/System restart"（串口复位原因实锤; A 验收首次调用即崩）。
2. `/api/download`（file_api.cpp）：栈上 plain[256]/enc[300]/disp[700] ≈1.2KB → 同类偶发
   RST/复位（即审查中标注"未做 Step B heap 化"的端点）。
两者都违反项目规则"HTTP 处理器内大缓冲必须 malloc"；修法 = 照 Step B 做请求级 heap。
**测试基础设施备注**：UI 自动化在弱信号 AP 上受 #loading 遮罩滞留/掉线干扰（部分步骤未跑完）；
串口常驻监听长会话后会停摆（.NET/pyserial 均现, USB 级, 重开即恢复）——均非固件 A 修 bug。

---

## 13. 栈炸弹修复（2026-09-05 已实施, 用户批准, 范围锁死）— /api/stat + /api/download

背景：§12 验收中发现 `/api/stat`、`/api/download`(file_api.cpp) 在 4KB 循环栈 HTTP 深链下溢出
→ 设备 Software/System restart（阻塞 A 验收 ④ 与后续 /api/download 回归）。

修复（`file_api.cpp`，接口语义不变）：
- `/api/stat`：SdEntry(~672B)+tmp[600]+name[256] 由栈上 → **请求级 heap**（malloc/free, 失败 500
  internal_error; 404/成功路径均释放）。
- `/api/download`：① 头三缓冲 plain[256]/enc[300]/disp[700] → **请求级 heap**, sendHeader 后立即
  free（头未发出前 malloc 失败可 500）；② 发送循环**对齐 /fs/file 已验证模型**：自适应缓冲
  {512,256,128} + `availableForWrite()==0 → yield()+wdtFeed`（不设轮数上限）+ `connected()` 断连 +
  每块喂狗；**Range 协议未改**（206/416/Content-Range 语义原样保留）。
编译：正常版 RODATA 16,340（静态 ≈63,080B）/ BOOT_AP 15,980 均通过。
验收门（用户定, 待设备稳定后执行）：/api/stat 正常 → /api/download 小文件 → Range → 64MB 完整 +
HEAD/MID/TAIL 1KiB MATCH → 无 Ex29/OOM/WDT；之后才开 FSDL 64MB×3 + BearSSL 套件。
（2026-09-05 已验：/api/stat ✓(200/404/dir) · /api/download 小/Range/64MB 完整+三切片一致 ✓ 均浏览器验收
ALL PASS；详见会话记录与 serial_logs。）

---

## 14. 上传"400 假象"根因修复（2026-09-05, 无头浏览器定性, 用户 ESP 端修复指令）

现象：manager 页上传完成 → 网页提示 400 → 文件其实已写入；手动刷新(慢)后出现新文件。
定性（Playwright 事件流实锤）：`POST /fs/edit` 上传成功回 200 但响应体是人类文案 **"上传成功"**，
manager.htm `onOperationComplete` 按 PUT/DELETE 同契约把响应体当**父目录路径** →
`GET /fs/list?dir=上传成功` → **400 BAD PATH**（页面可见的 400；上传本身成功）。
修复（`file_api_fs.cpp` handleOfsEditUploadDone）：成功响应体改回**父目录路径**（根级归一 "/"，与
DELETE/PUT 同口径），消除该 400 与误列目录请求。复测(稳定链路) ALL PASS：
事件流 = POST 200 → Listing '/'×2 → /fs/list 200 → 自动出新行 → 预览 200，**无 400**。
环境备注：Intel AX101 对 ESP AP 每小时掉线 ~15 次（Windows 提示音），是剩余不稳定源（非固件）；
另见 manager.htm 上传 Blob 强制 Content-Length 说明（非本次改动）。

设备侧待办（用户操作）：断电 → 重插/检查 SD → 上电稳定后再验。

---

## 15. 通用文件管理操作墨水屏通知（2026-09-05 已实施, 用户需求; 参照上传/下载通知）

新增 `/fs` 新建文件/夹、删除、重命名/移动 的墨水屏通知（上传/下载原有）：
- `file_api_fs.h/.cpp`：`OFS_OP_PHASE_*` 枚举 + `ofsOpSetPhaseCallback/ofsOpReport`（去重同上传/下载）；
  `handleOfsEditPut`（新建/夹/重命名/移动）与 `handleOfsEditDelete` 动手前报 START、成/败报 DONE/FAIL
  （校验类错误如"已存在/受保护"不通知）。
- `ink-reader-esp.ino`：`renderFileOpStatus`（配网页底行"操作中/操作成功/操作失败:<目标>"，局刷）
  + setup 注册；串口标记 `OP_NOTIFY_LINE`。
编译：正常版 RODATA 16,428 / 静态 ≈63,168B（+88）；BOOT_AP 16,068 均通过；已烧录 BOOT_AP。
"新建文件→复位"排查：受控复现 3 次均被 AX101 掉线打断（请求未送达/串口无 BOOT），未发现固件复位证据
（真复位仅与已修的 stat/download 栈炸弹相关）；待链路稳定（电源管理/STA/手机）后复核。

---

## 16. 新建/删除等改路径 handler 的复位根因与修复（2026-09-05 浏览器复测实证）

现象：manager 新建/删除等操作间歇复位（曾疑 AX101/SD）。浏览器+串口同捕实证链：
1. PUT create: `fs_put → OP_NOTIFY 操作中(渲染) → 卡 ~4.5s → Soft WDT`（文件未建）；
   多次 `epc1≈0x40003b5x`(ROM/软狗)、`Fatal exception:4 flag:3`。
2. 修 START 渲染(实验A)后 create 能建文件但仍崩；`stack=0`（diag 实测 handler 深链剩余连续栈 0B）。
3. 根因：这些 handler 位于 WiFi/HTTP 深链，剩余栈仅 0~100B；新增的大栈局部（path/lab/d/t/s2 等）
   与 handler 内 EPD 渲染使其栈溢出/忙等 → 各种复位（Exception 2/4、Software/System restart）。

修复（均已烧录 BOOT_AP 并通过复测 ALL PASS：3× 新建文件 立即可见+清理、文件夹 建/删、全程存活）：
- `file_api_fs.cpp`：PUT/DELETE 大局部（path/lab/d/t/s2/parent）改**文件级 static**（gOpPath/gOpA/gOpB/
  gOpC/gOpLab；单线程顺序 handler 安全）→ handler 栈≈0；删除 handler 内多余 reinit，保留 wdtFeed。
- 通知渲染移出 handler：`renderFileOpStatus` 零栈缓冲直写 gWebNotifyLine 只打日志；EPD 渲染由
  `wifiManagerLoop` 在 handleClient 后（浅栈）按 `ofsOpGetPhase` 变化调 renderPage（新增 getter）。
- `handleOfsEditPut` 新建/新建夹成功后 `fsCacheInvalidateDir(parent)`（此前漏：新建项需等其它操作
  失效缓存才在 /fs/list 可见——"新建后不出现"的另一半根因）。
残余观察（非本批 handler 代码）：操作成功后的空闲/收尾期偶发 Soft WDT，`epc1=0x40104a84 →
lmacRecycleMPDU`（WiFi MAC TX 缓冲回收，项目早记的堆碎片/突发 TX 崩溃类）——归入后续 lwIP/堆碎片的
独立排查（每次操作后 ~0.9s 的 EPD 渲染×2 亦叠加负载，可先只渲染 DONE/FAIL 减半验证）。

**追加(同日) "删除后自动刷新列表"复位修复**：实测 删除成功→页面自动刷新 /fs/list（缓存失效→实时 SD
列表）时深链 **stack=0** → `User exception(panic)` + Soft WDT（epc 0x40003b53）。修复：
- `handleOfsList` 的 path[300] → 文件级 static `gLsPath`；
- `sdListDirPaged` 帧内 `SdEntry(650B)+full(420B)` → 文件级 static `gPagedEntry/gPagedFull`
  （HTTP 单线程顺序, 零分配安全）。
复测：3×(建→删→连续5次列表) 全 200、串口 0 BOOT。静态成本 +~1.3KB（正常版 ≈66,020B, 堆顶 ≈14,160）。






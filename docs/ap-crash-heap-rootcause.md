# AP 崩溃根因定案：softAP 初始化堆耗尽（memset 传 NULL）

> 2026-08-31。配网 AP 启动 `WiFi.mode(WIFI_AP)` 必崩 Exception 29 的最终结论。
> 本文同时记录「链接布局假设被 addr2line + literal 对照证伪」的完整证据链，避免以后再回到这条死路。

## 结论一句话

**主固件 DRAM 静态占用 ≈ 63KB/80KB，`WiFi.mode(WIFI_AP)` 启动 softAP 时 SDK 分配缓冲需 heap > 15KB，而剩余 heap 仅 ≈15KB → malloc 返回 NULL → SDK 对 NULL 做 memset → Exception 29。**

**不是** 链接布局 / literal pool / core 版本 / IRAM 93% / GCSBS / 初始化顺序 / WiFi.mode() 本身。

## 崩溃证据

- `epc1 = 0x4000e1c3 = 0x4000e190(ROM memset) + 0x33`，`excvaddr = 0x18`（写近零地址）。
- `0x4000e1c3` 在 **ROM region**（`0x4000_0000`），不在 app 的 iram（`.text/.text1` 在 `0x4010_0000`）→ 就是 ROM memset，直接命中「上层传了坏指针」。
- 崩溃前探针打印 `FREE before mode free=15056 max=15056`（堆不碎片化，单纯只有 15KB）。

## Backtrace（完整栈，addr2line 对照 `build_bootap/ink-reader-esp.ino.elf`）

```
setup  (ink-reader-esp.ino:4812, WIFI_TEST 探针的 WiFi.mode(WIFI_AP))
  → ESP8266WiFiGenericClass::mode      (ESP8266WiFiGeneric.cpp:442)
    → wifi_set_opmode_current           (0x402dba92)
      → wifi_set_broadcast_if → wifi_softap_start (0x402bccc3)
        → ieee80211_hostap_attach       (0x402bb910)
          → ic_bss_info_update → chip_v6_unset_chanfreq
            → esf_buf_alloc             (0x402d7d2b)
              → malloc                  (umm_malloc.cpp:912, IRAM 0x40100dd4)
                → memset                (ROM) → ⚡ crash
```

- 关键帧来源 `.ino` 探针在 `setup` 最前，隔离了一切初始化。
- `builder` 里 fatal 帧 `malloc` 在 IRAM（`ICACHE_RAM_ATTR`，UM 可被 ISR 调用），链上还有 `fpm_close`（`mode()` 里 `m != WIFI_OFF && fpm_sleep!=NONE` 分支）。

## 证伪「链接布局」的静态证据

1. WiFi.mode = `0x402982bc`（.irom0.text），可正常反汇编。
2. 内部 `l32r a0, 0x4027feb0` 的字面量值 = `0x4000e190` = **ROM memset**（合法指针），`callx0 a0` 调的就是 memset —— 不是垃圾/错位指针。
3. 该地址段 objdump 标注为 `<progressSyncFreeReaderHeap+0x9c>` 只是「最近符号」标签，不代表字面量内容损坏。

## 定量对照（同一 Core、同一 WiFi.mode，唯一变量=剩余 heap）

| | probe3（成功） | 主固件（崩） |
|---|---|---|
| `mode()` | 0x40201508（同 core 函数） | 0x402982bc（同 core 函数） |
| DRAM 静态 (data+bss+rodata) | ~28 KB | **~63 KB** |
| `WiFi.mode` 前 free heap | **~50 KB** | **~15 KB** |
| softAP 初始化 | ✅ | ❌ malloc→NULL→memset |

## DRAM 归因审计（build_bootap）

- `dram0_0_seg` = 80 KB。
- `.data` = 0.9 KB；`.bss` = **43.5 KB**（510 个对象）；`.rodata` = **19.3 KB**（350 个条目，多为匿名字符串字面量 + 小 const 表）。
- 静态合计 ≈ **63.7 KB** → `_heap_start = 0x3fff7be8` → 剩 ~15 KB heap。

### .bss 主要对象（bytes）
```
4736  fb                       # EPD 物理帧缓冲 128×296/8，必留
4096  chapterPageOffsets       # 章节目录页偏移表（仅章节视图用）
1704  g_ic                     # 全局初始数据
1024  gFpBuf                   # 进度同步文件指纹缓冲
 864  winItems                 # UI 窗口项
 640  ofsListItemCb::buf       # SD 列表回调缓冲
 512  handleFmStatic::buf / ofsListItemCb::nameEsc / wvfState
 432  dns_table / 408 chapterRows / ... 数百个小对象
```
- 其余 ≈ 29 KB 为**数百个小对象**（app + 网络栈 lwIP/core 静态）。

### .rodata 主要对象（bytes）
```
1358  progress_sync.cpp.o    # 同步协议字符串
 652  wifi_manager.cpp.o     # 配网/JSON/HTML 字符串
 611  ink-reader-esp.ino.cpp.o
 564  file_api.cpp.o
 512  upcase.cpp.o            # SdFat upcase 查找表
 ...
```
- 无单一超大 const 表；19.3KB 是逐文件字符串字面量 + 小表累计。

## 对修复方向的意义

主固件无法在同一固件里「满血阅读器 + softAP 配网」共存，根因是静态 DRAM 已被吃到 63KB，软 AP 分配不出 >15KB。两条路：

- **A → B 顺序**（用户已定）：先做 A（削减静态 DRAM 到 ≤50~52KB，heap ≥26~28KB），目标 `WiFi.mode(WIFI_AP)` 稳定成功，证明单一固件可兼阅读器 + 配网；若砍到要牺牲阅读器功能才够，再走 B（独立配网固件）。
- **A 的候选动作**（按性价比）：
  1. `chapterPageOffsets`(4KB) → malloc 按需，仅章节视图分配。
  2. progress_sync 的静态缓冲（gFpBuf 1KB + 协议字符串 ~1.4KB + 路径/配置）→ 按需分配或 `#if` 可选。
  3. 大字符串字面量 → `PSTR()/PROGMEM`（.irom0.pstr 进 flash），针对 rodata 占比高的 wifi_manager/file_api/ino/weather/progress_sync。
  4. lwIP/网络栈裁剪（SNTP、IPv6、缓冲区池大小）——core 构建级，可能释放数 KB。

## 记录 / 产物

- 必崩固件与 ELF 已锁死于 `build_bootap/`（bin/elf/map 同一次编译，2026-08-31）。
- 完整栈日志：`web_test/crash_full.log`。
- 设备已被恢复为正常固件（`ink-reader-esp/build/ink-reader-esp.ino.bin`，无测试 flag），验证正常启动/全刷/按键扫描。

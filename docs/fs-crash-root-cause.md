# /fs 崩溃最终定位 —— SD 访问在纯 AP 低堆下触发 WiFi 中断崩溃

> 日期：2026-08-28（分页方案验证后）
> 本文件记录分页方案测试中获得的**决定性崩栈解码**，推翻/修正此前所有关于
> "项数太多 / sendContent / client.write / 每请求泄漏"的假设。

---

## 一、现象（分页方案）

- `/fs/edit` 页面加载成功（懒挂载，根目录列表能出）。
- 展开/点到一个文件夹 → **重启**。
- 分页把项数降到 50、甚至实测仅 `emitted=20`，**仍崩**。

## 二、异常签名（分页版）

```
[26s] FSREQ #1  enter=2200  done=1144 maxblk=256  emitted=20 more=0   ← 20 项, 成功但 maxblk 已极低
[26s] FSREQ #1  LOOP_AFTER heap=1144 maxblk=256 stack=288
[28s] FSREQ #2  enter=2936  stack=288
[29s] SD_REINIT reason=fs_list
[29s] Exception (29):  epc1=0x4000e1b2  excvaddr=0x00000000
```

- `epc1=0x4000e1b2` 落在 **`memset`**（0x4000e190）内部。
- `excvaddr=0x00000000` → **memset 往空指针写**。

## 三、崩栈解码（决定性）

```
loop_task(ETSEventTag*) core_esp8266_main.cpp:274
  → ppPeocessRxPktHdr / pp_tx_idle_timeout            (SDK WiFi 收包/发送)
  → hostap_input / ieee80211_send_proberesp           (AP 应答手机探针/关联)
  → ip4_output_if_opt_src (lwip2 ip4.c:1765)          (网络发包)
  → esf_buf_alloc                                     (分配 WiFi 发送缓冲)
  → memset (0x4000e1b2) 崩, 写空指针
```

## 四、根因（最终）

**纯 AP 模式下，手机作为客户端持续向 AP 发探测/关联包（`hostap_input`），AP 需
`esf_buf_alloc` 分配 WiFi 发送缓冲；在 `/fs/list` 的 SD 卡访问（`SD.open`/`openNextFile`）
期间，SD 访问长时间占用 CPU + 争抢同一块低堆，导致 `esf_buf_alloc` 拿到 NULL → memset
（写空指针）崩溃。**

关键证据链：
1. 崩溃紧贴 `SD_REINIT reason=fs_list`（即 SD 访问路径）。
2. 崩栈全是 SDK WLAN 层（`hostap_input`/`esf_buf_alloc`），非 `/fs` 应用代码。
3. `emitted=20`（远低于 50 上限）时 `maxblk=256` —— **项数不是瓶颈**，SD 访问 + WiFi
   争堆才是。
4. web_test 纯 STA 不崩：无 AP 的 `hostap_input` / 探针响应路径，且堆充裕。

## 五、推翻的旧假设

| 假设 | 结论 |
|---|---|
| 一次加载太多项 → 分页能解 | ❌ 分页后 emitted=20 仍 `maxblk=256`，非项数问题 |
| sendContent 内部 String 分配 | ❌ 换 client.write 反而触发硬件 WDT |
| client.write 更省 | ❌ 被证伪（见 A/B 实验） |
| 每请求 String 泄漏 | ❌ 未证实；`maxblk` 在单次 20 项已极低 |
| 主循环栈耗尽 | ⚠️ 一部分（stack 掉到 288），但主因是 AP WiFi esf_buf 崩溃 |

## 五.5、联网研究补充（2026-08-28）：真正的安全阈值是 "堆水位 < ~6500"

联网证据（esp8266/Arduino Discussion #8722；Universal-Arduino-Telegram-Bot Issue #56）：
- ESP8266 崩溃阈值是 **free heap < ~6500-5700**。低于此，**任何**需 `esf_buf_alloc`
  的 WiFi 数据路径（收包/发包/proberesp/关联）都可能返回 NULL → 崩溃。
- 我们纯 AP 会话：`config_enter 7464 → ap_created 5504 → config_ready 5080`。
  **进入 AP 后 heap 立刻降到 5080，已经低于 6500 崩溃线。**
- 所以更准确的根因是：**纯 AP 会话的整体基础堆水位本身就贴着/低于崩溃线**，
  SD 访问只是"压断骆驼的最后一根稻草"。这解释了**为什么组A（定时探针不碰 SD）下
  用户打开文件夹仍崩**——因为 `/fs/list` 的 SD 访问才是那个高频触发点，
  而 AP 会话基础堆水位（~5080）从一进 AP 就已经不安全。

关键机制吻合：讨论 #8722 明确 `ieee80211_getmgtframe` 失败 ← `esf_buf_alloc` 返回 NULL
（与我们的崩栈 `hostap_input → lsbpp_send_proberesp → esf_buf_alloc → memset` 完全一致）。
讨论还指出 **SDK 3.0.5 的 `esf_buf_alloc` 用 IRAM Heap**（预发布 SDK 3.0 不用），
这改变了 `esf_buf_alloc` 的可申请内存池。

### 潜在可操作的内存优化点（尚未验证）
- `softAP(ssid, psk, channel, ssid_hidden, max_connection=4, beacon_interval=100)`：
  - `max_connection` 降到 1~2 → 可能减少 AP 连接的 RX buffer 预留。
  - `beacon_interval` 调优 / `ssid_hidden=1` → 可能减小管理帧（beacon/探针应答）压力。
  - 这些都直接影响 `hostap_input`/`esf_buf_alloc` 的可用内存池，值得实验。

### MMU 4816H 实测（2026-08-28）—— 假设被证伪
按"让 esf_buf_alloc 用 IRAM second heap"的思路，切到 `16KB cache + 48KB IRAM + IRAM heap`
（`-DMMU_IRAM_SIZE=0xC000 -DMMU_ICACHE_SIZE=0x4000 -DMMU_IRAM_HEAP`）实测：
- **AP 会话堆没抬反而略降**：`config_enter 7336 / ap_created 5376 / config_ready 4648`
  （平衡模式为 7464/5504/5080）。
- **崩溃类型变了**：平衡模式是 **Exception 29（esf_buf_alloc → memset 空指针）**；
  MMU 版是 **panic `core_esp8266_main.cpp:191 __yield`（Exception 4, Software WDT）**。
- 展开文件夹 heap 仍极低：`FSREQ #2 enter=2976` → `SD_REINIT fs_list` → `panic __yield`。
- 副作随：**ICACHE 32→16KB 减半**（flash 取指缓存变小），可能拖慢代码执行 + 改变协程挂起时序。

**结论：MMU 4816H 方向不成立。** 印证 Issue #9033 的警告"48KB IRAM 不一定增加可用 RAM"；
`esf_buf_alloc` 未被真正喂到 IRAM heap，反而因 ICACHE 减半引入新的 Software WDT 崩溃点
（`ESP.wdtFeed()`/`yield()` 在 `/fs` SD 路径上于不可挂起协程上下文触发 `panic`）。

## 六、根本机制（联网研究定论，2026-08-28）

### 崩根因（多源证据一致）
- **`esf_buf_alloc` 在中断上下文（WiFi 收包/发包 ISR）里因 DRAM 堆不足返回 NULL** →
  `ieee80211_getmgtframe` 失败 → `pm_send_nullfunc`/`hostap_input` 崩溃。
  （`esp8266/Arduino` Discussion #8722，mhightower83 维护者原话："ieee80211_getmgtframe
  将失败当 esf_buf_alloc 返回 NULL"。）

### 为什么 MMU/IRAM heap 方向原理上就错了（决定性）
- `arduino-esp8266` MMU 文档明确：**"对于在中断禁用下调用 `umm_malloc`，`malloc` 恒从
  DRAM heap 分配"**。而 `esf_buf_alloc` 恰恰是在 WiFi 中断上下文调用 →
  **它永远拿不到 IRAM second heap**，只能从 DRAM 取。
- 所以任何"把 IRAM 变大/加 second heap"的做法都无法帮到 esf_buf_alloc —— 这直接解释
  了 MMU 4816H 实验失败（config_ready 反而略降、崩溃类型变了）。

### SDK 版本确认（重要，推翻"作者降级方案直接适用"）
- `platform.txt:52 build.sdk=NONOSDK22x_190703` → 我们默认编的是 **NONOSDK 2.2.0**
  （`user_interface.h` 里 `NONOSDK=0x22100`），**不是** 3.0.5。
- `boards.txt` 有 `generic.menu.sdk.nonosdk305=nonos-sdk 3.0.5 (experimental)` 可选项。
- 讨论 #8722 作者在 **STA 长时运行**场景（MQTT/TCP）用 SDK 3.0 稳定、SDK 3.0.5 崩；
  我们是在**纯 AP 短会话 + SD**更极端场景，且已用 SDK 2.2.0 仍崩 →
  **问题主因是 DRAM 堆水位偏低，而非单纯 SDK 版本**。

### 真正可治本的方向（唯一）
**提高 AP 会话的 DRAM 可用堆**（因为 esf_buf_alloc 在中断里只能用 DRAM）：
- 进配网前释放所有可释放的大缓冲（当前已做 progressSyncFreeReaderHeap 等，但 AP 会话
  config_ready 仍只有 ~4648-5080，低于 6500 崩溃线）。
- 进一步压缩 AP 会话的常驻占（在配网会话里 `free()` 掉 /fm、weather、hitokoto、索引块
  缓冲等与配网无关的运行时大块）。
- `max_connection` / 隐藏 SSID 等 WiFi 参数（减少 AP 自身 socket/管理帧缓冲预留）。

## 七、官方源码对照 + 两个假设证伪（2026-08-28）

### 官方 A7 源码确认（archive/a7_reconstruction/xz015_03A7_rebuild，即设备固件源码）
- 官方文件管理（`WEBServer.ino` 的 `webRead_fileManagement`/`webFileUpload`）**全部用 LittleFS**，
  **从不访问 SD**；`SDFS` 只用于 SD 信息/开关（`webPut_sdInit`），配网核心 `peiwang_mod` 不碰 SD。
- 官方 `softAP(ap_ssid, ap_password, random(1,14), 0, 1)` = **max_connection=1**（所有版本一致）。
- 官方配网成功走 `WIFI_AP_STA`（AP+STA 共存）；SD 挂载用 `ESP.wdtDisable/wdtEnable` 包裹。
- **官方从没在"纯 AP 配网 + SD 文件管理"场景下工作过** —— 我们走的正是官方没覆盖的路径。

### 假设证伪 1：MMU 4816H（IRAM second heap）
切 `16KB cache + 48KB IRAM + IRAM_HEAP`：`config_ready 4648`（反而更低），崩溃从 Exception 29
变成 `__yield` panic（Exception 4, Software WDT）。**证伪**（见上文；IRAM heap 帮不到中断里的
esf_buf_alloc，且 ICACHE 减半引入新崩溃）。

### 假设证伪 2：max_connection=1
改 `softAP(ssid, psk, 1, 0, 1)`：`config_ready 5048` vs 平衡模式 `5080`，**几乎无差别**；
崩溃仍是 **Exception 29**（`epc1=0x4000e1c3` memset, `excvaddr=0x18`）。**证伪** —— AP 结构的内存
主导项是 AP 基础设施本身，不是按 max_connection 线性扩展；官方用 1 是连接稳定性，非内存救急。

### 决定性结论
- AP 会话堆水位无论怎么调都在 **~5000-5400**（config_enter 7432 → ap_created 5472 → config_ready 5048），
 **从一进 AP 就贴着 <6500 崩溃线**。
- 三个假设（分页项数/sendContent/MMU/max_connection 等多个）已逐一证伪 → **根因是纯 AP 会话
  基础堆水位过低**，SD 访问是触发点，不是唯一原因。
- 真正出路（需用户拍板）：
  1. **大幅释放 AP 会话 DRAM**（能抬多少是实验问题；需评估能否到 6500+）。
  2. **改网络形态**（STA 模式复用 /fs；官方 + web_test + WebStick 都验证 STA 下不崩），但纯 AP
     场景（无路由器）下 /fs 需求无法简单迁移。
  3. **接受纯 AP 下 SD 文件管理天然受限**（限制目录规模/操作，降低崩溃概率，不根治）。

## 六、方向（待决策）

分页**降低单次 SD 峰值**是正确且有价值的（减少 WDT/闪存压力），但它**无法根治**
`SD 访问 + AP hostap_input + 低堆` 三者的内存竞争崩溃。真正解法需打破这个组合：

- **C1：独立轻量 AP 会话**（只注册 /fs 需路由 + 最小 AP 配置），仍避免不了 hostap_input。
- **C2：降低 AP 的探针响应开销**（如关闭部分 beacon/探针应答），SDK 层，不可控。
- **C3：把文件管理从"纯 AP"改为"STA 模式"**——但配网必须 AP，冲突。
- **C4：SD 访问期间屏蔽/延迟 WiFi 中断**——不可行（SD 与 WiFi 共享 CPU）。
- **C5：SD 访问改用更小的临时缓冲 + 更频繁喂狗**，尽量压低 SD 单次占用，
  减少与 esf_buf_alloc 的窗口重叠（治标，但能显著降低崩溃概率）。

**实测结论**：要根治，最可行的是让 `/fs` 文件管理在**不处于纯 AP + 手机探头响应竞态**
的窗口里运行，或**接受 AP 低堆下文件管理天然受限**（只浏览小目录、不做大量操作）。

（本文件为调试记录；具体实施待用户拍板后另行文档化。）

# ESP12F SD 卡 Web 文件管理项目研究结论

> 背景：file_manager 纯 AP 配网会话（192.168.4.1）下访问 `http://192.168.4.1/fs/edit` 展开文件夹会崩溃。
> 崩栈解码已定位根因：**纯 AP 低堆下 SD 访问与 AP `hostap_input`/`esf_buf_alloc` 的内存竞争，导致空指针崩溃**。
> 本报告研究 4 个候选开源项目 + 1 个官方基线，判断哪个最适合我们，并对照我们已有的 `/fs` 实现。

## 研究范围（已克隆到 `J:\code\esp8266\_research_fileserver\`）

| 项目 | 定位 | 关键结论 |
|------|------|---------|
| **WebStick** (`tobychui/WebStick`) | ESP8286/ESP12F 微型个人云盘（SD 卡） | **不适合**。纯 **STA** 模式（`WiFi.begin` 连路由器）+ **ESPAsyncWebServer** 异步模型，靠避开"纯 AP"躲过崩溃。列目录用整个 `String` 逐项拼接（无分页）。唯一可借鉴：流式分块上传、UTF-8 文件名过滤 |
| **esp-fs-webserver** (`cotestatnt/esp-fs-webserver`) | ESP32/8266 内置 `/edit` 文件管理器 + `/setup` 配网页 | **架构上是对的方向，但实现有低堆硬伤**。同步 `ESP8266WebServer`（同我们），但 `/list` 用 CJSON **整目录累积 + 全量 serialize**（无分页/limit/offset），5KB 堆下必崩。子代理给出的"推荐改进架构"**逐字就是我们已实现的分页流式方案**（见下） |
| **ViraMediaWeb** (`admin3314/ViraMediaWeb`) | ESP8266/32 轻量 web 文件管理器 | **不适合**。**仅 LittleFS**（无 SD），目录列表整 JSON 累积，每次请求重新分配 HTML String。仅可借鉴分块上传写入模式 |
| **SD-Card-File-Transfer** (`madddx`) | Arduino Uno + NodeMCU 双芯片 | **不适合**。ESP8266 只做 web 前端 + **串口桥**，SD 操作全在 Arduino Uno 上——恰好是我们（ESP12F 直连 SD）的反面教材。无低堆保护 |
| **官方 SDWebServer** (`esp8266/ESPWebServer`) | ESP8266WebServer 自带 SD 示例 | **同款同步模型基准**。`printDirectory` 用 `server.sendContent()` 逐项流式（不整串），`handleFileUpload` 原生 `HTTPUpload` 分块写盘，`deleteRecursive` 带 `yield()`——证实我们的方向正确 |

## 核心对比

### 全部 5 个项目的共同发现：**没有一个是"分页流式列目录"的**
- WebStick / esp-fs-webserver / ViraMediaWeb / SD项目 / 官方 SDWebServer：**全部整目录累积再发送**（除官方 SDWebServer 用 `sendContent` 逐项但无分页）。
- 它们在**堆充裕（STA 模式或 async）+ 目录不大**的环境下能跑，但**一旦纯 AP + 5KB 堆 + 大目录，全部会 OOM/碎片化崩溃**。

### esp-fs-webserver 子代理给出的"推荐改进架构"（与我们实现逐字对照）
```
GET /list?dir=/xxx&offset=0&limit=50
→ 打开 Dir（Cursor 模式）
→ 跳过 offset 条
→ 读取最多 limit 条
→ sendContent() chunked JSON stream: [{"name":"a.txt",...}, ...
→ 立即关闭 Dir，释放文件句柄
```
**这正是我们 `file_api_fs.cpp` 的 `handleOfsList()` + `sdListDirPaged()` 已实现的方案**：
- `GET /fs/list?dir=&start=N&count=M`，`count` clamp 到 `[1,50]`（协议硬上限）
- `sdListDirPaged` 先 `openNextFile` 跳过 `start` 项，再读 `count+1` 项（前 count 送 callback，第 count+1 存在则 `hasMore=true`）
- `sendContent_P` 逐项 chunked 流式发送，响应 `{"items":[...],"nextStart":N,"hasMore":bool}`
- 每项 `yield()+wdtFeed()`（抗 Soft WDT）

**结论：我们的 `/fs` 分页流式方案，在架构上已经达到/超过社区成熟方案的推荐值。换库不会带来更优的列目录实现。**

## 根治不了的根因（换库无法解决）

四个项目"规避崩溃"的手法都是**避开"纯 AP + SD 访问"这一组合**，而不是解决它：
- WebStick：**STA 模式**（连路由器）——所以没有 AP `hostap_input` 路径
- esp-fs-webserver：AP 模式但**用 LittleFS**（例子里没在纯 AP 下用 SD；它的 SD 示例 `csvLoggerSD` 是 STA）
- ViraMediaWeb：仅 LittleFS
- SD项目：**SD 放另一颗芯片**

我们设备的特殊性：**纯 AP 配网 + SD 卡 + EPD 共享 SPI + ~5KB 堆**，这个组合是任何现成项目都没覆盖的。崩栈铁证表明：只要纯 AP 下做 SD 访问，`esf_buf_alloc` 就可能被 SD 访问的 CPU/堆占用挤到返回 NULL → 崩。**这不是列目录实现或 web server 选型能治的。**

## 可借鉴的具体点（低风险，值得吸收）

来自 WebStick / esp-fs-webserver / 官方 SDWebServer 的**上传与工具函数**，对增强我们 `/fs` 有直接价值：
1. **流式分块上传**（官方 `SDWebServer::handleFileUpload` + 社区 `onUpload` 回调）→ 我们 `/fs/edit` 已走 `server.upload()` 原生分块，确认一致。
2. **UTF-8 文件名`安全过滤**（WebStick `getUtf8CharLength`/`filterBrokenUtf8`）→ 处理长中文文件名 / 截断边界，可复用。
3. **文件名截断**（WebStick `trimFilename`，保留扩展名 + UTF-8 安全）→ 与我们的 `utf8Truncate`/`splitNameExt` 思路一致。
4. **前端页面编译进 PROGMEM**（esp-fs-webserver `send_P(200, "text/html", _acedit_htm, ...)`）→ 零 RAM；我们 `manager.htm` 现从 LittleFS 读取（懒挂载），若想再省可考虑，**但 SD 卡的 LittleFS 区可能与我们的懒挂载共用资源，需谨慎**。
5. **SPI 速度降级 fallback**（WebStick `SD.begin` 三级降级 64→32→4MHz）→ 若 SD 不稳定可借鉴。

## 明确不需要的（避免引入）
- **换 ESPAsyncWebServer**：引入 2KB+/连接缓冲 + 异步事件队列，且不解决纯 AP + SD 竞争。
- **整目录累积 JSON**：正是我们要避免的。
- **LittleFS 代替 SD**：我们的业务（TXT/图片/索引）都在 SD。
- **cJSON 枚举目录**：手写流式 JSON 已足够。

## 最终建议

**换库/换架构无法根治崩溃。** 现成项目没有一个覆盖"纯 AP + SD + EPD 共享 SPI + 5KB 堆"这个组合，它们的规避手段（STA / LittleFS / 串口桥 / 异步）要么我们不适用，要么不针对根因。

真正能打破"纯 AP 下 SD 访问 × `esf_buf_alloc`"竞争的方向，依然是之前给过的几个可选路径（C1 独立轻量 AP 会话 / C5 进一步压低 SD 单次占用 / 接受 AP 低堆限制）。参考项目的价值主要体现在**上传与工具函数的小优化**，而非整体架构替换。

下一步建议（需用户拍板）：
- **如果**目标是"让 `/fs/edit` 在纯 AP 下尽量不崩" → 走 **C5**（压低 SD 单次占用/缩短与 WiFi 竞争窗口），配合参考项目里"分块写盘 + 喂狗 + 前端 PROGMEM 零 RAM"的小改进。
- **如果**要彻底根治 → 需要**网络形态让步**（例如配网后用 STA 模式复用同一条 /fs 链路），这是唯一经过验证的"绕开纯 AP hostap_input"的方式（WebStick 就是这么做的）。

---

## 附：治本新方向 —— MMU 切到 48KB IRAM + 2nd Heap（联网研究，2026-08-28）

### 发现
1. **ESP8266 崩溃线：free heap < ~6500-5700**（`esp8266/Arduino` Discussion #8722、`Universal-Arduino-Telegram-Bot` Issue #56）。
   我们的纯 AP 会话 `config_ready=5080` → **已低于崩溃线**。这解释了**组A下用户没定时碰 SD 但开文件夹仍崩**——
   根因不是"SD×AP 竞争"这一个原子原因，而是**纯 AP 会话基础堆水位就压在崩溃线下**，SD 访问只是高频触发点。

2. **`esf_buf_alloc` 可以从 IRAM heap 申请**（Discussion #8722 指 SDK 3.0.5 如此），
   而 ESP8266 Arduino core 支持 MMU 切到 **`16KB cache + 48KB IRAM + IRAM heap`**（`generic.menu.mmu.4816H`
   = `-DMMU_IRAM_SIZE=0xC000 -DMMU_ICACHE_SIZE=0x4000 -DMMU_IRAM_HEAP`）。
   切过去后，`esf_buf_alloc` 就有 IRAM 2nd heap 可用，即使 DRAM 堆 < 6500 也可能成功分配。

### 机制（mmu_iram.h）
- 当 `MMU_IRAM_SIZE > 32KB`（=0xC000=48KB）且未定义 `MMU_SEC_HEAP` 时，**自动建立 second heap**：
  `mmu_sec_heap_size() = 0xC000 - (_text_end + 32 - IRAM_start)`，即 **IRAM 里代码用剩下的空间变成 heap**。

### trade-off / 风险（需真机验证）
- **IRAM 已占 93%**（代码 28703B + ICACHE 32768 = 61471/65536）。48KB IRAM 模式下：
  IRAM 上限 32→48KB（+16KB），但代码 28KB 后 second heap 能拿多少取决于 `_text_end`。
- **ICACHE 32→16KB（cache 减半）**：flash 取指缓存变小，可能轻微降低性能（不崩）。
- Issue #9033 报告"48KB IRAM 不一定增加可用 RAM"（依赖板子/具体固件），所以**必须实测验证**。

### 如何尝试（编译切 MMU）
```bash
arduino-cli compile --fqbn esp8266:esp8266:d1_mini \
  --libraries "J:\code\esp8266\ink-reader-esp\libraries" \
  --build-path "J:\code\esp8266\ink-reader-esp\build_mmu" \
  --build-property "build.mmuflags=-DMMU_IRAM_SIZE=0xC000 -DMMU_ICACHE_SIZE=0x4000 -DMMU_IRAM_HEAP" \
  "J:\code\esp8266\ink-reader-esp\file_manager.ino"
```
验证点：编译后看 IRAM 占用 / 是否有空间给 second heap；烧录后看 `config_ready` 的 heap 是否
因 IRAM heap 加入而明显上升（`esf_buf_alloc` 若能复用 IRAM，AP 会话可能稳定）。

> 这是**机制上说得通但必须实测**的治本方向，不是确定解。若 second heap 太小或代码挤占，
> 可能无效果甚至更不稳定。与 A/B 对照实验结合判断。

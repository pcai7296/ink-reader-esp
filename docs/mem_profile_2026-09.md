# ESP8266 内存占用分布（不同场景实测, 2026-09）

> 来源固件：`build_fsdlfix`（BOOT_AP_MODE=1，含 64MB 传输修复），除标注外均为该二进制实机测量。
> 板：ESP-12F（ESP8266），80,192B RAM（0x13940）。编译报告：**全局/静态 69,216B（86.3%）**。

---

## 1. 总账（为什么堆只有 ~11K）

| 区块 | 大小 | 说明 |
|---|---:|---|
| RAM 总量 | 80,192 | ESP8266 DRAM 上限 |
| **静态已用（.bss+.data+.rodata+.noinit）** | **69,216** | 编译器汇总 |
| ├ .bss | 46,520 | 未初始化全局/静态 |
| ├ .rodata（落在 RAM） | 20,988 | 常量/字符串池（见 §3 大坑） |
| ├ .data | 1,708 | 初始化数据 |
| └ .noinit | 56 | umm_malloc 等 |
| **堆上限 ≈ 80,192 − 69,216** | **≈ 10,976** | 实际含 umm 元数据损耗，boot 早段实测 10,256–12,248 |

**含义：设备可用堆的“天花板”只有 ~11K，一切场景数字都在这之下。** 此前“主页 idle 堆 40K”的直觉不成立；本项目静态从早期 ~56K 增长到 69K（阅读器结构、标签/书签、进度同步、/fs web 层等新增），是堆基线持续下降的主因。

---

## 2. 静态 RAM 大头（nm 按符号排序，Top 表）

### 2.1 我方/应用代码可优化对象

| 符号 | BSS | 说明/优化方向 |
|---|---:|---|
| `fb` | 4,736 | 屏幕帧缓冲（296×128/8），不可减 |
| `chapterPageOffsets` | 4,096 | 章节→页码表静态数组（按最大章节数预留）；可改小/按需分配 |
| `g_ic` | 1,704 | SD/文件系统卷对象（SdFat），不可减 |
| `gFpBuf` | 1,024 | 进度同步指纹 1K 缓冲（v3），可改按需 |
| `gBooks` / `winItems` | 864×2 | 书架/窗口项数组 |
| handleOfsFile 静态（本次新增） | 700+300+300+256=**1,556** | disp/path/enc/plain 栈→静态（下载修复），单线程安全 |
| `ofsListItemCb` buf+nameEsc | 640+512 | /fs/list JSON 行缓冲 |
| `handleFmStatic` buf | 512 | 静态页服务缓冲 |
| `wvfState` | 512 | 天气壁纸状态 |
| `server`（ESP8266WebServer） | 272 | web 服务器对象 |
| `diagRing` | 256 | 环形日志（已压到 2×128） |
| 阅读器：chapterRows/indexRows/txtLines | 408/228/216 | 章节/索引/行缓冲 |
| `dns_table`/`arp_table` | 432/240 | lwIP 小表 |

### 2.2 SDK 网络协议栈 .bss（基本固定，动不了）

| 库对象 | BSS | 说明 |
|---|---:|---|
| libpp `wdev.o` | 12,168 | WiFi 设备/信道状态表 |
| libpp `esf_buf.o` | 2,724 | SDK 静态缓冲池 |
| libnet80211 `ieee80211.o` | 1,704 | 802.11 状态 |
| libpp `trc.o` | 1,132（+588 .data） | trace/日志 |
| libnet80211 `wl_cnx.o` | 940 | 连接管理 |
| libpp `pp.o` / libwpa `wpa.o` / libmain 等 | ~2,000+ | 其余协议层 |

**小计：SDK 网络栈静态 ≈ 24–26K，这是 AP/STA 模式的固有成本。** softAP 只额外占运行堆 ~2.7K（见 §4）。

### 2.3 ⚠️ RAM 内 .rodata 20,988B：字符串/常量池（最大单一优化项）

- 命名大 R 符号合计仅 ~1.3K（lookupTable 300、mapTable 212 等）；
- 其余 ≈ 19K 是**未 PROGMEM 的字符串字面量与匿名常量**（printf 格式串、调试串、表）——ESP8266 默认把普通 `"..."` 放 RAM `.rodata`。
- **优化候选：改用 `PSTR/F()`/`ICACHE_RODATA_ATTR` 可把大部分移入 flash（flash 只用到 88%），腾出最多 ~15–19K 堆。** 这是把“AP 空闲堆 ~6K”抬回“~12–15K”级的最有效手段，但涉及全部日志/页面字符串改造，工作量大、需分批做。

---

## 3. 不同场景堆快照（free heap / maxblk，B）

固件为 BOOT_AP 引导版；`进网络前` ≈ 正常固件主页级基线（无 AP 栈）。

| # | 场景 | heap | maxblk | 备注（去向/原因） |
|---|---|---|---:|---|
| 1 | 启动早期 u≈100ms | 10,256–12,248 | — | core 就绪，setup 未跑 |
| 2 | setup 完成、进 AP 前 | ~9,000 | 9,008 | SD+屏+时钟+fsCache 目录树建好 |
| 3 | softAP 启动后 | 6,288–8,280 | 6,256 | **AP −2.7K**（hostap/lwIP 分配，AUDIT ap_created 实测） |
| 4 | 路由注册 + server.begin | 4,392–7,640 | 4,296–7,624 | 每次 boot 波动（String/config/路由对象） |
| 5 | AP 空闲稳态 | 5,976–7,968 | 4,296–7,392 | 渲染/EEPROM/通知后 |
| 6 | 页面流（GET /fs/edit + /fs/list×2） | 5.9K→~4.7K | — | LittleFS 懒挂载 **−1.2K** 常驻；list 每请求瞬 −2K 后回升 |
| 7 | **上传 START** | **~1.1–1.3K** | 碎片化极重 | SD_REINIT(SD.begin 重入)+SD.open+“上传中”渲染 → 再塌 ~5.5K（见 §5） |
| 8 | 上传中（64MB，同步 493s） | 1,048–1,320 | 低 | 每包直写零 malloc，yield+喂狗存活；RX 方向不占 TX pbuf |
| 9 | 上传结束 | ~4,000–6,500 | 回升 | DONE 渲染后回 AP idle |
| 10 | **下载 START**（512B 缓冲分配后） | ~3,568 | 3,280 | SD 重开+“下载中”渲染后 |
| 11 | **下载中（64MB，同步 297s）** | **1,552–2,896** | — | 512B 块让位 lwIP TX pbuf；低于 ~1.5K 会 Ex29（见 §5） |
| 12 | 下载 END | 2,224–4,552 | 3,800 | 恢复 AP idle |

> 注：上传列（7–9）为 build_bootsta 老固件同场景实测（上传路径代码一致），新固件空闲基线低 ~1.2–2K，数值相应下移但形态相同。下载列（10–12）为本固件实测。

---

## 4. 各阶段增量去向（实测+代码推断）

| 增量 | Δheap | 去向 | 证据/置信度 |
|---|---:|---|---|
| boot→AP | −2.7K | softAP hostap/lwIP 分配 | AUDIT ap_created 前后实测，**高** |
| 路由+begin | −0.6~−1.2K | String 配置/路由对象/server | AUDIT rte_* 实测（boot 间波动），**高** |
| 页面 GET | −1.2K | LittleFS 挂载常驻（文件缓存/挂载结构） | LFS_MOUNT_LAZY 前后实测，**高** |
| 上传 START 附加 | ~−5.5K | SD.begin 重入分配 + SD.open + 状态渲染(EPD 局刷 455ms 帧/LUT) + 堆碎片化使 maxblk 坍缩 | 逐步探针未做（非崩溃源，未再细分），**中** |
| 下载中维持 | — | 512B 传输缓冲 + lwIP TX pbuf 浮动 | FSDL_PROGRESS heap 逐 5s 实测，**高** |

---

## 5. 结论要点

1. **内存是“静态墙”压出来的**：SDK 网栈 ~25K + 应用静态 ~28K + RAM 内字符串池 ~19K ≈ 69K 静态，堆上限仅 ~11K。
2. **各场景余量**：AP 空闲 ~6K（够 GET/列表）；上传可低至 ~1.1K 仍存活（RX/零 malloc）；**下载必须给 lwIP 留 ≥~1.5K**（TX pbuf 堆分配），传输缓冲 512B 是保护线（4096→1.9MB 崩、2048→14MB、1024→25MB、512→64MB 完成，均为实测）。
3. **可回收候选（按性价比）**：
   - ① RAM 字符串池 ~19K → PSTR/flash（最大单项，预计 +10–19K 堆；工程量大）；
   - ② 上传/下载期间 `LittleFS.end()`（UI 流程 +1.2K）；
   - ③ `chapterPageOffsets` 4,096 按需/缩容；
   - ④ 少量一次性缓冲（gFpBuf 等）改按需 malloc。
4. **不可回收**：SDK 网栈 ~25K、帧缓冲 4.7K、文件系统对象 1.7K。

（采集探针：boot/AP AUDIT、FSREQ、ABSAM、OFS_UP、FSDL 全在现固件内，后续回归可直接复用。）

---

## 6. 字符串/常量 flash 化审计（2026-09，build_standard map 实解析）

**RAM .rodata 总量 21,347B，其中 ~19K 来自我们 sketch 的字符串字面量**（esp8266 gcc 把未 PSTR 的字面量放 RAM `.rodata`；每函数一个 `.rodata.<mangled>.str1.1` 段，无符号名故 nm 不可见，只能按 map 逐段解析——见 `rodata_audit.py` 可复用）。

### 6.1 按源文件占用表（改造收益上限）

| 文件 | RAM 字符串/常量 | 说明 |
|---|---:|---|
| `ink-reader-esp.ino`（主文件） | 8,351 | 日志格式串 + 页面/菜单文案最多 |
| `wifi_manager.cpp` | 3,979 | 配网页面 HTML/JSON/状态串 |
| `file_api_fs.cpp` | 1,946 | /fs/* 错误/状态串 |
| `progress_sync.cpp` | 1,816 | 同步状态/协议串 |
| `file_api.cpp` | 1,453 | /api/* 错误串 |
| `weather_data.cpp` | 856 | 天气/错误串 |
| `fs_cache.cpp` / `epd_290a.cpp` / 其余 sketch | ~1,350 | 日志/常量 |
| 库（upcase/lfs/lwip/HTTPClient…） | ~1,500 | 不动（第三方） |

**sketch 合计 ≈ 19.8K → 理想收益 ≈ 19K；实际可安全 flash 化目标 ≥10–14K**（需按消费方 API 分类，见 6.3）。

### 6.2 为什么不能"链接期整体搬到 flash"（硬限制）

ESP8266 iROM（0x402xxxxx）**只支持 4 字节对齐访问**，而字符串要逐字节读（lstrlen/lbu）→ 不能把 .rodata 简单整体重定位到 flash；必须用 `PROGMEM/PSTR/F()` 逐个标注，读取经 flash 助记 API 或消费方原生支持 F 的重载。

### 6.3 可安全 flash 化的三类（按收益/风险排序）

1. **高收益低风险**：直接消费字面量的调用点 —— `Serial/Serial.printf`（改 `printf_P`/F 逐段）、`server.send(code, mime, body)`、`sendContent_P`（已在用）、`s.sendHeader`、页面 HTML 大串、比较用字符串表。
2. **中收益中风险**：`const char*` 常量表/提示文案 —— 转 `PSTR` + 调用点 `FPSTR()`/pgm_read 包装；凡参与 `String` 拼接需注意生命周期。
3. **低收益高风险（暂缓）**：需 `c_str()` 传给库 API / 可变内容 / 反复拼接的长路径 —— 保持 RAM。

### 6.4 改造协议（小步、可测）

每批只改 1 个文件 → 编译 → `python rodata_audit.py build\ink-reader-esp.ino.map` 看 `RAM .rodata` 降幅 → 烧录回归（boot/AP/页面/8MB 传输）。批次顺序建议：`wifi_manager.cpp` + `file_api_fs.cpp` + `file_api.cpp`（网络/传输域，收益 ~7.4K 且直接缓解 multipart 低堆 OOM）→ `ink-reader-esp.ino` 分批（8.3K）→ `progress_sync.cpp`/`weather_data.cpp`。

预期：每完成 ~10K，静态 69.2K→~59K，堆 11K→21K，AP 空闲 ~6K→~16K；multipart 解析 772B OOM、下载需 512B 保护线等问题一并根治（实测验证）。

### 6.5 参考

- ESP8266 官方 PROGMEM 指南（F()/PSTR/FPSTR）：https://arduino-esp8266.readthedocs.io/en/stable/PROGMEM.html
- String 高效用法（字面量在 RAM 的机制与 F() 场景）：https://cpp4arduino.com/2018/11/21/eight-tips-to-use-the-string-class-efficiently.html
- F() 需要消费方支持 `__FlashStringHelper` 重载（ESP8266WebServer send/sendHeader 均有）——逐调用点核对。


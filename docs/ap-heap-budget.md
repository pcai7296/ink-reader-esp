# 配网会话内存账本分析（/fs 崩溃根源核算）

> 目标：搞清楚为什么 `/fs·edit` 在纯 AP 配网会话下重启，以及**怎么写才不容易重启**。
> 所有数字来自实测采集日志（平衡/MMU/max_connection 三版），非推断。

## 一、实测内存账本（AP 会话各阶段 freeHeap）

| 阶段 | 平衡模式 | MMU4816H | max_connection=1 | 说明 |
|------|---------|----------|------------------|------|
| config_enter | 7464 | 7336 | 7432 | **未进 AP，堆充足 ~7.4KB** |
| ap_created | 5504 | 5376 | 5472 | **softAP 吃 ~1960B！** |
| config_ready | 5080 | 4648 | 5048 | 配网页/挂载再吃 ~400B |
| ABSAM 基线 | 5016 | 5112 | ~5112 | AP 会话稳定堆 ~5.0-5.1KB |
| FSREQ_ENTER | 2160 | 2160 | ~2160 | **进 /fs/list 时堆掉 ~3000B** |
| FSREQ_DONE | 1056 | 1056 | ~1056 | SD 遍历+sendContent 后更低 |

**核心结论：**
1. `softAP` 创建固定吃 **~1960B**（config_enter→ap_created，三版完全一致）。这是 softAP 的硬开销，
   **与 max_connection / MMU 无关**（都试过，无变化）。
2. **未进 AP 堆有 7.4KB，进 AP 只剩 ~5KB** —— 纯 AP 会话堆紧张是**固有约束**，不是配置问题。
3. `FSREQ_ENTER` 时堆从 5.1KB 掉到 ~2.16KB（差 ~3000B）—— **HTTP 客户端/请求处理的开销**。
4. SD 目录遍历 + sendContent 再叠加 → 堆低于崩溃线 → `esf_buf_alloc` 失败 → Exception 29。

## 二、BSS 大块（编译时静态占用，非动态堆）

`BSS 50816` 是编译时静态/全局占用，包括：
- `fb[128*296/8] = 4608B` 帧缓冲（配网显示热点信息需要，**不能释放**）
- `diagSdBuffer[1024]`（DIAG_SD 日志用，平时空闲）
- `txtLines[18]` String 数组（阅读行缓冲，`progressSyncFreeReaderHeap` 已清其动态内容 ~1KB）
- 静态 `nameEsc[512]` / `buf[512]` / `gFpBuf[1024]`（file_api/progress_sync 用）

> 关键：**BSS 不占运行时 freeHeap**（编译时扣掉）。把这些数组删/搬 PROGMEM **不会抬 AP 会话堆**。
> 要抬堆只能减 **动态堆的运行时占用**，或减 **BSS 总量**（BBS+data 越大 → DRAM 留给堆越少）。

## 三、为什么 STA 不崩、纯 AP 崩（根本差异）

- **STA 模式（web_test / WebStick / 官方）**：无 `hostap_input`（AP 不接收手机探针/关联包），
  WiFi 数据路径不触发 esf_buf_alloc 的中断竞争，且不经 softAP 那 ~1960B 的开销 → 堆充裕 → 不崩。
- **纯 AP 模式（我们）**：softAP 固定吃 1960B + 手机向 AP 发包触发 hostap_input 需要 esf_buf_alloc。
  AP 会话堆只有 ~5KB（贴 <6500 崩溃线），SD 访问 + HTTP 处理叠加 → esf_buf_alloc 失败。

## 四、官方源码对照（archive/a7_reconstruction/xz015_03A7_rebuild）

- 官方文件管理 `WEBServer.ino`（`webRead_fileManagement`/`webFileUpload`）**全程用 LittleFS**，
  **从不访问 SD**；配网核心 `peiwang_mod` 也不碰 SD。
- 官方把 SD 卡做成**独立可开关子系统**（`eepUserSet.sdState`/`SDFS.end`），与配网文件管理**彻底分离**。
- 官方配网用 `WIFI_AP_STA`（AP+STA 共存），文件管理用 flash → **官方从没在纯 AP 下碰过 SD**。

## 五、可执行建议（按可行性排序）

### A. 降低 /fs 请求处理时的堆峰值（治标最有效）
目标是让 `FSREQ_DONE` 的堆不跌破崩溃线。需要**压掉 HTTP 请求处理 + SD 遍历叠加的峰值**：
- `/fs/list` 用更小的 `count`（已在 [1,50]，但展开时实测 `emitted=20` 已崩 → 项数不是主因）。
- **减少配网页浏览器并发请求**：打开 `/fs/edit` 时会同时请求 HTML + /status + /list + favicon 等，
  多个 HTTP 连接的处理堆叠加。可让前端**串行请求**，或大幅精简 `/fs/status`（O(1)）返回体。
- **确认 `streamFile`/`sendContent` 缓冲大小**：当前用默认，若缓冲过大可调小。

### B. 减 BSS 总量（需要 ≤1.5KB 才够抬堆）
`config_ready 5048 → 要 6500+` 需抬 ~1.5KB。可尝试把部分静态大数组改 `malloc` 并在配网会话
`free()`（配网页用不到时），例如 `diagSdBuffer[1024]`、`gFpBuf[1024]` 等——但要确认它们不在配网页
路径上。**这是高风险改动，需严格验证不破坏其它功能**。

### C. 换网络形态（根治，但需权衡）
- 官方 + STA 实验都证明 **STA 模式不崩**。若能让 /fs 文件管理走 STA（设备连路由器后，或配网成功
  切 STA），则避开了纯 AP 的 hostap_input + softAP 1960B。
- **但纯 AP 的意义就是"连不到路由器时手动管理 SD 文件"**——改 STA 与此矛盾，需用户权衡。

### D. 接受纯 AP 下 SD 文件管理天然受限
限制 `/fs` 只浏览**小目录、少量操作**（单次 ≤ 10 项、禁止大目录/大文件操作），大幅降低崩溃概率，
但**不根治**（仍可能崩）。

## 六、我的判断

最**务实**的是 **A（降低 /fs 请求峰值 + 串行化浏览器请求）**，它不改变 AP 本质，但能显著抬高
触发崩溃的门槛。**B（减 BSS）** 若真有 ~1.5KB 可腾，配合 A 有可能冲到 6500+。**C（换 STA）**
是根治但违背纯 AP 场景。

建议先做 **A**（改动最小、不破坏配网本质），若仍不够再做 **B**。

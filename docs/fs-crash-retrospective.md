# /fs 文件管理在纯 AP 配网会话下重启 —— 全量复盘

> 日期：2026-08-28
> 范围：`file_manager` 设备固件在**纯 AP 配网会话**（192.168.4.1）里访问
> `/fs/edit`（官方 A7 文件管理构造复刻）时反复重启的根因、全部尝试与结论、及下一步建议。
> 本文是复盘 + 建议，**不含未经验证的修复承诺**。

---

## 一、现象

1. 打开 `http://192.168.4.1/fs/edit`（manager 页）→ **加载成功**（根目录列表能出）。
2. 点进一个**文件夹**（触发 `GET /fs/list?dir=<子目录>`）→ **设备重启**。

---

## 二、关键硬件/运行背景（决定性问题域）

- 主控 **ESP-12F（ESP8266）**，RAM 90%（静态 70760/80192），IRAM 93%，flash 87%。
- 配网会话是**纯 AP**（`WiFi.mode(WIFI_AP)`，AP 网段 192.168.4.1）。**不开 STA**（AP+STA 共存会触发 SDK phy Exception 29）。
- ESP8266 主循环栈仅 **4KB**；`ESP8266WebServer` 处理请求是同步回调，`handleClient()` 在 `loop()` 里被调。
- 学习项目 `web_test`（练习台）同款 `/fs` 代码能跑 33/33 通过，**因为它走纯 STA**（连局域网），堆充裕，且**从未在纯 AP 下测过**。

---

## 三、堆水位实测（串口 `AUDIT` 探针，逐阶段）

固件已在 `/fs/list` / 配网入口埋 `auditHeap(phase)` 探针，取真实堆值：

### 配网会话建立（首页进入）
| 阶段 | heap | 说明 |
|---|---|---|
| config_enter | 7544 | 点击进配网、AP 前 |
| ap_before | 7544 | |
| ap_created | 5584 | AP 结构创建后（AP 占用 ~1.9KB）|
| rte_before_begin | 5200 | 路由已注册（onNotFound 分发）后 |
| **config_ready** | **5160** | 会话就绪（**路由解耦后**基线；此前 server.on 注册时仅 ~3976）|

### `/fs/list?dir=/`（根目录，成功）
| 阶段 | heap | maxblk |
|---|---|---|
| fs_list_enter | 2640 | 1904 |
| fs_list_reinit | 2640 | 1904 |
| fs_list_chunked | 1760 | 1256 |
| fs_list_done | 1176 | 336 |

### `/fs/list?dir=<子目录>`（点文件夹，重启）
| 阶段 | heap | stack |
|---|---|---|
| fs_list_enter | 3312 | |
| fs_list_chunked | 2712 | |
| （之后）| 崩溃 | stack=**336** |

---

## 四、两次不同崩溃签名（重要：根因随改动演变）

### 版本A — 修复前（每请求 LittleFS.begin + server.on 路由）
```
Unhandled C++ exception: OOM
```
- 堆真正耗尽（C++ new 失败路径）。AP+路由+LFS 挂载堆基线仅 ~3KB，
  `/fs/list` 一次 `sendContent` 的 String 分配即 OOM。
- 之后的版本 B 里又出现过一次 **Exception 29**（epc1=0x4000e1c3，memset 内，
  excvaddr=0x18），是 onNotFound `Serial.printf(server.uri().c_str())` 读到已释放临时
  String 的空指针写。

### 版本B — 路由解耦后
```
Exception (4):  epc1=0x40100178  excvaddr=0x00000000
BOOT reason=Software Watchdog  Fatal exception:4  flag:3
```
- `epc1=0x40100178` 解码为 **`ets_intr_unlock`**（core_esp8266_main.cpp:224）——
  **软件看门狗（Soft WDT）超时复位**。
- `stack=336` —— **主循环栈几乎耗尽**，深调用链
  `handleClient → onNotFound → fileApiFsTryDispatch → handleOfsList → sdListDir → ofsListItemCb → snprintf/sendContent`
  在 4KB 栈 + 遍历（`sendContent` 每项进 `print`/`_currentClient.printf`）时没有足够
  让出/喂狗，触发软狗复位。

**为什么根目录成功、子目录崩？**
- 根目录 `/` 上过滤后条目少（日志 `DIR_DONE count=7`），`fs_list_done` 时堆还有 1176，
  遍历时间短，喂狗频率够。
- 子目录（如小说目录）里**文件多且全是 `.txt`/`.bmp` 白名单**，`ofsListItemCb` 几乎每项都
  触发 `snprintf`+`sendContent`；`sdListDir` 里**每 64 项才 `ESP.wdtFeed()+yield()`**，
  遍历 + 网络发送在 `handleClient()` 同步阻塞，软狗在两次喂狗之间超时 → 复位。

---

## 五、已做改动（工作区，未验证通过的都标出）

| 文件 | 改动 | 状态 |
|---|---|---|
| `wifi_manager.cpp` | 配网页 13 条 `server.on` 路由改为 onNotFound 精确分发（省 ~1.8KB 常驻路由对象堆）；onNotFound 不再 `Serial.printf(uri.c_str())` 读临时 String；OTA 未启用时不再注册 `/update` 路由 | **有效**（config_ready 从 3.9KB 提到 5.16KB）|
| `file_api.cpp` / `file_api.h` | 新增 `fileApiEnsureLfsMount()` 共享懒挂载；`/fm` 与 `/fs/edit` 只挂载一次 | **有效**（去掉 /fs/edit 每请求 LittleFS.begin 的 ~1KB 峰值）|
| `file_api_fs.cpp` | `handleOfsEditGet` 复用它；`ofsBlacklistedEntry`/`ofsWhitelistedFile` 改 strcasecmp 零 String；`handleOfsList` 加堆探针 | 部分（零 String 有效；探针待清）|
| `ink-reader-esp.ino` | 首页/阅读菜单进配网前调用 `progressSyncFreeReaderHeap()`；退出配网回阅读调 `progressSyncRestoreReaderHeap()` | 阅读器路径有效；首页路径（阅读器未开）为空操作 |

`data/manager.htm` 已从 web_test 移植（相对 URL、无 ACE/iconfont），`data/index.html`/`app.js`/`style.css` 旧 S6 UI 已删（git 历史保留）。

---

## 六、根因总结（按优先级）

1. **AP 会话堆基线太低**：纯 AP 下 AP 结构占 ~1.9KB + 中断/SDK 开销，`config_ready` 稳定在
   ~5KB（路由解耦后）。`/fs/list` 请求本身再吃掉 ~2KB（URI 解析 `_currentArgs`、chunked
   header、SD 打开）→ 子目录遍历时堆归零。
2. **Soft WDT 复位**：`/fs/list` 遍历 + `sendContent` 同步阻塞，`sdListDir` 每 64 项才喂狗，
   子目录文件多时遍历时间超过软狗窗口（默认 ~1.5-3s），或 4KB 栈被深链耗尽（stack=336）。
3. **`ESP8266WebServer` 底层每步 `String` 分配**：`send()`/`sendContent()` 内部
   `_prepareHeader`/`_currentClient.printf` 都构造临时 `String`，在低堆下必失败。
4. **学习项目 web_test 用纯 STA 测**，掩盖了纯 AP 下的堆/时序问题——这是"学习台通过、
   设备上失败"的根源差异。

---

## 七、下一步建议（按性价比排序，供决策）

### A. 让 `/fs/list` 高效且不阻塞（最直接，针对 Soft WDT）
- **降低 `sdListDir` 喂狗间隔**：从"每 64 项喂狗"改为**每 1 项就 `ESP.wdtFeed()`**；
  或在 `ofsListItemCb` 每项（尤其 `sendContent` 前）`yield()+wdtFeed()`。
  `sdListDir` 是 HTTP 处理器内的同步遍历，必须频繁让出。
- **减少每次 sendContent 的栈/堆**：`ofsListItemCb` 用固定 `buf[640]` 已好；可把
  `sendContent` 换为 `s.client().write(buf, len)` 直写（避免 WebServer 内部 String/chunk 头），
  但需自己拼 HTTP chunk 头，复杂度↑。
- **限制单次列表条数**：子目录文件多时，把 `sdListDir` 上限从 200 再降（如 100），
  并 `sendContent` 中频繁让出。

### B. 抬高 `/fs/list` 前可用堆（针对堆耗尽）
- **进入 `/fs/*` handler 时主动释放可腾静态缓冲**，例如 `chapterPageOffsets`(4KB) 等仅在
  阅读器用的大数组——但它是静态 BSS 不占运行堆。
- **确认 `_currentArgs` 解析**：WebServer 每请求 `new RequestArgument[]`；避免给 `/fs/list`
  传超长 `dir` 参数，或改用路径段而非 query 参数来减少参数缓冲。
- **`/fs/status` 的 O(1) 已做**（不遍历 SD）；`/fs/list` 是唯一需要 SDK 遍历的。

### C. 架构级（更大，但最彻底）
- 给 `/fs` 文件管理做一个**独立轻量 AP 会话**：起 AP 时**只注册 `/fs/*` 必需路由 +
  onNotFound 分发**，不注册配网页的 13 条路由（已解耦为 onNotFound，baseline 已从 3.9→5.16KB）；
  若仍不够则进一步**裁剪 `/fs` 用不到的配网页 handler**。
- **接受受限模式**：AP 堆确实撑不起完整文件管理时，明确 UI 支持范围（如只浏览 + 上传，
  不做递归删除/大量下载）。

### D. 清理
- 移除 `file_api_fs.cpp`/`wifi_manager.cpp` 里的 `auditHeap`/`probeN` 诊断探针后验收。
- 验收通过后：把 `file_manager` 改动提交（说明旧 S6 UI 已由 git 历史保留），并把 web_test
  本轮的 filter + 瘦版递归改动一并提交。

---

## 八、结论

**根因是"纯 AP 配网会话堆基线过低 + `/fs/list` 遍历未频繁喂狗/让出"叠加**，导致
**子目录列表时 Soft WDT 复位**（及更早版本的 OOM / Exception 29）。学习项目 web_test 因
走纯 STA 而堆充裕，掩盖了此问题。已做的部分改动（路由 onNotFound 分发、共享懒挂载、
零 String 过滤）确实抬高了基线并让页面/根目录列表成功；**下一步应优先处理
`/fs/list` 的喂狗节奏与 sendContent 栈/堆峰值**，而非继续加路由或加大腾堆。
```

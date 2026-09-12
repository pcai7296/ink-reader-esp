# 进度同步（LUMI1）实现要点与踩坑 · 2026-09-13 定稿

协议规范见 `progress-lumi1.md`（§2.1 地址与目标选择 / §2.3 超时 / §2.4 状态流 / §2.6 UDP 发现与绑定）。
本文只记**实现要点**与**本轮踩坑**；与协议文档冲突时以协议文档为准。

## 1. 地址与目标解析

- 设备连路由器（STA）时用**静态 IP `192.168.0.100`**：`wifiManagerStartSta()` 注入 `WiFi.config()`，
  默认值 `progress_sync.cpp:gSyncDevIp`，可用 LittleFS 根 `/sync_bind.dat` 的 `dev_ip=` 覆盖（置空串则回落 DHCP）。
- 三种地址语义别写混：
  | 模式 | 设备地址 | 同伴地址 |
  |---|---|---|
  | STA（连路由器） | 静态 `192.168.0.100` | 由目标解析得到（见下） |
  | 配网热点（管理网页） | `192.168.4.1`（`startAp()`） | 连上热点的手机 |
  | 同步热点（纯 AP） | `192.168.0.1/24`（`wifiManagerStartApOnly()`） | 固定租约仅 `192.168.0.100` |
- 同伴地址四项**依次尝试且都不锁定**（任一项失败自动降级）：
  1. `/sync.cfg` 的 `ip=`
  2. 绑定值 `TargetConfig.staIp`（等于出厂默认 `.10` 时视为"未绑定"）
  3. 上次成功 `/sync_bind.dat:last_ok`
  4. 广播 + 逐 IP 单播扫描（**60s 窗口**）
- 任一路径连上并取回 `200` 后：写 `last_ok`；若本次由**第 4 项（扫描）**命中，则**同时更新绑定值**。
  实测自动改绑：`.14 → .18`、`.10 → .9` 均自愈成功。
- 超时：连接 **3s**、非最终候选只试 **1 次**（快速回退）；HTTP 读取 **8s**；扫描窗口 **60s**（广播每 3s、单播扫描每 15s）。
- 同步页顺带画两行 `手机：<IP>` / `本机：<IP>`（只在本页已有刷新里画，不额外刷屏；阅读页不显示）。

## 2. 本轮定位到的根因（按发现顺序，均已修）

1. **阅读界面 P5 断言无条件 `RF_OFF("reader_assert")`** 会把同步刚建立的 WiFi 掐断
   （日志实证：`WIFI try-sta ok=1` 紧跟 `RF_OFF reason=reader_assert`）→ 同步会话期间放行 RF。
2. **同伴 HTTP 服务绑"当时的局域网 IP"**：换 IP 后 8384 停在旧地址（`/proc/net/tcp` 查无监听、UDP 8390 仍活
   → "发现能答、HTTP 不通"极具误导）→ 改绑 `0.0.0.0`。
3. **`WiFiUDP` 对象生命周期**：`stop()` 后再 `begin()` 会"能发不能收"（lwip 收包回调未重挂）
   → 常驻单对象、不 stop；但**每次 `new`/`delete` 会把设备卡死在 DISCOVER**（连看门狗都不复位）
   → 只在发包全失败时 `discoveryRebind()` 重绑一次。
4. **收包轮询形态**：主 loop 每 100ms 轮询收不到包，改用**紧循环**（`poll()+delay(20)+wdtFeed`）后 240ms 内收到。
5. **家用 AP 常丢"无线→无线"UDP 广播**（实测：同伴对 PC 的广播秒回、对设备的广播连日志都没有）
   → 必须保留逐 IP 单播扫描兜底；单播与 TCP 同通路、可达。
6. **设备 UDP 收发本身没问题**：`-DUDP_DISC_TEST=1` 钩子实测 50/50 包全收、PC 注入 `LUMIACK` 设备立即
   `DISCOVER ok` → 排错时先用钩子把"发 / 收 / 解析"三段分开，别猜。

## 3. 同伴端（`J:\code\Android\legado`，独立仓库）

- **只按 txt 文件名 + 路径锁定书本**：`EspBookMatch` 仅做 `originName` 精确 + `bookUrl` 路径形态校验
  （不做归一化 / 包含 / 按 size 择近，避免把另一个版本的书当目标）；**不做 `File.exists()` 探测**
  （分区存储下 `/storage/emulated/0/Download/xxx.txt` 存在却 `isFile=false`，曾把已导入的书误判成 `no-book`）。
- GET 取数顺序：ESP 位置快照 → **同伴自身 DB 进度**（`durChapterIndex/Pos` 经 `bytesBeforeCharInFile`
  换算字节偏移）；**绝不用设备推回的历史进度兜底**（那正是"删旧书 → 导同名新书后被旧进度覆写"的根因）。
  不在架上回 `404 no-book`（设备显示"手机上没有这本书"），在架上无进度回 `404 no-progress`。
- 服务端健壮性（服务开启期间）：`START_STICKY`（被杀自动重建）+ **30s 看门狗**（HTTP 服务 / UDP socket /
  唤醒锁失效即自愈）+ HTTP 启动重试 3 次 + 只服务**局域网来源**（非私网 403）+ Wi-Fi 变化回调即时重绑 +
  绑定**单飞锁**（防并发扫描）+ UDP 应答器自愈重绑。
- 地址来源只认**当前 Wi-Fi 网络的实际 IPv4**（`EspNetwork`，排除 VPN / 热点 / 移动数据 / 虚拟网卡）：
  用 `getLocalIPAddress().first()` 会把 VPN 地址报给设备，绑定后永远连不上。
- UI（用户拍板简化）：**一个 IP 输入框（默认 `192.168.0.100`）+ 一个确认按钮**；确认 = 保存 + 主动
  `LUMIBIND` 上报本机 IP；后台每 2 分钟直连该 IP 重试，扫描仅兜底。
- **状态记录与显示**：服务端每次成功响应按**来源 IP** 记 `deviceIp / lastContactTs / status`；设备页显示
  "运行状态：墨水屏拉取进度(x%)"、"最近联系：IP · 时间"、"已连接墨水屏 · IP"，输入框自动校正为设备真实 IP；
  页面打开期间 **3s 定时刷新**（原实现只在进入页面时渲染一次，设备连上来也看不到变化 —— 用户报
  "手表上完全没有关于同步的信息"的直接原因）。
- 构建 / 联调备忘：跨盘 KSP 增量会让 `assembleDebug` 失败 → 用 `-Pksp.incremental=false`；
  `gradle.properties` 已开并行 + 构建缓存（实测 7m15s → 1m35s）；
  无线调试：`wifi_sleep_policy=2` + 熄屏 → WiFi 休眠 → 无线调试自动关（IP 也会变），已置 0 并用
  `adb tcpip 5555` 固定端口；`adb pair <ip>:<端口> <配对码>` 一次即可。

## 4. 调试资产

- **`phone_sim_server.py`**（PC 模拟同伴：UDP 8390 应答 + HTTP 8384 返回 LUMI1，支持
  `--offset/--pct/--size/--no-book/--no-progress`）：设备 ↔ PC 全链路实测 PASS，用于同伴不可用时隔离设备侧问题。
  注意：它开着会与真实同伴抢答 `LUMIACK`，联调完要停掉。
- **`SYNC_AUTO_TEST` 钩子**（默认 0）：开机自动"打开最近阅读 → 等句柄就绪 → 发起同步 → 到比较页自动 APPLY"，
  打印 `SYNCAUTO open/ready/begin/compare/DONE … PASS|FAIL`；**等待 45s 无果会自动取消回阅读页**
  （`-DSYNC_AUTO_WAIT_MS` 可覆盖）—— 设备无法按键时不会卡在同步页。坑：必须自己置 `readerSyncOpen = true`
  （否则状态机停在 PREPARE）；`startTxtReader` 是异步的，要等句柄就绪再发起同步。

## 5. 实测结论（2026-09-13）

| 对手端 | 结果 |
|---|---|
| 手机（V2352A，DHCP 换过 `.14/.18/.10`） | 全链路 PASS，多次换 IP 自动发现并改绑 |
| 手表（W527，`192.168.0.9`） | 全链路 PASS，且该书的 `fs/h0/h1/h2` 与设备**逐字节一致**（`FP state=1 mask=0`），偏移可直接采用 |
| PC 模拟服务器（`192.168.0.14`） | 全链路 PASS（用于隔离设备侧问题） |

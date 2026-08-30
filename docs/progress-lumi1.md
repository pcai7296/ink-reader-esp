# LUMI1 阅读进度格式规范 v2

Lumi（手机阅读 App）与 ESP-12F 墨水屏固件之间**双向阅读进度同步**的共享线格式。
两端（`progress_lumi.cpp` 固件实现 / `LumiProgressCodec.kt` App 实现）必须逐规则一致。

**v2 变更（D0）**：传输层由 WebDAV 云中继改为**直连手机 HTTP**（见 §2）。LUMI1 载荷格式
（§3）不变，两端编解码 fixture 仍逐字节一致。

## 1. 目标与约束

- 载荷总字节数 **≤ 256B**（设备 `gBuf[256]` 静态接收缓冲），典型 ~100B。
- 位置单位：**原始 TXT 字节偏移**（与文本编码无关，两端读同一文件的同一字节位置）。
- 版本化、可扩展：未知 key 忽略，可选 key 驼峰命名。
- 文本格式、UTF-8、无 BOM、行尾仅 LF。

## 2. 传输层：直连手机 HTTP（v2，取代云端）

手机 = 进度服务器，**监听 `8384` 端口**，响应 LUMI1 文本，**明文 HTTP**（局域网内，无 TLS）。

### 2.1 固定地址

| 模式 | 设备角色 | 手机地址 |
|---|---|---|
| 局域网（设备 STA 已连） | STA 客户端 | `192.168.0.10`（`TargetConfig.staIp` 默认） |
| 设备热点（纯 AP） | softAP `192.168.0.1/24` | `192.168.0.100`（`TargetConfig.apIp` 默认，DHCP 固定租约仅此一址） |

- 热点模式为**纯 AP**（`WIFI_AP`，STA 断开，规避 AP/STA 同子网路由歧义）。
- 设备侧目标选择：`WiFi.status()==WL_CONNECTED && staIp 非空` → `staIp`，否则 → `apIp`
  （`wifiManagerSyncTarget()`，`loadTargetConfig` 对空 IP 填内存默认，不写 EEPROM）。

### 2.2 请求/响应

| 方向 | 请求 | 成功 | 失败 |
|---|---|---|---|
| 同步（设备 → 手机拉取） | `GET /progress?file=<RFC3986 编码文件名>` | `200` + LUMI1 文本 body | `404`（手机无此书进度） |
| 覆盖（设备 → 手机推送） | `PUT /progress`，body = LUMI1 + `file=<文件名>\n` | `200` | `400`（进度无效） |

- GET 的 `file` 查询参数做 RFC3986 percent-encoding（保留 `A-Za-z0-9-._~`，其余 UTF-8 字节转
  `%XX` 大写十六进制，`lumiUrlEncodeFilename`）。
- PUT 的 body 内 `file=` 行为**原始 UTF-8 文件名，不编码**（body 是文本，不是 URL）。
  例：`file=武炼巅峰.txt\n`。PUT 请求头 `Content-Type: text/plain; charset=utf-8`，
  `Content-Length` 按实际 body 字节数设置。
- GET 响应 `Content-Type: text/plain; charset=utf-8`；body ≤ 256B。

### 2.3 设备侧超时/重试（每阶段阻塞 ≤6s，循环喂狗）

| 阶段 | 规则 |
|---|---|
| 连接 | `WiFiClient.connect(手机, 8384)` 超时 **5s**；失败**重试 2 次、间隔 2s**；仍失败 → 报"无法连接手机" |
| 读 | 状态行/头部/body 读超时 **3s**；body 按 `Content-Length` 精确读，或无 CL 时读到连接关闭 |
| 等待手机接入热点 | 每 **500ms** 查 `WiFi.softAPgetStationNum()>0`，超时 **15s** → 报"无法连接手机"（仅热点模式） |

### 2.4 状态流

```
PREPARE(快照) → WIFI(STA已连→局域网 / 未连→纯AP) → [WAIT_CLIENT] → CONNECT → GET → PARSE
→ COMPARE(比较页) → [同步→APPLY] | [覆盖→UPLOAD=PUT] → FINISH
```

### 2.5 ~~云端路径（WebDAV，已弃用）~~

> **已弃用（v2，直连 HTTP 取代）**：以下云端路径仅历史参考，固件传输层已删除，
> `WebdavConfig`/EEPROM 保留但 UI 隐藏、`/webdav` 端点仅返回弃用提示。

```
<服务器根>/Apps/Books/.LumiBooks/Cache/<RFC3986 编码文件名>.lumi
```

- `<服务器根>` = WebDAV 服务器根（设备端点与 App `serverUrl` 指向同一云端根）。
- 文件名段做 RFC3986 percent-encoding（保留 `A-Za-z0-9-._~`，其余 UTF-8 字节转 `%XX`（**大写十六进制**））。
- 与旧 `.Moon+/Cache/*.po` 平级独立命名空间，互不干扰。

## 3. 载荷格式

```
LUMI1
ts=1724400000000
size=12345678
offset=123456
pct=73.10
```

### 3.1 通用规则

| 规则 | 值 |
|---|---|
| 编码 | UTF-8，无 BOM |
| 行尾 | 仅 `\n`（LF）；**任何含 `\r` 的行 → 整包无效** |
| 单行长度上限 | **128B**（不含 `\n`）；超长 → 整包无效 |
| 首行 | 恰为 `LUMI1`（版本号）；不符 → 无效 |
| 字段行 | `key=value`，每行一个；`=` 前后无空格 |
| 未知 key | 忽略（扩展兼容） |
| 重复必填 key | 整包无效（严格） |
| 空行 | 无效 |
| 尾部换行 | 允许（最后一个字段行后可有 `\n`） |

### 3.2 必填字段

| key | 类型 | 语义 | 校验 |
|---|---|---|---|
| `ts` | uint64 | Unix 毫秒时间戳 | 纯数字（1–19 位） |
| `size` | uint32 | 源文件字节数 | 纯数字 |
| `offset` | uint32 | 原始 TXT 字节偏移 | 纯数字；**`offset > size` → 无效** |
| `pct` | float | 进度百分比 0.00–100.00 | 数字+`.`；`atof` 解析；**∉[0,100] → 无效** |

`pct` 输出用 `%.2f`（四舍五入两位小数，**纯数字，无 `%` 后缀**）。

### 3.3 可选字段（驼峰命名，设备端忽略除 file 外的其余字段）

| key | 类型 | 语义 |
|---|---|---|
| `file` | string ≤64B | 文件名（UTF-8）。直连 `PUT /progress` 用它校验目标文件；`GET /progress` 响应必须携带 |
| `chapterIndex` | int | Lumi 侧章序号（扩展保留） |
| `charOffset` | int | Lumi 侧章内字符偏移（扩展保留） |

### 3.4 文件指纹组（v3，原子可选组）

用于**文件差异检测**（三点采样，非密码学完整文件证明；SHA-1 仅作差异检测，不用于安全认证/防篡改）。

| key | 类型 | 语义 |
|---|---|---|
| `fs` | uint64 十进制 | 发送方文件字节数（非负，≤20 位；拒绝 `+1`/`0x`/小数/空/溢出） |
| `h0` | 40 位小写 hex | 文件头 `[0, min(size,1024))` 的 SHA-1 |
| `h1` | 40 位小写 hex | 文件中部 `[max(0,size/2-512), min(size,size/2+512))` 的 SHA-1 |
| `h2` | 40 位小写 hex | 文件尾 `[max(0,size-1024), size)` 的 SHA-1 |

- **Fingerprint always describes the sender's current file. The receiver compares it with its own current file.**（GET：手机发送/ESP 比较；PUT：ESP 发送/手机比较）
- **原子组**：`fs/h0/h1/h2` 四字段必须全部存在且合法，否则**整组视为不存在**（半组/非法/重复/超长 → 无指纹）；解析器不得保留半组状态
- **原始文件字节**：不做字符集解码、换行归一化（CRLF/LF 保持原样）、BOM 剥离或任何文本变换
- **生成失败**（open/seek/read 不足/hash 失败）：整组省略，不发半成品
- **比较三态**：双方均有完整指纹组才比较——`fs` 不同→SIZE；`h0/h1/h2` 不同→HEAD/MIDDLE/TAIL；全同→MATCH；任一方无完整组→**UNKNOWN**（不警告）
- **容量**：编码后消息 ≤256B（实际发送字节，含字段/分隔符/行尾，不含 `\0`）；指纹组放不下→整组省略，**绝不截断任何行**
- 位置不足 1024 字节时取实际区间（小文件允许重叠）；size=0 → 三处 SHA-1("") 合法

## 4. 双向换算规则

### 4.1 手机 → 设备（拉取 → 本地）

设备解析 LUMI1 得到 `offset`。应用时（`SYNC_APPLY`）按跨版本规则：
`|offset/size_local×100 − pct| > 1` → 视为源文件与本地不同版本，改用 `target = pct/100 × size_local`；
否则直接用精确字节偏移（误差 <1 页）。

### 4.2 设备 → 手机（本地 → 推送）

设备上传 `lumiMake(ts, size_local, offset_local, pct_local)` 并追加 `file=<文件名>` 行。
App 拉取后镜像同一跨版本规则换算，再经段落级字节偏移表翻译为 `(chapterIndex, charOffset)` 精确跳转。

### 4.3 无时钟语义

设备无有效时钟（BL8025T 离线且 EEPROM 无校准）时 `ts=0`：
- App 自动拉取：跳过（视为"未知新旧"）；
- App 手动拉取：生效，但 UI 提示"设备无时钟，请核对进度"。

## 5. 并发写

手机自动推送与设备手动覆盖可能同时写同一进度：**last-write-wins**，不做合并（进度为单值语义）。

## 6. 使用约束

- **文件名一致性**：手机导入的文件名必须与 SD 卡文件名一致（首版精确同名；M5 起评估别名/模糊匹配）。
- **编码**：设备端仅 UTF-8 渲染。GBK 书字节偏移仍有效，但设备显示乱码 → 建议两端使用 UTF-8 书籍。
- 本规范唯一事实来源；固件 `progress_lumi.cpp` 与 App `LumiProgressCodec.kt` 的 fixture 交叉核对测试
  保证两端行为一致（fixture 字面量在 `pc_tests/progress_lumi_test.cpp` 与
  `app/src/test/.../devicesync/LumiProgressCodecTest.kt` 中逐字节一致）。

## 7. 变更记录

- v1：初始规范（2026，M0 地基）。
- v2（D0）：传输层 WebDAV 云 → 直连手机 HTTP（端口 8384、固定地址 192.168.0.10/192.168.0.100、
  GET/PUT、超时/重试/响应码 200/404/400）；云端路径 §2.5 标弃用。
- v3（D1）：新增文件指纹组 §3.4（`fs`+`h0/h1/h2`，原子可选组，SHA-1 三点采样差异检测；
  发送方语义、三态 MATCH/UNKNOWN/MISMATCH、256B 编码预算硬规则、整组省略禁截断）。

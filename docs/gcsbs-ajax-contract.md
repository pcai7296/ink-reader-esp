# GCSBS.html 前后端 AJAX 契约（复刻官方配网页）

> 权威基准文档。前端 = `data/system/GCSBS.html`（官方，1368 行）；后端 = `wifi_manager.cpp` 的
> `handleOfficialAjax()`（L629-706）+ `wifiManagerBegin()` 的 `onNotFound` 分发（L1466-1498）。
> 前端调用的每一个端点 → 后端当前实现状态 → 缺口。用于设备接上后按优先级补后端。

## 端点总表

前缀约定：`Read_*`/`Put_*` 大多为 GET（响应纯文本，前端 `innerHTML=responseText`）；
`Put_*` 带 `?lx` 时前端判 `lx` 决定发 PUT 还是 READ；表单 POST 端点用 `FormData(form)`。

### GET 读端点（`innerHTML`/变量）
| 前端调用 | 行号 | 用途 | 后端现状 |
|---|---|---|---|
| `/Read_scanWifi` | L1301 | 扫描 WiFi，返回 `<option>` 列表 | 占位 `"[]"`（失效）|
| `/Read_fileSync` | L520 | 剩余空间（文件系统）| 占位 `"0"`（前端当空闲字节，上传恒被拦）|
| `/Read_clockMod` | L1061 | 时钟风格 | 占位 `"未开发"` |
| `/Read_history` | L985 | 历史记录开关 | 占位 `"关"` |
| `/Read_clockCalibrationState` | L1001 | 强制校准状态 | 占位 `"开"` |
| `/Read_info` | L907 | 系统信息 | 占位 `"{}"` |
| `/Read_staNameIp` | L1349 | WiFi 名称+IP，前端判**末位数字**=连接成功 | 占位 `"未连接"` |
| `/Read_city` | L1363 | 城市名 | 已实现（读 WeatherConfig）|
| `/Read_InAWord` | L879 | 自定义一句话 | 占位 `""` |
| `/Read_updataAddress` | L893 | 固件更新地址 | 占位 `""` |
| `/Read_clockCompensate` | L1117 | 时钟补偿 | 占位 `"0"` |
| `/Read_clockJZJG` | L1145 | 时钟校准间隔 | 占位 `"60"` |
| `/Read_clockQSJG` | L1159 | 时钟全刷间隔 | 占位 `"25"` |
| `/Read_setOutputPower` | L1131 | 发射功率 | 已实现 |
| `/Read_clockTimeZone` | L1103 | 时区 | 已实现（但见 A5 参数错位）|
| `/Read_ntpAdd` | L1089 | NTP 地址 | 已实现 |
| `/Read_sdFrequency` | L1075 | SD 频率 | 已实现 |
| `/Read_fileManagement` | L1187 | 文件列表 HTML | 占位 `"[]"`（文件管理链断）|

### GET 切换对（`?lx`：1=Put 0=Read）
| 端点 | 行号 | 说明 | 后端现状 |
|---|---|---|---|
| `/Put_nightUpdata`/`Read_nightUpdata` | L922 | 夜间更新 | 已实现 |
| `/Put_batDisplayType`/`Read_batDisplayType` | L954 | 电量显示 | 已实现 |
| `/Put_setRotation`/`Read_setRotation` | L969 | 屏幕旋转 | 已实现（内部值 0-3 循环但显示仅两态，见 A9）|
| `/Put_history`/`Read_history` | L984 | 历史记录 | **404/占位** |
| `/Put_clockCalibrationState`/`Read_clockCalibrationState` | L1000 | 强制校准 | **404/占位** |
| `/Put_clockFormat`/`Read_clockFormat` | L1015 | 时钟格式 | 已实现 |
| `/Put_sdInit`/`Read_sdInit` | L1030 | SD 卡挂载 | 已实现（与 sdCapacity 同标志，见 A8）|
| `/Put_sdCapacity`/`Read_sdCapacity` | L1045 | SD 容量显示 | 已实现（复用 sdEnabled，非独立开关，见 A8）|
| `/Put_clockMod`/`Read_clockMod` | L1060 | 时钟风格 | **404/占位** |

### POST 表单端点
| 端点 | 行号 | form 字段 | 后端现状 |
|---|---|---|---|
| `/Wifi` | L645 | `password` + 手动 `ssid` 或 `sel1` 下拉 | **404**（handleSave 只接小写 `/wifi`）|
| `/Weather` | L671 | `WeatherKey`+`city` | **404** |
| `/InAWord` | L691 | textarea `inAWord`(64) | **404** |
| `/ClockCompensate` | L712 | `ClockCompensate` | **404** |
| `/ClockJZJG` | L814 | `ClockJZJG` | **404** |
| `/ClockQSJG` | L834 | `ClockQSJG` | **404** |
| `/Put_clockTimeZone` | L733 | `ClockTimeZone` | 已实现但**参数名错位**（读 clockTimeZone/tz，恒 0）|
| `/Put_sdFrequency` | L754 | `sdFrequency` | 已实现 |
| `/Put_ntpAdd` | L823 | `ntpAdd` | 已实现 |
| `/SetOutputPower` | L794 | `SetOutputPower` | 已实现 |
| `/FileUpdata` | L608 | FormData `fileName`=上传文件(multipart) | **404 + 无 multipart 处理** |
| `/DeleteFile` | L1197 | `dom.name` | **404**（不在分发列表）|
| `/RenameFile` | L1220 | FormData | **404**（不在分发列表）|
| `/EnableBmp` | L1240 | `dom.name` | **404**（不在分发列表）|
| `/StopBmp` | L1259 | `dom.name` | **404**（不在分发列表）|
| `/webPut_longPress` | L854 | `LongPress` | **404**（`/web*_` 前缀不匹配分发）|
| `/webRead_longPress` | L1173 | — | **404**（同上）|

## 已实现字段（9 组）
`clockFormat`、`batDisplayType`、`nightUpdata`、`sdInit`、`sdFrequency`、`ntpAdd`、
`setOutputPower`、`Read_clockTimeZone`、`Read_city`。其余均占位或 404。

## 致命断点（前端配置链路完全失效）
- **A1 `/Wifi`(POST)**：官方「保存并连接」404。项目 `handleSave()` 只接小写 `/wifi`（L1481），大写 `/Wifi` 进 `handleOfficialAjax` 无分支 → 404。
- **A17 `/RESET`**：无 `ESP.restart()` → `<a href='/RESET'>` 点重启 404。
- **A5 `/Put_clockTimeZone`**：前端字段 `ClockTimeZone`(L277) 对不上后端 `arg("clockTimeZone")`/`arg("tz")`，`toInt()` 恒 0 → 时区永远写 0。**已修**（改成读 `ClockTimeZone`+支持小数/负值）。
- **文件管理链全断**：`/Read_fileManagement` 返回占位 `[]` → `/DeleteFile`/`/RenameFile`/`/EnableBmp`/`/StopBmp` 表单生成不了且四个 POST 全 404。

## 404 归类
- **进 handleOfficialAjax 但无分支**（内部 else `server.send(404)`，L704）：`/Wifi` `/Weather` `/InAWord` `/ClockCompensate` `/ClockJZJG` `/ClockQSJG` `/FileUpdata` `/RESET`，以及 `/Put_history` `/Put_clockCalibrationState` `/Put_clockMod`。
- **完全不在外部分发列表**（onNotFound "Not Found"，L1497）：`/webPut_longPress` `/webRead_longPress`（前缀 `/web*_` 不匹配 `startsWith("/Put_")`/`startsWith("/Read_")`）、`/DeleteFile` `/RenameFile` `/EnableBmp` `/StopBmp`。

## 占位文案（不响应改动，语义断裂）
`/Read_scanWifi`→`[]`、`/Read_fileSync`→`0`、`/Read_history`→`关`、`/Read_clockMod`→`未开发`、
`/Read_clockCalibrationState`→`开`、`/Read_info`→`{}`、`/Read_staNameIp`→`未连接`、
`/Read_InAWord`→`""`、`/Read_updataAddress`→`""`、`/Read_clockCompensate`→`0`、
`/Read_clockJZJG`→`60`、`/Read_clockQSJG`→`25`、`/Read_fileManagement`→`[]`。

## 语义错位
- **A8** `/Read_sdCapacity`/`Put_sdCapacity` 复用 `settingsSetSdEnabled`，与 `/Put_sdInit` 共用同一标志，非独立「SD 容量」开关。
- **A9** `/Read_setRotation`/`Put_setRotation` 内部值循环 0-3 但显示只有「270度/90度」两态（1/2/3 全显示 270）。

## 其余
- `/system/*` 静态资源（bootstrap.css、jquery.js 等 6+ 个）`serveSystemAsset()`（L1464、L605-624）已实现。
- 文件管理模态框 `<a href="/edit">`（L173）无后端路由，项目用 `/fs/edit`。
- `handleRoot()`（L708）仍是自研配网页（流式输出），官方 GCSBS.html 仅 `/system/*` 可访问；**根页面 `/` 尚未切到官方 GCSBS**。

## 优先级建议（设备接上后 / 等待期补后端）
1. `/Wifi`(POST) 落库 password/ssid（区分/复用小写 `/wifi` 的 handleSave）
2. `/RESET` 加 `ESP.restart()`
3. `/Put_clockTimeZone` 字段名对齐 `ClockTimeZone`（已修）
4. 先补 `/Read_fileManagement` 输出文件列表 HTML，再实现 `/DeleteFile` `/RenameFile` `/EnableBmp` `/StopBmp` + `/FileUpdata`
5. `/Read_scanWifi` 返回 `<option>` 列表（复用 JSON 版 handleScanWifi）、`/Read_staNameIp` 返回「SSID+IP」真实文本（前端判末位数字）
6. `/webPut_longPress`/`/webRead_longPress` 补进分发
7. 占位字段回填真实持久化 + 对应 POST（clockCompensate/clockJZJG/clockQSJG）

# file-api.md — ESP-12F 文件管理 HTTP API 契约（v1 冻结版）

> 统一文件管理 API：Web / Android App / Legado 都只是本 API 的客户端。
> 设备为 ESP-12F + 墨水屏阅读器；服务器仅配网/同步会话在线（AP 192.168.4.1 或局域网 STA）。
> 本契约是下一轮 Android/Legado 客户端开发的依据，请勿随意更改；变更需同步更新本文件。

## 1. 通用约定

- **认证（2026-09-12 修订, 用户拍板）**：**全部端点免鉴权**（管理密码机制已整体废除, 与
  官方 A7 管理页同款"无鉴权"语义）。服务端仅配网/同步会话在线（AP 192.168.4.1 需物理
  接触才能连入; STA 局域网管理态同网段可达）——**本 API 仅限可信局域网使用**, 客户端
  无需发送任何凭据头。`X-Admin-Pass` 头会被忽略; `401 unauthorized` 不再返回。
  v2 若引入鉴权再走 Bearer Token（POST /api/login）。
- **路径**：query 参数一律 UTF-8 percent 编码（`encodeURIComponent`，`+` 须传 `%2B`）。
  服务端 `server.arg()` 解码后经 `normalizeApiPath` 规范化：
  - 必须 `/` 开头；折叠连续 `/`；**拒绝 `..` / `.` 段、`\`、控制字符** → `400 invalid_path`
  - 编码形态（`%2e%2e`）按字面量处理，无穿越风险
- **响应**：成功 `{"ok":true,...}`；失败 `{"ok":false,"error":"<枚举>"}`。
- **错误码枚举**：

  | error | HTTP | 场景 |
  |---|---|---|
  | invalid_path | 400 | 路径非法/穿越 |
  | invalid_name | 400 | 文件名非法 |
  | invalid_query | 400 | 搜索空查询 |
  | protected | 403 | 受保护路径（见 §6） |
  | upload_in_progress | 403 | 下载 .uploading |
  | not_found | 404 | 路径不存在 |
  | exists | 409 | 目标已存在（v1 禁覆盖） |
  | not_empty | 409 | 目录非空 |
  | invalid_move | 409 | 移动防环 |
  | resume_mismatch | 409 | 续传偏移不符（含 serverOffset） |
  | busy | 409 | 传输进行中 |
  | invalid_range | 416 | Range 非法（见 §4） |
  | upload_size_mismatch | 400 | 实收 ≠ X-File-Size |
  | insufficient_storage | 507 | SD 写失败 |
  | internal_error | 500 | 底层 IO 失败 |

- **编码**：请求/响应 JSON 均 UTF-8；文件名保持 UTF-8。

## 2. 端点总表

> 2026-09-12 起全部端点免鉴权（见 §1）; 表内"鉴权"列保留 "无" 仅为历史对照。

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | /api/status | 无 | 设备+SD 状态（容量会话缓存） |
| GET | /api/capacity | 无 | `{total,used,free}`（total 卡直读, used 目录遍历求和） |
| GET | /api/files?path= | 无 | 目录列表（chunked 流式, ≤200 项截断） |
| GET | /api/stat?path= | 无 | 单文件信息 |
| GET | /api/search?q=&path=&depth= | 无 | 递归搜索（depth≤8, 结果≤200, 扫描≤10000） |
| GET | /api/download?path= | 无 | 下载（支持 Range, 见 §4） |
| GET | /api/upload-status?path=&name= | 无 | `.uploading` 临时文件状态（续传用） |
| POST | /api/upload?path= | 无 | multipart 上传（见 §5） |
| POST | /api/mkdir?path= | 无 | 新建目录 |
| POST | /api/delete?path= | 无 | 删除（文件/空目录） |
| POST | /api/rename?path=&name= | 无 | 同目录改名 |
| POST | /api/move?path=&dest= | 无 | 跨目录移动（防环） |

## 3. 端点细节

### GET /api/status
```json
{"ok":true,"apiVersion":1,"sdMounted":true,"sdTotal":30979129344,
 "sdUsed":57969760,"sdFree":30921159584,"apIp":"192.168.4.1","staIp":"",
 "heap":12345,"busy":false}
```

### GET /api/files?path=/books
```json
{"ok":true,"path":"/books",
 "items":[{"name":"novel.txt","path":"/books/novel.txt","type":"file","size":10240,
           "protected":false,"pending":false},
          {"name":"sub","path":"/books/sub","type":"dir","size":0,
           "protected":false,"pending":false}],
 "truncated":false,"count":2}
```
- `items` 为 SD 枚举顺序（未排序，**客户端自行排序**——目录优先+名称升序）
- `protected`: 受保护路径（§6）；`pending`: 上传临时文件（.uploading）

### GET /api/stat?path=
```json
{"ok":true,"name":"test.txt","type":"file","size":10240,
 "path":"/books/test.txt","protected":false,"pending":false}
```

### GET /api/capacity
```json
{"ok":true,"total":30979129344,"used":57969760,"free":30921159584}
```
- total = 卡直读 sectorCount×512（可靠）；used = 目录遍历求和（文件真实字节, FAT 开销未计入）；
  会话启动预计算缓存, 秒回

### GET /api/search?q=武炼&path=/&depth=4
```json
{"ok":true,"query":"武炼","path":"/",
 "items":[{"name":"武炼巅峰.txt","path":"/武炼巅峰.txt","type":"file","size":57969760,
           "protected":false,"pending":false}],
 "searched":254,"found":1,"truncated":false}
```
- 名称包含匹配（忽略大小写）；`depth` 钳制 1..8；`found`=匹配数, `searched`=扫描数

### POST /api/mkdir?path=/books/new
`201` → `{"ok":true}`；已存在 → `409 exists`

### POST /api/delete?path=
`200` → `{"ok":true}`；目录非空 → `409 not_empty`；受保护 → `403 protected`

### POST /api/rename?path=/books/a.txt&name=b.txt
同目录改名；目标扩展名受保护 → `403`；源不存在 → `404`

### POST /api/move?path=/books/a.txt&dest=/docs
跨目录；`dest` 必须是已存在目录；防环（dest 是 path 自身/子孙）→ `409 invalid_move`

## 4. 下载与 Range

```
GET /api/download?path=/books/novel.txt
```
- 无 Range → `200` + `Accept-Ranges: bytes` + `Content-Disposition: attachment;
  filename="download"; filename*=UTF-8''<percent编码名>`
- `Range: bytes=s-e` / `bytes=s-` → `206` + `Content-Range: bytes s-e/size` + 精确 Content-Length
- **不支持**（→ `416 invalid_range` + `Content-Range: bytes */size`）：
  - suffix `bytes=-N`（v1 不实现）
  - 多段 `bytes=a-b,c-d`（v1 不实现 multipart/byteranges）
  - 越界（s ≥ size）或 e < s
- 传输 4KB 块流式 + 喂狗；`.uploading` 文件 → `403 upload_in_progress`
- 目录 → `400 invalid_path`；不存在 → `404`

## 5. 上传（v1 = 完整重传 + 服务端跳过前缀；字节级增量续传为 v2）

```
POST /api/upload?path=/books       （multipart/form-data; filename = 最终文件名）
X-File-Size: <文件总字节>            （必填, 用于校验）
X-Resume: 1                          （可选, 续传）
X-Resume-Offset: <提示>              （可选, 与 .uploading 实际尺寸窗口差 ≤256KB）
```

**状态机（顺序固定）**：
1. 规范化路径 + 净化文件名（取 basename, 拒 `..`/分隔符/控制符/超长）
2. 最终目标受保护（含扩展名规则）→ `403 protected`
3. 最终文件已存在 → `409 exists`（v1 禁覆盖, 无 overwrite 选项）
4. X-Resume: 校验 `.uploading` 实际尺寸与提示偏移窗口（≤256KB）→ 按**实际尺寸**跳过前缀；
   不符 → `409 resume_mismatch` + `{"serverOffset":N}` 供重试
5. 空间不足/写失败 → `507 insufficient_storage`（`.uploading` 保留供重试）
6. 完成: 实收 == X-File-Size → 内部 rename 提交 → `201 {"ok":true,"size":N}`
   不符 → `400 upload_size_mismatch`（`.uploading` 保留）

**`.uploading` 临时文件规则**：
- 可删除（/api/delete）；**不可** rename/move/download（`403`）
- 中断（断连/断电）后残留：会话内保留供 X-Resume；**新会话启动自动清理**（启动扫描删除）
- `GET /api/upload-status?path=&name=` → `{"ok":true,"exists":bool,"size":N}` 供恢复判断

**断连续传流程（客户端）**：
1. 上传中断 → 查询 upload-status 得 `size`
2. 重发完整文件 + `X-Resume:1` + `X-Resume-Offset:size` → 服务端按实际尺寸跳过前缀 → 201
3. 若 `409 resume_mismatch` → 用响应 `serverOffset` 重试一次

**限制**：单并发传输（传输中其他变更端点 → `409 busy`；服务器为单客户端模型）。

## 6. 保护规则（禁删/禁改/禁移动/禁覆盖上传；读取不限）

- 系统目录：`/.tiemereader` 及其子孙（目录前缀边界, `/.tiemereader_backup` 不误伤）
- 索引/数据扩展名（精确匹配, 大小写不敏感）：
  `.i1 .z1 .i1p .v1 .vz1 .v1p .bm .bmt .i2 .z2`
  （"test.i1.bak" 扩展名是 .bak → 不保护）
- `.uploading`：可删, 不可改名/移动/下载

## 7. curl 示例

```bash
# 列表（编码路径）
curl "http://192.168.4.1/api/files?path=%2Fbooks"
# 下载 + Range
curl -r 0-1023 "http://192.168.4.1/api/download?path=%2Fbooks%2Fnovel.txt" -o part.bin
# 上传（X-File-Size 必填）
curl -X POST "http://192.168.4.1/api/upload?path=%2Fbooks" \
  -H "X-File-Size: 1048576" \
  -F "file=@novel.txt"
# 变更操作带密码
curl -X POST "http://192.168.4.1/api/mkdir?path=%2Fbooks%2Fnew"
```

## 8. 性能基线（S4.5 分层实测, 10MB, web_test STA）

| 段 | 速率 | 说明 |
|---|---|---|
| SD→RAM | 1.02 MB/s | SD 读非瓶颈 |
| RAM→SD | 0.95 MB/s | SD 写非瓶颈 |
| RAM→HTTP | 0.21 MB/s | **网络 TX 是墙**（ESP TCP 栈） |
| SD→HTTP（下载） | 0.23 MB/s | 网络受限 |
| HTTP→SD（上传） | 0.14 MB/s | 网络受限 |

- P0 基线（上传 ≥100KB/s, 下载 ≥150KB/s）**达标**；P1（200/300KB/s）未达标；
  P2（500KB/s/1MB/s）为优化项。**不因追速度牺牲稳定性**；AP 模式吞吐另行实测。
- 传输缓冲 4KB 动态分配（空闲不占 RAM）；已关 Nagle。

## 9. 客户端接入指南（Android / Legado）

1. 发现：**配网/管理热点固定 `http://192.168.4.1`**；STA 模式为设备**静态 `192.168.0.100`**（设备连路由器时；
   该地址由 `wifiManagerStartSta()` 注入，可用 LittleFS 根 `/sync_bind.dat:dev_ip=` 覆盖，`dev_ip=` 置空串回落 DHCP）。
   注：**本项目从未实现 mDNS**（历史文档曾写"经 mDNS/手动 IP"，与实现不符，2026-09-13 修正）；
   进度同步的 UDP 发现（`8390`，`LUMIDISC`/`LUMIWHO`/`LUMIBIND`）是**进度同步专用**，文件管理 API 不走它。
2. 启动：`GET /api/status` 检查 `apiVersion==1` 与 `sdMounted`
3. 浏览：`GET /api/files`（客户端排序）；容量 `GET /api/capacity`
4. 上传：multipart + X-File-Size；中断恢复见 §5 流程；进度用 XHR/OkHttp 上传监听
5. 下载：OkHttp/Range 断点（§4）；文件名取 `Content-Disposition filename*`
6. 变更操作无需凭据（免鉴权, 见 §1; 2026-09-12 修订）
7. 错误处理：按 §1 错误码枚举映射 UI 文案
8. 读开放/变更必验：**不要把密码存明文**（v1 为局域网明文 HTTP, v2 计划 Bearer/HTTPS）

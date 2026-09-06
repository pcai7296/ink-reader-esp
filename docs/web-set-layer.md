# web-set-layer.md — 管理设置页(/set)「不可设置」项 Web 层落地（2026-09）

> 权威基准：本文件记录 `data/set.htm`（根页 `/` 与 `/set` 共用, 由 `wifi_manager.cpp::handleSetStatic()` 服务）
> 中 5 个原标注「不可设置」项改造成 **Web 层可设置（保存/持久化/回显）** 的契约。
> 屏幕端真实行为（时钟样式/倒计时/B粉/历史展示/强制校准策略/补偿换算）**不在本次范围**, 属后续战役（见 §4）。

## 1. 五个设置项 ↔ key ↔ EEPROM 字段

| UI 项 | /settings POST key | SettingsConfig 字段(232+, v3) | 值域/默认 | 回显 JSON key |
|---|---|---|---|---|
| 历史记录 | `history` | `historyEnabled` u8 | 0=关 1=开, 默认 1 | `history` |
| 时钟强制校准 | `clockCalibrationState` | `clockCalibrationState` u8 | 0=关 1=开, 默认 1 | `clockCalibrationState` |
| 时钟类型 | `clockMod` | `clockMod` u8 | 0=简洁 1=精美, 默认 0 | `clockMod` |
| 时钟补偿 | `clockCompensate` | `clockCompensate` i16 | clamp [-32767,32767], 默认 0 | `clockCompensate` |
| 多功能输入框 | `inAWord` | 独立 EEPROM 块 `INAWORD@600` | UTF-8 ≤63B, 空=一言模式 | `inAWord` |

- 解析/钳制在 `handleSettingsSave()`; 回显在 `handleSettingsJson()`; 便捷读写 `settingsGet/Set*`（wifi_manager.h/.cpp）。
- 保存成功走既有 `webNotifyAdd` 墨水屏提示（SETTINGS_WEB_SAVE 同链路）。

## 2. EEPROM 布局（改动后）

- `SettingsConfig` @ 232：尾部追加 4 字段（reserved[4] 之后, checksum 之后不参与校验）。
  sizeof 60→68 → 232..300，`static_assert(232+sizeof<=360)` 保留通过。
- **version 门控迁移**: 数据版本 1/2/3 均合法。
  - 写盘一律 v3（saveSettingsConfig）。
  - 读到 ≤v2（旧固件无新字段）→ 新 4 字段按默认（防 0xFF 残留被当成真实值, 如补偿显示 -1）；
  - v3 起信任字段值；越界仍由 `settingsFillDefaults()` 兜底（history/ccs/clockMod>1→默认; clockCompensate==-32768 哨兵→0, setter 已排除该值）。
- **InAWord 独立块** @ 600（避开 Settings 300/WebDAV 360/Target 470/Admin 520..~592）：
  `InAWordConfig { magic 'INAW' u32; text[64]; checksum u16 }`, static_assert 不重叠/不越界。
  读: magic+checksum 坏 → 空文本（不写回）。写: `settingsSetInAWord()`。

## 3. InAWord 保存净化规则（防 /settings.json 裸拼接断引号）

`sanitizeInAWord()`:
- 剔除 `"`、`\` 与全部控制字符(<0x20)；
- UTF-8 感知整字截断: 非完整序列字节跳过、末尾放不下整字符则截断（不劈开多字节）；
- 落盘 ≤63B NUL 结尾；空串=一言模式。
- 回显时 /settings.json 以原始拼接内联（内容已净化, 无引号/反斜杠/换行）。

## 4. 屏幕端后续战役（不在本次范围, 实现前需先逆 A7 定语义）

- 时钟类型: 简洁(大时间小温湿度无一言) / 精美(中置时间大温湿度+一言自定义), 倒计时+B粉两种风格均支持
- InAWord 语义: 一言模式 / 自定义句(≤21 汉字) / `倒+yyyyMMdd+事件` 倒计时 / `B粉+UID` B站粉丝 / 文本「重置系统」=恢复出厂
- 历史记录开关的屏幕端效果（主页最近阅读记录/展示）
- 时钟强制校准: 每日 23:30 静默联网校准; 失败策略（停机休眠 vs 不停机）——须对照 A7 实际行为校准文案
- 时钟补偿: 单位与换算公式（ppm? 秒/天?）——现仅存原始 int16
- 上述任一项落地时: 反向验证 A7 默认值, 若与本文件默认不同仅改 defaults 一行 + 文档

## 5. 相关文件

- `wifi_manager.h`: SettingsConfig 尾部 4 字段 + 8 个便捷读写声明 + InAWord 读写声明
- `wifi_manager.cpp`: INAWORD 常量/struct/static_assert; settingsFillDefaults; load/saveSettingsConfig(v3); getter/setter; handleSettingsSave 5 key; handleSettingsJson 5 key
- `data/set.htm`: 5 行控件化（checkbox/select/number/textarea）+「仅存值」warn 标注 + 文案更新（面板 v2）
- 页面 UI 语义: 标 `tag warn`「仅存值」= 可保存/持久化/回显; 屏幕端行为随后续固件版本开放（页首/页脚说明一致）

## 7. 屏幕端实装状态（2026-09，官方 A7 语义落地）

| 项 | 状态 | 实现要点 |
|---|---|---|
| 时钟类型 clockMod | ✅ 已实装(待目测微调坐标) | `renderClockPage` 双风格: 简洁=大数码管/去一言/小温湿度; 精美=新布局/大温湿度(7段小号数字)/一言·自定义句; 倒计时/B粉两风格显示; 伪装恒简洁; 一言仅精美拉取(fetchHitokotoFlow 门控) |
| 一言 | ✅ | 迁移入精美; 简洁不拉取不显示 (hitokotoEnabled 仍为总开关) |
| 倒计时 | ✅ | 文本`倒yyyymmdd事件`现分类现解析(Hinnant 日期差), 距X还有N天 |
| B粉 | ✅ | `bili_fans.h/.cpp` 拉 api.bilibili.com/x/relation/stat; <1万原值/≥1万一位小数W; 会话内缓存 |
| 重置系统 | ✅ | InAWord 恰为"重置系统"→ 进时钟页恢复全部默认并重启(每开机一次) |
| 时钟补偿 | ✅ 已实装(待趋势验证) | `clockManagerCompTick` 每真实分钟结算(软件钟=显示扣除累积 ms/1000; 芯片在场=每满±1000ms 直接回拨/拨快芯片秒); 单位≈每真实分钟修正 ms("快加慢减", 官方公式输入) |
| 23:30 静默校准 | ✅ 已实装(待真机窗口验证) | `clockManagerSilentCalTick`: 23:30-23:44 + 凭据 + 时间有效 + 当日一次; 成功写芯片/EEPROM; 失败: 强制校准开(默认)→进深睡 / 关→不睡次日再试 |
| WiFi 历史 history | ✅ 已实装 | LittleFS `/wifi_hist.dat`("ssid|pass", 上限8, 去重, ssid 更新置顶); `/wifi_hist` GET (开关关→空); handleSave 钩子; set.htm「历史网络」下拉回选 |
| set.htm 标注 | ✅ 已去除 | 五处「仅存值」warn 已移除, 文案更新至官方语义(面板 v3) |

已知取舍/待验证: 1) 补偿深睡后软件路径累积清零(芯片路径持久, 推荐芯片); 深睡分钟后大跳不补偿。2) 静默校准需设备当时空闲未深睡(本机 5 分钟自动深睡, 23:30 常在睡眠 → 以唤醒后次日/当日窗口为准)。3) 两风格布局坐标为"合理近似", 待实机目测微调。4) 页面切换 history/clockMod 等需刷新或重新进页面生效。

## 6. 实测记录（2026-09-06）

- 编译: 普通变体 + `BOOT_AP_MODE=1` 变体均过; 静态 RAM 66,192B（+~172B）; IRAM/flash 余量不变。
- 设备烧录: 固件 @0x0 + LittleFS 镜像 @0x200000（2072576B=mklittlefs -b 8192 -p 256 -s 2072576, 与 `_FS_start=0x40400000` 布局一致）, hash 校验通过。
  - ⚠️ LittleFS 整片重写已清空设备内 `/stats/*` 阅读统计与 `/fslist/*` SD 缓存（缓存自动重建; 统计归零）;
    刷写前已整片读回备份 `build/fs_backup_20260906.bin`（0x200000..0x3FA000）。
- 验收（HTTP 于 AP 192.168.4.1）: 默认回显 history=1/clockCalibrationState=1/clockMod=0/clockCompensate=0/inAWord="" ✓;
  5 项 POST 保存→回显一致 ✓; inAWord 含 `"` `\` CRLF → 净化后 JSON 不裂 ✓; clockCompensate=99999 → 钳 32767 ✓;
  **硬复位后全部值仍在（EEPROM v3 持久化）** ✓; 既有 14 字段（clockFormat 等）回归 ✓; /set 页无 mock 残留、/fs/edit 新版 manager 正常 ✓。
- 设备当前为 BOOT_AP 验收固件; 页面新版已上设备; 测试值已恢复默认。
- 注意: arduino-cli `--build-path` 重建会清空目标目录——曾存放 build_bsln_bootap 内的临时辅助脚本(_peek.py 等, gitignored)会被删, 需要时重建。

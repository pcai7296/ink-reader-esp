# AGENTS.md — ink-reader-esp

**Active firmware workspace.** 复刻 V14 xz014 固件的文件管理器 + TXT 阅读器。

## OVERVIEW
ESP8266 Arduino 固件，主控 ESP‑12F，2.9" SSD1680 墨水屏横放（逻辑 296×128）。核心功能：SD 文件浏览、TXT 阅读（索引/翻页/记忆/章节）、电池、时钟（BL8025T）、配网。

## STRUCTURE
```
ink-reader-esp/
├── ink-reader-esp.ino          # 主固件 (3480+ 行, APP_* 状态机)
├── epd_290a.h / .cpp         # EPD 驱动 (296×128 SSD1680, 全刷/局刷/休眠)
├── rot_map.h                 # 四向旋转坐标规范 (mapFbRot 纯函数, 唯一权威)
├── wifi_manager.h / .cpp     # 时钟/配网/天气配置/OTA/Web管理页
├── weather_data.h / .cpp     # 天气数据层 (心知天气 API 解析 + HTTP 获取)
├── hitokoto.h / .cpp         # 一言数据层 (v1.hitokoto.cn 解析 + HTTP 获取)
├── bmp_show.h / .cpp         # SD 卡 BMP 图片显示 (1/4/8/16/24 位深)
├── file_api.h / .cpp         # HTTP 文件 API 路由 (/api/status|files|upload|download|...)
├── file_api_fs.h / .cpp      # 文件 API 上传状态墨水屏回调 + 传输缓冲
├── sd_file_ops.h / .cpp      # SD 操作层 (目录遍历/删除/搜索/重命名)
├── sd_path.h / .cpp          # 纯函数路径安全/保护 (isProtectedPath)
├── fs_cache.h / .cpp         # SD 目录树 → LittleFS 缓存
├── progress_sync.h / .cpp    # 阅读进度同步 (LUMI1 协议, UDP 发现)
├── progress_lumi.h / .cpp    # LUMI1 编解码 + SHA-1 文件指纹
├── reader_utils.h / .cpp     # 阅读器工具函数
├── stats.h / .cpp            # 统计/调试
├── globals.h                 # 全局常量/宏/类型定义
├── file_list.h               # 文件列表数据结构
├── fb_gfx.h                  # 帧缓冲图形原语
├── weather_icons.h           # 6 个 24×24 天气图标 (PIL 生成)
├── weather_small_icons.h     # 小天气图标
├── nav_icons.h               # 导航图标
├── rot_map.h                 # 旋转坐标映射
├── gb2312_unicode.h / .c     # (遗留, 固件已不再引用)
├── font16_cn.h               # (遗留, 中文渲染用 u8g2 内置字库)
├── font16x16.h               # Tahoma 16px ASCII
├── font8x8.h                 # 小 ASCII 8px
├── make_font_cn.py           # 中文字体生成 (PIL)
├── make_font16.py            # ASCII 字体生成
├── make_weather_icons.py     # 天气图标生成
├── make_nav_icons.py         # 导航图标生成
├── make_small_icons.py       # 小图标生成
├── docs/                     # 协议/设计文档 (file-api.md, progress-lumi1.md)
├── data/                     # LittleFS 数据 (manager.htm — Web 管理页)
├── libraries/                # 本地库副本 (U8g2_for_Adafruit_GFX 等)
└── build/                    # 编译输出 (ink-reader-esp.ino.bin)
```

## WHERE TO LOOK
| Task | File | Notes |
|------|------|-------|
| 主页/文件列表 | `ink-reader-esp.ino` §renderHome/renderAll/renderListRow | APP_HOME/APP_BROWSER |
| TXT 阅读 | `ink-reader-esp.ino` §TXT 阅读实现 | buildTxtIndex / 排版 / 翻页 |
| 章节目录 | `ink-reader-esp.ino` §章节 | chapterRows, ChapterRow |
| 按键 | `ink-reader-esp.ino` §按键状态机 | KEY2=GPIO0/KEY3=GPIO3 长按机 |
| 屏幕刷新 | `ink-reader-esp.ino` §refresh | display(displayPartial) |
| 电池 | `ink-reader-esp.ino` §电池电量 | GPIO12 采样 + SD MISO 冲突 |
| 时钟 | `ink-reader-esp.ino` §renderClockPage + `wifi_manager.cpp` | BL8025T 探测 |
| 天气 | `ink-reader-esp.ino` §天气页面 + `weather_data.cpp` | APP_WEATHER; 心知天气 API |
| 一言 | `ink-reader-esp.ino` §fetchHitokotoFlow + `hitokoto.cpp` | 时钟页; v1.hitokoto.cn, 2s 超时 |
| 配网 | `wifi_manager.cpp` | wifiManagerBegin; 端点 / /status /info /settings /wifi /clear |
| 天气配置 | `wifi_manager.cpp` §loadWeatherConfig/saveWeatherConfig | EEPROM 160-232 |
| 设置页 | `ink-reader-esp.ino` §设置页面 + `wifi_manager.cpp` §设备设置 | APP_SETTINGS; EEPROM 232-244; 4 项: 时钟格式/时区/一言/恢复默认 |
| Web 设置层(v3) | `wifi_manager.cpp` + `data/set.htm` | 原 5 个「不可设置」项已 Web 层可设置(history/clockCalibrationState/clockMod/clockCompensate/inAWord); SettingsConfig 232..300 + INAWORD@600; 契约与屏幕端后续战役见 `docs/web-set-layer.md`; 页面「仅存值」=只持久化, 设备端随后续固件 |
| BMP 图片 | `bmp_show.cpp` + `ink-reader-esp.ino` §showBmpFile | APP_BMP=9; 文件管理器打开 .bmp 全屏 |
| OTA | `wifi_manager.cpp` (ESP8266HTTPUpdateServer) | /update 端点, admin/333333 |
| 中文字体数据 | `u8g2Fonts` + `chinese_gb2312` 字库 | 253KB flash, UTF-8 输入 |
| 索引格式 | `ink-reader-esp.ino` §formatIndexNumber | 8 字节 ASCII 补零 |
| 调试 | `ink-reader-esp.ino` §diagLog/diagFlushSd | DIAG_SD=1 时写 debug_trace.log |

## CONVENTIONS
- **UTF-8 全链路**: 全文本 UTF-8 → U8g2_for_Adafruit_GFX + `u8g2Fonts` 渲染；不混用 GBK
- **双缓冲**: fb 保持物理 128×296，`setPix(x,y)` → `(col=y, row=295−x)`
- **部分刷新优先**: 局刷覆盖列表/菜单；全刷只用于进入新页面
- **看门狗**: `ESP.wdtFeed()` 在索引循环、翻页循环、SD 遍历中必须调用
- **SD/SPI 恢复**: 每次 SD 操作前 `SPI.begin()` + 恢复 CS 高
- **文件名截断**: utf8Truncate 保扩展名（splitNameExt）
- **目录排序**: 文件夹优先 + 名称升序 strcmp
- **调试**: `traceFmt`/`traceFmtLevel` 走 diagLog；DIAG_SERIAL=1 时 Serial 输出

## ANTI-PATTERNS (THIS PROJECT)
- **长期禁用 WDT**: `ESP.wdtDisable()` 后必须 `ESP.wdtEnable(8000)`；否则每 9 秒重启
- **KEY3 时开 Serial 监视**: GPIO3=RX，按下会往串口灌垃圾
- **KEY2 直接 digitalRead**: 与 DC 共享，必须先 `pinMode(INPUT_PULLUP)` 读再恢复
- **全刷做小交互**: 翻页/菜单/错误提示必须部分刷新
- **忽略 SD CS 恢复**: SD 与 EPD 共享 SPI，CS 不恢复会导致 EPD 花屏
- **`catch {}` 空捕获 / `@ts-ignore` / `as any`**: 禁用
- **`String` 大循环分配**: TXT 索引循环内避免 String 拼接；用 `rows[line] +=` 谨慎

## UNIQUE STYLES
- `drawTextUTF8(x, y, s, maxW, black)` — y 是基线，实际显示 y+13
- `utf8Truncate` 双边界 (容量 + 像素宽度)
- `formatIndexNumber` 用 `%08lu` 写 8 字节十进制 ASCII
- `KState` 结构 + `scanKey()` 实现短按/长按机
- `isChapterTitle` 用 GB2312 字节对直接匹配（不依赖 UTF-8 解码）
- `txtCharWidth` 查表 ASCII 字符宽度（不是等宽）

## COMMANDS
```bash
# ⚠️ 必须 --build-path 指向项目 build 目录，否则 bin 输出到临时目录（曾烧录旧 bin 误判修复无效）
arduino-cli compile --fqbn esp8266:esp8266:d1_mini --libraries libraries --build-path "J:\code\esp8266\ink-reader-esp\build" ink-reader-esp.ino
# 烧录前核对 build\ink-reader-esp.ino.bin 时间戳（bin 名是 ink-reader-esp.ino.bin，不是 .d1_mini.bin）
esptool.py --port COM20 --baud 460800 write_flash 0x0 "J:\code\esp8266\ink-reader-esp\build\ink-reader-esp.ino.bin"
# ⭐ 一体化工具(勿重建, 详见根 AGENTS.md): 条件编译+烧录+串口监听+自动释放占口+日志递增
python esp_dev.py                          # 全流程(无宏/普通版)
python esp_dev.py --boot-ap                # = BOOT_AP_MODE=1 编译并烧录监听
python esp_dev.py -D SERIAL_REMOTE=1 --steps build
python esp_dev.py --fs 0x200000=build/data.littlefs.bin   # 固件+数据一起烧
python esp_dev.py --steps monitor          # 只监听(日志 serial_logs/log_N.txt 自动递增, 断线自连)
python esp_dev.py --dry-run --boot-ap      # 打印命令不执行
```

## NOTES
- IRAM ~60KB/64KB（当前 93%），慎用 IRAM 变量
- RAM 64% 已用（**51908**/80192, 2026-09-12 审查轮 #2 后; DATA 1736+RODATA 9868+BSS 40304），自由池 ~28KB；新增大缓冲优先 malloc 请求级/懒分配而非静态 BSS（本轮两处"栈→static"是为救深链栈峰值，代价即 +820B BSS，见审查轮 #2 ⑥）
- **flash 余量 ≈58KB**（IROM 960532/1048576, 91%）——PSTR 扫荡把 8.3KB 从 RAM 换到 flash 后已不宽裕，新功能先估 flash
- `fb` 帧缓冲 4736 字节 (128×296/8; 2026-09-12 勘误, 原记 4608)
- `diagRing` 24 条环形日志
- 小说索引构建参考 `J:\code\esp8266\legacy\archive\test\` 的 `.i1`/`.z1` 样例
- 排版算法已 PC 端 Python 模拟验证 100%（`simulate_index.py`）
- ✅ **UTF-8 全链路已修复**（原乱码根因：GB2312 分支拦截 UTF-8 首字节 + 2 字节查表）：`buildTxtIndex`/`readTxtPage`/`drawReaderLine`/`isChapterTitle`/`normalizeReaderLines` 全部对齐 PC 模拟器 `sim_engine.py`（b==0xE0 读 2 字节、ch_px=14）
- **索引构建抗打断（官方 A7 同款）**：无 `.i1b` 断点文件——页表即断点。`记录[N-1]=txt大小` 是完整性标记；重启检测到末记录≠txt大小 → 从页表末条页首偏移续扫（`beginResumeIndexBuildFromPartial`），最多重扫一页；重扫页章节经 `resumeChapterSeed` 去重
- **构建中实时保存进度（对齐官方 A7 边读边建可存进度）**：双句柄写 `.i1`（"r+" 写记录[0] + 追加句柄）有概率破坏 FAT 引发 SD 卸载（实测），所以构建中进度改写入**独立 sidecar** `<索引名>p`（如 `小说.i1p`，8 字节记录[0] 同格式）——不同文件，无双句柄冲突；`finishTxtIndexBuild` 合并回 `.i1` 记录[0] 后删除 sidecar；构建中断（掉电/休眠/重启）后 `startTxtReader` 优先读 sidecar 恢复阅读位置，`beginTxtIndexBuild`（全新构建）清旧 sidecar；**主页 `loadRecentReadSummary` 同样优先读 sidecar**（否则构建中断后主页显示第一页、阅读器却恢复正确进度——已修）；阅读菜单构建中显示"索引建立中 已建n页"（对齐 A7 菜单看构建进度）；重建入口带确认框"真的要重建吗！长按重建，短按退出"（对齐 A7）
- **⛔ 时间链路（用户重要决策，必须遵守）**：时间持久化**优先写外挂 BL8025T**；**BL8025T 不在线时降级写 EEPROM**（flash，KEY1 复位不清——用户担心的"内部RTC/内存"是 rtcUserMemory 已避开）。**当前状态（2026-08-26 换机后）：本机外挂 BL8025T 正常（用户实测）**——开机 `CLOCK_8025T_READ` 从芯片恢复时间、NTP 成功后 `CLOCK_8025T_SAVE` 写芯片；探测已支持双引脚（13/14 V14 源码优先、4/5 ESP12F 默认），读法为先写寄存器地址 0x00（重复 START）再读（用户资料），写入含 Control Register=0x00（24 小时制）。**历史（旧机器 2026-08-23 实测）**：BL8025T 双引脚扫描（13/14 + 4/5）均无 I2C 设备（`CLOCK_8025T_DIAG reqN=0`）——当时芯片硬件离线（未供电/虚焊/损坏），软件无法解决；EEPROM 降级兜底（`CLOCK_EEPROM_SAVE epoch=.. savedAt=..` + 开机 `loadPersistedClock` 恢复），无 RTC 时时间靠 ESP8266 内部时钟漂移，长时间会不准。**EEPROM 降级逻辑保留为兜底：BL8025T 不在线时自动生效，无需人工干预**
- **开机分流（KEY1 复位后 1 秒 KEY3 检测窗口 + 恢复免刷，2026-08-25 重构）**：①KEY3 窗口**并入最近阅读页表扫描**——窗口起点提前到 `loadRecentReadSummary` 之前，扫描（0~3.2s）期间每 512B 块轮询一次 `readKey3`，扫描后补足到最短 1s；2 次连续低 ≈10ms 防抖（对齐 `KEY_DEBOUNCE_MS`）；窗口内按过 KEY3 → **提前短路直接一次全刷回首页**（跳过恢复渲染，避免"先重刷阅读界面再全刷首页"的 2×刷新——用户实测冗余）；未按 → 按休眠记录恢复。②**启动恢复刷新策略（优化④）**：e-ink 面板双稳态，任意复位/断电都物理保留复位前画面——**不能按复位原因判定**：本机 KEY1 复位上报 `REASON_DEFAULT_RST`（"Power On"，硬件差异），按 EXT_SYS_RST/深睡唤醒判定的原因门会让优化永远不生效（实测踩坑）。判定改为：有记录且模式=首页/阅读/浏览 → **阅读恢复=免刷**（`renderTxtPageNoRefresh`：仅渲染进帧缓冲不写屏，面板已显示同页；`fixedRefreshCount` 保持 0 → 首次翻页局刷写屏、第 6 次定次全刷自愈残影）、**首页/浏览=局刷**；强制全刷例外：其他模式兜底回首页（内容不同）、章节目录双渲染（先阅读页后目录）、低电休眠唤醒（整屏"电量过低"通知局刷擦除留整屏残影，开机测压 ≤3300mV 强制全刷）
- **文件管理器白名单（对齐 A7 过滤机制）**：A7 扩展名过滤表（RAM 0x3FFE8DB9：.i1/.i2/.z1/.z2）+ 文件 UI（"短按选择/长按确认"、"可用隐藏，共 N"、"文件过大>600K"）。复刻实现：`isWhitelistedFile`——文件只显示 `.txt`(阅读)/`.bmp`(图片)，其他格式一律不显示；目录始终显示；`isBlacklistedEntry` 补齐隐藏 `.i1/.i2/.z1/.z2/.v1/.vz1/.i1p/.v1p`（横/竖索引+章节+sidecar）+ 修正系统目录黑名单（`.tiemereader` 原来漏写为 `.timereader`）
- **阅读菜单对齐 A7（反编译确认 7 项 + 功能全实现）**：菜单 = 字体选择/退出/自动翻页/全刷间隔：/旋转/跳转/休眠，每项行内显示当前值（A7 菜单项数组 0x3FFF0984、12B/项×7 循环、无"你读了百分之xx"——复刻原底部那行是自创已移除）。功能：①自动翻页（档位 0/1/2/5/10/25，超上限提示"换页倍率过高"对齐 A7）；②全刷间隔 1..10 次可调（`fixedRefreshEvery` 替代原固定 FIXED_REFRESH_EVERY=6）；③跳转=**数字键盘输入页码**（详见下方"跳转数字键盘"条目；原"右调大/中调小"逐页调整已废弃——用户嫌慢）；④字体选择=自带/外部（外部暂不可用回落自带，对齐 A7"外部字体初始化失败/已使用自带字体"）；⑤旋转=**四向 0°/90°/180°/270° 弹窗选择**（菜单项打开"旋转方向"弹窗：中短上移/右短下移/右长确认/中长取消回菜单，**选中方案才切换**，选当前方向确认=仅关弹窗回正文；当前方向行右缘实心方块标记；`renderRotSelOverlay`/`applyReaderRotation`；`readerRot` 为唯一语义, 存储走 `storedToRot/rotToStored` 兼容编码）——**排版参数化**（`txtLineCount`/`txtLineWidth`：横类 90/270 8 行×283px / 竖类 0/180 18 行×118px）、**索引按几何类别分离**（竖类 .v1/.vz1，横类 .i1/.z1；翻转共用同类别索引, 秒开零重建）、**fb 方向映射**（`fbRot`：经 `mapFbRot` 四分支, 仅阅读页渲染时临时置位, 非阅读页恒 90）、菜单/跳转/同步弹窗跟随方向；章节目录/倍速弹窗固定横屏；⑥退出/休眠
- **跳转数字键盘（13 键, 2026-08-24 实现）**：`renderJumpOverlay`/`drawJumpKey`/`jumpCursor`/`jumpRejectMs`。①布局：横屏两行 6+6（`1-6` / `7 8 9 0 < 回车`）+ **取消**小键（回车正下方，36×22）；竖屏 3×3（`1-9`）+ 第 4 行 `0 < 回车` + 取消（竖屏键宽 36、弹窗 118×200 居中，逻辑高 296）。②交互：**光标移动设计（用户定稿）**：右键短按 = 下移 1 个、中键短按 = 上移 2 个（`+1` / `+11 mod 13`，与阅读菜单一致）；选中键下方画向下实心小三角；长按右键 = 执行（数字 0-9 末尾追加 / `<` 退格 / `回车` 跳转 / `取消` 回菜单）；长按中键 = 取消回菜单。③初始编辑值 = 当前页号；追加上限 = 总页数位数（如 140146 → 6 位），超位数拒绝并在页码行尾显示 `!` 1s（`jumpRejectMs`）；退格删空显示 `0/N页`，回车 `jumpToPage()` clamp 到 [1,总页数]（索引构建中 clamp 到已建页数）→ 写进度 → 全刷。④编辑/移光标全程局刷，仅回车全刷。⑤**坑：键宽必须 ≥ 文字宽 + 4px**（`drawTextUTF8` 的 maxW=键宽-4 经 `utf8Truncate` 按像素截断）——汉字 16px/字，"取消""回车" 32px → 键宽必须 ≥36px，否则只显示首字（实测 28px 键只显示"取"）；文字居中偏移 `(w-tw)/2` 可能为负 → 左对齐保护（`tx<x` 时 `tx=x`）
- **时间校准页与天气页（UI+逻辑对齐 A7 反编译）**：①校准页文案=A7（获取NTP时间/:成功/:失败/获取NTP时间失败，改用天气时间/手动跳过校准/此时 按下按键3 可跳过校准）；**逻辑同步**：NTP 超时不再直接 FAILED，新增 `CLOCK_WEATHER` 降级状态——请求心知 now.json 解析 `last_update` 作为时钟基准（`clockManagerTryWeatherTime`，civilToEpoch−时区偏移），失败才 FAILED；右键短按跳过在降级阶段同样生效。②天气获取步骤提示：`fetchWeather` 加 `onStep` 回调（0=实况/1=未来/2=生活指数），每个端点 HTTP 成功后更新 `wFetchStep` 并局刷（对齐 A7"获取天气实况数据/获取未来天气数据/获取生活指数"）；超时显示"* 连接超时 *"。③**天气壁纸背景**：天气页从 SD 卡加载 `天气壁纸N.bmp`（N=2+weatherIconIndex：晴→2 多云→3 阴→4 雨→5 雪/雾→6），复用 bmpShowFromSd 画入帧缓冲（只写非白像素，先 fillRect 白底）；**文件不存在则留空白底**。④主页天气摘要：SD 缓存 `/.tiemereader/weather.dat`（"天气名|温度"），重启后主页仍显示（A7 主页简洁天气信息）。⑤充电灯珠核查结论：主板红灯=TP4054 CHRG 硬件驱动 LED，**A7 固件无充电状态读取**（无 GPIO 读充电/无充电字符串/无闪电位图），MCU 读不到；闪电标为复刻自创（电压迟滞 3950/4050mV）
- **电量显示移植官方逻辑（A7 反编译 + V14 源码交叉验证）**：①采样对齐 A7——GPIO12 电池开关 + GPIO5 拉高 + **20 次** ADC 平均 + 关 + INPUT 防漏电（A7 反汇编 cc50 确认；原复刻 16 次）；换算 `sum×5607/(N×1024)` 与官方一致。②**百分比换官方 4 阶多项式**（V14 getBatVolBfb：`497.50976x⁴−7442.07254x³+41515.70648x²−102249.34377x+93770.99821`）+ A7 边界（<3.2V→0%、>4.2V→100%、负→3%）——原分段表 3.7V→40% 而官方曲线 3.7V→76.5%，整体偏低是"电量不准"根因。③**充电检测（闪电标）为复刻自创**：A7 固件反编译确认无充电检测（无闪电位图、无"充电"字符串、电池显示函数无充电分支）；isCharging 改为电压阈值+迟滞（≥4050mV 进入 / <3950mV 退出），TP4054 恒流充电早期电压低无法电压判定，如需插电即显示需硬件读 TP4054 CHRG 引脚
- **5 分钟自动休眠在构建期间失效的根因（已修）**：`notePhysicalKeyActivity` 原把"持续按压态"（`k2.down||k3.down`）也算活跃 → **电源噪声**（EPD 刷新/SD 写卡等大电流脉冲经电源耦合到 GPIO3=RX / GPIO0=DC）误判"一直按住" → 休眠计时被永久刷新 → 构建期间永不自动休眠。修复：休眠计时只认按键事件（短按/长按边沿，真实场景无超过 5 分钟的按住）；KEY 串口调试行改为仅按键事件打印（去掉无条件 250ms 轮询，减少串口对 GPIO3 的噪声耦合）；自动休眠触发打印 `SLEEP_AUTO idle=.. building=.. mode=..` 便于验证
- **按键电源噪声防抖（scanKey 内置）**：目标项目原无任何防抖（readKey2 仅 30µs 稳定延时、readKey3 直接读、scanKey 纯边沿）。已加 `KEY_DEBOUNCE_MS=10`：`KState` 增 `rawStable`/`rawChangeAt`，电平变化需持续 10ms 才进入状态机，瞬时噪声毛刺（EPD 大电流脉冲等）直接忽略；对交互无感（短按判定延迟 ≤10ms，长按 500ms 计时从确认按下起算）
- **FixedRefresh 定次刷新（对齐官方 DisplaySetup.ino）**：阅读翻页每 6 次局刷做一次**单次全刷**（直接显示本页；官方"黑+白两次清屏再局刷"≈3s 太慢，已改单次≈1.5s），防局刷残影；进入阅读/章节跳转全刷归零
- **索引页首偏移必须用真实字节计数**：`indexTaskStep` 块读缓冲（idxBuf 2048B）时 `File::position()` 只停在重填点（2048 对齐），用它记页首会把整张页表量化到 2048 边界 → 连续几条记录同偏移 → 翻页读同一内容"没反应"（串口 trace 实测：页2-6 全 offset=2048）。已改 `indexScanPos` 逐字节累计（`idxReadByte` 消费 +1，peek 不加），与 PC 模拟器/官方一致；`startTxtReader` 增页表单调性校验（记录[1..] 必须严格递增），旧对齐坏索引自动全量重建（不续建）
- **翻页卡死已修复**：`readTxtPage` SD 读失败立即 `break`（原 `c<0` 只记日志不退出，`available()` 卡真时死循环且每圈喂狗 → 按键永不响应，仅 KEY1 硬复位可恢复）
- **段首缩进**：2 个全角空格（28px=两字）；布局(索引)不含缩进，内容行 >264px 时不缩进防末字截断（原 7 半角空格=35px≈3字 且满行截断）
- **渲染已对齐官方机制**：全文本走 `U8g2_for_Adafruit_GFX` + `u8g2Fonts`（`drawTextUTF8`/`utf8Width`/`drawReaderLine`），中文字库 = u8g2 内置 `chinese_gb2312`（UTF-8 输入）；`font16_cn.h`/`gb2312_unicode` 位图链路已弃用，死代码已删
- **刷新断电策略（对齐官方"画完 display.powerOff()"）**：`epd.display`（全刷）末尾 `powerOff()`（SSD1680 0x02，保留 RAM 图像）；`displayPartial`（局刷）**先 `powerOn()` 再写 0x32 部分 LUT**（GxEPD2 时序）且**局刷后保持上电**——断电态写 LUT 不可靠，曾致连续翻页局刷无显示（只有紧跟全刷后的那次局刷有效）；休眠路径 `epd.sleep()` 断电，5 分钟自动休眠兜底省电
- 启动 Soft WDT 复位已修复：`loadRecentReadSummary` 顺序连续读 + 每 64 页喂狗 + 批量 read（《武炼巅峰》.i1 1.1MB / 142417 页）；2026-08-25 再改 **512B 块读（64 条/次）**——逐条 8B 读以 VFS 调用开销为主（~23µs/次，102k 条 ≈2.35s），块读全表扫 <0.5s（优化①；独立静态缓冲 `recScanBuf`，不共用 indexTaskStep 的 `idxBuf`，防破坏后台构建残留位置）；**启动页号复用（优化②）**：`loadRecentReadSummary` 算出的页号经 `gBootHintPage` 传 `startTxtReader`（读走即清零），`parsePageRecord(hint)==saved` 校验通过则跳过 17 次 seek 二分（≈1.1s），横竖屏索引不同/页表变更自动回退二分
- EEPROM 布局: WifiConfig 0-105 / CLOCK 112-125 / WeatherConfig 160-232 (magic 'WTHR') / SettingsConfig 232-244 (magic 'SET3', 含 portrait=阅读旋转兼容编码 0横/1竖/2横翻/3竖翻, checksum 后不参与校验, 旧数据 0/1 兼容) / WebdavConfig 256-470 (magic 'WDV2') / TargetConfig 470-508 (magic 'TGRT', 连接对象)
- **主页最近阅读 × 索引构建中（2026-09 修复）**：构建期/中断后 `.i1` 半截(缺尾部 size 标记)不得当"完整索引"→ `loadRecentReadSummary` 增判定：`recentReadBuilding` = sidecar 存在 ‖ 运行时正在构建同书 ‖ `.i1` 尾记录≠txt 大小；主页主卡在 `recentReadBuilding` 时**仍显示书名 + "第N页 构建中"**（页码来自 sidecar 偏移在已建页表内的命中，不显示误导性总页数/百分比；页码不即时刷新），构建完成且正停在主页时自动重载摘要并局刷主卡（`finishTxtIndexBuild` 尾部）。顺带修复：A) 大书构建期 SD 忙导致 recentread.dat/索引误打不开→误报"暂无阅读记录"，读/存在性判定加 `reinitSdBus` 重试；B) sidecar 分支扫描未 `seek(8)` 会把记录[0]=进度误当页2 → 页码错位（扫描前统一 `index.seek(8)`）；C) **APP_HOME 分支不喂 `indexTaskStep`** → 退出阅读器停在主页时后台构建停摆（与 closeTxtReader "后台继续"注释矛盾），现主页同样喂步进（delay 5/30 同其他模式）。实测用普通版固件（BOOT_AP 版开机直达配网页，测不到首页）
- **web 五项屏幕端实装（2026-09，官方 A7 语义，详见 docs/web-set-layer.md §7）**：`renderClockPage` 双风格(clockMod: 简洁=大数码管去一言/精美=新布局承载一言·自定义句; 倒计时`倒yyyymmdd事件`/`B粉UID` 两风格显示; 文本"重置系统"进时钟页全恢复重启); `bili_fans.h/.cpp`(B粉 HTTP); `clockManagerCompTick`(补偿≈每真实分钟修正 ms, 软件钟显示扣除/芯片在场每满±1000ms 回拨芯片秒); `clockManagerSilentCalTick`(每天 23:30 静默校准: 成功写芯片/EEPROM, 失败按强制校准开关 → 2=停机深睡/3=不睡次日; 主 loop 空闲界面调用); history=WiFi 历史(LittleFS `/wifi_hist.dat` "ssid|pass" 上限8 去重, `/wifi_hist` GET, handleSave 钩子, set.htm「历史网络」回选)。set.htm 五处「仅存值」标注已移除(面板 v3)。布局坐标为"合理近似"待实机目测微调; 补偿趋势与 23:30 窗口待真机验证。
- **阅读旋转方向全局持久**：`readerRot`（0/90/180/270）↔ SettingsConfig.portrait 兼容编码（`storedToRot` 非法值→90 旧默认横屏, `rotToStored` 保存）；开机 `setup` 恢复上次方向 → 文件管理器打开 TXT/主页最近阅读恢复/睡眠唤醒/重建索引全部按该方向（竖类 .v1 / 横类 .i1）；阅读菜单旋转后 `settingsSetPortrait(rotToStored(readerRot))` 保存；设置页"恢复默认"会清回横屏（下次开机生效）；`loadRecentReadSummary` 主页索引后缀按类别感知（修复前硬编码 .i1）
- **四向旋转坐标规范（唯一权威, 见 rot_map.h）**：物理 fb 恒 128 列 × 296 行（非 296×128！）；逻辑画布 rot 0/180=128×296（竖类）、90/270=296×128（横类）。映射：0° `px=x,py=y`；90° `px=y,py=295−x`（既有产线公式, 未变）；180° `px=127−x,py=295−y`；270° `px=127−y,py=x`。`mapFbRot(rot,x,y,&px,&py)` 输入为当前 rot 的逻辑画布坐标、输出恒物理 fb 坐标, 越界/非法 rot 返回 false 且不写 px/py（防御保险丝, 先于任何 fb 数组索引计算）。**旋转角必须用 uint16_t（270 超 uint8_t 上限, 截断为 14 —— rot_map_test 42 断言实测踩坑）**。`setPix` 的 default 分支仅防御非法 rot, 正常路径不得依赖。**UI 显示度数约定（用户定稿）：默认横屏(内部90°)=0°，显示角=(内部角−90+360)%360，顺时针递增；弹窗选项按显示度数升序 `rotSelTable={90,180,270,0}` → "0° 横屏/90° 竖翻/180° 横翻/270° 竖屏"；内部 rot 与存储编码不受影响**
- **标签系统（阅读菜单"标签"项，第7项/共10项）**：每书一份 `<书名>.bm`（txtPath 去扩展名+.bm，同目录同名，不同目录同名书互不影响），append-only 8字节ASCII**页首偏移**记录（同 .i1 记录[0] 格式，复用 formatIndexNumber），上限50，跨横竖屏方向共用；损坏(size%8!=0)读只认完整记录+trace BM_CORRUPT，追加前自愈重写。交互：菜单"标签"→子菜单弹窗 [标记本页][历史标记]（跟随 readerRot）；**标记本页**=追加 txtPageStart+局刷提示"已标记 标记N"/满50"标签已满"；**历史标记**=APP_MARKS 全屏列表（**固定横屏**，仿章节目录骨架：顶栏右上=选中序号/总条数(项维度)，底栏=p/P 页(页维度)），中短上移/右短下移跨页自动翻/中长回正文(局刷)/行内右长→操作框 [跳转][删除][取消]。跳转=offset<txtSize 校验(否则"标记失效")→findPageCeil(构建中 clamp txtIndexedPages)→写进度→全刷回正文——offset 恒来自 txtPageStart 必为 page-start，findPageCeil 仅复用既有定位。删除=两阶段防丢失：RAM 缓冲→.bmt 写+回读校验→rename 替换，兜底 RAM 直写+校验，全败提示"删除失败"；名称=位置编号删除后顺延，光标指顺延条+分页重算。睡眠降级：saveSleepRecord 将 APP_MARKS 快照为 APP_READER（唤醒回正文且方向/进度不变）。黑名单加 .bm/.bmt（仅浏览器展示受限）。数据函数 markPath/markCountRead/markAppend/markLoadPage/markDeleteOne/jumpToMark 位于 startTxtReader 之后。**坑（实测白屏根因）：列表/弹窗局刷后 SPI 总线在 EPD 侧，且 reinit 过总线后旧 File 句柄不可信 → 标签任何 SD 访问前必须先 `reinitSdBus`，并统一走 `markEnsureTxtFile()`（恢复总线+无条件重开 txtFile）；百分比用进入列表时的 markTxtSize 快照；跳转 findPageCeil==0 时提示"跳转失败"不静默兜底第1页（曾致 findPageCeil=0→兜底页1→读正文 TXT_READ_FAIL 全零行白屏）**
- **UI 方向两层分类**：Reader-Direction UI（阅读菜单/跳转键盘/进度同步弹窗/旋转弹窗/标签子菜单 —— 渲染前置 `fbRot = readerRot`）vs Fixed-Landscape UI（章节目录/倍速弹窗/文件管理器/时钟/天气/设置/BMP/**历史标记列表** —— 恒 90° 不置 fbRot）。新增 UI 时先归类再实现
- 天气夜间判断用 `localtime` 本地小时（configTime +8），勿用 UTC 公式
- 天气 KEY 存 EEPROM 明文，禁止写入 Serial/trace 调试输出
- **进度同步直连手机（LUMI1，阅读菜单"进度同步"项）**：手机=进度服务器（端口 8384 明文 HTTP），协议规范 `docs/progress-lumi1.md`——`GET /progress?file=<RFC3986>`→200+LUMI1/404；`PUT /progress`（body=LUMI1+`file=` 原始 UTF-8 文件名）→200/400；载荷必填 `ts/size/offset/pct`（offset=原始 TXT 字节偏移、编码无关；未知 key 忽略可扩展）。状态机 `progress_sync.cpp`：PREPARE→WIFI→[DISCOVER]→[WAIT_CLIENT]→CONNECT→GET→PARSE→COMPARE→[APPLY|UPLOAD]→FINISH（每阶段阻塞≤6s、循环喂狗）；比较页两键双方向=同步(手机→本地)/覆盖(本地→手机，二次确认)。手机端服务器=Legado 改造（`J:\code\Android\legado` 新包 `io.legado.app.esp`，自 Lumi_Books 移植）；**目标选择优先级：`/sync.cfg` 的 `ip=` → UDP 发现（LUMIDISC/LUMIACK :8390，≤2s 首合法 ACK）→ TargetConfig.staIp/apIp**；STA 模式走发现，AP 模式固定 192.168.0.100；跨版本文件（size 不同）按 pct 换算兜底。**（两端 txt 字节一致即可精确对齐；历史遗留的"手机端 181 字节水印"问题**用户已自行解决**，相关临时脚本 fix_wulian.py/diff_wulian.py 已删）
- **`/sync.cfg`（SD 根，可选）**：纯文本 `ssid=`/`password=`/`ip=`/`port=` 四行；存在且 ssid 非空 → 同步时直接 `WiFi.begin(ssid,pass)`（先 `WiFi.persistent(false)` 防写 flash 配网区），优先于 EEPROM 配网页凭据；`ip=` 时跳过 UDP 发现，`port=` 覆盖默认 8384（1..65535 纯数字，供联调/换端口，如 `port=38623`）；换网络（路由器/手机热点）改文件即可，不重烧、不动 EEPROM
- **v3 文件指纹检测（2026-08）**：`docs/progress-lumi1.md §3.4`——`fs`+`h0/h1/h2`（头/中/尾各 1KB 的 SHA-1，纯 C 实现 `progress_lumi.cpp::lumiSha1`（RFC 3174 无依赖，pc_test 可测），区域公式 `lumiFingerprintRegions`；**发送方语义**：指纹永远描述发送方文件，接收方对比自己文件；**三态** MATCH/UNKNOWN/MISMATCH（4 位掩码 bit0=size bit1=head bit2=middle bit3=tail，仅 MISMATCH 有诊断意义）；**原子组**：四字段全存在且合法才有效（半组/非法/重复/溢出 → 整组视为不存在）；256B 编码预算硬上限（整组省略禁截断）。ESP 端：PREPARE 生成 SyncSnapshot（复用已开 `txtFile` 句柄：size→三处读 1KB→SHA-1→progress，**任一步失败整组省略**；`extern File txtFile` 在 progress_sync.cpp）；UPLOAD 用 `lumiMakeEx` 携带指纹；PARSE 后三态比较（`gFileFpState`/`gFileMismatch`）；比较页 MISMATCH 黑底白字警告（"！！文件不一致：大小/头部/中部/尾部"替换标题行，⚠ 不在 GB2312 字库用 !!）；手机端 GET 响应附指纹、PUT 弹窗按位警告。跨平台一致性由 pc_test（70 断言）与 Android `LumiProgressCodecTest`（Knuth 序列同预期）锚定
- **⛔ 阅读架构（2026-09 定稿 → 同日按用户"抄官方"修正）**：**阅读数据源 = 当前介质**（官方 A7 `fsSetBySdState()` 同款：SD 启用→书/`.i1`/`.z1` 都在 SD，阅读直读 SD；内部模式→走 LittleFS）。实现：`readerFs()` = `browseFs()`；`readerBusReady(reason)` 在 SD 介质下 `reinitSdBus`（EPD/电池采样共用引脚），内部介质恒真；`SD.end()` 仅在**内部介质阅读**时执行（SD 介质阅读时 SD 是数据源必须在线）；`startTxtReader` 两种介质都直接可读。**不再有"SD 仅文件管理"拦截**（该拦截曾导致用户设备完全进不去书）。~~可选能力：`readerImportFromSd()`（>100MB 与容量前置校验 + 回读校验 + 旁带 `.i1/.z1` 复制）~~ → **该函数 2026-09-12 全库审查轮已删（死代码）**，当前没有"拷到内部"入口（docs §15 已标"已废弃"）。写入模式严格按 `fs::FS`：读 `"r"`/新建重写 `"w"`/**追加必须 `"a"`**（SD 的 `FILE_WRITE` 是追加语义，照搬会截断 `.i1/.z1/.bm`）。本轮稳定性/续航修复对两种介质同时生效。计划/验收见 `docs/reader-lfs-migration.md`（§16 为本次修正）。
- **WiFi 生命周期（P5, 2026-09）**：`wifiManagerRfOff(reason)` 统一关 RF（对齐官方 `WifiShutdown`：Clock_8025T:93 / DisplaySetup:194 / DisplayTxt:854）；调用点=时钟各终态、天气页退出、配网页退出、进入阅读、阅读循环兜底断言；日志 `RF_OFF reason=…`。疑似 6–8h vs 25h 续航差的主因（RF 常开 ~70–100mA）。
- **P3/P4 已实机验收（2026-09，T1=307KB 书）**：Web 文件管理 `POST /fs/edit` 上传 → `GET /fs/file?path=/T1_300k.txt` 校验 **307,356 B** 一致（确认落 LittleFS）；索引在 LittleFS 构建 `pages=681 chapters=53`；`AUTOTEST_FWD_DONE fwdOk=680 stoppedAtEnd=1 page=681 total=681`（整本零失败）+ `AUTOTEST_BACK_DONE backOk=225`；阅读期间**零 SD 访问**（无 `SD_REINIT`）、零 `PAGE_REC_*`/`READ_FAIL`/崩溃/WDT/重启，`rf=0`。细节见 `docs/reader-lfs-migration.md §14`。
- **迁移期踩坑（勿重犯）**：① `/fs/*` 原先**硬编码 SD**（上传写 SD），已改 `activeFileFs()` 跟随介质——否则"Web 上传进 LittleFS"根本走不通；② `BOOT_AP_MODE` 分支原先在"本地介质提前 return"之后 → sdEnabled=0/无卡时**永远进不了配网页**，已前移到分流之前（本地介质跳过 SD 目录缓存）；③ 自测/脚本阻塞在 setup 时必须**自己驱动 `indexTaskStep()`**，否则异步索引永不完成（现象 `total=1`、翻页全失败）；④ 阅读期写睡眠快照/休眠原先 `reinitSdBus(...)` → 破坏"零 SD 访问"，已移除；⑤ 验收编译开关：`-DREADER_AUTOTEST=1 -DFORCE_LOCAL_MEDIUM_TEST=1`，配网页部署再加 `-DBOOT_AP_MODE=1 -DWIFI_TEST_FORCE_AP=1`（强制热点、不连已保存 STA）。
- **2026-09-12 静态内存审计轮（P0-P5, 每批一 commit 可回退）**：RAM 69368→51088 (-18.3KB, 86%→63%), 自由池 10.8KB→~29KB。①P0 删只写不读死数据: diagRing(2×128 环形日志)/chapterRowOffsets/gOfsUpPhasePath+getter (-576B)；②P1 章节目录懒分配: chapterRows(408)+chapterPageOffsets(4KB) 合并一块 malloc——enterChapterList/唤醒恢复经 chapterEnsureBuffers() 分配, closeTxtReader/跳章回正文 chapterFreeBuffers() 释放, 失败降级=chapterPrevPage 走 seekChapterOffset 全扫兜底 (-4480B)；③P3 进度同步懒分配: 指纹 1KB 读块在 computeFileFingerprint 内即用即还, GET 响应 body 256B 改 gBodyBuf 在 SYNC_GET malloc/PARSE 消费完 free/Cancel 防御释放 (-1280B)；④P4 统计书表会话级: gBooks 864B 改 statsEnsureBooks/statsReleaseBooks——会话开始或统计页加载, 会话结束或离开统计页释放; 分配失败→gSessionBook=-1 只计全局, statsSave 书表未加载时不写防零表覆盖 (-856B)；⑤P2 Web scratch 共享竞技场: /fs 七组互斥缓冲(list 三件/edit 五件/file path/upload path/fm buf/api parent)并入 file_api_fs.cpp `char gWebArena[1456]` 固定槽位指针, 槽位偏移表见文件顶部注释, sizeof 引用改显式常量 (-2.8KB)；⑥P5 RODATA 扫荡: traceFmt/traceFmtLevel/debugFmt/debugLine/Serial.printf/Serial.println/snprintf/showMsg/showMiniPrompt/drawTextUTF8 的字面量 PSTR 化 (ESP8266 flash 直接映射, 普通 const char* 函数可读), RODATA 18164→9872 (-8.3KB RAM, flash +10KB)。新代码约定: 格式串/常量文案一律 PSTR()；大 scratch 先问"这个缓冲什么窗口用"再决定 静态/竞技场槽位/懒 malloc
- **2026-09-12 全库审查修复轮（要点存档）**：①HTTP 栈炸弹批量 heap 化——/api/upload-status 的 SdEntry、ofsDeleteRecursive/usedWalk/cleanupWalk/searchWalk 的每层 420B 路径缓冲（usedWalk/cleanupWalk 补深度上限 16；core 3.1.2 Dir::fileName() 返回 String，递归内每层存活 1 个属正常）；②上传三连修：done 回调 upReset 后读 gUp->actualSize 的 use-after-free（先拷出）、START calloc 失败后 WRITE/END 空解引用（加 !gUp 守卫）、补 UPLOAD_FILE_ABORTED 分支（断连后 gTransferActive 永久 true → 变更端点恒 409 死锁）；③自动翻页 25 档回绕到 0（原"超上限提示"让 25 档永久卡死无法关闭，单键循环下以回绕替代 A7 上限提示）；④finishTxtIndexBuild 收尾行上限 txtLineCount()-1（原硬编码 7 → 竖屏末页 9~17 行章节丢失）；⑤startTxtReader 有效索引分支二次打开 ready 判空（原 (0/8)-1 下溢 0xFFFFFFFF → 兜底走重建）、恢复偏移 ≥ txtFile.size() 作废（换小同名文件防"隐形阅读器"，对齐同步路径检查）、findPageByOffset/findPageCeil/offsetToPage 二分读回校验（read()=-1 垃圾不进搜索决策）；⑥组合键回首页对 APP_CLOCK_DISGUISE 失效（伪装契约=停用全部按键）；⑦索引构建步进统一提到 loop 前部（原设置/统计/时钟/天气/BMP/伪装页构建停摆；APP_NETWORK/APP_CLOCK_CONNECT 仍绝不喂——SD 扫描×WiFi 争堆）；⑧/api 变更端点补 fsCacheInvalidateDir（与 /fs/* 双轨一致，防 /fs/list 读旧快照）；fsCacheServeList 落地首行路径比对（原注释承诺读回校验但实现只跳过）+ 构建截断落 /fslist/_TRUNCATED 标记 + hasMore 只看窗口外实有项；⑨sdRename/sdMove 目标缓冲 560B（原 300/320 静默截断 → FAT 错名 rename 不可逆）；⑩bmp_show：块尾按 kBpp 判定重填+残余 memmove（24/16 位深 EOF 短读越界读栈）、SD 回退走 reinitSdBus + LittleFS.begin 只做一次（重复 begin 泄漏挂载结构）；⑪epd：EPD_DEBUG 默认 0（GPIO3=RX 与 KEY3 互扰）、sleep() 先 powerOff（局刷后带电进深睡）；⑫死代码删除 buildTxtIndex 同步版/readerImportFromSd；readLineCapped 限长行读（损坏 .z1 防 String 吞堆）；utf8ChopOne 砍尾不切多字节；BMP 退出不再 freeItemList（清窗口导致返回空列表）；globals.h AppMode 枚举补 APP_STATS=12；stats 写入去 remove→rename 掉电窗口
- **2026-09-12 审查修复轮 #2（用户自审 6 提交 → 交叉审查 → 4 项修复 + 文档纠偏）**：
  ①**组合键回首页漏收尾（本次修复）**：原 `if (appMode == APP_READER) closeTxtReader()` —— 从章节目录/历史标记按组合键回首页时不释放章节缓冲(4.5KB)、不 `progressFlushForce`、不关 `txtFile`/索引句柄、不 `statsOnSessionEnd`；且 READER 分支走 `closeTxtReader()` 会先全刷文件管理器再全刷首页（2×全刷）。改为抽出 `endReaderSessionNoRender()`（不渲染收尾）+ `isReaderFamilyMode(m)`（READER/CHAPTERS/MARKS 三者），组合键只做一次全刷回首页；收尾内顺带清会话级弹窗标记（readerMenuOpen/readerJumpOpen/readerRotSelOpen/readerSyncOpen/readerMarkMenuOpen/chapterSpeedPopup/markActionOpen——不清会让下次进书首个按键被当菜单键）并在同步状态机在跑时 `progressSyncCancel()`（否则 WiFi 常开 + `readerSyncOpen` 永久 true，因 `progressSyncLoop` 只在 APP_READER 分支喂）。实机验收 `combo_home_test.py`。
  ②**深链栈压力（本次修复）**：`/api/rename|move` 各压 `char target[560]`（只为查目标扩展名）→ 新纯函数 `isProtectedBaseName()`（只看 basename 扩展名 + `.tiemereader` 同名；目录部分调用方已查过 `isProtectedPath`），省 1120B 栈；`sdRename/sdMove` 的 `char dir/target[560]` 改 `static char sPathScratch[560]`（单线程顺序调用、互不嵌套）+ 显式长度校验（>541B 返回新错误码 `SD_INVALID_PATH` → HTTP 400 `invalid_path`，绝不静默截断）。上限推导：`normalizeApiPath` 出参 ≤299 + '/' + `sanitizeUploadName` ≤240 + NUL。
  ③**fs_cache 递归栈（本次修复）**：`fsCacheScanOne` 的 `nameEsc[260]` 改 static（只在"转义→printf"间存活，其间无递归；`child[180]` 必须是递归入参故留在栈上），3 层共省 ~780B 栈。
  ④**误提交清理（本次修复）**：`.zcode/plans/plan-sess-*.md`（agent 计划文件）`git rm --cached` + `.gitignore` 加 `.zcode/` `.omo/`。
  ⑤**文档纠偏**：§15 标"已废弃"、§16 与本文档阅读架构条目改述 `readerImportFromSd()` **已删**（原文仍写"可选能力存在"，与代码矛盾）。
  ⑥**代价与余量**：两处栈→BSS 迁移使 RAM 51088→**51908**（+820B = 560+260），换掉深链 ~1.7KB 栈峰值与 SD 扫描 ~780B 栈；IROM 960328→960532。**flash 应用区余量 ≈58KB**（960.5KB/1MB，PSTR 扫荡换来 8.3KB RAM）——后续加功能先看这个数。
  ⑦**未采纳/待决**：`1ec9806` 把整轮审查修复打包进一个 `mem(P0)` 提交（追溯性弱，要点已存本文件）。
- **⛔⛔ PSTR(flash) 文案只能经"安全通道"消费（2026-09-12 实机崩溃定位；P5 扫荡的系统性回归，已修）**：
  `PSTR(s)` 把字面量放进段 `.irom0.pstr.<file>.<line>.<n>` @progbits,1 = **flash 映射区(IROM0)**；
  **flash 只支持 32 位对齐读**，直接逐字节数据访问会编译成 `l8ui` → **Exception 3 (LoadStoreError) 重启**
  （实测：读 PSTR 首字节即崩，`excvaddr` 正好等于该字面量地址；普通字面量在 `.rodata`(DRAM) 逐字节读没问题，
  所以此坑此前一直潜伏）。**现象**：进"章节目录"必崩重启循环（`epc1`=…`utf8Truncate` 的 `l8ui`，
  `excvaddr`= 文案 "章节目录" 的 flash 地址）——P5 把 ~46 处 `drawTextUTF8(..., "文案", ...)` 改成
  `PSTR("文案")`，而这些站点经 `utf8Truncate` 逐字节读 flash。
  **安全通道（只能用这几种）**：① printf 家族（`snprintf`/`printf`/`vsnprintf_P`，newlib 内部走对齐读）；
  ② `_P` 变体（`strlen_P`/`memcpy_P`/`strncpy_P`…）；③ `pgm_read_byte`（=对齐读+移位）；
  ④ `F()`/`FPSTR()` → `__FlashStringHelper*`（`Print::print` 与 `drawTextUTF8` 的该重载内部用 `_P`）。
  **禁止**：`*p`、`p[i]`、`strlen/strcmp/memcpy`、`String(PSTR)`、`Serial.print(const char*)`、
  把 PSTR 当 `const char*` 交给任何会逐字节读的函数。
  **本次修复**：`utf8Width()`/`utf8Truncate()` 加 `isFlashPtr()` + `flashSafeStr()`（pgm_read_byte 搬进栈缓冲）
  → 全部 `drawTextUTF8(..., PSTR(..))` / `showMsg(PSTR(..))` 站点一次性安全；`showMsg` 的 `msg2[0]` 改
  `flashCharAt`；`Serial.println(PSTR(..))` ×2 改 `F(..)`。新文案优先用普通字面量（DRAM 天然安全），
  确要省 RAM 再用 PSTR 且只走安全通道。
- **🔎 板载传感器普查（2026-09-12，对照官方开源硬件 + 实机探针）**：官方 PCB 工程 =
  立创开源「[专业版] V2.43-2.9寸SD墨水屏阅读器」（作者 jie326513988 = 甘草酸不酸，与 V14 源码 commit 的
  326513988@qq.com 同一人）；其 1.54 寸工程页写明整机配置：**SHT30 温湿度 + RX8025T 时钟 + 前置光 LED 恒流
  驱动 + 1000 万次静音按键 + SD（≤32G）+ Type-C，休眠电流 31µA**。**本板实测 I²C 总线（GPIO13/14）
  只有两颗器件：`0x32`=BL8025T 时钟、`0x44`=SHT30 温湿度**（0x01..0x7F 全扫，4/5 引脚对无器件）。
  **SHT30 读法（官方 V14 `Get_bat_vcc.ino::get_dht30_data()` 同款，实机验证通过）**：
  ① `pinMode(12,OUTPUT); digitalWrite(12,HIGH)` —— **传感器由 `bat_switch_pin`=GPIO12(MOS 供电门) 供电，
  不上电就扫不到**（这正是此前一直没发现它的原因：项目原有 I2C 扫描没拉高 GPIO12）；
  ② 稳定 ≥100ms → `SPI.end()` 释放 13/14 → `pinMode(15/5,OUTPUT)+HIGH`(CS 全高) → `Wire.begin(13,14)`;
  ③ 软复位 `0x30A2` + 5ms（省这步会读失败）→ 读序列号 `0x3780` → 单次测量 `0x2400` + 25ms → `requestFrom(6)`；
  ④ 结果：`raw 0x6E24 → 30.3℃`、`0x9464 → 58.0%RH`，两段 CRC8(poly 0x31) 均 ok；
  ⑤ 收尾：`digitalWrite(12,LOW); pinMode(12,INPUT)` 断供电防漏电 → `SPI.begin()` + `reinitSdBus()`。
  **供电门 = 零静态成本**（不读时传感器完全断电），按需唤醒读取对续航无影响。
  探针钩子：`-DSENSOR_SCAN=1`（默认 0；开机 2.5s 跑一次，打印 found 列表 + 序列号 + 温湿度 + 时钟页两风格渲染冒烟）。
  **已实装（2026-09-12）**：`sht30Read()`/`sht30Tick()`（5 分钟节流，开机在重绘前首读一次，日志 `SHT30_OK t=.. h=.. ui=室29℃58%`）；
  **时钟页温湿度改用板载 SHT30(室内)** —— 简洁风格底部行显示 `室29℃58%`（紧凑整数，避免被 168px 截断），
  精美风格温/湿 7 段数字区用室内值并在左侧标"室"；**传感器缺失时自动回落原网络(城市)值**，行为不变。
  主页暂未显示（排版已满，待用户定位置）。写新代码记住：读传感器 = SPI.end()→CS 高→GPIO12 高→Wire→
  断电→SPI.begin()+reinitSdBus，**不要在渲染函数里直接读**（渲染期总线/句柄状态不可控，用缓存值）。
  **未实现功能**：官方主页/配网页会显示室内温湿度（V14 有 `Bitmap_tempSHT30`/`Bitmap_humiditySHT30` 图标 +
  `dht30_temp/dht30_humi`），本复刻固件还没有——**A7 是否也显示待复核**（问用户/再反编译）。
- **🎨 时钟页/状态栏图标化（2026-09-12 用户需求：变量用图标不用文字）**：新增 `clock_icons.h`
  （`make_clock_icons.py` 生成；图标是**逐像素手写点阵**，因为 PIL 画 1-bit 会糊成块）+ `drawClockIcon(x,y,idx)`。
  13 个 16×16 图标：温度计/水滴/太阳/月亮/日历/时钟/对勾/感叹/旗子/B站/电池/电池充电/房子。替换点：
  时钟页（两风格）`温/湿`→温度计/水滴（并去掉 ℃/% 文字）、`上午/下午`→太阳/月亮、`年月日`→日历+`09-12 周五`、
  `已校准/未校准`→对勾/感叹、倒计时→旗子、B粉→B站图标（`clockSubIcon()` 按 InAWord 类型选图）、室内→房子；
  主页状态栏与天气页的电量 → 电池/电池充电图标 + `100%`（去掉"电量"二字，省 32px）。
  **顺带修复用户报的"天气页电量旁闪电错位"**：原 `drawLightningIcon(tx-10, 58-13)` 把 13 高的闪电画在
  y=45..58，而文本基线 58 的字形占 58..74 → 整整高 14px；主页那个位置同样会与长电量文字压字。
  现统一改为"图标(16×16)与文字同高(y 对齐字形顶)"，充电时直接换 `CK_ICON_BATTERY_CHG`（带闪电的电池）。
  渲染冒烟：`-DSENSOR_SCAN=1` 钩子会连渲染"简洁+精美"两风格各一次（`SENSOR_SCAN clock-render ok`），无异常。
  要调整图标形状 → 改 `make_clock_icons.py` 里的点阵字符串后重跑生成脚本即可（点阵即源码，一眼可核对）。
- **📺 天气页中间条 & 电量位置（2026-09-12 用户两轮拍板后的最终形态）**：
  **中间条 = InAWord（一言/自定义句/倒计时/B粉）**，全宽 `[4,292]` 居中，事件类型用图标（旗子/B站），
  空且夜间跳过时显示"夜间不更新"；**电量 = 屏幕左下角**（16×16 电池/带闪电电池图标 @ (2,112) + `drawText8`
  8px 小字百分比 @ (20,116)）。**官方依据**：V14 `DisplayMain.ino:257-301` 双横线之间正是 InAWord，
  电量在角落（`Bitmap_bat3/2/1` 21×12 @ (0,115) + 4px 字体 `setCursor(4,113)`）。
  为不与三列预报重叠，**预报三行的公共左边界预留 `xmin ≥ 62`**（保住"三列跨行对齐"，V14 同法）。
  实机数字自检（`-DWEATHER_RENDER_TEST=1` 造假天气数据走完整渲染并打印布局）：
  `strip inaword avail=288 total=83 sx=106 end=189 fit=1` +
  `corner batt=[2,18]x[112,128] txt8_x=20 fc_xmin=73 fc_right=226` → 互不重叠、不截断，两遍渲染无异常。
  **演进记录（避免反复）**：正中一度放过"电量"（用户否决：与天气无关）→ 放过"室内温湿度"（用户再拍板改为
  InAWord）→ 电量最终落左下角。**室内温湿度只在时钟页显示，天气页不显示。**
- **🔧 天气页水滴图标方向修正（2026-09-12 用户反馈）**
  （中间条/电量位置的最终结论见上面 📺 条，此处只留图标教训）：
  ② **湿度图标方向反了**（用户："尾巴朝下，尾巴朝上才是水滴"）：`make_small_icons.py::icon_humidity` 原为
  `polygon([(3,5),(9,5),(6,11)])` 尖在**下**、`ellipse([4,2,8,6])` 圆头在**上** = 倒过来的水滴；
  已改为尖朝上 `polygon([(6,1),(3,6),(9,6)])` + 圆身在下 `ellipse([3,5,9,11])`（点阵核对通过）。
  `clock_icons.h` 的 16×16 水滴本来就是尖朝上，无需改。**教训**：图标改完要**把点阵反解成 ASCII 逐行核对**
  （本模型不能读图，`weather_small_icons.h` 的 26 字节/图可直接解码打印），否则方向类错误只能等用户肉眼发现。
- **🐛 两条"显示层误报"的根因与教训（2026-09-12 用户报，均已修）**：
  ① **"配好了网络却显示未配网"** = `wifiManagerHasCredentials()`（wifi_manager.cpp:1528）读的是 RAM 里的
  `config`，却**漏了兄弟函数都有的 `if (config.ssid[0]=='\0') loadConfig();` 补读**（`EnsureSta`/`StartSta`
  都有，注释还写着"避免误报未配置"）→ **刚开机时 config 全 0 → 必然判 false** → 首页→时钟走
  `renderClockNoWifi()`（"未配网 / 未保存 WiFi 配置，跳过校准"）并跳过校准。修：补上 loadConfig +
  打印 `WIFI_CFG load=.. magic=.. want=.. ver=.. ssidlen=.. passlen=..` 与 `WIFI_CRED has=.. ssid=[..] passlen=..`
  （**只打长度，绝不打印密码**）→ 一眼区分"没加载 / EEPROM 真没凭据 / 密码短于 8 位"。
  **教训：凡读取内存态 `config` 的函数，入口必须先确保 `loadConfig()` 已跑过**（同一模块内三处只有一处漏 → 静默误报）。
  ② **天气页左上角显示 `15::3`** = 更新时间硬取 `lastUpdate[11..14]`，前提是 ISO8601 用 `'T'` 分隔
  （`"2026-09-12T15:30:00"`，`':'` 在 13）。接口实际会回**空格分隔**的 `"2026-09-12 15:30:00+08:00"` →
  整体错位一位 → `"%c%c:%c%c"` 取出 `1 5 : 3` → `15::3`。修：新 helper `formatHhmm()` **在串里找第一个
  `dd:dd` 形状**（不假设分隔符/是否补零），找不到给 `--:--`。设备自检（`-DWEATHER_RENDER_TEST=1`）：
  `hhmm T-form=[15:30] space-form=[15:30]` ✓（旧实现在 space-form 下是 `15::3`）。
  **教训：外部数据的时间/日期**禁止**硬编码下标切片**，一律用形状匹配（对外接口格式会变）。
- **🔎 板载时钟芯片布局判定的隐患（2026-09-12 顺带发现，待用户决定是否加固）**：开机
  `CLOCK_8025T ... type=BL|RX` 是**双布局盲试**（BL 偏移 0 / RX 偏移 8，先试 BL）。本机芯片实为
  **RX 布局**：某次开机 BL 那 7 个字节凑巧是合法 BCD → 采信 BL → 时间读成 **2024-09-12**（错 2 年）；
  本次开机 BL 解析失败、RX 成功 → `epoch=1789199464` = **2026-09-12**（正确）。加固思路：把
  **星期寄存器**（parse 目前忽略的 `buf[off+3]`）与"由日期算出的星期"交叉校验，或在两布局都成功时
  优先 RX —— 需要一个判据把"偶然合法"排除掉。
- **🌐 B粉接口不可用的根因与修法（2026-09-12 用户要求实测，已修 + 设备端验证）**：
  `bili_fans.cpp::fetchBiliFollower` 原用**明文 HTTP** `http://api.bilibili.com/x/relation/stat?...`，
  而该域名**现在强制 HTTPS**：PC 实测该 HTTP 请求 → **307**（加 `jsonp=jsonp`、换 UA 都一样），
  ESP8266 `HTTPClient` **不跨协议跟随重定向** → 只能拿到 `HTTP307` → 永远失败。
  **修法**：改用 B 站**接口镜像** `api.biliapi.net`（PC 实测明文 HTTP **200**，JSON 与原域名同形：
  `{"code":0,...,"data":{...,"follower":1428204,...}}`），失败再试 `.com` 兜底 —— 都不需要 TLS，
  省掉 ESP8266 上约 20KB 的 BearSSL 握手堆（实测整个请求 heap 只掉 440B）。
  **设备端实测**（钩子 `-DBILI_FANS_TEST=1`：开机拉起 STA → 真打接口 → 打印）：
  `BILI_TEST sta=1 ip=192.168.0.12` → `result ok=1 val=1428212 err=[]` ✓ 真实粉丝数；同一次还打了
  `HITOKOTO_TEST ok=1 text=[错过了雪花，我就等你看雪落。]` ✓ ——**一言本来就是明文 HTTP 200
  （v1.hitokoto.cn 不跳 HTTPS），无需改**。连带确认 `WIFI_CRED has=1 ssid=[11A] passlen=13`（"未配网误报"已修）。
- **⚠️ 流程教训（2026-09-12 踩过，代价=用户白测一轮）**：`python esp_dev.py --steps flash` **只烧不编译**
  （只有不带 `--steps` 的全流程才编译）。我改完 `formatHhmm()` 只把它编进了 `build_wtest/`，然后用
  `--steps flash` 烧了**旧的 `build/`**（bin 15:45 vs 源码 15:49）→ 用户看到"修复无效"。
  **纪律：先 `arduino-cli compile --build-path build`，再烧，并核对 `build/*.bin` 时间戳 > 源码时间戳。**
- **⛔ 伪装模式是"出不来"的高危状态（2026-09-12 用户报"KEY1 后按 KEY3 回不了首页"，已加固）**：
  `APP_CLOCK_DISGUISE`(11) 由**阅读界面中长**(老板快捷键 `enterClockDisguise()`)进入，并经
  `saveSleepRecord()` 写进睡眠记录 → **跨复位/跨固件烧录保留**。该模式**按设计停用全部按键**（含组合键），
  唯一出口 = "KEY1 复位 + 开机 KEY3 窗口"；而窗口原本只有 1 秒且早于界面重绘，**稍早(ESP 未启动)/
  稍晚(窗口已过)即错过，错过没有第二次机会** —— 用户观感就是"按不回来"。
  加固：① 伪装恢复路径在时钟页显示完成后追加 **3 秒 KEY3 宽限**（按下即回首页 + 覆盖记录为首页，
  trace `DISGUISE_EXIT key3 grace=`）；② 开机 KEY3 窗口 1.0s → **1.5s**（`#define KEY3_WINDOW_MS`，
  用户习惯"KEY1 后约 1 秒按"，原值贴边；窗口与页表扫描重叠，对启动耗时几乎无影响）。
  **测试纪律（踩过）**：自动化脚本/测试钩子**禁止**在阅读界面注入"中键长按"(=进伪装，会把设备锁进
  只能靠按键逃生的状态，且状态持久化)——需要"取消/返回"语义时用中键短按或组合键。
  另外 PC 侧**无法**模拟 KEY3（=GPIO3=RX；实测 pyserial `send_break`/`break_condition` 都不能让固件
  `readKey3()` 读到低电平）→ 涉及 KEY3 的验收必须由用户在设备上按。
- **⛔ 进度落盘与"KEY1 硬复位回首页"的冲突（2026-09-12 用户报 bug，已修）**：用户回首页的习惯是
  **KEY1 硬复位 + 1 秒内按 KEY3**（boot `key3Held` 分支直接回首页）——硬复位**不经过**
  `closeTxtReader`/`enterSleepMode`/`saveSleepRecord`，所以 `progressFlushForce()`（退出/换书/休眠落盘）
  永不执行；而"显式跳转"原先只调 `writeProgress()`（登记到 RAM，50 页/5 分钟阈值才落盘）→ 复位瞬间待写偏移
  消失 → `.i1` 记录[0] 仍是旧偏移 → 重进书回到跳转前（用户实测："菜单章节/跳转跳转后回首页进度消失"）。
  **修复**：① `writeProgressNow(off, reason)` = 登记 + 立即落盘，用于全部显式跳转（`jumpToPage`=菜单跳转键盘 /
  章节目录行跳转 / 标签跳转 / 进度同步应用）；② 节流 50 页/5 分钟 → **10 页/60 秒**
  （宏 `PROG_FLUSH_PAGES`/`PROG_FLUSH_MS`）。**实机验收**（`-DPROGRESS_RESET_TEST=1` 钩子）：
  `PROG_FLUSH reason=jump_chapter off=20618636` → 直读 `.i1` 一致 → `ESP.restart()`(等价 KEY1) →
  `TXT restored page=50043 offset=20618636`（与跳转前完全一致）✓。
  **推论（写新代码时记住）**：任何"用户会立刻断电/复位"的关键状态，不能只放在 RAM 里等节流落盘。
- **章节目录自动定位到当前章（2026-09-12 用户需求，已实机验收）**：阅读菜单 → 章节时，列表直接翻到
  "当前阅读页所属章节"所在页 + 光标停在该行（不再每次从第 1 页第 1 行开始）。实现：`chapterBuildPageTable()`
  扫描 .z1 时**顺带**解析每行页号，记录最后一条 `page <= txtPage` 的章序号（`chapterCurIdx`，.z1 页号单调
  不减 → 必为当前章），零额外 I/O；`enterChapterList()` 用 `chapterPageOffsets[章序号/CHAPTER_ROWS]` 定位
  列表页、`章序号%CHAPTER_ROWS` 定光标（越界/低堆降级/当前页在首章之前 → 退回第 1 页）。trace
  `CHAPTER_AUTOJUMP readPage=.. curCh=.. listPage=.. row=..`。实机：《武炼巅峰》readPage=70000 →
  listPage=499 sel=5，本页页码 69844…69960 ✓。回归钩子 `#if CHAPTER_AUTOJUMP_TEST`（默认 0）。
- **2026-09-12 审查修复轮 #3（实机取证 + 两个系统性问题收口）**：
  ① **审查 #1 实机验收 PASS**（`-DCOMBO_SESSION_TEST=1` 钩子走真实菜单路径 + 真实 comboHome 分发）：
  `COMBO_HOME from mode=3 heap=19856` → `COMBO_HOME close session mode=3 keepBuild=0 heap=25448`
  → `COMBO_TEST_OUT mode=0 delta=5592 PASS`（4.5KB 章节缓冲归还堆 + `PROG_FLUSH reason=close` + 关句柄 + 统计收口）；
  mode=2 阅读页路径 Δ=+1080B，且**不再多刷一次文件管理器界面**（原实现 2×全刷 ≈3s）。
  ② **章节目录崩溃已修**（PSTR 规则见上）：修复后 `CHAPTER_ENTER → CH_TABLE built pages=826 → RENDER_FULL →
  COMBO_TEST_IN mode=3 chapters=6`，标题 hex 解出 "第一卷 崛起凌…" ✓ 真实数据。
  ③ 审查 #4：arena 改访问器 `webArenaRequestPathSlot/webArenaStreamSlot` + 槽位表 `static_assert` 编译期自检；
  审查 #6：`AppMode` 收敛到新头文件 `app_mode.h`（globals.h 与 ino 都 include，不再两处并列）。
  ④ **实机测试环境坑（下次勿踩）**：pyserial 打开 COM 会经 DTR/RTS **触发 ESP 自动复位**（脚本须等启动完成
  心跳再操作）；CH340 在"长时间只读不发命令"后**读管道会静默停摆**（日志停止增长）→ 关键步骤用"单次注入 +
  短窗口读取"，且只认本次窗口内的新行；盲注入按键穿阅读菜单会**误触休眠(深睡不可逆)/重建索引(删 14 万页索引)**
  → 需要确定性场景一律走 `#if COMBO_SESSION_TEST` 这类钩子（默认 0，零成本）。
  ⑤ 钩子用法：`python esp_dev.py --serial-remote -D COMBO_SESSION_TEST=1 --build-path build_remote --steps build`
  然后烧录 build_remote，上电读串口（打印 COMBO_TEST_BEGIN/IN/OUT）；验收脚本 `combo_home_test.py` 为交互版。
## 文件管理 HTTP API（2026-08 新增, 契约见 docs/file-api.md）

- 统一 API: Web/Android/Legado 都是客户端; 服务器复用配网会话的 ESP8266WebServer
- 模块: sd_path(纯函数路径安全/保护) → sd_file_ops(SD 操作层) → file_api(HTTP 路由)
- 端点: /api/status capacity files stat search download upload(仅配网会话) mkdir delete rename move
- **全部端点免鉴权 (2026-09-12 用户拍板)**: 管理密码机制已整体废除, X-Admin-Pass 头被忽略; 仅限可信局域网使用 (AP 需物理接触连入; STA 管理态同网段可达)。契约见 docs/file-api.md §1
- 关键坑(已踩):
  - File::name() 只返回 basename, 递归/完整路径必须用 fullName()(嵌套目录丢失路径)
  - fullName() 根相对无前导 /, fillEntry 需规范化, 否则 isProtectedPath 判不了
  - ESP8266 循环栈仅 4KB: HTTP 处理器内大缓冲必须 malloc; 递归回调(搜索)内禁止栈上大局部
  - 传输循环不加 yield()(实测拖慢 2x); TCP 吞吐 ~0.2MB/s 是栈/链路瓶颈(非 SD)
  - 上传断连残留 .uploading 的 FAT 目录项尺寸只在 close/flush 更新 → 每 64KB flush
  - 删除打开的文件会失败 → 先关句柄
- 性能基线(S4.5): SD 读写 ~1MB/s, 网络 ~0.2MB/s(P0 达标); 传输缓冲 4KB 动态
- web_test(J:\code\esp8266\web_test 独立仓库) 是 PC 测试台(STA 连局域网 + /api + /bench)

- **📡 进度同步"发现得到手机、却连不上"的根因与修法（2026-09-12 整轮实机定位，两侧都改）**：
  **症状链**：① 阅读界面 P5 断言无条件 `RF_OFF("reader_assert")` 把刚建立的 WiFi 掐断（日志实证
  `WIFI try-sta ok=1` 紧跟 `RF_OFF reason=reader_assert`）→ 修：同步会话期间放行 RF；② 手机 HTTP 服务
  原先绑 **"当时的局域网 IP"**（`NetworkUtils.getLanServerIp`），手机换 IP 后 8384 停在旧地址（`/proc/net/tcp`
  查无 20C0、UDP 8390 仍活 → "发现能答、HTTP 不通"极具误导）→ 修：绑 `0.0.0.0`；③ **UDP 发现收包**：
  手机日志证明"收到 LUMIDISC 且回 3 次 LUMIACK"但设备恒 `DISCOVER timeout` → 用 `-DUDP_DISC_TEST=1` 钩子
  （新建 WiFiUDP、12s 内打印每个包）隔离：**新建对象 50/50 包全收**（含手机 `LUMIACK 192.168.0.18`）⇒
  **根因 = 复用同一个 `WiFiUDP` 对象**：`stop()` 后再 `begin()` 能发不能收（lwip 收包回调未重挂）。
  修：`progress_sync.cpp` 改 `static WiFiUDP *gDiscUdp`，每次发现 `discoveryBegin()` 新建、`discoveryStop()` 释放。
  修后实录：`DISCOVER localPort=8390` → `DISCOVER ok ip=192.168.0.18` → `CONNECT ok` → `GET code=404`。
  **手机端（J:\code\Android\legado，独立仓库）**：用户拍板"**只按 txt 文件名/路径精确锁定**" ⇒
  `EspBookMatch` 删掉全部"归一化/包含/按 size 择近"猜测逻辑，只做 `originName` 精确 + `bookUrl` 指向的 txt 存在校验；
  GET/PUT 先核实书架：不在架上回 `404 no-book`（ESP 显示 **"手机上没有这本书"**），在架上但手机侧没进度回
  `404 no-progress`；**GET 不再用 `deviceSnapshot`（设备推回的历史进度）兜底** —— 这是"删旧书→导同名新书后被旧进度
  覆写"的真根因；服务启动时 `clearStaleDeviceEntries()` 清理书架上已删书的残留快照；UDP socket 提为类字段 +
  `reuseAddress` + 失败重试（原进程被杀后 `EADDRINUSE` → 发现失效）。
  **构建/联调备忘**：① 该项目 `assembleDebug` 在本机会因**跨盘 KSP 增量**失败（缓存 C: / 项目 J:），
  用 `-Pksp.incremental=false` 可绕过（同 app/build.gradle 里 glide-svg 处理器被删是同类坑）；
  ② 手机无线调试老自动关：`wifi_sleep_policy=2`（仅插电不睡）+ 不插电熄屏 → WiFi 休眠 → Android 11+ 随之
  关无线调试（IP 也会变）；已 `settings put global wifi_sleep_policy 0`，并用 `adb tcpip 5555` 改成**固定端口**
  （`adb connect <ip>:5555`，重启手机前有效，不再受"无线调试"开关影响）；③ 授权：`adb pair <ip>:<配对端口> <配对码>`
  一次即可。

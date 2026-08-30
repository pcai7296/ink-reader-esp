# ink-reader-esp

> ESP8266 墨水屏电子书阅读器固件（复刻「墨水平板 V2.9」官方 A7 固件，支持自定义扩展）。
> 主控 **ESP-12F (esp8266)**，屏幕 **2.9" 128×296 SSD1680**（横放 → 逻辑 296×128）。

## 功能

- **文件管理器**：SD 卡多级目录浏览 / 滚动 / 选中 / 删除（文件夹优先 + 名称升序）
- **TXT 阅读**：页索引（`.i1`）+ 章节目录（`.z1`）、自动翻页、跳转数字键盘、阅读进度记忆、断点续建索引
- **阅读方向**：四向 0°/90°/180°/270° 旋转（横屏/竖屏各自独立索引，翻转换向秒开）
- **标签系统**：每书一份 `<书名>.bm` 书签标记，跨方向共用
- **时钟**：外挂 **BL8025T** RTC（断电走时）+ NTP 校准 + 电量显示（官方 4 阶多项式）
- **天气**：心知天气 API（实况 / 未来 / 生活指数，天气壁纸背景）
- **一言**：时钟页 v1.hitokoto.cn 每日一言
- **BMP 图片**：SD 卡 1/4/8/16/24 位深图片全屏显示
- **配网 + Web 管理页**：WiFi AP 热点 + 浏览器文件上传/下载/删除，墨水屏同步显示"上传中/上传完毕"
- **进度同步**：直连手机（LUMI1 协议）双向同步阅读进度，含文件指纹校验
- **OTA 升级**：ESP8266HTTPUpdateServer
- **开机引导**（BOOT_AP_MODE=1）：配网启动分阶段提示「正在初始化 → 正在加载储存卡 → 热点信息」，全程局部刷新

## 硬件

| 部件 | 说明 |
|------|------|
| 主控 | ESP-12F (esp8266)，Arduino core 3.1.2，FQBN `d1_mini` |
| 屏幕 | 2.9" SSD1680 (GDEH029A1)，CS=15 / DC=GPIO0 / RST=2 / BUSY=GPIO4 / MOSI=13 / SCLK=14 |
| SD 卡 | CS=GPIO5，与屏幕共享 SPI（SD_SCK_MHZ(20)） |
| RTC | BL8025T，I2C 0x32，SDA=13 / SCL=14（与 SPI 共用引脚需总线仲裁） |
| 按键 | KEY1=硬件复位 / KEY2=GPIO0（与 EPD DC 共用，time-sliced）/ KEY3=GPIO3 |
| 电池 | GPIO12 采样（与 SD MISO 复用，需防漏电） |

## 目录结构

```
ink-reader-esp/
├── ink-reader-esp.ino      # 主固件 (3480+ 行)
├── epd_290a.h / .cpp         # EPD 驱动 (296×128 SSD1680)
├── rot_map.h                 # 四向旋转坐标规范 (mapFbRot 纯函数, 唯一权威)
├── wifi_manager.h / .cpp     # 时钟 / 配网 / 天气配置模块
├── weather_data.h / .cpp     # 天气数据层 (心知天气 API)
├── hitokoto.h / .cpp         # 一言数据层 (v1.hitokoto.cn)
├── bmp_show.h / .cpp         # SD 卡 BMP 图片显示
├── file_api_fs.*             # Web 文件上传/下载 API + 上传状态墨水屏回调
├── sd_file_ops.* / sd_path.* # SD 操作层 / 纯函数路径安全
├── fs_cache.*                # SD 目录树 → LittleFS 缓存
├── progress_sync.* / progress_lumi.*  # 阅读进度同步 (LUMI1)
├── pc_tests/                 # PC 端测试源码 (不参与固件编译)
├── docs/                     # 协议 / 设计文档 (file-api.md, progress-lumi1.md 等)
└── data/                     # LittleFS 数据 (manager.htm — Web 管理页)
```

## 依赖（必须先安装）

固件**唯一的外部库**为 [U8g2_for_Adafruit_GFX](https://github.com/olikraus/U8g2_for_Adafruit_GFX) 及其传递依赖。请在 Arduino IDE 库管理器安装：

```
① U8g2_for_Adafruit_GFX        （中文/多字节渲染）
② Adafruit_GFX_Library         （U8g2_for_Adafruit_GFX 依赖）
③ Adafruit_BusIO               （Adafruit_GFX 依赖）
```

> 其余依赖均为 **ESP8266 Arduino core 自带**（SD / SDFS / LittleFS / SPI / Wire / EEPROM / ESP8266WebServer / WiFi / HTTPClient / HTTPUpdateServer），安装 core 后无需额外处理。项目内 `libraries/` 目录是本地安装副本，**未随源码上传**，请按上面的库名自行安装。

## 编译

需要 **Arduino CLI**（含 esp8266 core 3.1.2）。务必用 `--build-path` 指定项目 `build/` 目录，否则 bin 会输出到临时目录：

```bash
arduino-cli compile --fqbn esp8266:esp8266:d1_mini \
  --libraries libraries \
  --build-path "J:\code\esp8266\ink-reader-esp\build" \
  ink-reader-esp.ino
```

### 开机引导固件（BOOT_AP_MODE=1）

默认固件是正常功能模式。若要编译成「开机直进配网 AP」的引导版（配网启动分阶段提示），加宏：

```bash
arduino-cli compile --fqbn esp8266:esp8266:d1_mini \
  --libraries libraries \
  --build-path "J:\code\esp8266\ink-reader-esp\build" \
  --build-property "compiler.cpp.extra_flags=-DBOOT_AP_MODE=1" \
  ink-reader-esp.ino
```

烧录前核对 `build\ink-reader-esp.ino.bin` 时间戳（bin 名是 `ink-reader-esp.ino.bin`）。

## 烧录

```bash
esptool.py --port COM20 --baud 460800 write_flash 0x0 \
  "J:\code\esp8266\ink-reader-esp\build\ink-reader-esp.ino.bin"
```

## Web 管理页 / 配网

- 配网 AP：SSID `MSP-50BB`，密码在 `wifi_manager.cpp` 的 `AP_PASSWORD`（默认 `333333333`），IP `192.168.4.1`
- Web 管理页：`data/manager.htm`（Material 风格 UI），上传用 Blob 强制 Content-Length
- 文件 API：`/api/status /files /download /upload /delete /mkdir /rename /move`  契约见 `docs/file-api.md`

## 进度同步（LUMI1）

手机端开启进度服务器后，阅读菜单「进度同步」直连手机（端口 8384 明文 HTTP）。协议规范见 `docs/progress-lumi1.md`。⚠️ 两端 txt 必须逐字节一致（同名且内容相同），否则按百分比换算会偏移。

## 测试

`pc_tests/` 下有 PC 端测试（纯函数，不参与固件编译，两个独立的 `main()` 会冲突需分别编译）。`docs/` 下是逆向 / 设计 / 根因复盘文档。

## License

本项目为个人复刻项目，未指定开源协议。参考固件版权归原厂商所有。

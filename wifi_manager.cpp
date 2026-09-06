#include "wifi_manager.h"
#include "file_api.h"
#include "file_api_fs.h"
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>
#include <time.h>
#include <user_interface.h>
#include <WiFiUDP.h>
#include <Wire.h>
#include <SPI.h>

// ---------- 板载 BL8025T/RX8025T 外部 RTC 芯片 (I2C, V14 官方方案) ----------
// 芯片自带 32.768kHz 晶振 + VBAT (主板电池/电容), 断电后继续走时
// 注意: I2C 用 GPIO13(SDA)/GPIO14(SCL), 与 SD/EPD 的 SPI 共用引脚, 需总线仲裁
#define RTC8025_I2C_ADDR 0x32
#define RTC8025_SDA_PIN 13
#define RTC8025_SCL_PIN 14
static bool rtc8025Present = false;      // 芯片存在且可读
static bool rtc8025TimeValid = false;    // 芯片时间合法
static bool rtc8025TypeRX = false;       // true=RX8025T(读取偏移8) false=BL8025T(顺序读)
static time_t rtc8025NowEpoch = 0;       // 探测时读出的当前时间 (UTC epoch)

// 日历年→天数 (Howard Hinnant civil_from_days 逆运算), 不依赖 mktime/localtime 时区
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (int)(m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

// 把“当作 UTC 的本地时间”转成 UTC epoch; BL8025T 存北京时间 → epoch = civilAsUtc - 8*3600
static time_t civilToEpoch(int y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s) {
  return (time_t)(daysFromCivil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL + s);
}

static uint8_t bcdToDec(uint8_t v) { return (uint8_t)(((v >> 4) & 0x0F) * 10 + (v & 0x0F)); }
static uint8_t decToBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// 读 16 字节寄存器; 返回 true 表示 I2C 有应答
static uint8_t rtc8025LastReqN = 0;   // 诊断: 最近一次 requestFrom 返回字节数
static uint8_t rtc8025LastMode = 0;   // 诊断: 最近成功读法 0=restart 1=addr+stop 2=direct(官方)
static int rtcI2C_SDA = RTC8025_SDA_PIN;   // 实际探测到的引脚 (默认 13, V14 源码)
static int rtcI2C_SCL = RTC8025_SCL_PIN;   // 默认 14
static bool rtc8025ReadRaw(uint8_t buf[16]) {
  // 读法兼容: 官方 V14 (BL8025_RTC::_read) 是直接 requestFrom(addr,16), 不先写寄存器地址
  // (芯片上电寄存器指针默认 0x00, 自动递增连续输出)。而"写地址+重复 START"读法在部分
  // ESP8266 core 的 Wire 实现上有兼容问题 (endTransmission(false) 后 requestFrom 无应答)。
  // 因此逐级尝试三种读法, 任一成功即算可读。
  static const uint8_t modes[3] = {2, 1, 0};   // 尝试顺序: 官方 direct → addr+stop → addr+restart
  for (uint8_t m = 0; m < 3; m++) {
    if (modes[m] == 2) {
      // 官方法: 直接 requestFrom (默认 stop=true, 总线释放)
      uint8_t n = Wire.requestFrom((uint8_t)RTC8025_I2C_ADDR, (uint8_t)16);
      rtc8025LastReqN = n;
      if (n >= 7) {
        for (int i = 0; i < 16; i++) {
          if (Wire.available()) buf[i] = (uint8_t)Wire.read();
          else buf[i] = 0xFF;
        }
        rtc8025LastMode = 2;
        return true;
      }
    } else if (modes[m] == 1) {
      // 先写寄存器地址 0x00 (带 STOP), 再读
      Wire.beginTransmission(RTC8025_I2C_ADDR);
      Wire.write(0x00);
      Wire.endTransmission(true);
      delay(2);
      uint8_t n = Wire.requestFrom((uint8_t)RTC8025_I2C_ADDR, (uint8_t)16);
      rtc8025LastReqN = n;
      if (n >= 7) {
        for (int i = 0; i < 16; i++) {
          if (Wire.available()) buf[i] = (uint8_t)Wire.read();
          else buf[i] = 0xFF;
        }
        rtc8025LastMode = 1;
        return true;
      }
    } else {
      // 先写寄存器地址 0x00 (重复 START, 原实现)
      Wire.beginTransmission(RTC8025_I2C_ADDR);
      Wire.write(0x00);
      Wire.endTransmission(false);
      delay(2);
      uint8_t n = Wire.requestFrom((uint8_t)RTC8025_I2C_ADDR, (uint8_t)16);
      rtc8025LastReqN = n;
      if (n >= 7) {
        for (int i = 0; i < 16; i++) {
          if (Wire.available()) buf[i] = (uint8_t)Wire.read();
          else buf[i] = 0xFF;
        }
        rtc8025LastMode = 0;
        return true;
      }
    }
  }
  return false;
}

// 从寄存器 0x00 起写 7 字节 (秒/分/时/星期/日/月/年) + 配置寄存器 (对齐 V14 BL8025_RTC::_begin):
// reg8-0x0F 全写 0, 其中 Control Register(0x0F)=0x00 强制 24 小时制 —— 否则芯片处于 12 小时制
// 时, 小时 BCD 含 AM/PM 标志位, 读取解析失败(小时>23) → 时间读不到/回退旧值 (实测根因)。
static bool rtc8025WriteRaw(const uint8_t data[7]) {
  // 类型区分写入 (实测本机芯片为 RX 布局, 时钟寄存器在 0x08-0x0E):
  //   BL: 时钟 0x00-0x06 + 0x08-0x0F 全 0 (0x0F Control=0x00 → 24 小时制)
  //   RX: 时钟 0x08-0x0E, 只写这 7 字节; RX8025T 只支持 24H, 不动 0x0F 扩展/控制。
  //       ⚠️ 绝不能对 RX 写 0x08-0x0F 全 0 — 会把正在走的时钟清零 (寄存器布局不同)。
  uint8_t base = rtc8025TypeRX ? 0x08 : 0x00;
  Wire.beginTransmission(RTC8025_I2C_ADDR);
  Wire.write(base);
  for (int i = 0; i < 7; i++) Wire.write(data[i]);
  bool ok = (Wire.endTransmission() == 0);
  delay(1);
  if (!rtc8025TypeRX) {
    // reg8-0x0F: 闹钟/定时器/扩展/标志/控制 全 0 (Control=0x00 → 24 小时制, 正常模式)
    Wire.beginTransmission(RTC8025_I2C_ADDR);
    Wire.write(0x08);
    for (int i = 0; i < 8; i++) Wire.write(0x00);
    bool ok2 = (Wire.endTransmission() == 0);
    delay(1);
    return ok && ok2;
  }
  return ok;
}

// 解析一组寄存器为 tm 字段; off 为秒寄存器偏移 (BL=0, RX=8); 返回是否合法
static bool rtc8025Parse(const uint8_t buf[16], int off, time_t &epoch) {
  if (off + 7 > 16) return false;
  uint8_t sec = bcdToDec(buf[off + 0]);
  uint8_t min = bcdToDec(buf[off + 1]);
  uint8_t hour = bcdToDec(buf[off + 2]);
  uint8_t day = bcdToDec(buf[off + 4]);
  uint8_t mon = bcdToDec(buf[off + 5]);
  uint8_t yr = bcdToDec(buf[off + 6]);
  if (sec > 59 || min > 59 || hour > 23) return false;
  if (day < 1 || day > 31 || mon < 1 || mon > 12) return false;
  if (yr < 20 || yr > 99) return false;  // 2020-2099
  epoch = civilToEpoch(2000 + yr, mon, day, hour, min, sec) - 8 * 3600;  // 北京时间→UTC
  return true;
}

// 探测 BL8025T/RX8025T: 双引脚候选扫描 (13/14 = V14 源码; 4/5 = ESP12F 默认 I2C), 
// 先写寄存器地址再读(用户资料), 找到设备后记录引脚供后续读写; 访问后恢复 SPI
static bool rtc8025Init() {
  rtc8025Present = false;
  rtc8025TimeValid = false;
  rtc8025TypeRX = false;
  rtc8025NowEpoch = 0;
  // 释放 SPI 外设: SD/EPD 初始化后 GPIO13/14 的 PIN_FUNC 被 SPI 占用, 软件 I2C (Wire)
  // 无法驱动 → 实测: 干净环境 Wire(13,14) 能读到 0x32 芯片 (16 字节), SD 挂载后同一
  // 代码 reqN=0。SPI.end() 释放引脚后 Wire 才正常; 探测结束 SPI.begin() 恢复,
  // 主流程 reinitSdBus 重挂 SD。
  SPI.end();
  // 总线仲裁: 确保 SD/EPD 都不被选中
  pinMode(15, OUTPUT); digitalWrite(15, HIGH);   // EPD CS
  pinMode(5, OUTPUT); digitalWrite(5, HIGH);     // SD CS
  const int pairs[2][2] = {{13, 14}, {4, 5}};    // 候选引脚对
  for (int p = 0; p < 2 && !rtc8025TimeValid; p++) {
    int sdaPin = pairs[p][0], sclPin = pairs[p][1];
    // ⚠️ 不要在这里做"9 脉冲 SCL 解锁" — 实测 (probe2 vs probe3) 它会把 BL8025T 状态机
    // 推进错误状态, 之后芯片不响应地址 (beginTransmission 返回 2/NACK, requestFrom 0 字节)。
    // 正常启动总线无挂死, 直接 Wire.begin 即可 (干净环境/SD 挂载后均验证可读 0x32)。
    Wire.begin(sdaPin, sclPin);
    Wire.setClock(100000);
    uint8_t buf[16];
    memset(buf, 0xFF, sizeof(buf));
    bool ok = rtc8025ReadRaw(buf);
    if (ok) {
      // I2C 有应答 (读到 ≥7 字节) 即认为芯片存在 — 即使时间字段非法 (出厂全 0 / 12 小时制),
      // 也记录引脚供后续 24 小时制修复与 NTP 写入; 否则 present=false 会跳过修复 (实测月=0 解析失败)。
      rtc8025Present = true;
      rtcI2C_SDA = sdaPin;
      rtcI2C_SCL = sclPin;
      time_t t;
      if (rtc8025Parse(buf, 0, t)) {          // BL 布局 (秒在 reg0)
        rtc8025TypeRX = false;
        rtc8025TimeValid = true;
        rtc8025NowEpoch = t;
      } else if (rtc8025Parse(buf, 8, t)) {   // RX 布局 (秒在 reg8)
        rtc8025TypeRX = true;
        rtc8025TimeValid = true;
        rtc8025NowEpoch = t;
      }
      Serial.printf_P(PSTR("CLOCK_8025T pin=%d/%d mode=%d type=%s valid=%d epoch=%lu raw=%02X %02X %02X %02X %02X %02X %02X\n"),
                    sdaPin, sclPin, rtc8025LastMode, rtc8025TypeRX ? "RX" : "BL",
                    rtc8025TimeValid ? 1 : 0, (unsigned long)rtc8025NowEpoch,
                    buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);
    } else {
      // 诊断: 打印该引脚对的 SDA 状态 + 扫描 I2C 地址
      int sdaLevel = digitalRead(sdaPin);
      Serial.printf_P(PSTR("CLOCK_8025T_DIAG pin=%d/%d reqN=%d mode=%d sda=%d"), sdaPin, sclPin, rtc8025LastReqN, rtc8025LastMode, sdaLevel);
      for (uint8_t addr = 0x30; addr <= 0x77; addr++) {
        Wire.beginTransmission(addr);
        uint8_t ack = Wire.endTransmission();
        if (ack == 0) Serial.printf_P(PSTR(" addr=0x%02X"), addr);
      }
      Serial.println();
    }
  }
  if (rtc8025Present && !rtc8025TimeValid) {
    // 解析失败(常见: 芯片处于 12 小时制, 小时 BCD 带 AM/PM 标志位 >23) →
    // 只写 0x0F (Control Register) = 0x00 强制 24 小时制后重读 (对齐 V14 BL8025_RTC::_begin)。
    // ⚠️ 只写 0x0F 单字节: BL 的 Control 在 0x0F; RX 时钟在 0x08-0x0E 且只支持 24H,
    //    0x0F 非时钟 — 写 0 对两种类型都安全, 绝不能写 0x08-0x0F 整段 (RX 会清时钟)。
    Wire.beginTransmission(RTC8025_I2C_ADDR);
    Wire.write(0x0F);
    Wire.write(0x00);
    Wire.endTransmission();
    delay(2);
    uint8_t buf[16];
    memset(buf, 0xFF, sizeof(buf));
    if (rtc8025ReadRaw(buf)) {
      time_t t;
      if (rtc8025Parse(buf, 0, t)) {
        rtc8025TypeRX = false; rtc8025TimeValid = true; rtc8025NowEpoch = t;
      } else if (rtc8025Parse(buf, 8, t)) {
        rtc8025TypeRX = true; rtc8025TimeValid = true; rtc8025NowEpoch = t;
      }
    }
    if (rtc8025TimeValid) Serial.println(F("CLOCK_8025T_FIXED_24H"));
  }
  // 恢复 SPI 总线 (GPIO13/14 归 SPI, 供 EPD/SD 使用)
  SPI.begin();
  Serial.printf_P(PSTR("CLOCK_8025T present=%d type=%s valid=%d epoch=%lu\n"),
                rtc8025Present ? 1 : 0, rtc8025TypeRX ? "RX" : "BL",
                rtc8025TimeValid ? 1 : 0, (unsigned long)rtc8025NowEpoch);
  return rtc8025Present;
}

// 写入当前时间 (北京时间语义); 调用前需保证 Wire 已初始化
static bool rtc8025WriteEpoch(time_t epochUtc) {
  time_t local = epochUtc + 8 * 3600;  // UTC → 北京时间
  struct tm tm;
  gmtime_r(&local, &tm);
  uint8_t data[7];
  data[0] = decToBcd((uint8_t)tm.tm_sec);
  data[1] = decToBcd((uint8_t)tm.tm_min);
  data[2] = decToBcd((uint8_t)tm.tm_hour);
  data[3] = 0x00;                                  // 星期 (未用)
  data[4] = decToBcd((uint8_t)tm.tm_mday);
  data[5] = decToBcd((uint8_t)(tm.tm_mon + 1));
  data[6] = decToBcd((uint8_t)((tm.tm_year + 1900) % 100));
  SPI.end();   // 释放 SPI 占用, 否则 Wire 无法驱动 GPIO13/14 (与探测同理)
  pinMode(15, OUTPUT); digitalWrite(15, HIGH);
  pinMode(5, OUTPUT); digitalWrite(5, HIGH);
  Wire.begin(rtcI2C_SDA, rtcI2C_SCL);   // 用探测到的引脚 (13/14 或 4/5)
  Wire.setClock(100000);
  bool ok = rtc8025WriteRaw(data);
  SPI.begin();  // 恢复 SPI
  Serial.printf_P(PSTR("CLOCK_8025T_SAVE ok=%d epoch=%lu\n"), ok ? 1 : 0, (unsigned long)epochUtc);
  return ok;
}

// 读取当前时间; 调用前需保证 Wire 已初始化; 返回是否有效
static bool rtc8025ReadEpoch(time_t &epoch) {
  if (!rtc8025Present) return false;
  SPI.end();   // 释放 SPI 占用, 否则 Wire 无法驱动 GPIO13/14 (与探测同理)
  pinMode(15, OUTPUT); digitalWrite(15, HIGH);
  pinMode(5, OUTPUT); digitalWrite(5, HIGH);
  Wire.begin(rtcI2C_SDA, rtcI2C_SCL);   // 用探测到的引脚 (13/14 或 4/5)
  Wire.setClock(100000);
  uint8_t buf[16];
  memset(buf, 0xFF, sizeof(buf));
  bool ok = rtc8025ReadRaw(buf) && rtc8025Parse(buf, rtc8025TypeRX ? 8 : 0, epoch);
  SPI.begin();  // 恢复 SPI
  return ok;
}

// A7 对齐: 同步时间后写入时钟芯片的结果提示。成功显示"读取数据正常",
// 失败显示"数据出错或不存在，使用软件时钟"并降级写 EEPROM(软件时钟)。
static char clockChipMessage[64] = "";
static bool clockChipOk = false;

namespace {
const size_t EEPROM_SIZE = 1024;  // 512→1024: 原有 0-508 布局不变, 新增管理区(管理/OTA 密码) 520+
                                  // ESP8266 EEPROM 模拟以 4KB flash 扇区承载, begin(1024) 不影响已有区域
const int EEPROM_ADDR = 0;
const uint32_t CONFIG_MAGIC = 0x57494649UL;
const uint8_t CONFIG_VERSION = 1;
const char AP_PASSWORD[] = "333333333";
const uint32_t STA_TIMEOUT_MS = 15000UL;
const uint32_t NTP_TIMEOUT_MS = 12000UL;
const char NTP_SERVER[] = "cn.pool.ntp.org";
const uint32_t RTC_CLOCK_MAGIC = 0x52544354UL;
// ESP.rtcUserMemory 块号 (0-127, 每块4字节): 0 ↔ system_rtc_mem 字64 (用户区起始)
// 注意: 若启用 OTA, eboot 会占用用户区前 128 字节 (块 0-31); 本机为串口烧录无 OTA
const uint32_t RTC_CLOCK_OFFSET = 0;
const uint32_t RTC_TICKS_PER_SECOND = 150000UL;
const int CLOCK_EEPROM_ADDR = 112;
const uint32_t CLOCK_EEPROM_MAGIC = 0x434C4B33UL;
// 天气配置独立区（避开 WifiConfig 0-105 与 CLOCK 112-125; 160+72=232 <= 256）
const int WEATHER_EEPROM_ADDR = 160;
const uint32_t WEATHER_MAGIC = 0x57544852UL;

struct WifiConfig {
  uint32_t magic;
  uint8_t version;
  char ssid[33];
  char password[65];
  uint16_t checksum;
};

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;   // Web 固件升级 (/update)
WifiConfig config;
String apSsid;
String staIp;
uint32_t deadline = 0;
bool active = false;
bool wifiConnectPending = false;   // 保存 WiFi 且响应已发出后置位, 由 loop 下一轮断 AP 连 STA(照官方"保存立即返回")
int  wifiSaveResult = 0;           // 最近一次"保存并连接"结果: 0=进行中 1=成功 2=失败(前端轮询 /status 读取; 照官方 getData2 轮询)
char wifiSaveIp[16] = "";          // 连接成功时的 STA IP
void (*renderPage)(bool) = nullptr;
void (*exitPage)() = nullptr;
void (*clockRender)(bool) = nullptr;
void (*clockDone)() = nullptr;
time_t syncedTime = 0;
time_t lastCalibrationTime = 0;
uint32_t syncedAtMs = 0;
uint32_t syncedRtcTicks = 0;
bool rtcBaseValid = false;
bool clockSkippedSession = false;
uint32_t clockSuccessDeadline = 0;
struct RtcClockRecord {
  uint32_t magic;
  uint32_t epoch;
  uint32_t rtcTicks;
};
struct PersistClockRecord {
  uint32_t magic;
  uint32_t epoch;      // 上次校准时刻（lastCalibrationTime 的持久化）
  uint32_t savedAt;    // 最近一次已知墙钟时间（掉电兜底基准）
  uint16_t checksum;
};

enum State { IDLE, TRY_STA, STA_ONLY, CONNECTING, AP_ONLY, STA_AP, ERROR };
State state = IDLE;
enum ClockState { CLOCK_IDLE, CLOCK_STA, CLOCK_NTP, CLOCK_WEATHER, CLOCK_SUCCESS, CLOCK_SKIPPED, CLOCK_FAILED };
ClockState clockState = CLOCK_IDLE;
uint32_t clockDeadline = 0;

uint16_t checksumBytes(const uint8_t *bytes, size_t len) {
  uint16_t sum = 0x5A5A;
  for (size_t i = 0; i < len; ++i) {
    sum = static_cast<uint16_t>((sum << 5) ^ (sum >> 11) ^ bytes[i]);
  }
  return sum;
}

uint16_t checksum(const WifiConfig &value) {
  return checksumBytes(reinterpret_cast<const uint8_t *>(&value),
                       offsetof(WifiConfig, checksum));
}

uint16_t weatherChecksum(const WeatherConfig &value) {
  return checksumBytes(reinterpret_cast<const uint8_t *>(&value),
                       offsetof(WeatherConfig, checksum));
}

// ---- 管理配置（EEPROM 偏移 520, 'ADMN'; 独立区, 避开 WifiConfig/CLOCK/Weather/Settings/Webdav/Target）----
// 分层原则: 普通配置(WiFi 凭据/热点) < 管理员(所有写端点) < OTA(/update)。
// 管理密码与 OTA 密码相互独立、均不依赖 AP 密码; 两者都由用户在配网页设置, 设备无随机密码机制。
const int ADMIN_EEPROM_ADDR = 520;
const uint32_t ADMIN_MAGIC = 0x41444D4EUL;   // 'ADMN'
const uint8_t ADMIN_VERSION = 1;
struct AdminConfig {
  uint32_t magic;
  uint8_t version;
  char password[32];    // ⚠️ 已废弃（管理密码机制删除）; 保留字段仅为 EEPROM 布局/checksum 兼容, 永不再用于鉴权
  char otaPass[32];     // OTA 升级密码（空=未设置; 未设置时 /update 不可用）
  uint16_t checksum;
};
static_assert(ADMIN_EEPROM_ADDR >= 508, "Admin region overlaps Target region (470-508)");
static_assert(ADMIN_EEPROM_ADDR + sizeof(AdminConfig) <= EEPROM_SIZE,
              "Admin region overflows EEPROM_SIZE");

uint16_t adminChecksum(const AdminConfig &value) {
  return checksumBytes(reinterpret_cast<const uint8_t *>(&value),
                       offsetof(AdminConfig, checksum));
}

bool loadAdminConfig(AdminConfig &out) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(ADMIN_EEPROM_ADDR, out);
  if (out.magic != ADMIN_MAGIC || out.version != ADMIN_VERSION ||
      adminChecksum(out) != out.checksum) {
    // 首次使用/损坏: 清空（未设置）, 内存标记 magic; 不写 EEPROM
    memset(&out, 0, sizeof(out));
    out.magic = ADMIN_MAGIC;
    out.version = ADMIN_VERSION;
    return false;
  }
  out.password[sizeof(out.password) - 1] = '\0';
  out.otaPass[sizeof(out.otaPass) - 1] = '\0';
  return true;
}

bool saveAdminConfig(const AdminConfig &in) {
  AdminConfig cfg = in;
  cfg.magic = ADMIN_MAGIC;
  cfg.version = ADMIN_VERSION;
  cfg.checksum = adminChecksum(cfg);
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(ADMIN_EEPROM_ADDR, cfg);
  return EEPROM.commit();
}

// ---- Web 设置修改 → 墨水屏提示（每次保存成功回调墨水瓶: line1=标题 line2=变更摘要）----
static WebSettingsNotifyCb g_webNotifyCb = nullptr;   // 匿名区内部回调
void webNotifyCbSet(WebSettingsNotifyCb cb) { g_webNotifyCb = cb; }   // 匿名区名, 导出区转发调用
static void fireWebNotify(const char *l1, const char *l2) {
  Serial.printf_P(PSTR("WEB_NOTIFY %s | %s\n"), l1 ? l1 : "", l2 ? l2 : "");
  if (g_webNotifyCb) g_webNotifyCb(l1, l2 ? l2 : "");
}
// 摘要缓冲: 各保存 handler 逐项 webNotifyAdd("选项=值 ") 后 webNotifyDone(标题) 统一回调
static char g_webNotifySum[72];
static void webNotifyReset() { g_webNotifySum[0] = '\0'; }
static void webNotifyAdd(const char *item) {
  size_t b = strlen(item);
  if (b == 0) return;   // 空串直接忽略(防 memcpy 截断已有摘要)
  size_t a = strlen(g_webNotifySum);
  if (a + b >= sizeof(g_webNotifySum)) return;   // 超长丢弃尾部, 保前段
  memcpy(g_webNotifySum + a, item, b + 1);
}
static void webNotifyDone(const char *title) {
  fireWebNotify(title, g_webNotifySum[0] ? g_webNotifySum : "");   // l2 空=只显示标题(如"正在连接"), 不再 fallback 杂文案
  g_webNotifySum[0] = '\0';
}

// 天气 KEY 掩码: 已配置显示 "****…****<尾4>", 未配置返回空; 配网页不回显明文
static String maskWeatherKey(const char *key) {
  size_t len = strlen(key);
  if (len == 0) return String("");
  String m;
  m.reserve(len);
  size_t keep = (len > 4) ? 4 : 0;
  for (size_t i = 0; i < len - keep; i++) m += '*';
  if (keep > 0) m += (key + (len - keep));
  return m;
}

void savePersistedClock(time_t epoch, time_t savedAt);

bool loadRtcClock() {
  RtcClockRecord record;
  if (!ESP.rtcUserMemoryRead(RTC_CLOCK_OFFSET, reinterpret_cast<uint32_t *>(&record), sizeof(record))) return false;
  if (record.magic != RTC_CLOCK_MAGIC || record.epoch < 1600000000UL) return false;
  uint32_t ticks = system_get_rtc_time();
  if (ticks < record.rtcTicks) {
    // RTC 计数器倒退 = 电池断开/完全掉电过 (计数器从0重新计数), 记录基准失效
    Serial.println(F("CLOCK_RTC_TICKS_REGRESS"));
    return false;
  }
  syncedTime = record.epoch + (ticks - record.rtcTicks) / RTC_TICKS_PER_SECOND;
  lastCalibrationTime = record.epoch;
  syncedRtcTicks = ticks;
  rtcBaseValid = true;
  syncedAtMs = millis();
  Serial.printf_P(PSTR("CLOCK_RTC_READ epoch=%lu ticks=%lu\n"), (unsigned long)syncedTime, (unsigned long)ticks);
  return true;
}

void saveRtcClock(time_t epoch) {
  RtcClockRecord record = { RTC_CLOCK_MAGIC, static_cast<uint32_t>(epoch), system_get_rtc_time() };
  bool rtcOk = ESP.rtcUserMemoryWrite(RTC_CLOCK_OFFSET, reinterpret_cast<uint32_t *>(&record), sizeof(record));
  if (rtcOk) Serial.printf_P(PSTR("CLOCK_RTC_SAVE epoch=%lu ticks=%lu\n"), (unsigned long)epoch, (unsigned long)record.rtcTicks);
  else Serial.println(F("CLOCK_RTC_SAVE_FAIL"));
  syncedTime = epoch;
  lastCalibrationTime = epoch;
  syncedRtcTicks = record.rtcTicks;
  rtcBaseValid = rtcOk;
  syncedAtMs = millis();
  savePersistedClock(epoch, epoch);
}

static uint16_t persistChecksum(const PersistClockRecord &record) {
  uint32_t v = record.magic ^ record.epoch ^ record.savedAt;
  return static_cast<uint16_t>(v ^ (v >> 16));
}

bool loadPersistedClock() {
  PersistClockRecord record;
  EEPROM.get(CLOCK_EEPROM_ADDR, record);
  if (record.magic != CLOCK_EEPROM_MAGIC || record.epoch < 1600000000UL ||
      persistChecksum(record) != record.checksum) return false;
  lastCalibrationTime = record.epoch;
  syncedTime = (record.savedAt > record.epoch) ? record.savedAt : record.epoch;
  rtcBaseValid = false;
  syncedAtMs = millis();
  Serial.printf_P(PSTR("CLOCK_EEPROM_READ epoch=%lu savedAt=%lu\n"), (unsigned long)record.epoch, (unsigned long)record.savedAt);
  return true;
}

void savePersistedClock(time_t epoch, time_t savedAt) {
  PersistClockRecord record = { CLOCK_EEPROM_MAGIC, static_cast<uint32_t>(epoch), static_cast<uint32_t>(savedAt), 0 };
  record.checksum = persistChecksum(record);
  EEPROM.put(CLOCK_EEPROM_ADDR, record);
  if (EEPROM.commit()) Serial.printf_P(PSTR("CLOCK_EEPROM_SAVE epoch=%lu savedAt=%lu\n"), (unsigned long)epoch, (unsigned long)savedAt);
  else Serial.println(F("CLOCK_EEPROM_SAVE_FAIL"));
}

// A7 对齐: 同步时间(校时)后写入时钟芯片。写入成功 → 提示"时钟芯片：读取数据正常";
// 写入失败/芯片不在线 → 提示"时钟芯片：数据出错或不存在，使用软件时钟"并降级写 EEPROM(软件时钟)。
static void clockManagerPersistSync(time_t t) {
  bool ok = rtc8025Present && rtc8025WriteEpoch(t);
  clockChipOk = ok;
  if (ok) {
    strncpy(clockChipMessage, "时钟芯片：读取数据正常", sizeof(clockChipMessage) - 1);
  } else {
    strncpy(clockChipMessage, "时钟芯片：数据出错或不存在，使用软件时钟", sizeof(clockChipMessage) - 1);
    savePersistedClock(t, t);
  }
}

bool loadConfig() {
  memset(&config, 0, sizeof(config));
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(EEPROM_ADDR, config);
  config.ssid[sizeof(config.ssid) - 1] = '\0';
  config.password[sizeof(config.password) - 1] = '\0';
  return config.magic == CONFIG_MAGIC && config.version == CONFIG_VERSION &&
         config.ssid[0] != '\0' && strlen(config.password) >= 8 &&
         checksum(config) == config.checksum;
}

bool saveConfig(const String &ssid, const String &password) {
  if (ssid.length() == 0 || ssid.length() >= sizeof(config.ssid) ||
      password.length() < 8 || password.length() >= sizeof(config.password)) return false;
  memset(&config, 0, sizeof(config));
  config.magic = CONFIG_MAGIC;
  config.version = CONFIG_VERSION;
  ssid.toCharArray(config.ssid, sizeof(config.ssid));
  password.toCharArray(config.password, sizeof(config.password));
  config.checksum = checksum(config);
  EEPROM.put(EEPROM_ADDR, config);
  return EEPROM.commit();
}

void clearConfig() {
  memset(&config, 0, sizeof(config));
  EEPROM.put(EEPROM_ADDR, config);
  EEPROM.commit();
}

// ---- 天气配置 ----

const char *stateText() {
  switch (state) {
    case TRY_STA: return "正在连接 WiFi";
    case STA_ONLY: return "WiFi 已连接(局域网管理)";
    case CONNECTING: return "正在连接 WiFi";
    case AP_ONLY: return "热点配网模式";
    case STA_AP: return "WiFi 已连接，热点保持开启";
    case ERROR: return "WiFi 连接失败，热点保持开启";
    default: return "网络管理未启动";
  }
}

// HTML 转义（配网页预填值防注入/断标签）
static String escapeHtml(const char *s) {
  String out;
  for (const char *p = s; *p; ++p) {
    switch (*p) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += *p;
    }
  }
  return out;
}

// JSON 字符串转义（管理 API 端点用；HTML 实体不是合法 JSON 转义）
static String jsonEscape(const char *s) {
  String out;
  for (const char *p = s; *p; ++p) {
    char c = *p;
    switch (c) {
      case '"': out += F("\\\""); break;
      case '\\': out += F("\\\\"); break;
      case '\n': out += F("\\n"); break;
      case '\r': out += F("\\r"); break;
      case '\t': out += F("\\t"); break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04X", (uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

void handleStatus() {
  String json = F("{\"state\":\"");
  json += stateText();
  json += F("\",\"apSsid\":\"");
  json += apSsid;
  json += F("\",\"apIp\":\"192.168.4.1\",\"staConnected\":");
  json += (WiFi.status() == WL_CONNECTED) ? F("true") : F("false");
  json += F(",\"staIp\":\"");
  json += WiFi.localIP().toString();
  json += F("\",\"wifiSaveResult\":");
  json += wifiSaveResult;
  json += F(",\"wifiSaveIp\":\"");
  json += wifiSaveIp;
  json += F("\"}");
  server.send(200, "application/json; charset=utf-8", json);
}

void handleSave() {
  bool hasWeather = server.hasArg("city") || server.hasArg("wkey") || server.hasArg("night");
  if (hasWeather) {
    WeatherConfig wc;
    loadWeatherConfig(wc);
    String city = server.arg("city");
    String key = server.arg("wkey");
    if (city.length() > 0 && city.length() < sizeof(wc.city)) {
      city.toCharArray(wc.city, sizeof(wc.city));
    }
    // 天气 KEY 不回显/不覆盖: 提交为空或等于掩码占位值 → 保留已有 KEY（掩码由 handleRoot/maskWeatherKey 生成）
    if (key.length() > 0 && key != maskWeatherKey(wc.key)) {
      key.toCharArray(wc.key, sizeof(wc.key));
    }
    wc.nightUpdata = (server.arg("night") == "1") ? 1 : 0;
    if (!saveWeatherConfig(wc)) {
      server.send_P(400, PSTR("text/plain; charset=utf-8"), PSTR("天气配置长度无效"));
      return;
    }
    Serial.printf_P(PSTR("WEATHER_WEB_SAVE cityLen=%u night=%u\n"),
                  static_cast<unsigned>(city.length()),
                  static_cast<unsigned>(wc.nightUpdata));
    webNotifyReset();
    char wnb[64];
    if (city.length() > 0) { snprintf(wnb, sizeof(wnb), "城市=%s ", city.c_str()); webNotifyAdd(wnb); }
    if (key.length() > 0) webNotifyAdd("私钥=已保存 ");
  }
  if (!server.hasArg("ssid") || !server.hasArg("password")) {
    if (hasWeather) {
      server.send_P(200, PSTR("text/html; charset=utf-8"), PSTR("<meta charset='utf-8'><p>天气设置已保存。</p><a href='/'>返回</a>"));
      webNotifyDone("修改成功");
      return;
    }
    server.send_P(400, PSTR("text/plain; charset=utf-8"), PSTR("缺少 WiFi 名称或密码"));
    return;
  }
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (!saveConfig(ssid, password)) {
    server.send_P(400, PSTR("text/plain; charset=utf-8"), PSTR("输入长度无效"));
    return;
  }
  // ★ 照官方"保存立即返回 + 后台连接": 此处只保存凭据并【先发响应】(保持 AP), 不断 AP——避免停 AP 截断 HTTP 响应。
  //   断 AP → 切 STA → 连接 交给 loop 下一轮 wifiConnectPending 处理(响应已发出, fetch 不再 Failed to fetch)。
  Serial.printf_P(PSTR("WIFI_WEB_SAVE ssidLen=%u\n"), static_cast<unsigned>(ssid.length()));
  server.send_P(200, PSTR("text/html; charset=utf-8"), PSTR("<meta charset='utf-8'><p>已保存，正在连接 WiFi…</p><a href='/'>返回</a>"));
  wifiSaveResult = 0; wifiSaveIp[0] = '\0';   // 重置为"进行中"
  webNotifyReset();
  webNotifyDone("正在连接");   // 只显示标题, 不出现"WIFI=连接中"之类生硬前缀
  wifiConnectPending = true;   // 响应送完后由 loop 断 AP 连 STA
}

void handleClear() {
  clearConfig();
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  state = AP_ONLY;
  Serial.println(F("WIFI_WEB_CLEAR"));
  server.sendHeader("Location", "/");
  server.send(303);
  webNotifyReset();
  webNotifyAdd("配置=已清除 ");
  webNotifyDone("修改成功");
}

// DHCP 固定租约: 仅 192.168.0.100 (手机热点模式固定地址, 规避 AP 池随机分配)。
// core 3.1.2 提供 wifi_softap_set_dhcps_lease (user_interface.h + cores/esp8266/LwipDhcpServer-NonOS.cpp)。
// 注意: set_dhcps_lease 要求 DHCP server 未运行 (isRunning()==false) → 必须先 stop 再 set 再 start。
static bool apSetFixedLease() {
  bool ok = false;
  wifi_softap_dhcps_stop();
  struct dhcps_lease lease;
  lease.enable = true;
  lease.start_ip.addr = IPAddress(192, 168, 0, 100).v4();
  lease.end_ip.addr   = IPAddress(192, 168, 0, 100).v4();
  ok = wifi_softap_set_dhcps_lease(&lease);
  wifi_softap_dhcps_start();
  Serial.printf_P(PSTR("WIFI_AP_LEASE fixed=192.168.0.100 ok=%d\n"), ok ? 1 : 0);
  return ok;
}

void startAp() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[16];
  snprintf(name, sizeof(name), "MSP-%02X%02X", mac[4], mac[5]);
  apSsid = name;
  // ★ 分阶段 WiFi 模式(用户定稿): 常态/扫描 = 纯 AP(WIFI_AP, 堆低, 扫描不 OOM);
  //   仅"保存连接"时临时切共存(WIFI_AP_STA) 连 STA; 连接成功/失败后回纯 AP(关 STA, 管理 web 在线)。
  //   避 core 3.1.2 WiFi.mode L430 memcpy(wifi_station_hostname) 空指针崩: 先 wifi_fpm_set_sleep_type(NONE_SLEEP_T),
  //   顺序用官方 initAp(): softAPConfig → softAP → mode 最后。
  auditHeap("ap_before");
  wifi_fpm_set_sleep_type(NONE_SLEEP_T);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(apSsid.c_str(), AP_PASSWORD, 1, 0, 1);
  WiFi.mode(WIFI_AP);
  auditHeap("ap_created");
  Serial.printf_P(PSTR("WIFI_AP_START ssid=%s ok=%d ip=%s\n"), apSsid.c_str(), ok ? 1 : 0, WiFi.softAPIP().toString().c_str());
}

}

// ---- 天气配置（定义在匿名命名空间外，与 wifi_manager.h 的全局声明匹配；
//     匿名空间内符号（WEATHER_EEPROM_ADDR/WEATHER_MAGIC/weatherChecksum）同编译单元可见）----
bool loadWeatherConfig(WeatherConfig &out) {
  EEPROM.begin(EEPROM_SIZE);   // 确保已初始化（天气页路径不经过 wifiManagerBegin/clockManagerBegin）
  EEPROM.get(WEATHER_EEPROM_ADDR, out);
  if (out.magic != WEATHER_MAGIC || weatherChecksum(out) != out.checksum) {
    // 首次使用/损坏：填默认值（城市"深圳"，夜间开关关）
    memset(&out, 0, sizeof(out));
    out.magic = WEATHER_MAGIC;
    strncpy(out.city, "深圳", sizeof(out.city) - 1);
    out.nightUpdata = 0;
    return false;
  }
  return true;
}

bool saveWeatherConfig(const WeatherConfig &in) {
  if (in.city[0] == '\0' || strlen(in.city) >= sizeof(in.city) ||
      strlen(in.key) >= sizeof(in.key)) return false;
  WeatherConfig cfg = in;
  cfg.magic = WEATHER_MAGIC;
  cfg.checksum = weatherChecksum(cfg);
  EEPROM.begin(EEPROM_SIZE);   // 确保已初始化（配网页外路径也能保存）
  EEPROM.put(WEATHER_EEPROM_ADDR, cfg);
  return EEPROM.commit();
}

// ---- 设备设置（EEPROM 偏移 232, 独立区; 结构扩展, 232+68=300 <= 360）----
const int SETTINGS_EEPROM_ADDR = 232;
const uint32_t SETTINGS_MAGIC = 0x53455433UL;   // 'SET3'
const int16_t DEFAULT_TZ_OFFSET_MIN = 480;       // UTC+8
// 编译期保护：checksum 之前字段(到 tzOffsetMin)不超 24 字节; 整个结构不超 360 让位区
static_assert(offsetof(SettingsConfig, checksum) + sizeof(uint16_t) <= 24,
              "SettingsConfig checksum region too large");
static_assert(SETTINGS_EEPROM_ADDR + sizeof(SettingsConfig) <= 360,
              "Settings region overflows (must stay <= 360 before Webdav)");

// ---- WebDAV 配置（EEPROM 偏移 360, 独立区; 结构 150 字节, 360+150=510 <= 512）----
const int WEBDAV_EEPROM_ADDR = 360;
const uint32_t WEBDAV_MAGIC = 0x57445632UL;   // 'WDV2'
static_assert(WEBDAV_EEPROM_ADDR + sizeof(WebdavConfig) <= 512,
              "Webdav region overflows EEPROM_SIZE");

uint16_t webdavChecksum(const WebdavConfig &value) {
  return checksumBytes(reinterpret_cast<const uint8_t *>(&value),
                       offsetof(WebdavConfig, checksum));
}

bool loadWebdavConfig(WebdavConfig &out) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(WEBDAV_EEPROM_ADDR, out);
  if (out.magic != WEBDAV_MAGIC || webdavChecksum(out) != out.checksum) {
    memset(&out, 0, sizeof(out));
    out.magic = WEBDAV_MAGIC;   // 未配置/损坏: 清空但标记 magic(首字节空即未配置)
    return false;
  }
  return true;
}

bool saveWebdavConfig(const WebdavConfig &in) {
  if (in.endpoint[0] == '\0' || strlen(in.endpoint) >= sizeof(in.endpoint) ||
      strlen(in.username) >= sizeof(in.username) || strlen(in.password) >= sizeof(in.password)) return false;
  WebdavConfig cfg = in;
  cfg.magic = WEBDAV_MAGIC;
  cfg.checksum = webdavChecksum(cfg);
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(WEBDAV_EEPROM_ADDR, cfg);
  return EEPROM.commit();
}

uint16_t settingsChecksum(const SettingsConfig &value) {
  return checksumBytes(reinterpret_cast<const uint8_t *>(&value),
                       offsetof(SettingsConfig, checksum));
}

// 新设置字段默认值/容错：checksum 之后字段不参与校验，非法或 0xFF 残留→填默认
void settingsFillDefaults(SettingsConfig &s) {
  if (s.longPressMs == 0 || s.longPressMs == 0xFFFF) s.longPressMs = 500;
  // ntpServer: 首字节非法(0xFF/0) → 填默认
  if (s.ntpServer[0] == 0 || s.ntpServer[0] == 0xFF) strncpy(s.ntpServer, "cn.pool.ntp.org", sizeof(s.ntpServer) - 1);
  if (s.sdFrequency == 0 || s.sdFrequency > 40) s.sdFrequency = 20;
  if (s.fullRefreshMin == 0 || s.fullRefreshMin > 120) s.fullRefreshMin = 25;
  if (s.calibIntervalMin == 0 || s.calibIntervalMin > 720) s.calibIntervalMin = 60;
  if (s.batDisplayType > 1) s.batDisplayType = 1;
  if (s.nightUpdate > 1) s.nightUpdate = 1;
  if (s.fastFlip > 1) s.fastFlip = 1;
  if (s.setRotation > 3) s.setRotation = 1;
  if (s.outputPower == 0 || s.outputPower > 20) s.outputPower = 19;
  if (s.sdEnabled > 1) s.sdEnabled = 1;   // 非法残留/旧默认0 → 归一为启用SD（本地介质仅显式关闭时启用）
  if (s.albumAuto > 1) s.albumAuto = 0;
  // 2026-09 Web 层新增字段: 0xFF 残留/非法 → 默认（真实迁移由 loadSettingsConfig 的 version≤2 门控处理）
  if (s.historyEnabled > 1) s.historyEnabled = 1;
  if (s.clockCalibrationState > 1) s.clockCalibrationState = 1;
  if (s.clockMod > 1) s.clockMod = 0;
  if (s.clockCompensate == static_cast<int16_t>(0x8000)) s.clockCompensate = 0;  // -32768 = 哨兵(setter 已排除)
}

bool loadSettingsConfig(SettingsConfig &out) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(SETTINGS_EEPROM_ADDR, out);
  if (out.magic != SETTINGS_MAGIC || (out.version != 1 && out.version != 2 && out.version != 3) ||
      settingsChecksum(out) != out.checksum) {
    // 首次使用/损坏：填默认值（24 小时制 / UTC+8 / 一言开 / 默认启用 SD）
    memset(&out, 0, sizeof(out));
    out.magic = SETTINGS_MAGIC;
    out.version = 3;
    out.clockFormat = 0;
    out.tzOffsetMin = DEFAULT_TZ_OFFSET_MIN;
    out.hitokotoEnabled = 1;
    out.sdEnabled = 1;   // 默认启用 SD（关介质需显式设置 → 本地 flash）
    out.historyEnabled = 1;            // 默认: 历史记录开
    out.clockCalibrationState = 1;     // 默认: 时钟强制校准开
    settingsFillDefaults(out);
    return false;
  }
  // 兼容旧数据：checksum 之后的新字段可能是 EEPROM 残留（0xFF 或旧 padding）
  if (out.hitokotoEnabled > 1) out.hitokotoEnabled = 1;
  if (out.portrait > 3) out.portrait = 0;   // 四向: 0横/1竖/2横翻/3竖翻; 非法残留(如 0xFF)→旧默认横屏
  // v1→v2: 旧固件无"SD 介质"开关, sdEnabled 残留默认 0 视作"未显式设置" → 迁移为启用 SD;
  // v2 之后用户显式关闭(0) 才保持本地介质。本迁移仅于内存, 下轮真正 save 以 v2 写回持久。
  if (out.version == 1) out.sdEnabled = 1;
  // v2→v3 (2026-09): 旧版数据无 Web 层新字段(history/clockCalibrationState/clockMod/clockCompensate),
  // 其残留(0xFF 等)不算真实值 → 一律按默认; 新版(v3 起)保存后才信任字段值。
  if (out.version <= 2) {
    out.historyEnabled = 1;
    out.clockCalibrationState = 1;
    out.clockMod = 0;
    out.clockCompensate = 0;
  }
  // 新字段: 非法残留/无值 → 填默认(0xFF 或 0 均视为未设置)
  settingsFillDefaults(out);
  return true;
}

bool saveSettingsConfig(const SettingsConfig &in) {
  SettingsConfig cfg = in;
  cfg.magic = SETTINGS_MAGIC;
  cfg.version = 3;
  if (cfg.hitokotoEnabled > 1) cfg.hitokotoEnabled = 1;
  if (cfg.portrait > 3) cfg.portrait = 0;   // 四向: 0横/1竖/2横翻/3竖翻
  cfg.checksum = settingsChecksum(cfg);
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(SETTINGS_EEPROM_ADDR, cfg);
  return EEPROM.commit();
}

// ---- InAWord 多功能输入框（EEPROM 独立块, 偏移 600; 避开 Settings 232-300/Webdav/Target/Admin 520+）----
const int INAWORD_EEPROM_ADDR = 600;
const uint32_t INAWORD_MAGIC = 0x494E4157UL;   // 'INAW'
struct InAWordConfig {
  uint32_t magic;      // 'INAW'
  char text[64];       // 原文 UTF-8（保存时净化; 空串 = 一言模式）
  uint16_t checksum;
};
static_assert(INAWORD_EEPROM_ADDR >= ADMIN_EEPROM_ADDR + sizeof(AdminConfig),
              "InAWord region overlaps Admin region");
static_assert(INAWORD_EEPROM_ADDR + sizeof(InAWordConfig) <= EEPROM_SIZE,
              "InAWord region overflows EEPROM_SIZE");

uint16_t inAWordChecksum(const InAWordConfig &value) {
  return checksumBytes(reinterpret_cast<const uint8_t *>(&value), offsetof(InAWordConfig, checksum));
}

static void loadInAWordConfig(InAWordConfig &out) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(INAWORD_EEPROM_ADDR, out);
  if (out.magic != INAWORD_MAGIC || inAWordChecksum(out) != out.checksum) {
    // 首次/损坏: 空文本（一言模式）
    memset(&out, 0, sizeof(out));
    out.magic = INAWORD_MAGIC;
  }
  out.text[sizeof(out.text) - 1] = '\0';
}

// 净化 + UTF-8 整字截断: 剔除 `"` `\` 与控制字符, 不劈开多字节字符; dst 容量 ≥ 2
static void sanitizeInAWord(const char *src, char *dst, size_t cap) {
  size_t n = strlen(src), i = 0, wi = 0;
  if (cap < 2) { if (cap) dst[0] = '\0'; return; }
  while (i < n && wi + 1 < cap) {
    unsigned char c = (unsigned char)src[i];
    if (c == '"' || c == '\\' || c < 0x20) { ++i; continue; }
    int len = 1;
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) len = 3;
    else if (c >= 0xF0 && c <= 0xF4) len = 4;
    bool ok = (size_t)len <= n - i;
    for (int k = 1; ok && k < len; ++k) {
      unsigned char nx = (unsigned char)src[i + k];
      if (nx < 0x80 || nx > 0xBF) ok = false;
    }
    if (!ok) { ++i; continue; }            // 非法/孤立序列: 跳过该字节
    if (wi + (size_t)len >= cap) break;    // 放不下整字符 → 截断(不劈开)
    for (int k = 0; k < len; ++k) dst[wi++] = src[i + k];
    i += (size_t)len;
  }
  dst[wi] = '\0';
}

const char* settingsGetInAWord() {
  static char buf[sizeof(InAWordConfig().text)];
  InAWordConfig cfg;
  loadInAWordConfig(cfg);
  strncpy(buf, cfg.text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

bool settingsSetInAWord(const char *v) {
  if (!v) return false;
  InAWordConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  sanitizeInAWord(v, cfg.text, sizeof(cfg.text));
  cfg.text[sizeof(cfg.text) - 1] = '\0';
  cfg.magic = INAWORD_MAGIC;
  cfg.checksum = inAWordChecksum(cfg);
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(INAWORD_EEPROM_ADDR, cfg);
  return EEPROM.commit();
}

uint8_t settingsGetClockFormat() {
  SettingsConfig s;
  loadSettingsConfig(s);
  return s.clockFormat;
}

int16_t settingsGetTzOffsetMin() {
  SettingsConfig s;
  loadSettingsConfig(s);
  return s.tzOffsetMin;
}

bool settingsSetClockFormat(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s;
  loadSettingsConfig(s);
  s.clockFormat = v;
  return saveSettingsConfig(s);
}

bool settingsSetTzOffsetMin(int16_t v) {
  if (v < -720 || v > 840) return false;   // UTC-12 .. UTC+14
  SettingsConfig s;
  loadSettingsConfig(s);
  s.tzOffsetMin = v;
  return saveSettingsConfig(s);
}

uint8_t settingsGetHitokotoEnabled() {
  SettingsConfig s;
  loadSettingsConfig(s);
  return s.hitokotoEnabled;
}

bool settingsSetHitokotoEnabled(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s;
  loadSettingsConfig(s);
  s.hitokotoEnabled = v;
  return saveSettingsConfig(s);
}

uint8_t settingsGetPortrait() {
  SettingsConfig s;
  loadSettingsConfig(s);
  return s.portrait;
}

bool settingsSetPortrait(uint8_t v) {
  if (v > 3) return false;   // 四向: 0横/1竖/2横翻/3竖翻
  SettingsConfig s;
  loadSettingsConfig(s);
  s.portrait = v;
  return saveSettingsConfig(s);
}

uint16_t settingsGetLongPressMs() {
  SettingsConfig s; loadSettingsConfig(s); return s.longPressMs;
}
bool settingsSetLongPressMs(uint16_t v) {
  if (v < 100 || v > 5000) return false;
  SettingsConfig s; loadSettingsConfig(s); s.longPressMs = v; return saveSettingsConfig(s);
}
const char* settingsGetNtpServer() {
  // ⚠️ 返回静态缓冲, 不能返回局部 SettingsConfig s.ntpServer (悬垂指针, %s 读野指针崩溃 Fatal exception:28)
  static char buf[sizeof(SettingsConfig().ntpServer)];
  SettingsConfig s; loadSettingsConfig(s);
  strncpy(buf, s.ntpServer, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}
bool settingsSetNtpServer(const char *v) {
  if (!v || strlen(v) >= sizeof(SettingsConfig().ntpServer)) return false;
  SettingsConfig s; loadSettingsConfig(s); strncpy(s.ntpServer, v, sizeof(s.ntpServer) - 1);
  s.ntpServer[sizeof(s.ntpServer) - 1] = '\0'; return saveSettingsConfig(s);
}
uint8_t settingsGetSdFrequency() {
  SettingsConfig s; loadSettingsConfig(s); return s.sdFrequency;
}
bool settingsSetSdFrequency(uint8_t v) {
  if (v < 1 || v > 80) return false;
  SettingsConfig s; loadSettingsConfig(s); s.sdFrequency = v; return saveSettingsConfig(s);
}
uint8_t settingsGetFullRefreshMin() {
  SettingsConfig s; loadSettingsConfig(s); return s.fullRefreshMin;
}
bool settingsSetFullRefreshMin(uint8_t v) {
  if (v < 1 || v > 120) return false;
  SettingsConfig s; loadSettingsConfig(s); s.fullRefreshMin = v; return saveSettingsConfig(s);
}
uint8_t settingsGetCalibIntervalMin() {
  SettingsConfig s; loadSettingsConfig(s); return s.calibIntervalMin;
}
bool settingsSetCalibIntervalMin(uint8_t v) {
  if (v < 1 || v > 720) return false;
  SettingsConfig s; loadSettingsConfig(s); s.calibIntervalMin = v; return saveSettingsConfig(s);
}
uint8_t settingsGetBatDisplayType() {
  SettingsConfig s; loadSettingsConfig(s); return s.batDisplayType;
}
bool settingsSetBatDisplayType(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s; loadSettingsConfig(s); s.batDisplayType = v; return saveSettingsConfig(s);
}
uint8_t settingsGetNightUpdate() {
  SettingsConfig s; loadSettingsConfig(s); return s.nightUpdate;
}
bool settingsSetNightUpdate(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s; loadSettingsConfig(s); s.nightUpdate = v; return saveSettingsConfig(s);
}
uint8_t settingsGetFastFlip() {
  SettingsConfig s; loadSettingsConfig(s); return s.fastFlip;
}
bool settingsSetFastFlip(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s; loadSettingsConfig(s); s.fastFlip = v; return saveSettingsConfig(s);
}
uint8_t settingsGetSetRotation() {
  SettingsConfig s; loadSettingsConfig(s); return s.setRotation;
}
bool settingsSetSetRotation(uint8_t v) {
  if (v > 3) return false;
  SettingsConfig s; loadSettingsConfig(s); s.setRotation = v; return saveSettingsConfig(s);
}
uint8_t settingsGetOutputPower() {
  SettingsConfig s; loadSettingsConfig(s); return s.outputPower;
}
bool settingsSetOutputPower(uint8_t v) {
  if (v < 10 || v > 20) return false;
  SettingsConfig s; loadSettingsConfig(s); s.outputPower = v; return saveSettingsConfig(s);
}
uint8_t settingsGetSdEnabled() {
  SettingsConfig s; loadSettingsConfig(s); return s.sdEnabled;
}
bool settingsSetSdEnabled(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s; loadSettingsConfig(s); s.sdEnabled = v; return saveSettingsConfig(s);
}
uint8_t settingsGetAlbumAuto() {
  SettingsConfig s; loadSettingsConfig(s); return s.albumAuto;
}
bool settingsSetAlbumAuto(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s; loadSettingsConfig(s); s.albumAuto = v; return saveSettingsConfig(s);
}
uint8_t settingsGetHistoryEnabled() {
  SettingsConfig s; loadSettingsConfig(s); return s.historyEnabled;
}
bool settingsSetHistoryEnabled(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s; loadSettingsConfig(s); s.historyEnabled = v; return saveSettingsConfig(s);
}
uint8_t settingsGetClockCalibrationState() {
  SettingsConfig s; loadSettingsConfig(s); return s.clockCalibrationState;
}
bool settingsSetClockCalibrationState(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s; loadSettingsConfig(s); s.clockCalibrationState = v; return saveSettingsConfig(s);
}
uint8_t settingsGetClockMod() {
  SettingsConfig s; loadSettingsConfig(s); return s.clockMod;
}
bool settingsSetClockMod(uint8_t v) {
  if (v > 1) return false;
  SettingsConfig s; loadSettingsConfig(s); s.clockMod = v; return saveSettingsConfig(s);
}
int16_t settingsGetClockCompensate() {
  SettingsConfig s; loadSettingsConfig(s); return s.clockCompensate;
}
bool settingsSetClockCompensate(int16_t v) {
  SettingsConfig s; loadSettingsConfig(s); s.clockCompensate = v; return saveSettingsConfig(s);
}

// ---- Web 端点：设备设置（/settings）与系统信息（/info） ----
// 与 handleSave 分离：/wifi 管网络与天气，/settings 管设置页字段（时钟格式/时区/一言）
void handleSettingsSave() {
  bool any = false;
  webNotifyReset();
  char tb[64];
  if (server.hasArg("clockFormat")) {
    int v = server.arg("clockFormat").toInt();
    uint8_t nv = static_cast<uint8_t>(v == 1 ? 1 : 0);
    if (settingsGetClockFormat() != nv && settingsSetClockFormat(nv)) {
      any = true;
      snprintf(tb, sizeof(tb), "时钟格式=%s ", nv ? "12小时制" : "24小时制");
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("tz")) {
    int v = server.arg("tz").toInt();
    if (v >= -720 && v <= 840 && settingsGetTzOffsetMin() != v &&
        settingsSetTzOffsetMin(static_cast<int16_t>(v))) {
      any = true;
      int sign = v < 0 ? -1 : 1;
      int h = (v < 0 ? -v : v) / 60, m = (v < 0 ? -v : v) % 60;
      if (m == 0) snprintf(tb, sizeof(tb), "时区=UTC%c%d ", sign > 0 ? '+' : '-', h);
      else snprintf(tb, sizeof(tb), "时区=UTC%c%d:%02d ", sign > 0 ? '+' : '-', h, m);
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("hitokoto")) {
    int v = server.arg("hitokoto").toInt();
    uint8_t nv = static_cast<uint8_t>(v == 1 ? 1 : 0);
    if (settingsGetHitokotoEnabled() != nv && settingsSetHitokotoEnabled(nv)) {
      any = true;
      snprintf(tb, sizeof(tb), "一言=%s ", nv ? "开" : "关");
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("portrait")) {   // 阅读旋转方向 (全局持久, 非法值由 setter 拒绝); 通知显示度数
    int v = server.arg("portrait").toInt();
    uint8_t nv = static_cast<uint8_t>(v);
    if (settingsGetPortrait() != nv && settingsSetPortrait(nv)) {
      any = true;
      int deg = (nv == 1) ? 270 : (nv == 2) ? 180 : (nv == 3) ? 90 : 0;
      snprintf(tb, sizeof(tb), "阅读旋转=%d° ", deg);
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("longPress")) {
    int v = server.arg("longPress").toInt();
    if (settingsGetLongPressMs() != v && settingsSetLongPressMs(static_cast<uint16_t>(v))) {
      any = true;
      snprintf(tb, sizeof(tb), "长按=%ums ", static_cast<unsigned>(v));
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("ntpServer")) {
    String ns = server.arg("ntpServer");
    if (strcmp(settingsGetNtpServer(), ns.c_str()) != 0 && settingsSetNtpServer(ns.c_str())) {
      any = true;
      snprintf(tb, sizeof(tb), "NTP=%s ", ns.c_str());
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("sdFrequency")) {
    int v = server.arg("sdFrequency").toInt();
    if (settingsGetSdFrequency() != v && settingsSetSdFrequency(static_cast<uint8_t>(v))) {
      any = true;
      snprintf(tb, sizeof(tb), "SD频率=%uMHz ", static_cast<unsigned>(v));
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("fullRefresh")) {
    int v = server.arg("fullRefresh").toInt();
    if (settingsGetFullRefreshMin() != v && settingsSetFullRefreshMin(static_cast<uint8_t>(v))) {
      any = true;
      snprintf(tb, sizeof(tb), "全刷间隔=%u分钟 ", static_cast<unsigned>(v));
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("calibInterval")) {
    int v = server.arg("calibInterval").toInt();
    if (settingsGetCalibIntervalMin() != v && settingsSetCalibIntervalMin(static_cast<uint8_t>(v))) {
      any = true;
      snprintf(tb, sizeof(tb), "校准间隔=%u分钟 ", static_cast<unsigned>(v));
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("batDisplay")) {
    int v = server.arg("batDisplay").toInt();
    uint8_t nv = static_cast<uint8_t>(v == 1 ? 1 : 0);
    if (settingsGetBatDisplayType() != nv && settingsSetBatDisplayType(nv)) {
      any = true;
      snprintf(tb, sizeof(tb), "电量显示=%s ", nv ? "百分比" : "电压");
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("nightUpdate")) {
    int v = server.arg("nightUpdate").toInt();
    uint8_t nv = static_cast<uint8_t>(v == 1 ? 1 : 0);
    if (settingsGetNightUpdate() != nv && settingsSetNightUpdate(nv)) {
      any = true;
      snprintf(tb, sizeof(tb), "夜间更新=%s ", nv ? "更新" : "不更新");
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("fastFlip")) {
    int v = server.arg("fastFlip").toInt();
    uint8_t nv = static_cast<uint8_t>(v == 1 ? 1 : 0);
    if (settingsGetFastFlip() != nv && settingsSetFastFlip(nv)) {
      any = true;
      snprintf(tb, sizeof(tb), "快速翻页=%s ", nv ? "开" : "关");
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("setRotation")) {
    int v = server.arg("setRotation").toInt();
    if (settingsGetSetRotation() != v && settingsSetSetRotation(static_cast<uint8_t>(v))) {
      any = true;
      snprintf(tb, sizeof(tb), "屏幕旋转=方向%u ", static_cast<unsigned>(v));
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("outputPower")) {
    int v = server.arg("outputPower").toInt();
    if (settingsGetOutputPower() != v && settingsSetOutputPower(static_cast<uint8_t>(v))) {
      any = true;
      snprintf(tb, sizeof(tb), "功率=%udB ", static_cast<unsigned>(v));
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("sdEnabled")) {
    int v = server.arg("sdEnabled").toInt();
    uint8_t nv = static_cast<uint8_t>(v == 1 ? 1 : 0);
    if (settingsGetSdEnabled() != nv && settingsSetSdEnabled(nv)) {
      any = true;
      snprintf(tb, sizeof(tb), "SD卡=%s ", nv ? "启用" : "未启用");
      webNotifyAdd(tb);
    }
  }
  // ---- 2026-09 Web 层新增设置 (仅持久化+回显; 屏幕端行为后续战役) ----
  if (server.hasArg("history")) {
    int v = server.arg("history").toInt();
    uint8_t nv = static_cast<uint8_t>(v == 1 ? 1 : 0);
    if (settingsGetHistoryEnabled() != nv && settingsSetHistoryEnabled(nv)) {
      any = true;
      snprintf(tb, sizeof(tb), "历史=%s ", nv ? "开" : "关");
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("clockCalibrationState")) {
    int v = server.arg("clockCalibrationState").toInt();
    uint8_t nv = static_cast<uint8_t>(v == 1 ? 1 : 0);
    if (settingsGetClockCalibrationState() != nv && settingsSetClockCalibrationState(nv)) {
      any = true;
      snprintf(tb, sizeof(tb), "强制校准=%s ", nv ? "开" : "关");
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("clockMod")) {
    int v = server.arg("clockMod").toInt();
    uint8_t nv = static_cast<uint8_t>(v == 1 ? 1 : 0);
    if (settingsGetClockMod() != nv && settingsSetClockMod(nv)) {
      any = true;
      snprintf(tb, sizeof(tb), "时钟类型=%s ", nv ? "精美" : "简洁");
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("clockCompensate")) {
    int v = server.arg("clockCompensate").toInt();
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    if (settingsGetClockCompensate() != v && settingsSetClockCompensate(static_cast<int16_t>(v))) {
      any = true;
      snprintf(tb, sizeof(tb), "补偿=%d ", v);
      webNotifyAdd(tb);
    }
  }
  if (server.hasArg("inAWord")) {
    const char *cur = settingsGetInAWord();
    if (strcmp(cur, server.arg("inAWord").c_str()) != 0 && settingsSetInAWord(server.arg("inAWord").c_str())) {
      any = true;
      snprintf(tb, sizeof(tb), "自定义句=%s ", settingsGetInAWord()[0] ? "已存" : "已清");
      webNotifyAdd(tb);
    }
  }
  if (!any) {
    server.send_P(200, PSTR("text/plain; charset=utf-8"), PSTR("no_change"));   // 无实际变化: 不写 EEPROM 也不提示
    return;
  }
  configTime(settingsGetTzOffsetMin() * 60, 0, NTP_SERVER);   // 时区立即生效
  Serial.println(F("SETTINGS_WEB_SAVE"));
  server.send_P(200, PSTR("text/html; charset=utf-8"), PSTR("<meta charset='utf-8'><p>设置已保存。</p><a href='/'>返回</a>"));
  webNotifyDone("修改成功");
}

// ---- OTA 升级密码端点 (/admin) ----
// 管理密码机制已删除; 此处仅维护 OTA 升级密码。新密码留空 = 保持不变。
void handleAdminSave() {
  AdminConfig a;
  loadAdminConfig(a);
  String newOta = server.arg("otapass");
  if (newOta.length() >= sizeof(a.otaPass)) {
    server.send_P(400, PSTR("text/plain; charset=utf-8"), PSTR("密码长度无效"));
    return;
  }
  bool otaChanged = false;
  if (newOta.length() > 0) {
    memset(a.otaPass, 0, sizeof(a.otaPass));
    newOta.toCharArray(a.otaPass, sizeof(a.otaPass));
    otaChanged = true;
  }
  if (!saveAdminConfig(a)) {
    server.send_P(400, PSTR("text/plain; charset=utf-8"), PSTR("保存失败"));
    return;
  }
  Serial.println(F("ADMIN_WEB_SAVE"));
  server.send_P(200, PSTR("text/html; charset=utf-8"), PSTR("<meta charset='utf-8'><p>已保存（OTA 密码修改后，重启进入配网才生效）。</p><a href='/'>返回</a>"));
  if (otaChanged) {
    webNotifyReset();
    webNotifyAdd("OTA密码=已保存 ");
    webNotifyDone("修改成功");
  }
}

// ---- WebDAV 设置端点 (/webdav): ⚠️ 已弃用 (D0 起进度同步改直连手机 HTTP) ----
// 结构/EEPROM/保存函数保留 (WebdavConfig/loadWebdavConfig/saveWebdavConfig), 端点仅返回弃用提示。
void handleWebdavGet() {
  server.send_P(200, PSTR("text/html; charset=utf-8"), PSTR("<meta charset='utf-8'><p>WebDAV 配置已弃用（改用手机直连同步）。</p><a href='/'>返回</a>"));
}

void handleWebdavSave() {
  server.send_P(200, PSTR("text/html; charset=utf-8"), PSTR("<meta charset='utf-8'><p>WebDAV 配置已弃用（改用手机直连同步），未保存任何更改。</p><a href='/'>返回</a>"));
}

// GET+POST 合并（省路由对象堆——配网会话堆仅 ~1KB）
void handleWebdavAny() {
  if (server.method() == HTTP_POST) { handleWebdavSave(); return; }
  handleWebdavGet();
}

void handleInfo() {
  SettingsConfig s;
  loadSettingsConfig(s);
  String json = F("{\"version\":\"2.1\",\"state\":\"");
  json += stateText();
  json += F("\",\"staConnected\":");
  json += (WiFi.status() == WL_CONNECTED) ? F("true") : F("false");
  json += F(",\"staIp\":\"");
  json += WiFi.localIP().toString();
  json += F("\",\"clockFormat\":");
  json += s.clockFormat;
  json += F(",\"tzOffsetMin\":");
  json += s.tzOffsetMin;
  json += F(",\"hitokotoEnabled\":");
  json += s.hitokotoEnabled;
  json += F(",\"portrait\":");
  json += s.portrait;
  json += F(",\"sketchSize\":");
  json += ESP.getSketchSize();
  json += F(",\"freeSketchSpace\":");
  json += ESP.getFreeSketchSpace();
  json += F("}");
  server.send(200, "application/json; charset=utf-8", json);
}

// ---- /settings.json: 全量只读设置快照（官方设置面板 /set 回显用, 零副作用）----
void handleSettingsJson() {
  SettingsConfig s;
  loadSettingsConfig(s);
  WeatherConfig wc;
  loadWeatherConfig(wc);
  String json = F("{\"clockFormat\":");
  json += s.clockFormat;
  json += F(",\"tzOffsetMin\":");
  json += s.tzOffsetMin;
  json += F(",\"hitokotoEnabled\":");
  json += s.hitokotoEnabled;
  json += F(",\"longPressMs\":");
  json += s.longPressMs;
  json += F(",\"ntpServer\":\"");
  json += settingsGetNtpServer();
  json += F("\",\"sdFrequency\":");
  json += s.sdFrequency;
  json += F(",\"fullRefreshMin\":");
  json += s.fullRefreshMin;
  json += F(",\"calibIntervalMin\":");
  json += s.calibIntervalMin;
  json += F(",\"batDisplayType\":");
  json += s.batDisplayType;
  json += F(",\"nightUpdate\":");
  json += s.nightUpdate;
  json += F(",\"setRotation\":");
  json += s.setRotation;
  json += F(",\"outputPower\":");
  json += s.outputPower;
  json += F(",\"sdEnabled\":");
  json += s.sdEnabled;
  json += F(",\"portrait\":");
  json += s.portrait;
  json += F(",\"fastFlip\":");
  json += s.fastFlip;
  json += F(",\"city\":\"");
  json += wc.city[0] ? wc.city : "";
  json += F("\",\"weatherKey\":");
  json += (wc.key[0] != '\0') ? F("1") : F("0");
  AdminConfig ad;
  loadAdminConfig(ad);
  json += F(",\"otaSet\":");
  json += (ad.otaPass[0] != '\0') ? F("1") : F("0");
  // ---- 2026-09 Web 层新增设置（仅回显; inAWord 保存时已净化, 无引号/反斜杠/控制字符）----
  json += F(",\"history\":");
  json += s.historyEnabled;
  json += F(",\"clockCalibrationState\":");
  json += s.clockCalibrationState;
  json += F(",\"clockMod\":");
  json += s.clockMod;
  json += F(",\"clockCompensate\":");
  json += s.clockCompensate;
  json += F(",\"inAWord\":\"");
  json += settingsGetInAWord();
  json += F("\"");
  json += F("}");
  server.send(200, "application/json; charset=utf-8", json);
}

// ---- GET /set: 官方设置面板静态页（LittleFS /set.htm, 懒挂载同 /fs/edit）----
void handleSetStatic() {
  if (!fileApiEnsureLfsMount()) {
    server.send_P(500, PSTR("text/plain; charset=utf-8"), PSTR("LittleFS mount failed"));
    return;
  }
  File f = LittleFS.open("/set.htm", "r");
  if (!f) {
    server.send_P(404, PSTR("text/plain; charset=utf-8"), PSTR("set page not found"));
    return;
  }
  server.streamFile(f, "text/html; charset=utf-8");
  f.close();
}

bool wifiManagerHasCredentials() {
  return config.ssid[0] != '\0' && strlen(config.password) >= 8;
}

bool wifiManagerEnsureSta(uint32_t timeoutMs) {
  // 天气页等路径可能未经过 wifiManagerBegin/clockManagerBegin，config 未加载，
  // 此时从 EEPROM 补读 WiFi 凭据，避免误报"未配置"。
  if (config.ssid[0] == '\0') loadConfig();
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!wifiManagerHasCredentials()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);
  uint32_t deadline = millis() + timeoutMs;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(100);
    ESP.wdtFeed();
  }
  return WiFi.status() == WL_CONNECTED;
}

// ---- 进度同步用的非阻塞 WiFi 接口 ----
bool wifiManagerStartSta() {
  if (config.ssid[0] == '\0') loadConfig();
  if (!wifiManagerHasCredentials()) return false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);
  return true;
}
bool wifiManagerIsStaUp() { return WiFi.status() == WL_CONNECTED; }
void wifiManagerStopSta() { WiFi.disconnect(); WiFi.mode(WIFI_OFF); }

// ---- 配网会话堆预算审计探针（临时, 审计完成后移除）----
// 轻量: 只读 heap/maxFreeBlock; PSTR 格式串驻 flash, 不分配堆, 不影响被测环境
void auditHeap(const char *phase) {
  Serial.printf_P(PSTR("AUDIT %s heap=%u maxblk=%u\n"),
                  phase, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
}

// 前置声明: 连接对象/扫描端点 (定义见文件尾) + 推送一次性标志
void handleScanWifi();
void handleScanDevices();
void handleTargetGet();
void handleTargetSave();
void handleTargetAny();   // GET+POST 合并（省路由堆）
static bool pushDone = false;

// Web 设置修改 → 墨水屏提示回调注册（转发到匿名区 webNotifyCbSet; 墨水瓶 setup 调用）
void wifiManagerSetWebNotifyCb(WebSettingsNotifyCb cb) { webNotifyCbSet(cb); }

// 导出 server 实例（匿名命名空间内, 内部链接; 供 file_api 等模块经 wifiManagerServer() 复用）
ESP8266WebServer &wifiManagerServer() { return server; }

void wifiManagerBegin(void (*renderCallback)(bool), void (*exitCallback)()) {
  renderPage = renderCallback;
  exitPage = exitCallback;
  active = true;
  Serial.println(F("NET_ENTER"));
  auditHeap("config_enter");   // 审计: 点击配网后、AP 前
  // 进配网: 若已存 WiFi 凭据 → 先试连路由器 1 次(纯 STA, 不起 AP, 避开 AP+STA 共存崩溃),
  // 连上=局域网管理(显示IP); 失败/超时 → 回热点模式(AP)。无凭据 → 直接热点。
  // ★ 共存(WIFI_AP_STA)已判死刑: STA beacon 解析/共存触发 SDK phy 崩溃(Exception 29),
  //   任何时刻只保持一种模式: TRY_STA/STA_ONLY 阶段纯 STA, AP_ONLY 阶段纯 AP。
  loadConfig();
  if (wifiManagerHasCredentials()) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid, config.password);
    state = TRY_STA;
    deadline = millis() + 12000UL;
    Serial.printf_P(PSTR("WIFI_TRY_STA ssid=%s\n"), config.ssid);
  } else {
    startAp();   // 无凭据: 直接热点配网
    state = AP_ONLY;
    Serial.println(F("WIFI_NO_CRED_AP"));
  }
  // ⚠️ 配网页路由不再走 server.on()（路由对象常驻堆吃 ~1.8KB）:
  // 改由 onNotFound 精确分发（与 /api/* 同款, 省路由对象堆）。配网会话堆硬约束,
  // 12+ 路由对象累积会把 AP 手机关联/文件管理堆压到 OOM（实测 /fs/list OOM）。
  // 分发逻辑见下方dispatchWeb()。
  // 诊断: 未匹配请求（含 favicon.ico 等）——确认请求是否到达服务器
  // ⚠️ 全部分发走 onNotFound（零路由对象堆）: /api/* /fs/* /fm/* 由 fileApiTryDispatch,
  // 配网页路由（原 server.on 注册 13 条, 吃 ~1.8KB 常驻堆）也在此精确分发,
  // 让配网会话堆只被真正匹配的请求占用, 把基线留给文件管理 /fs/*。
  server.onNotFound([]() {
    // ★ CORS 预检 (OPTIONS): 手机/WebView 跨源上传 multipart 会先发 OPTIONS 预检,
    //   此前返回 404 + 无 CORS 头 → 浏览器拦截 POST → 上传"转圈无响应"。返回 200 + CORS 头排除该因素。
    if (server.method() == HTTP_OPTIONS) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type, X-File-Size, X-Resume, X-Resume-Offset, Range");
      server.send_P(204, PSTR("text/plain"), PSTR(""));
      return;
    }
    if (fileApiTryDispatch()) return;   // /api/* /fs/* /fm/*（零路由对象堆, 见 file_api.cpp）
    String uri = server.uri();
    HTTPMethod m = server.method();
    if (uri == "/" || uri == "/set")          { handleSetStatic(); return; }
    if (uri == "/status" && m == HTTP_GET)    { handleStatus(); return; }
    if (uri == "/info" && m == HTTP_GET)      { handleInfo(); return; }
    if (uri == "/settings.json" && m == HTTP_GET) { handleSettingsJson(); return; }
    if (uri == "/settings" && m == HTTP_POST) { handleSettingsSave(); return; }
    if (uri == "/wifi" && m == HTTP_POST)     { handleSave(); return; }
    if (uri == "/clear" && m == HTTP_POST)    { handleClear(); return; }
    if (uri == "/admin" && m == HTTP_POST)    { handleAdminSave(); return; }
    if (uri == "/webdav")                     { handleWebdavAny(); return; }
    if (uri == "/scanwifi" && m == HTTP_GET)  { handleScanWifi(); return; }
    if (uri == "/scandevices" && m == HTTP_GET){ handleScanDevices(); return; }
    if (uri == "/target")                     { handleTargetAny(); return; }
    if (uri == "/update") {
      server.send_P(200, PSTR("text/html; charset=utf-8"), PSTR("<meta charset='utf-8'><p>OTA 未启用：请先在配网页设置 OTA 密码。</p><a href='/'>返回</a>"));
      return;
    }
    // 低堆安全日志: 不把 server.uri() 的临时 String 经 .c_str() + %s 传给 vsnprintf
    //（临时 String 在 3-4KB 堆上可能已释放, %s 读到回收内存 → Exception 29 实测）。
    // 改用固定栈缓冲先拷贝 uri（短暂; 4KB 循环栈可承受）, 不触堆。
    char uriBuf[64];
    String u = server.uri();
    size_t mn = u.length();
    if (mn >= sizeof(uriBuf)) mn = sizeof(uriBuf) - 1;
    if (mn) memcpy(uriBuf, u.c_str(), mn);
    uriBuf[mn] = '\0';
    Serial.printf_P(PSTR("HTTP_404 uri=%s heap=%u\n"), uriBuf, (unsigned)ESP.getFreeHeap());
    server.send_P(404, PSTR("text/plain; charset=utf-8"), PSTR("Not Found"));
  });
  // 收集上传协议头（file_api; 管理密码已废除, 不再收集 X-Admin-Pass）
  server.collectHeaders("X-File-Size", "X-Resume", "X-Resume-Offset", "Range");
  // Web 固件升级（OTA）：/update GET=上传页 POST=固件上传。
  // 认证用独立 OTA 密码（EEPROM 管理区, 与 AP 密码/管理密码分离）; 未设置 OTA 密码 → /update 不可用。
  {
    AdminConfig ad;
    loadAdminConfig(ad);
    if (ad.otaPass[0] != '\0') {
      httpUpdater.setup(&server, "/update", "admin", ad.otaPass);
    }
    // 未设置 OTA 密码: 不注册 /update 路由（省 2 个路由对象堆）, 由 onNotFound 兜底提示。
  }
  // 文件管理 API + LittleFS Web UI（/fm/）：统一 API 供 Web/Android/Legado 使用
  // 二次 WiFi.mode(WIFI_AP) 提前到路由/挂载之前执行: AP 结构在堆充足时初始化,
  // 路由注册(1.8KB)+LFS 挂载(~1KB)之后只剩 ~3KB, AP 后台延迟分配(实测 5s 内 -2.9KB)
  // 会把手机关联/DHCP 处理的堆压到 OOM（Unhandled C++ exception: OOM 实测）。
  // 仅"无凭据→热点"入口(state==AP_ONLY)需要在此强化 mode; TRY_STA 阶段保持纯 STA 不切 AP。
  if (state == AP_ONLY) WiFi.mode(WIFI_AP);
  fileApiInit();
  auditHeap("rte_before_begin");   // 探针: fileApiInit 后, server.begin 前
  server.begin();
  auditHeap("rte_after_begin");   // 探针: server.begin 后
  pushDone = false;
  Serial.println(F("WIFI_WEB_START"));
  // 配网会话分阶段(用户定稿): ①有凭据→TRY_STA 纯 STA 试连(本文件开头已设 state, 不覆盖);
  //   ②连上=STA_ONLY(局域网管理, 显示 IP); ③失败/超时或本无凭据→AP_ONLY 纯热点。
  // 绝不 AP+STA 共存: 共存时 STA 连接/beacon 解析触发 SDK phy 崩溃(Exception 29 epc1=0x4000df64,
  // 实测 8~15s 必崩) —— 任何时刻只保持一种模式。
  // ⚠️ AP_ONLY 时上面 WiFi.mode(WIFI_AP) 已执行(s0_verify4: 有它 DHCP 正常+heap 7.3KB)。
  auditHeap("config_ready");   // 审计: 配网入口完成（路由+LFS已注册, 二次 mode 已执行）
  Serial.printf_P(PSTR("WIFI_CFG_STATE=%d heap=%u\n"), (int)state, (unsigned)ESP.getFreeHeap());
  // ★ 配网页显示用局刷(用户要求: 每次不用全刷, 全刷阻塞 1s+)。首次从"正在加载储存卡"提示页
  //   局刷切换, 残影由 FIXED_REFRESH 定次全刷自愈。
  if (renderPage) renderPage(false);
}

// 配网会话 5s 塌陷审计: AP_ONLY 期间每 500ms 采样 heap（串口紧凑格式, 不影响被测环境）
static uint32_t gAuditTs = 0;

void wifiManagerLoop() {
  if (!active) return;
  server.handleClient();
  // ---- 进配网"先试连 WiFi"(用户需求): TRY_STA 纯 STA 试连(无 AP) ----
  if (state == TRY_STA) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      // 连上: 保持纯 STA, 管理 web 在局域网 IP 上监听; 屏幕显示 IP + 局域网访问地址。
      staIp = WiFi.localIP().toString();
      wifiSaveResult = 1;
      snprintf(wifiSaveIp, sizeof(wifiSaveIp), "%s", staIp.c_str());
      state = STA_ONLY;
      Serial.printf_P(PSTR("WIFI_STA_ONLY_OK ip=%s heap=%u\n"), staIp.c_str(), (unsigned)ESP.getFreeHeap());
      webNotifyReset();
      webNotifyAdd("局域网访问:");
      webNotifyAdd(staIp.c_str());
      webNotifyDone("WiFi已连接");
      if (renderPage) renderPage(false);
    } else if (static_cast<int32_t>(millis() - deadline) >= 0) {
      // 12s 超时/失败: 关 STA → 纯 AP 热点(管理 web 切回 192.168.4.1)。
      WiFi.disconnect();
      WiFi.mode(WIFI_OFF);
      Serial.println(F("WIFI_TRY_STA_FAIL -> AP"));
      startAp();
      state = AP_ONLY;
      wifiSaveResult = 2;
      webNotifyReset();
      webNotifyAdd("WiFi连接失败,已开热点");
      webNotifyDone("连接失败");
      if (renderPage) renderPage(false);
    }
  } else if (state == STA_ONLY) {
    // 已连 STA: 若中途掉线(路由器重启等) → 保底回热点, 管理 web 不失效。
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("WIFI_STA_LOST -> AP"));
      WiFi.mode(WIFI_OFF);
      startAp();
      state = AP_ONLY;
      if (renderPage) renderPage(false);
    }
  }
  // 分阶段: 常态纯 AP; 仅"保存连接"时临时切共存(WIFI_AP_STA)连 STA, 连完回纯 AP(管理 web 在线)。
  if (wifiConnectPending) {
    wifiConnectPending = false;
    loadConfig();
    WiFi.mode(WIFI_AP_STA);   // 临时共存: AP 仍在(管理页可达), 同时启用 STA 接口去连路由器
    WiFi.begin(config.ssid, config.password);
    state = CONNECTING;
    deadline = millis() + 15000UL;
    Serial.println(F("WIFI_CONNECT_START"));
  }
  // 请求边界探针: /fs/list handler 在 handleClient 内执行完（gFsListJustHandled=true）,
  // 回到此处即"handleClient 收尾完成 + 下一轮 loop 前"状态, 打印堆/块/栈。
  if (gFsListJustHandled) {
    gFsListJustHandled = false;
    fsListProbe("LOOP_AFTER");
  }
  // 对照实验探针采样定时（AP_ONLY 下每 1000ms; 与 gAuditTs=500ms 分开, 互不干扰）
  static uint32_t gAbTs = 0;
  if (state == CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      // 连接成功: 先在 STA 态记录 IP, 再【关 STA 回纯 AP】(IP 在切回 AP 后已失效, 必须前置取值)
      staIp = WiFi.localIP().toString();
      wifiSaveResult = 1;
      snprintf(wifiSaveIp, sizeof(wifiSaveIp), "%s", staIp.c_str());
      WiFi.mode(WIFI_AP);
      WiFi.disconnect();
      state = AP_ONLY;
      Serial.printf_P(PSTR("WIFI_STA_CONNECTED(back-AP) ip=%s heap=%u\n"), staIp.c_str(), (unsigned)ESP.getFreeHeap());
      webNotifyReset();
      webNotifyAdd(wifiSaveIp);
      webNotifyDone("连接成功");
      if (renderPage) renderPage(false);
    } else if (static_cast<int32_t>(millis() - deadline) >= 0) {
      // 超时失败: 关 STA 回纯 AP(AP 没关过, 无须重建), 反馈失败, 管理 web 持续可达。
      WiFi.mode(WIFI_AP);
      WiFi.disconnect();
      state = AP_ONLY;
      wifiSaveResult = 2;
      Serial.println(F("WIFI_STA_FAIL(AP_ONLY)"));
      webNotifyReset();
      webNotifyAdd("请检查WiFi密码/信号");
      webNotifyDone("连接失败");
      if (renderPage) renderPage(false);
    }
  }
  if (state != CONNECTING && !pushDone) {
    pushDone = true;             // STA 连接结果确定后推一次设备信息到连接对象
    wifiManagerPushDeviceInfo();
  }
  // 诊断: AP 关联站数（排查"手机连上热点但拿不到 IP"的 DHCP 问题）
  static uint32_t lastStaLog = 0;
  if (state == AP_ONLY && static_cast<int32_t>(millis() - lastStaLog) >= 5000) {
    lastStaLog = millis();
    Serial.printf_P(PSTR("AP_STA_NUM=%u heap=%u\n"), (unsigned)WiFi.softAPgetStationNum(),
                  (unsigned)ESP.getFreeHeap());
  }
  // 审计: 5s 塌陷曲线（500ms 采样; AP_ONLY 全程, 覆盖手机未连/关联/DHCP/开页面）
  if (state == AP_ONLY && static_cast<int32_t>(millis() - gAuditTs) >= 500) {
    gAuditTs = millis();
    Serial.printf_P(PSTR("H %u\n"), (unsigned)ESP.getFreeHeap());
  }
  // 对照实验探针: AP_ONLY 下每 1000ms 采一次（验证"纯 AP 下 SD 访问 × AP hostap_input 竞争"）。
  // 见 file_api_fs.h ofsAbTestTick 注释。编译实验版用 -DOFS_ABTEST_MODE=<0|1|2>（默认 0=不碰 SD）。
  if (state == AP_ONLY && static_cast<int32_t>(millis() - gAbTs) >= 1000) {
    gAbTs = millis();
    ofsAbTestTick();
  }
  // ★ 上传结束回配网页: 上传状态回调(renderUploadStatus)显示"上传完毕/失败"提示后,
  //   handleClient 返回到此, 延时 ~1.5s 让用户看清, 再恢复配网页(热点信息)。
  //   用 phase 边沿检测(非结束态→结束态)触发一次, 避免每轮重复。
  if (state == AP_ONLY) {
    static int gOfsUpLastPhase = -1;
    int cur = ofsUpGetPhase();
    static uint32_t gOfsUpDoneAt = 0;
    bool ended = (cur == OFS_UP_PHASE_DONE || cur == OFS_UP_PHASE_FAIL);
    if (ended && gOfsUpLastPhase != cur) {
      gOfsUpDoneAt = millis();   // 刚结束: 启动恢复计时
    } else if (ended && gOfsUpDoneAt != 0 && static_cast<int32_t>(millis() - gOfsUpDoneAt) >= 1500) {
      gOfsUpDoneAt = 0;
      if (renderPage) renderPage(false);   // 回配网页(局刷)
    } else if (!ended) {
      gOfsUpDoneAt = 0;                    // 非结束态(新一轮上传): 清计时
    }
    gOfsUpLastPhase = cur;
  }
  // ★ 通用文件操作通知渲染(2026-09): renderFileOpStatus 只写底行不渲染(handler 深链 stack≈0,
  //   EPD 渲染在 handler 内会 Soft WDT)。此处 handleClient 已返回、栈浅, 检测 phase 变化即 renderPage。
  if (state == AP_ONLY) {
    static int gOfsOpLastPhase = -1;
    int opCur = ofsOpGetPhase();
    if (opCur != gOfsOpLastPhase && opCur != OFS_OP_PHASE_IDLE) {
      gOfsOpLastPhase = opCur;
      if (renderPage) renderPage(false);   // 局刷配网页(底行显示 操作中/操作成功/失败)
    }
  }
  ESP.wdtFeed();
}

void wifiManagerHandleKeys(int middleEvent, int rightEvent) {
  if (!active) return;
  if (middleEvent == 2) {
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    active = false;
    state = IDLE;
    Serial.println(F("NET_EXIT"));
    if (exitPage) exitPage();
  }
  (void)rightEvent;
}

bool wifiManagerIsActive() { return active; }
const char *wifiManagerApSsid() { return apSsid.c_str(); }
const char *wifiManagerStateText() { return stateText(); }
const char *wifiManagerStaIp() { return staIp.c_str(); }
bool wifiManagerIsStaOnly() { return state == STA_ONLY; }   // 进配网已连 WiFi(局域网管理), 无热点
bool wifiManagerIsTryingSta() { return state == TRY_STA; }  // 正在试连(无 AP/无 IP 阶段)

void clockManagerBegin(void (*renderCallback)(bool), void (*doneCallback)()) {
  clockRender = renderCallback;
  clockDone = doneCallback;
  clockState = CLOCK_STA;
  clockDeadline = millis() + STA_TIMEOUT_MS;
  clockSkippedSession = false;
  syncedTime = 0;
  syncedRtcTicks = 0;
  rtcBaseValid = false;
  EEPROM.begin(EEPROM_SIZE);
  // 立即设置本地时区（设置页可调，默认 UTC+8）: 即使跳过校准(WiFi 未连), localtime() 也能正确显示北京时间
  configTime(settingsGetTzOffsetMin() * 60, 0, NTP_SERVER);
  // 时间源优先级: 板载 BL8025T (断电后继续走时) > EEPROM 兑底。
  // 注意: 不再用 ESP8266 内部 RTC 内存(loadRtcClock) — KEY1 复位会清掉, 且用户要求时间只走外挂 RTC。
  rtc8025Init();
  if (rtc8025Present && rtc8025TimeValid) {
    syncedTime = rtc8025NowEpoch;
    lastCalibrationTime = 0;
    rtcBaseValid = false;
    syncedAtMs = millis();
    // 从 EEPROM 恢复上次校准时间 (用于“距上次校准”)
    PersistClockRecord rec;
    EEPROM.get(CLOCK_EEPROM_ADDR, rec);
    if (rec.magic == CLOCK_EEPROM_MAGIC && rec.epoch >= 1600000000UL &&
        persistChecksum(rec) == rec.checksum) {
      lastCalibrationTime = rec.epoch;
    }
    Serial.printf_P(PSTR("CLOCK_8025T_READ epoch=%lu\n"), (unsigned long)syncedTime);
  } else {
    // BL8025T 不在线(实测 I2C 扫描无设备): 从 EEPROM 恢复校准时间 (flash, KEY1 不清)
    loadPersistedClock();
  }
  Serial.println(F("CLOCK_ENTER"));
  if (!loadConfig()) {
    clockState = CLOCK_FAILED;
    Serial.println(F("CLOCK_NO_WIFI_CONFIG"));
    if (clockRender) clockRender(true);
    return;
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);
  Serial.printf_P(PSTR("CLOCK_WIFI_BEGIN ssidLen=%u\n"), static_cast<unsigned>(strlen(config.ssid)));
  if (clockRender) clockRender(true);
}

// A7 对齐: NTP 失败后改用天气时间 — 请求心知 now.json, 解析 last_update 作为时钟基准。
// last_update 格式: "YYYY-MM-DDTHH:MM:SS+08:00" (前 19 字符为本地时间, 按设置时区转 epoch)
static bool clockManagerTryWeatherTime() {
  ESP.wdtFeed();
  WeatherConfig wc;
  loadWeatherConfig(wc);
  if (wc.key[0] == '\0' || wc.city[0] == '\0') {
    Serial.println(F("CLOCK_WEATHER_NOCFG"));
    return false;
  }
  WiFiClient client;
  HTTPClient http;
  char url[180];
  snprintf(url, sizeof(url),
           "http://api.seniverse.com/v3/weather/now.json?key=%s&location=%s&language=zh-Hans&unit=c",
           wc.key, wc.city);
  http.setTimeout(3000);   // 阻塞 ≤3s < WDT 8s
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  String body = (code == HTTP_CODE_OK) ? http.getString() : String("");
  http.end();
  ESP.wdtFeed();
  if (code != HTTP_CODE_OK) {
    Serial.printf_P(PSTR("CLOCK_WEATHER_HTTP %d\n"), code);
    return false;
  }
  int pos = body.indexOf("\"last_update\"");
  if (pos < 0) return false;
  int q = body.indexOf('"', pos + 14);   // 值起始引号
  if (q < 0) return false;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (sscanf(body.c_str() + q + 1, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return false;
  // 本地时间(按设置时区) → UTC epoch (civilToEpoch 把本地当 UTC, 再减时区偏移)
  int64_t t = civilToEpoch(y, mo, d, h, mi, s) - (int64_t)settingsGetTzOffsetMin() * 60;
  if (t <= 1600000000LL) return false;
  syncedTime = (time_t)t;
  syncedAtMs = millis();
  // 时间持久化: 优先写外挂 BL8025T; 芯片不在线(实测 I2C 扫描无设备)时降级写 EEPROM
  // (EEPROM 是 flash, KEY1 复位不会清 — 用户担心的"内部RTC/内存"是 rtcUserMemory, 已避开)
  clockManagerPersistSync((time_t)t);
  clockState = CLOCK_SUCCESS;
  clockSuccessDeadline = millis() + 1800UL;
  Serial.printf_P(PSTR("CLOCK_WEATHER_TIME epoch=%lld\n"), (long long)t);
  if (clockRender) clockRender(false);
  return true;
}

void clockManagerLoop() {
  if (clockState == CLOCK_IDLE || clockState == CLOCK_SKIPPED || clockState == CLOCK_FAILED) return;
  if (clockState == CLOCK_SUCCESS) {
    if (static_cast<int32_t>(millis() - clockSuccessDeadline) >= 0) {
      if (clockDone) clockDone();
      clockState = CLOCK_IDLE;
    }
    ESP.wdtFeed();
    return;
  }
  if (clockState == CLOCK_STA) {
    if (WiFi.status() == WL_CONNECTED) {
      clockState = CLOCK_NTP;
      clockDeadline = millis() + NTP_TIMEOUT_MS;
      configTime(settingsGetTzOffsetMin() * 60, 0, NTP_SERVER);
      Serial.printf_P(PSTR("CLOCK_WIFI_CONNECTED ip=%s\n"), WiFi.localIP().toString().c_str());
      if (clockRender) clockRender(false);
    } else if (static_cast<int32_t>(millis() - clockDeadline) >= 0) {
      clockState = CLOCK_FAILED;
      Serial.println(F("CLOCK_WIFI_TIMEOUT"));
      if (clockRender) clockRender(false);
    }
  } else if (clockState == CLOCK_NTP) {
    time_t now = time(nullptr);
    if (now > 1600000000) {
      clockSkippedSession = false;
      syncedTime = now;
      syncedAtMs = millis();
      // 时间持久化: 优先写外挂 BL8025T; 芯片不在线(实测 I2C 扫描无设备)时降级写 EEPROM
      // (EEPROM 是 flash, KEY1 复位不会清 — 用户担心的"内部RTC/内存"是 rtcUserMemory, 已避开)
      clockManagerPersistSync(now);   // 写板载 BL8025T(断电后继续走时); 失败/不在线降级 EEPROM
      clockState = CLOCK_SUCCESS;
      clockSuccessDeadline = millis() + 1800UL;
      Serial.printf_P(PSTR("CLOCK_NTP_SUCCESS epoch=%lu\n"), static_cast<unsigned long>(now));
      if (clockRender) clockRender(false);
    } else if (static_cast<int32_t>(millis() - clockDeadline) >= 0) {
      // 对齐 A7: NTP 失败 → 改用天气时间 (天气接口 last_update 字段)
      clockState = CLOCK_WEATHER;
      Serial.println(F("CLOCK_NTP_TIMEOUT -> weather time"));
      if (clockRender) clockRender(false);
    }
  } else if (clockState == CLOCK_WEATHER) {
    if (!clockManagerTryWeatherTime()) {
      clockState = CLOCK_FAILED;
      Serial.println(F("CLOCK_WEATHER_FAILED"));
      if (clockRender) clockRender(false);
    }
    ESP.wdtFeed();
    return;
  }
  ESP.wdtFeed();
}

void clockManagerHandleKeys(int middleEvent, int rightEvent) {
  if (clockState == CLOCK_IDLE || clockState == CLOCK_SUCCESS || clockState == CLOCK_SKIPPED || clockState == CLOCK_FAILED) return;
  // 校准界面: 右键短按即跳过校准 (用户要求: 只需短按, 响应更快; 对齐 A7 "按下按键3可跳过校准",
  // 天气降级阶段 CLOCK_WEATHER 同样可跳过)
  if (rightEvent == 1) {
    clockSkippedSession = true;
    clockState = CLOCK_SKIPPED;
    Serial.println(F("CLOCK_SKIP"));
    // 调试: 打印当前 epoch 与本地时间字符串, 确认时区/显示是否正确
    {
      time_t dbgNow = clockManagerNow();
      struct tm *dbgTm = dbgNow > 1600000000UL ? localtime(&dbgNow) : nullptr;
      char dbgBuf[40];
      if (dbgTm) {
        snprintf(dbgBuf, sizeof(dbgBuf), "%04d-%02d-%02d %02d:%02d:%02d", dbgTm->tm_year + 1900,
                 dbgTm->tm_mon + 1, dbgTm->tm_mday, dbgTm->tm_hour, dbgTm->tm_min, dbgTm->tm_sec);
      } else {
        snprintf(dbgBuf, sizeof(dbgBuf), "(invalid)");
      }
      Serial.printf_P(PSTR("CLOCK_SKIP_DEBUG epoch=%lu local=%s lastCal=%lu\n"), (unsigned long)dbgNow, dbgBuf,
                    (unsigned long)lastCalibrationTime);
    }
    clockManagerPersistNow();
    if (clockDone) clockDone();
  }
  (void)middleEvent;
}

bool clockManagerIsActive() {
  return clockState == CLOCK_STA || clockState == CLOCK_NTP || clockState == CLOCK_WEATHER;
}

// 开机早期探测外挂 BL8025T (用户重要决策: 时间只读外挂 RTC)。
// 若不在开机时探测, rtc8025Present=false → clockManagerNow 读 BL8025T 直接失败 → 回退内存旧值。
void clockManagerProbeRtc() {
  rtc8025Init();
}

bool clockManagerIsSynced() { return syncedTime > 1600000000; }
time_t clockManagerNow() {
  // 用户要求(重要): 读取时间只读外挂 BL8025T (KEY1 复位会清掉 ESP8266 内部 RTC 内存/内存漂移)
  time_t t = 0;
  if (rtc8025ReadEpoch(t)) return t;
  // BL8025T 不可用(总线忙/未探测到) → 回退内存漂移
  if (syncedTime > 1600000000UL) {
    if (rtcBaseValid) return syncedTime + (system_get_rtc_time() - syncedRtcTicks) / RTC_TICKS_PER_SECOND;
    return syncedTime + (millis() - syncedAtMs) / 1000UL;
  }
  return 0;
}
bool clockManagerWasSkipped() { return clockSkippedSession; }
void clockManagerPersistNow() {
  if (syncedTime > 1600000000UL && lastCalibrationTime > 0) {
    savePersistedClock(lastCalibrationTime, clockManagerNow());
  }
}

time_t clockManagerLastCalibration() { return lastCalibrationTime; }
uint32_t clockManagerMinutesSinceCalibration() {
  time_t now = clockManagerNow();
  if (lastCalibrationTime == 0 || now <= lastCalibrationTime) return 0;
  uint32_t minutes = static_cast<uint32_t>((now - lastCalibrationTime) / 60UL);
  return minutes > 599940UL ? 599940UL : minutes;
}
uint32_t clockManagerHoursSinceCalibration() {
  time_t now = clockManagerNow();
  if (lastCalibrationTime == 0 || now <= lastCalibrationTime) return 0;
  uint32_t hours = static_cast<uint32_t>((now - lastCalibrationTime) / 3600UL);
  return hours > 9999UL ? 9999UL : hours;
}
int clockManagerStage() { return static_cast<int>(clockState); }
const char *clockManagerStageText() {
  // 文案对齐官方 A7 时间校准页（反编译字符串：获取NTP时间 / :成功 / :失败 / 改用天气时间 / 手动跳过校准）
  switch (clockState) {
    case CLOCK_STA: return "获取NTP时间";
    case CLOCK_NTP: return "获取NTP时间";
    case CLOCK_WEATHER: return "获取NTP时间失败，改用天气时间";
    case CLOCK_SUCCESS: return "获取NTP时间:成功";
    case CLOCK_SKIPPED: return "已跳过校准";
    case CLOCK_FAILED: return "获取NTP时间:失败";
    default: return "获取NTP时间";
  }
}

// A7 对齐: 校时同步时间后时钟芯片写入结果提示 (成功=读取数据正常, 失败=数据出错或不存在,使用软件时钟)
const char *clockManagerClockChipText() { return clockChipMessage; }
bool clockManagerClockChipOk() { return clockChipOk; }
bool clockManagerSyncSucceeded() { return clockState == CLOCK_SUCCESS; }

// ==================== 连接对象配置（EEPROM 470, 'TGRT'）====================
// 局域网/热点下各选一台设备(如运行"墨水屏同步代理"App 的手机)作为连接对象:
// 配网模式开启时设备主动推送自身信息到选中 IP, App 收到后自动设置设备地址。
const int TARGET_EEPROM_ADDR = 470;
const uint32_t TARGET_MAGIC = 0x54475254UL;
static_assert(TARGET_EEPROM_ADDR + sizeof(TargetConfig) <= 512,
              "Target region overflows EEPROM_SIZE");

uint16_t targetChecksum(const TargetConfig &v) {
  return checksumBytes(reinterpret_cast<const uint8_t *>(&v), offsetof(TargetConfig, checksum));
}

bool loadTargetConfig(TargetConfig &out) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(TARGET_EEPROM_ADDR, out);
  bool valid = (out.magic == TARGET_MAGIC && targetChecksum(out) == out.checksum);
  if (!valid) {
    memset(&out, 0, sizeof(out));
    out.magic = TARGET_MAGIC;
  }
  // 内存默认: 空 IP 填默认 (不写 EEPROM)。局域网 192.168.0.10 / 热点 192.168.0.100。
  if (out.staIp[0] == '\0') strncpy(out.staIp, "192.168.0.10", sizeof(out.staIp) - 1);
  if (out.apIp[0] == '\0')  strncpy(out.apIp,  "192.168.0.100", sizeof(out.apIp) - 1);
  return valid;
}

bool saveTargetConfig(const TargetConfig &in) {
  TargetConfig cfg = in;
  cfg.magic = TARGET_MAGIC;
  cfg.checksum = targetChecksum(cfg);
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(TARGET_EEPROM_ADDR, cfg);
  return EEPROM.commit();
}

// 简单 JSON 字符串取值: {"key":"value"} → value（不含引号）
static String jsonStrValue(const char *json, const char *key) {
  String k = String("\"") + key + "\":\"";
  const char *p = strstr(json, k.c_str());
  if (!p) return "";
  p += k.length();
  String out;
  while (*p && *p != '"') { out += *p; p++; }
  return out;
}

static bool validIpv4(const String &s) {
  if (s.length() < 7 || s.length() > 15) return false;
  int dots = 0;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '.') dots++;
    else if (!(c >= '0' && c <= '9')) return false;
  }
  return dots == 3;
}

// ---- WiFi 扫描 (主动扫描含隐藏, ~2-5s): {"networks":[{"ssid","rssi","secure"}]} ----
void handleScanWifi() {
  WiFi.scanDelete();
  int8_t n = WiFi.scanNetworks(false, true);
  // 分阶段后常态是纯 AP(堆~7K), String 一次性拼接安全(此前共存模式才 OOM; chunked 流式实测会让前端
  // JSON 在 580 处截断 → SyntaxError: Unterminated string)。沿用 String + 一次性 send。
  String json = F("{\"networks\":[");
  if (n >= 0) {
    for (int8_t i = 0; i < n; i++) {
      if (i) json += ',';
      json += F("{\"ssid\":\"");
      json += jsonEscape(WiFi.SSID(i).c_str());
      json += F("\",\"rssi\":");
      json += WiFi.RSSI(i);
      json += F(",\"secure\":");
      json += (WiFi.encryptionType(i) != ENC_TYPE_NONE) ? F("true") : F("false");
      json += '}';
    }
  }
  json += F("]}");
  WiFi.scanDelete();
  server.send(200, "application/json; charset=utf-8", json);
  Serial.printf_P(PSTR("SCAN_WIFI n=%d\n"), (int)n);
}

// ---- 连接设备扫描: UDP 探测(广播 INKPING, App 应答 name/model) + 热点 station 列表 ----
void handleScanDevices() {
  const uint16_t PING_PORT = 8083;
  const uint32_t WAIT_MS = 3000;
  struct Dev { String ip; String name; String model; };
  Dev lan[10]; int lanN = 0;
  Dev ap[10];  int apN = 0;

  // 热点关联的 station 先入 ap 列表（名字待 UDP 响应匹配后补充）
  {
    struct station_info *si = wifi_softap_get_station_info();
    for (; si != nullptr && apN < 10; si = si->next.stqe_next) {
      IPAddress ip(si->ip.addr);
      ap[apN].ip = ip.toString();
      ap[apN].name = "热点设备";
      ap[apN].model = "";
      apN++;
    }
    wifi_softap_free_station_info();
  }

  WiFiUDP udp;
  if (udp.begin(PING_PORT)) {
    IPAddress staIp = WiFi.localIP();
    if (staIp.isSet() && staIp != IPAddress(0, 0, 0, 0)) {
      IPAddress lanBcast(staIp[0], staIp[1], staIp[2], 255);
      udp.beginPacket(lanBcast, PING_PORT);
      udp.write((const uint8_t *)"INKPING", 7);
      udp.endPacket();
    }
    IPAddress apIp = WiFi.softAPIP();
    if (apIp.isSet() && apIp != IPAddress(0, 0, 0, 0)) {
      IPAddress apBcast(apIp[0], apIp[1], apIp[2], 255);
      udp.beginPacket(apBcast, PING_PORT);
      udp.write((const uint8_t *)"INKPING", 7);
      udp.endPacket();
    }
    uint32_t t0 = millis();
    while ((int32_t)(millis() - t0) < (int32_t)WAIT_MS) {
      int sz = udp.parsePacket();
      if (sz > 0 && sz < 256) {
        char buf[256];
        int n = udp.read(buf, min(sz, 255));
        buf[n] = '\0';
        IPAddress from = udp.remoteIP();
        bool isAp = (apN > 0) && (from[0] == WiFi.softAPIP()[0] &&
                                  from[1] == WiFi.softAPIP()[1] &&
                                  from[2] == WiFi.softAPIP()[2]);
        Dev *list = isAp ? ap : lan;
        int *cnt = isAp ? &apN : &lanN;
        int idx = -1;
        for (int i = 0; i < *cnt; i++) {
          if (list[i].ip == from.toString()) { idx = i; break; }
        }
        if (idx < 0 && *cnt < 10) {
          idx = *cnt;
          (*cnt)++;
          list[idx].ip = from.toString();
          list[idx].name = "";
          list[idx].model = "";
        }
        if (idx >= 0) {
          String name = jsonStrValue(buf, "name");
          String model = jsonStrValue(buf, "model");
          if (!name.isEmpty()) list[idx].name = name;
          if (!model.isEmpty()) list[idx].model = model;
          if (list[idx].name.isEmpty()) list[idx].name = "未知设备";
        }
      }
      delay(10);
      ESP.wdtFeed();
    }
    udp.stop();
  }

  String json = F("{\"lan\":[");
  for (int i = 0; i < lanN; i++) {
    if (i) json += ',';
    json += F("{\"ip\":\"");
    json += jsonEscape(lan[i].ip.c_str());
    json += F("\",\"name\":\"");
    json += jsonEscape(lan[i].name.c_str());
    json += F("\",\"model\":\"");
    json += jsonEscape(lan[i].model.c_str());
    json += F("\"}");
  }
  json += F("],\"ap\":[");
  for (int i = 0; i < apN; i++) {
    if (i) json += ',';
    json += F("{\"ip\":\"");
    json += jsonEscape(ap[i].ip.c_str());
    json += F("\",\"name\":\"");
    json += jsonEscape(ap[i].name.c_str());
    json += F("\",\"model\":\"");
    json += jsonEscape(ap[i].model.c_str());
    json += F("\"}");
  }
  json += F("]}");
  server.send(200, "application/json; charset=utf-8", json);
  Serial.printf_P(PSTR("SCAN_DEVICES lan=%d ap=%d\n"), lanN, apN);
}

// ---- 连接对象端点 ----
void handleTargetAny() {
  if (server.method() == HTTP_POST) { handleTargetSave(); return; }
  handleTargetGet();
}

void handleTargetGet() {
  TargetConfig t;
  loadTargetConfig(t);
  String json = F("{\"staIp\":\"");
  json += jsonEscape(t.staIp);
  json += F("\",\"apIp\":\"");
  json += jsonEscape(t.apIp);
  json += F("\"}");
  server.send(200, "application/json; charset=utf-8", json);
}

void handleTargetSave() {
  TargetConfig t;
  loadTargetConfig(t);
  String sta = server.arg("staIp");
  sta.trim();
  String ap = server.arg("apIp");
  ap.trim();
  if (!sta.isEmpty() && !validIpv4(sta)) {
    server.send_P(400, PSTR("text/plain; charset=utf-8"), PSTR("局域网 IP 无效"));
    return;
  }
  if (!ap.isEmpty() && !validIpv4(ap)) {
    server.send_P(400, PSTR("text/plain; charset=utf-8"), PSTR("热点 IP 无效"));
    return;
  }
  sta.toCharArray(t.staIp, sizeof(t.staIp));
  ap.toCharArray(t.apIp, sizeof(t.apIp));
  if (!saveTargetConfig(t)) {
    server.send_P(400, PSTR("text/plain; charset=utf-8"), PSTR("保存失败"));
    return;
  }
  Serial.printf_P(PSTR("TARGET_WEB_SAVE sta=[%s] ap=[%s]\n"), t.staIp, t.apIp);
  wifiManagerPushDeviceInfo();   // 保存后立即推送设备信息给选中 App (App 收到自动更新设备地址)
  server.send_P(200, PSTR("text/html; charset=utf-8"), PSTR("<meta charset='utf-8'><p>连接对象已保存。</p><a href='/'>返回</a>"));
  webNotifyReset();
  webNotifyAdd("连接对象=已保存 ");
  webNotifyDone("修改成功");
}

// ---- 配网模式开启后向连接对象推送设备信息 (HTTP POST, 3s 超时, 失败静默) ----
void wifiManagerPushDeviceInfo() {
  TargetConfig t;
  loadTargetConfig(t);
  const char *target = nullptr;
  if (WiFi.status() == WL_CONNECTED && t.staIp[0]) target = t.staIp;
  else if (t.apIp[0]) target = t.apIp;
  if (!target) {
    Serial.println(F("PUSH_DEVICE no-target"));
    return;
  }
  IPAddress tip;
  if (!tip.fromString(target)) {
    Serial.printf_P(PSTR("PUSH_DEVICE bad-ip [%s]\n"), target);
    return;
  }
  WiFiClient c;
  if (!c.connect(tip, 8082)) {
    Serial.printf_P(PSTR("PUSH_DEVICE connect-fail [%s]\n"), target);
    return;
  }
  char body[160];
  snprintf(body, sizeof(body),
           "{\"device\":\"ink\",\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"ap\":\"192.168.4.1\"}",
           (WiFi.status() == WL_CONNECTED) ? "sta" : "ap",
           WiFi.localIP().toString().c_str(),
           apSsid.c_str());
  c.print("POST /device_info HTTP/1.1\r\nHost: ");
  c.print(target);
  c.print("\r\nContent-Type: application/json\r\nContent-Length: ");
  c.print(strlen(body));
  c.print("\r\nConnection: close\r\n\r\n");
  c.print(body);
  uint32_t t0 = millis();
  while (c.connected() && (int32_t)(millis() - t0) < 3000) {
    while (c.available()) c.read();
    delay(5);
  }
  c.stop();
  Serial.printf_P(PSTR("PUSH_DEVICE ok target=[%s] body=[%s]\n"), target, body);
}

// ---- 进度同步直连手机 (D0): 纯 AP / 目标地址 ----

// 纯 AP: WIFI_AP(STA 断开, 规避 AP/STA 同子网路由歧义), softAP 192.168.0.1/24,
// DHCP 固定租约仅 192.168.0.100 (手机热点模式固定地址)。SSID/密码与配网热点一致。
bool wifiManagerStartApOnly() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[16];
  snprintf(name, sizeof(name), "MSP-%02X%02X", mac[4], mac[5]);
  apSsid = name;
  WiFi.persistent(false);
  WiFi.disconnect();                        // 断开 STA (纯 AP)
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 0, 1), IPAddress(192, 168, 0, 1), IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  apSetFixedLease();                        // 固定租约仅 192.168.0.100
  Serial.printf_P(PSTR("WIFI_AP_ONLY_START ssid=%s ok=%d ip=%s\n"),
                apSsid.c_str(), ok ? 1 : 0, WiFi.softAPIP().toString().c_str());
  return ok;
}

// 目标手机 IP: STA 已连且 staIp 非空 → staIp；否则 → apIp (loadTargetConfig 后取, 含默认值)
const char* wifiManagerSyncTarget() {
  static char buf[16];
  TargetConfig t;
  loadTargetConfig(t);
  const char* ip = (WiFi.status() == WL_CONNECTED && t.staIp[0]) ? t.staIp : t.apIp;
  snprintf(buf, sizeof(buf), "%s", ip);
  return buf;
}

// 调试: 当前 softAP IP 字符串 (如 192.168.0.1)
const char* wifiManagerApIp() {
  static char buf[16];
  snprintf(buf, sizeof(buf), "%s", WiFi.softAPIP().toString().c_str());
  return buf;
}

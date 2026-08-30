#pragma once

#include <Arduino.h>
#include <ESP8266WebServer.h>

// HTTP 服务器实例访问器（server 定义于 wifi_manager.cpp 匿名命名空间, 内部链接,
// 其他模块（file_api 等）必须经此访问器复用, 不能 extern）
ESP8266WebServer &wifiManagerServer();

// 文件 API 用: 校验请求携带的管理密码（X-Admin-Pass/apass）; 未设置密码或错误 → false
bool wifiManagerAdminPassValid();

// 配网堆预算审计探针（临时, 审计完成后移除）
void auditHeap(const char *phase);

void wifiManagerBegin(void (*renderCallback)(bool), void (*exitCallback)());
void wifiManagerLoop();
void wifiManagerHandleKeys(int middleEvent, int rightEvent);
bool wifiManagerIsActive();
const char *wifiManagerApSsid();
const char *wifiManagerStateText();
const char *wifiManagerStaIp();

void clockManagerBegin(void (*renderCallback)(bool), void (*doneCallback)());
void clockManagerLoop();
void clockManagerHandleKeys(int middleEvent, int rightEvent);
void clockManagerProbeRtc();   // 开机早期探测外挂 BL8025T (否则 rtc8025Present=false, 时间读不到)
bool clockManagerIsActive();
bool clockManagerIsSynced();
void clockManagerPersistNow();
time_t clockManagerNow();
time_t clockManagerLastCalibration();
uint32_t clockManagerMinutesSinceCalibration();
uint32_t clockManagerHoursSinceCalibration();
bool clockManagerWasSkipped();
int clockManagerStage();
const char *clockManagerStageText();
// A7 对齐: 校时同步时间后时钟芯片写入结果提示 (成功=读取数据正常, 失败=数据出错或不存在,使用软件时钟)
const char *clockManagerClockChipText();
bool clockManagerClockChipOk();
bool clockManagerSyncSucceeded();

// ---- 天气配置（EEPROM 偏移 160, 独立区, 不干扰 WifiConfig/时钟区）----
struct WeatherConfig {
    uint32_t magic;        // 0x57544852UL 'WTHR'
    char city[32];         // UTF-8 城市名（默认"深圳"）
    char key[32];          // 心知天气 API KEY（空=未配置）
    uint8_t nightUpdata;   // 夜间不更新开关 (0=关 1=开)
    uint16_t checksum;
};
// 读取天气配置；magic/checksum 校验失败返回 false 并填充默认值
bool loadWeatherConfig(WeatherConfig &out);
// 保存天气配置（长度校验）；成功返回 true
bool saveWeatherConfig(const WeatherConfig &in);

// 是否已配置 WiFi 凭据（ssid 非空且密码 >= 8 位）
bool wifiManagerHasCredentials();
// 确保 STA 已连接：未连接且已配置凭据则重连（最多等待 timeoutMs）；返回是否已连接
bool wifiManagerEnsureSta(uint32_t timeoutMs);
// 非阻塞 WiFi 接口（进度同步状态机用）：开始 STA 连接 / 轮询是否已连 / 关闭
bool wifiManagerStartSta();
bool wifiManagerIsStaUp();
void wifiManagerStopSta();

// ---- 设备设置（EEPROM 偏移 232, 独立区, 避开 WifiConfig/CLOCK/WeatherConfig）----
// 布局兼容说明：hitokotoEnabled/portrait 位于 checksum 之后 → checksum 范围/位置不变，
// 旧版本(step01)数据无需迁移即可读取；reserved 复用为 portrait (四向: 0=横屏 1=竖屏 2=横屏翻转 3=竖屏翻转,
// 旧数据 0/1 含义不变=兼容编码, 数值语义由 file_manager.ino storedToRot/rotToStored 隔离)
struct SettingsConfig {
    uint32_t magic;        // 0x53455433UL 'SET3'
    uint8_t version;       // 1
    uint8_t clockFormat;   // 0=24小时制 1=12小时制
    int16_t tzOffsetMin;   // 时区偏移（分钟），默认 480 = UTC+8
    uint16_t checksum;
    uint8_t hitokotoEnabled; // 1=时钟页显示一言 0=关闭（容错：非 0/1 视为 1）
    uint8_t portrait;      // 阅读旋转方向 0横/1竖/2横翻/3竖翻（兼容编码, 全局持久: 上次模式下次恢复）
};
// 读取设置；校验失败返回 false 并填充默认值（24小时制 / UTC+8）
bool loadSettingsConfig(SettingsConfig &out);
// 保存设置；成功返回 true
bool saveSettingsConfig(const SettingsConfig &in);
// 便捷读取：时钟格式 (0=24h 1=12h)
uint8_t settingsGetClockFormat();
// 便捷读取：时区偏移（分钟）
int16_t settingsGetTzOffsetMin();
// 便捷读取：一言开关 (1=开 0=关)
uint8_t settingsGetHitokotoEnabled();
// 便捷读取：阅读旋转方向兼容编码 (0横/1竖/2横翻/3竖翻; 业务层须经 storedToRot 转角度)
uint8_t settingsGetPortrait();
// 修改并持久化设置项；成功返回 true
bool settingsSetClockFormat(uint8_t v);
bool settingsSetTzOffsetMin(int16_t v);
bool settingsSetHitokotoEnabled(uint8_t v);
bool settingsSetPortrait(uint8_t v);

// ---- WebDAV 配置（EEPROM 偏移 256, 独立区; 避开 WifiConfig/CLOCK/Weather/Settings）----
// ⚠️ 已弃用（2026, D0 直连手机 HTTP 取代）：结构/EEPROM/保存函数保留不删除，
//    配网页 UI 已隐藏，/webdav 端点仅返回弃用提示；进度同步改走 progress_sync.cpp 直连手机 8384。
struct WebdavConfig {
    uint32_t magic;         // 0x57445632UL 'WDV2'
    char endpoint[128];     // WebDAV 服务器根(如 https://dav.example.com/dav/)
    char username[40];      // Basic Auth 用户名
    char password[40];      // Basic Auth 密码
    uint16_t checksum;
};
// 读取 WebDAV 配置；magic/checksum 校验失败返回 false 并清空(未配置)
bool loadWebdavConfig(WebdavConfig &out);
// 保存 WebDAV 配置（endpoint 非空，长度校验）；成功返回 true
bool saveWebdavConfig(const WebdavConfig &in);

// ---- 连接对象配置（EEPROM 偏移 470, 独立区; 512 字节内剩余尾部）----
// 进度同步直连手机: 局域网/热点下各选一台(运行进度服务器 App 的手机)作为连接对象。
// 局域网 STA 已连 → staIp (默认 192.168.0.10); 设备热点模式 → apIp (默认 192.168.0.100)。
struct TargetConfig {
    uint32_t magic;        // 0x54475254UL 'TGRT'
    char staIp[16];        // 局域网连接对象 IP (如 192.168.0.10, NUL 结尾)
    char apIp[16];         // 热点连接对象 IP (如 192.168.0.100)
    uint16_t checksum;
};
// 读取连接对象配置；校验失败返回 false 并清空；空 IP 填内存默认(不写 EEPROM)
bool loadTargetConfig(TargetConfig &out);
// 保存连接对象配置；成功返回 true
bool saveTargetConfig(const TargetConfig &in);
// 配网模式开启时向连接对象推送设备信息(HTTP POST, 非阻塞尝试); 失败静默
void wifiManagerPushDeviceInfo();

// ---- 进度同步直连手机 (纯 AP / 目标地址) ----
// 纯 AP: WIFI_AP(STA 断开, 规避 AP/STA 同子网路由歧义), softAP 192.168.0.1/24,
// DHCP 固定租约仅 192.168.0.100 (手机热点模式固定地址)。SSID/密码与配网热点一致。
bool wifiManagerStartApOnly();
// 目标手机 IP: STA 已连且 staIp 非空 → staIp；否则 → apIp (loadTargetConfig 后取, 含默认值)
const char* wifiManagerSyncTarget();
// 调试: 当前 softAP IP 字符串 (如 192.168.0.1)
const char* wifiManagerApIp();

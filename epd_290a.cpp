/**
 * @file epd_290a.cpp
 * @brief 2.9 inch A01 e-paper driver (IL3820 / GDEH029A1)
 * @note init sequence aligned with GxEPD2_290 (used by original firmware)
 *       BUSY: HIGH = busy, DC=GPIO0, BUSY=GPIO4
 */

#include "epd_290a.h"

// LUT full update - same as GxEPD2_290 LUTDefault_full
const uint8_t EPD_290A::lut_full_update[] = {
    0x50, 0xAA, 0x55, 0xAA, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// LUT partial update - same as GxEPD2_290 LUTDefault_part
const uint8_t EPD_290A::lut_partial_update[] = {
    0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x13, 0x14, 0x44, 0x12,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

EPD_290A::EPD_290A() {
    _spiSettings = SPISettings(4000000, MSBFIRST, SPI_MODE0);
}

void EPD_290A::init() {
    // init pins
    pinMode(EPD_CS_PIN, OUTPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    pinMode(EPD_DC_PIN, OUTPUT);
    digitalWrite(EPD_DC_PIN, HIGH);
    pinMode(EPD_RST_PIN, OUTPUT);
    digitalWrite(EPD_RST_PIN, HIGH);
    pinMode(EPD_BUSY_PIN, INPUT);

    // init SPI
    SPI.begin();

    // init panel
    reset();
    initDisplay();
}

void EPD_290A::reset() {
    digitalWrite(EPD_RST_PIN, LOW);
    delay(20);
    digitalWrite(EPD_RST_PIN, HIGH);
    delay(200);
    // after reset, panel should become idle (BUSY=LOW)
    unsigned long start = millis();
    while (digitalRead(EPD_BUSY_PIN) == HIGH && millis() - start < 1000) {
        delay(10);
    }
}

void EPD_290A::sleep() {
    sendCommand(0x10);
    sendData(0x01);
    delay(100);
    // 注意: 不能把 RST(GPIO2) 拉低 — ESP-12F 板载蓝灯低电平点亮,
    // 休眠期间保持 RST 高电平熄灭 LED; SSD1680 深睡只需 RST 脉冲唤醒。
    digitalWrite(EPD_RST_PIN, HIGH);
}

// 上电 (booster on): 0x22 0xC0 -> 0x20。断电(powerOff)后再次刷新前必须调用。
// GxEPD2_290 同款时序; SSD1680 断电不丢 RAM, 无需 reset 即可重新上电。
void EPD_290A::powerOn() {
    sendCommand(0x22);
    sendData(0xC0);
    sendCommand(0x20);
    waitBusy(2000);
}

// 断电: 0x02。刷新完成后关断 DC-DC, 面板保持当前图像;
// 省电 + 防面板长期带电老化, 对齐官方"画完 display.powerOff()"。
void EPD_290A::powerOff() {
    sendCommand(0x02);
    waitBusy(1000);
}

// SPI ops
void EPD_290A::beginTransfer() {
    SPI.beginTransaction(_spiSettings);
    digitalWrite(EPD_CS_PIN, LOW);
}

void EPD_290A::endTransfer() {
    digitalWrite(EPD_CS_PIN, HIGH);
    SPI.endTransaction();
}

void EPD_290A::spiTransfer(uint8_t data) {
    SPI.transfer(data);
}

// CRITICAL: every transfer MUST assert CS (LOW), else panel ignores everything
void EPD_290A::sendCommand(uint8_t cmd) {
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, LOW);
    SPI.beginTransaction(_spiSettings);
    SPI.transfer(cmd);
    SPI.endTransaction();
    digitalWrite(EPD_CS_PIN, HIGH);
#if EPD_DEBUG
    Serial.printf("C:%02X ", cmd);
#endif
}

void EPD_290A::sendData(uint8_t data) {
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);
    SPI.beginTransaction(_spiSettings);
    SPI.transfer(data);
    SPI.endTransaction();
    digitalWrite(EPD_CS_PIN, HIGH);
}

void EPD_290A::sendDataBulk(const uint8_t *data, int len) {
    digitalWrite(EPD_CS_PIN, LOW);
    digitalWrite(EPD_DC_PIN, HIGH);
    SPI.beginTransaction(_spiSettings);
    for (int i = 0; i < len; i++) {
        SPI.transfer(pgm_read_byte(&data[i]));
    }
    SPI.endTransaction();
    digitalWrite(EPD_CS_PIN, HIGH);
}

// wait busy - HIGH = busy (same as GxEPD2_290 busy_level=HIGH)
void EPD_290A::waitBusy(int timeout) {
    unsigned long start = millis();
    while (digitalRead(EPD_BUSY_PIN) == HIGH) {
        if (millis() - start > timeout) {
            Serial.println("EPD busy timeout!");
            break;
        }
        delay(10);
    }
    _busyMs = millis() - start;
}

// set RAM area (with data entry mode 0x11 0x03)
void EPD_290A::setMemoryArea(int x, int y, int w, int h) {
    sendCommand(0x11);  // data entry mode
    sendData(0x03);     // x increase, y increase

    sendCommand(0x44);  // set RAM X address
    sendData((x >> 3) & 0xFF);
    sendData(((x + w - 1) >> 3) & 0xFF);

    sendCommand(0x45);  // set RAM Y address
    sendData(y & 0xFF);
    sendData((y >> 8) & 0xFF);
    sendData((y + h - 1) & 0xFF);
    sendData(((y + h - 1) >> 8) & 0xFF);
}

// set pointer
void EPD_290A::setPointer(int x, int y) {
    sendCommand(0x4E);
    sendData((x >> 3) & 0xFF);

    sendCommand(0x4F);
    sendData(y & 0xFF);
    sendData((y >> 8) & 0xFF);
}

// init display - aligned with GxEPD2_290::_InitDisplay + _Init_Full
void EPD_290A::initDisplay() {
#if EPD_DEBUG
    Serial.printf("\nBUSY after reset = %d (0=idle, 1=busy)\n", digitalRead(EPD_BUSY_PIN));
#endif
    // driver output control
    sendCommand(0x01);
    sendData((EPD_HEIGHT - 1) & 0xFF);
    sendData(((EPD_HEIGHT - 1) >> 8) & 0xFF);
    sendData(0x00);

    // booster soft start - critical!
    sendCommand(0x0C);
    sendData(0xD7);
    sendData(0xD6);
    sendData(0x9D);

    // VCOM setting (GxEPD2_290 uses 0xA8 for GDEH029A1)
    sendCommand(0x2C);
    sendData(0xA8);

    // dummy line period
    sendCommand(0x3A);
    sendData(0x1A);

    // gate line width
    sendCommand(0x3B);
    sendData(0x08);

    // load LUT (full refresh)
    sendCommand(0x32);
    sendDataBulk(lut_full_update, 30);

    // set RAM area
    setMemoryArea(0, 0, EPD_WIDTH, EPD_HEIGHT);
    setPointer(0, 0);

    // power on (0x22 0xC0 -> 0x20), waitBusy 含在 powerOn() 内
    powerOn();
#if EPD_DEBUG
    Serial.printf("BUSY after power-on = %d (power-on took %lu ms)\n", digitalRead(EPD_BUSY_PIN), _busyMs);
#endif
}

// full display
void EPD_290A::display(const uint8_t *image) {
    waitBusy(2000);
    powerOn();   // 上次刷新后已断电 (powerOff), 刷新前须重新上电

    // set RAM area
    setMemoryArea(0, 0, EPD_WIDTH, EPD_HEIGHT);
    setPointer(0, 0);

    // send image data (CS held low for whole burst)
    sendCommand(0x24);
    beginTransfer();
    digitalWrite(EPD_DC_PIN, HIGH);
    for (int i = 0; i < (EPD_WIDTH * EPD_HEIGHT) / 8; i++) {
        SPI.transfer(image[i]);
    }
    endTransfer();
#if EPD_DEBUG
    Serial.printf("IMG sent %d bytes, BUSY=%d\n", (EPD_WIDTH * EPD_HEIGHT) / 8, digitalRead(EPD_BUSY_PIN));
#endif

    // refresh (full)
    sendCommand(0x22);
    sendData(0xC4);
    sendCommand(0x20);
    sendCommand(0xFF);

    waitBusy(3000);
#if EPD_DEBUG
    Serial.printf("Refresh done, BUSY=%d (refresh took %lu ms)\n", digitalRead(EPD_BUSY_PIN), _busyMs);
#endif
    powerOff();   // 画完断电 (对齐官方 display.powerOff())
}

// partial display - fast refresh, aligned with GxEPD2_290::_Init_Part + _Update_Part
// 关键时序: 必须先 powerOn 再写 0x32 部分 LUT (GxEPD2 的 _InitDisplay 末尾已 _PowerOn,
// 之后 _Init_Part 才写 LUT)。断电状态下写 LUT 不可靠 → 连续局刷(翻页)画面不更新,
// 仅紧跟全刷后的那次局刷有效(实测: 13c2e06 加 powerOff 后翻页局刷失效, 只有第6次全刷显示内容)。
// 局刷后保持面板上电: 连续局刷稳定(与 13c2e06 之前数月行为一致); 断电交给全刷(display)
// 与休眠(epd.sleep / 5分钟自动休眠), 不丢省电收益。
void EPD_290A::displayPartial(const uint8_t *image) {
    waitBusy(2000);

    // ensure power is on BEFORE writing the partial LUT
    powerOn();

    // switch to partial LUT (like GxEPD2_290::_Init_Part)
    sendCommand(0x32);
    sendDataBulk(lut_partial_update, 30);

    // set RAM area
    setMemoryArea(0, 0, EPD_WIDTH, EPD_HEIGHT);
    setPointer(0, 0);

    // send image data to RAM (CS held low)
    sendCommand(0x24);
    beginTransfer();
    digitalWrite(EPD_DC_PIN, HIGH);
    for (int i = 0; i < (EPD_WIDTH * EPD_HEIGHT) / 8; i++) {
        SPI.transfer(image[i]);
    }
    endTransfer();

    // partial refresh (0x22 0x04 = update with RAM LUT, like _Update_Part)
    sendCommand(0x22);
    sendData(0x04);
    sendCommand(0x20);
    sendCommand(0xFF);

    waitBusy(2000);

    // restore full LUT for next full refresh
    sendCommand(0x32);
    sendDataBulk(lut_full_update, 30);
    // 注: 此处不再 powerOff —— 连续局刷时保持上电, 避免断电态写 LUT 导致翻页无显示
}

// clear screen
void EPD_290A::clear(uint8_t color) {
    uint8_t *buffer = (uint8_t *)malloc((EPD_WIDTH * EPD_HEIGHT) / 8);
    if (buffer) {
        memset(buffer, color, (EPD_WIDTH * EPD_HEIGHT) / 8);
        display(buffer);
        free(buffer);
    }
}

// greyscale (simplified, just full display)
void EPD_290A::displayGreyscale(const uint8_t *image, uint8_t depth) {
    display(image);
}

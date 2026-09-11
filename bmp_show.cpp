// bmp_show.cpp — SD 卡 BMP 图片显示（移植自官方 A7 Bmp.ino，适配 SDFS + 本项目帧缓冲）
// 帧缓冲 fb 由 ink-reader-esp.ino 定义（物理 128×296），setPix(x,y,black) 逻辑坐标写入
#include "bmp_show.h"
#include <SD.h>   // 提供 SD 对象（SDFS 实例，File API 与 LittleFS 兼容）
#include <LittleFS.h>   // 壁纸 LittleFS 优先(P1/P2: 阅读期间 SD 已卸载)

#define BMP_W 296
#define BMP_H 128

// 与 ink-reader-esp.ino 共享的帧缓冲（物理 128×296）
extern uint8_t fb[128 * 296 / 8];

// 逻辑坐标写入（与 ink-reader-esp.ino 的 setPix 同映射；setPix 是 inline 无外部符号，此处自带一份）
static inline void bmpSetPix(int x, int y, bool black) {
    if (x < 0 || x >= BMP_W || y < 0 || y >= BMP_H) return;
    int px = y;                     // 物理列 = 逻辑y
    int py = (296 - 1) - x;         // 物理行 = 295 - 逻辑x
    int idx = py * (128 / 8) + (px >> 3);
    uint8_t mask = 0x80 >> (px & 7);
    if (black) fb[idx] &= ~mask; else fb[idx] |= mask;
}

#define BMP_W 296
#define BMP_H 128

// 调色板缓冲（黑白屏只需黑白判定，不需要彩色调色板）
static uint8_t mono_palette[40];   // 最多 256 项/8
static uint8_t color_palette[40];  // 保留官方结构（with_color=false 时不用）

static uint16_t read16(File &f) {
  uint16_t r;
  ((uint8_t *)&r)[0] = (uint8_t)f.read();
  ((uint8_t *)&r)[1] = (uint8_t)f.read();
  return r;
}

static uint32_t read32(File &f) {
  uint32_t r;
  ((uint8_t *)&r)[0] = (uint8_t)f.read();
  ((uint8_t *)&r)[1] = (uint8_t)f.read();
  ((uint8_t *)&r)[2] = (uint8_t)f.read();
  ((uint8_t *)&r)[3] = (uint8_t)f.read();
  return r;
}

bool bmpShowFromSd(const char *path) {
  // 架构(P1/P2): 阅读期间 SD 已卸载; 壁纸属天气/UI 域 → LittleFS 优先, SD 兼容回退(按需重挂载)。
  File file;
  if (LittleFS.begin()) file = LittleFS.open(path, "r");
  if (!file) {
    SD.begin(5, SD_SCK_MHZ(20));
    file = SD.open(path, "r");
  }
  if (!file) return false;

  bool ok = false;
  if (read16(file) == 0x4D42) {                 // 'BM'
    read32(file);                               // fileSize（未用）
    read32(file);                               // reserved（未用）
    uint32_t imageOffset = read32(file);        // 像素数据偏移
    uint32_t headerSize = read32(file);         // DIB 头长度
    (void)headerSize;
    uint32_t width = read32(file);              // 原始宽度
    int32_t height = read32(file);              // 原始高度（负=自顶向下）
    uint16_t planes = read16(file);
    uint16_t depth = read16(file);
    uint32_t format = read32(file);             // 压缩标志 0=未压缩 3=BITFIELDS

    bool flip = true;
    if (height < 0) { height = -height; flip = false; }

    int32_t w = width;                          // 绘制宽度（clamp 到屏幕）
    int32_t h = height;
    if (w > BMP_W) w = BMP_W;
    if (h > BMP_H) h = BMP_H;

    if (w > 0 && h > 0 && planes == 1 && (format == 0 || format == 3) &&
        (depth == 1 || depth == 4 || depth == 8 || depth == 16 || depth == 24)) {
      // 行大小（原始宽度，4 字节对齐）
      uint32_t rowSize = (width * depth / 8 + 3) & ~3u;
      if (depth < 8) rowSize = ((width * depth + 8 - depth) / 8 + 3) & ~3u;

      // 调色板（depth<=8）
      if (depth <= 8) {
        file.seek(imageOffset - (4u << depth), SeekSet);
        for (uint16_t pn = 0; pn < (1u << depth); pn++) {
          uint8_t blue = (uint8_t)file.read();
          uint8_t green = (uint8_t)file.read();
          uint8_t red = (uint8_t)file.read();
          file.read();   // 保留字节
          bool whitish = ((uint16_t)red + green + blue) > (3 * 0x80);
          if (pn % 8 == 0) mono_palette[pn / 8] = 0;
          mono_palette[pn / 8] |= (whitish ? 1u : 0u) << (pn % 8);
        }
      }

      uint8_t bitmask = 0xFF;
      uint8_t bitshift = 8 - depth;
      if (depth < 8) bitmask >>= depth;

      uint8_t input_buffer[150];   // 像素流读块
      uint32_t rowPos = flip ? imageOffset + (uint32_t)((int64_t)height - h) * rowSize : imageOffset;
      for (int32_t row = 0; row < h; row++, rowPos += rowSize) {
        int32_t yrow = flip ? (h - row - 1) : row;   // 逻辑行（自顶向下）
        uint32_t in_remain = rowSize;
        uint32_t in_idx = 0;
        int in_bytes = 0;           // 有符号：SDFS File::read 在 I/O 错误时返回 -1
        uint8_t in_byte = 0;
        uint8_t in_bits = 0;
        file.seek(rowPos, SeekSet);
        for (int32_t col = 0; col < w; col++) {
          if (in_idx >= (uint32_t)in_bytes) {   // 需要读下一块
            in_bytes = file.read(input_buffer, in_remain > sizeof(input_buffer) ? sizeof(input_buffer) : in_remain);
            if (in_bytes <= 0) break;   // 损坏文件/I/O 错误保护（read 失败返回 -1，防死循环防越界）
            in_remain -= (uint32_t)in_bytes;
            in_idx = 0;
          }
          uint16_t red, green, blue;
          bool whitish;
          switch (depth) {
            case 24:
              blue = input_buffer[in_idx++];
              green = input_buffer[in_idx++];
              red = input_buffer[in_idx++];
              whitish = ((red + green + blue) > (3 * 0x80));
              break;
            case 16: {
              uint8_t lsb = input_buffer[in_idx++];
              uint8_t msb = input_buffer[in_idx++];
              if (format == 0) {   // 555
                blue  = (lsb & 0x1F) << 3;
                green = ((msb & 0x03) << 6) | ((lsb & 0xE0) >> 2);
                red   = (msb & 0x7C) << 1;
              } else {             // 565
                blue  = (lsb & 0x1F) << 3;
                green = ((msb & 0x07) << 5) | ((lsb & 0xE0) >> 3);
                red   = (msb & 0xF8);
              }
              whitish = ((red + green + blue) > (3 * 0x80));
              break;
            }
            case 1:
            case 4:
            case 8:
            default: {
              if (in_bits == 0) {
                in_byte = input_buffer[in_idx++];
                in_bits = 8;
              }
              uint16_t pn = (in_byte >> bitshift) & bitmask;
              whitish = mono_palette[pn / 8] & (1u << (pn % 8));
              in_byte <<= depth;
              in_bits -= depth;
              break;
            }
          }
          if (!whitish) bmpSetPix(col, yrow, true);   // 非白 → 黑
        }
      }
      ok = true;
    }
  }
  file.close();
  return ok;
}

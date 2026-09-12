# 外部字体（TTF → .bin）规范与工具

> 目的：让用户把自己喜欢的 TTF 字体转成墨水屏可用的 `.bin` 字库，拷到 SD 卡根目录即可在
> **阅读菜单 → 字体选择 → 外部** 使用（对齐 A7 的"外部字体"入口）。
> 状态（2026-09-12）：**PC 端脚本 + 文件格式已完成并自检通过**；固件端加载/渲染为下一步。

## 1. 为什么不直接放 TTF

* ESP8266 只有 ~26KB 可用堆，TTF 解析（FreeType 级）+ 字形栅格化放不下；墨水屏也不需要矢量缩放。
* 现有正文渲染走 u8g2 `chinese_gb2312`，**CJK 字格固定 16×16、步进 16px**；分页/排版索引是按
  "CJK 14px 宽" 预先算好的，**换字体不能改字格/步进**，否则整本书的页码与换行全变。
  → 所以外部字体 **v1 只替换 CJK 字形**（ASCII 仍由内置字体画，ASCII 宽度表被索引依赖）。

## 2. 生成工具 `make_ext_font.py`

```bash
# 默认：CJK 统一汉字（U+4E00..U+9FA5，≈654KB）
python make_ext_font.py C:\Windows\Fonts\simhei.ttf -o font.bin

# 带标点与全角（推荐）
python make_ext_font.py simhei.ttf -o font.bin --ranges cjk,punct,fullwidth

# TTC 选子字体（如微软雅黑）
python make_ext_font.py msyh.ttc --index 0 -o font.bin

# 打印样例字形点阵（文本核对，不用看图）
python make_ext_font.py simhei.ttf -o font.bin --preview

# 反解已有 .bin（核对固件将读到的点阵）
python make_ext_font.py --dump font.bin 武炼巅峰
```

* `--cell 16`：字格边长，**必须与固件一致**（当前 16）。
* 生成后脚本会**回读自检**：块头一致 + 数据长度足够 + 「一」不是空白（能挡住"字体没加载成功"这类错）。
* 实测：黑体 → 21206 字形 / 662.7 KB / `cjk+punct+fullwidth` ✓；微软雅黑 TTC ✓。

## 3. 文件格式（EFNT v1，小端）

```
偏移 0   4B  magic 'E','F','N','T'
4        1B  version = 1
5        1B  保留 = 0
6        1B  cellW   （CJK 字格宽，16）
7        1B  cellH   （字格高，16）
8        1B  blockCount
9..11    3B  保留 = 0
12       块表，每块 16B：
            u32 startCp   起始码点（含）
            u32 endCp     结束码点（含）
            u32 dataOff   该块位图起始（文件绝对偏移）
            u8  w, u8 h   该块字格宽高
            u16 保留 = 0
12+16*N  位图区：块内按码点升序，每字形 ceil(w/8)*h 字节，每行 MSB-left
```

**关键设计：块必须是"连续码点区间"** —— 固件端用 `(cp - startCp)` 直接算偏移，
**O(1) 查找、零 RAM 索引、零额外读**（SD 随机读一次 32B 即可）。
稀疏码点集（例如"只要 GB2312 收录的字"）会导致下标错位，v1 **不支持**；要更小体积请用更小字格或裁范围。

16×16 字格：`ceil(16/8)*16 = 32 B/字形`；CJK 全区间 20902 字 ≈ 654 KB（SD 卡完全够）。

## 4. 固件端计划（下一步）

1. **导入/探测**：阅读菜单"字体选择 → 外部"→ 打开 `/font.bin`，校验 magic/version/cell（非 16 或不认识 → 回落到内置字体并提示，对齐 A7 "外部字体初始化失败"）。
2. **渲染**：正文绘制改为逐字符分派 —— CJK（U+4E00..U+9FA5 及已含块）从 SD 取 32B 点阵画 16×16；
   ASCII 仍走内置。字格/步进与内置完全一致 ⇒ **排版与索引无需重建**。
3. **缓存**：一节页面里重复字很少，直接"每字一次 seek+32B 读"即可；若实测慢，再加 64~128 字形的 LRU（32B×128 = 4KB）。
4. **总线**：所有 SD 读取前 `reinitSdBus`（共享 SPI；漏了会卡死在 `SPIClass::transfer` 被看门狗复位，已踩过）。
5. **验收**：同一本书分别在"自带/外部"下翻若干页，对比 `TXT restored page=` 与页码不变（索引未变），
   并抽几个字反解点阵与 `--dump` 结果一致。

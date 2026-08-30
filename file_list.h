#pragma once
// file_list.h — 文件列表分页窗口化声明（独立头文件, 避开 Arduino 原型生成器对
// 自定义返回类型函数的前置原型插入 → "FileItem does not name a type" 编译错误）
//
// 背景: 大目录(网络小说II 493 个 txt)不能全量缓存 —— ESP8266 堆启动后仅 ~15KB,
//       493×69B≈34KB 的 malloc/realloc 必失败(实测只显示 7 个)。
// 方案: 只缓存当前可视窗口 LIST_WINDOW 项(静态数组, 不占堆), itemCount 记总数,
//       滚动越界时重新枚举定位窗口(openDir 从头扫, 纯目录项扫描 ~ms 级)。
// 排序: 放弃全局排序(窗口化无法跨窗口排序), 按 SD 目录枚举顺序显示(官方同款)。

#define LIST_ROWS 6
#define LIST_WINDOW (LIST_ROWS * 2)   // 窗口缓冲 12 项 = 当前页 + 预取下页

struct FileItem {
    char name[64];     // UTF-8 文件名
    uint32_t size;
    bool isDir;
};

extern FileItem winItems[LIST_WINDOW];   // 静态窗口缓冲 (12×69B≈828B, 不占堆)
extern int winCount;                     // 窗口内实际项数
extern int itemCount;                    // 目录总项数（第一遍计数）
extern int selIndex;                     // 选中项（全局索引）
extern int topIndex;                     // 窗口首项（全局索引）

// 全局项索引 → 窗口内项指针（idx 不在窗口返回 NULL）
inline FileItem *itemAt(int idx) {
    if (idx < topIndex || idx >= topIndex + winCount) return NULL;
    return &winItems[idx - topIndex];
}

// 释放窗口内容（窗口是静态数组不占堆, 保留仅作语义复位）
inline void freeItemList() {
    winCount = 0;
    itemCount = 0;
}

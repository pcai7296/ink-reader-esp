// rot_map_test.cpp — 四向旋转坐标映射 PC 测试（固件侧，纯函数无 Arduino 依赖）
// 被测实现: rot_map.h（唯一权威坐标规范, file_manager/AGENTS.md 同步维护）
// 物理 fb 恒 128 列 × 296 行; 逻辑画布 rot 0/180=128x296(竖类), 90/270=296x128(横类)
// 编译(MinGW-W64, 项目根): g++ -std=c++11 -Wall -Wextra pc_tests\rot_map_test.cpp -I. -o pc_tests\rot_map_test.exe
// 运行: .\rot_map_test.exe   （全部断言通过输出 ALL PASS，退出码 0）

#include "../rot_map.h"
#include <stdio.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name) do { \
    if (cond) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s\n", name); } \
} while (0)

// 断言 mapFbRot(rot, x, y) 输出 (ex, ey) 且返回 true
#define CHECK_MAP(rot, x, y, ex, ey, name) do { \
    int px = -999, py = -999; \
    bool ok = mapFbRot(rot, x, y, px, py); \
    if (ok && px == (ex) && py == (ey)) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s (rot=%u (%d,%d) -> (%d,%d), want (%d,%d))\n", \
        name, (unsigned)(rot), (x), (y), px, py, (ex), (ey)); } \
} while (0)

// 断言 mapFbRot(rot, x, y) 返回 false（越界/非法）
#define CHECK_INVALID(rot, x, y, name) do { \
    int px = -999, py = -999; \
    bool ok = mapFbRot(rot, x, y, px, py); \
    if (!ok) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s (rot=%u (%d,%d) 应拒绝, 却返回 (%d,%d))\n", \
        name, (unsigned)(rot), (x), (y), px, py); } \
} while (0)

int main() {
    printf("== mapFbRot 四向坐标映射测试 ==\n");

    // ---- 0° 竖屏 128x296: 恒等 ----
    CHECK_MAP(0, 0, 0, 0, 0, "rot0 左上 (0,0)->(0,0)");
    CHECK_MAP(0, 127, 0, 127, 0, "rot0 右上 (127,0)->(127,0)");
    CHECK_MAP(0, 0, 295, 0, 295, "rot0 左下 (0,295)->(0,295)");
    CHECK_MAP(0, 127, 295, 127, 295, "rot0 右下 (127,295)->(127,295)");
    CHECK_MAP(0, 63, 147, 63, 147, "rot0 中心 (63,147)->(63,147)");

    // ---- 90° 横屏 296x128: px=y, py=295-x（既有产线公式） ----
    CHECK_MAP(90, 0, 0, 0, 295, "rot90 左上 (0,0)->(0,295)");
    CHECK_MAP(90, 295, 0, 0, 0, "rot90 右上 (295,0)->(0,0)");
    CHECK_MAP(90, 0, 127, 127, 295, "rot90 左下 (0,127)->(127,295)");
    CHECK_MAP(90, 295, 127, 127, 0, "rot90 右下 (295,127)->(127,0)");
    CHECK_MAP(90, 147, 63, 63, 148, "rot90 中心 (147,63)->(63,148)");

    // ---- 180° 竖屏倒置: px=127-x, py=295-y ----
    CHECK_MAP(180, 0, 0, 127, 295, "rot180 左上 (0,0)->(127,295)");
    CHECK_MAP(180, 127, 0, 0, 295, "rot180 右上 (127,0)->(0,295)");
    CHECK_MAP(180, 0, 295, 127, 0, "rot180 左下 (0,295)->(127,0)");
    CHECK_MAP(180, 127, 295, 0, 0, "rot180 右下 (127,295)->(0,0)");
    CHECK_MAP(180, 63, 147, 64, 148, "rot180 中心 (63,147)->(64,148)");

    // ---- 270° 横屏倒置: px=127-y, py=x ----
    CHECK_MAP(270, 0, 0, 127, 0, "rot270 左上 (0,0)->(127,0)");
    CHECK_MAP(270, 295, 0, 127, 295, "rot270 右上 (295,0)->(127,295)");
    CHECK_MAP(270, 0, 127, 0, 0, "rot270 左下 (0,127)->(0,0)");
    CHECK_MAP(270, 295, 127, 0, 295, "rot270 右下 (295,127)->(0,295)");
    CHECK_MAP(270, 147, 63, 64, 147, "rot270 中心 (147,63)->(64,147)");

    // ---- 越界拒绝（各 rot 独立逻辑域） ----
    CHECK_INVALID(0, 128, 0, "rot0 x=128 拒绝");
    CHECK_INVALID(0, 0, 296, "rot0 y=296 拒绝");
    CHECK_INVALID(0, 129, 297, "rot0 x=129 y=297 拒绝");
    CHECK_INVALID(0, -1, 0, "rot0 x=-1 拒绝");
    CHECK_INVALID(0, 0, -1, "rot0 y=-1 拒绝");
    CHECK_INVALID(90, 296, 0, "rot90 x=296 拒绝");
    CHECK_INVALID(90, 0, 128, "rot90 y=128 拒绝");
    CHECK_INVALID(90, 297, 129, "rot90 x=297 y=129 拒绝");
    CHECK_INVALID(90, -1, 0, "rot90 x=-1 拒绝");
    CHECK_INVALID(90, 0, -1, "rot90 y=-1 拒绝");
    CHECK_INVALID(180, 128, 0, "rot180 x=128 拒绝");
    CHECK_INVALID(180, 0, 296, "rot180 y=296 拒绝");
    CHECK_INVALID(180, 129, 297, "rot180 x=129 y=297 拒绝");
    CHECK_INVALID(180, -1, 0, "rot180 x=-1 拒绝");
    CHECK_INVALID(180, 0, -1, "rot180 y=-1 拒绝");
    CHECK_INVALID(270, 296, 0, "rot270 x=296 拒绝");
    CHECK_INVALID(270, 0, 128, "rot270 y=128 拒绝");
    CHECK_INVALID(270, 297, 129, "rot270 x=297 y=129 拒绝");
    CHECK_INVALID(270, -1, 0, "rot270 x=-1 拒绝");
    CHECK_INVALID(270, 0, -1, "rot270 y=-1 拒绝");

    // ---- 非法 rot: 防御保险丝 ----
    CHECK_INVALID(45, 10, 10, "rot=45 非法拒绝");
    CHECK_INVALID(255, 10, 10, "rot=255 非法拒绝");

    printf("== %d pass, %d fail ==\n", g_pass, g_fail);
    if (g_fail == 0) { printf("ALL PASS\n"); return 0; }
    return 1;
}

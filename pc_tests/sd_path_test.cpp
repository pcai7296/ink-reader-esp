// sd_path_test.cpp — 文件 API 路径安全 + 保护规则纯函数 PC 测试
// 被测实现: sd_path.h/.cpp（固件侧纯函数, 无 Arduino 依赖）
// 编译(MinGW-W64, 项目根): g++ -std=c++11 -Wall -Wextra pc_tests\sd_path_test.cpp sd_path.cpp -I. -o pc_tests\sd_path_test.exe
// 运行: .\sd_path_test.exe   （全部断言通过输出 ALL PASS，退出码 0）

#include "../sd_path.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name) do { \
    if (cond) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s\n", name); } \
} while (0)

// normalizeApiPath: 期望成功并得到规范化结果
#define CHECK_NORM_OK(raw, want, name) do { \
    char out[512]; \
    bool ok = normalizeApiPath((raw), out, sizeof(out)); \
    if (ok && strcmp(out, (want)) == 0) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s (raw=[%s] ok=%d out=[%s] want=[%s])\n", name, (raw), ok ? 1 : 0, out, (want)); } \
} while (0)

// normalizeApiPath: 期望拒绝
#define CHECK_NORM_BAD(raw, name) do { \
    char out[512]; \
    bool ok = normalizeApiPath((raw), out, sizeof(out)); \
    if (!ok) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s (raw=[%s] 应拒绝, 却返回 ok out=[%s])\n", name, (raw), out); } \
} while (0)

// sanitizeUploadName: 期望成功并得到结果
#define CHECK_NAME_OK(raw, want, name) do { \
    char out[256]; \
    bool ok = sanitizeUploadName((raw), out, sizeof(out)); \
    if (ok && strcmp(out, (want)) == 0) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s (raw=[%s] ok=%d out=[%s] want=[%s])\n", name, (raw), ok ? 1 : 0, out, (want)); } \
} while (0)

// sanitizeUploadName: 期望拒绝
#define CHECK_NAME_BAD(raw, name) do { \
    char out[256]; \
    bool ok = sanitizeUploadName((raw), out, sizeof(out)); \
    if (!ok) { g_pass++; printf("PASS: %s\n", name); } \
    else { g_fail++; printf("FAIL: %s (raw=[%s] 应拒绝, 却返回 ok out=[%s])\n", name, (raw), out); } \
} while (0)

int main() {
    printf("== normalizeApiPath 路径规范化/安全 ==\n");

    // 合法路径
    CHECK_NORM_OK("/", "/", "根目录");
    CHECK_NORM_OK("/books", "/books", "单层目录");
    CHECK_NORM_OK("/books/test.txt", "/books/test.txt", "深层文件");
    CHECK_NORM_OK("/books/", "/books/", "目录尾斜杠保留");
    CHECK_NORM_OK("//books///test.txt", "/books/test.txt", "折叠连续斜杠");
    CHECK_NORM_OK("/中文/文件名.txt", "/中文/文件名.txt", "UTF-8 中文路径");
    CHECK_NORM_OK("/a b/c+d", "/a b/c+d", "空格与+号保留(已解码)");
    CHECK_NORM_OK("/.tiemereader", "/.tiemereader", "系统目录本身");
    CHECK_NORM_OK("/.tiemereader/weather.dat", "/.tiemereader/weather.dat", "系统目录内文件");
    CHECK_NORM_OK("/.tiemereader_backup/x", "/.tiemereader_backup/x", "相似前缀非系统目录(边界)");

    // 非法路径: 必须拒绝
    CHECK_NORM_BAD("", "空串拒绝");
    CHECK_NORM_BAD("books", "无前导斜杠拒绝");
    CHECK_NORM_BAD("../etc/passwd", "路径穿越 .. 开头拒绝");
    CHECK_NORM_BAD("/../etc/passwd", "路径穿越 /../ 拒绝");
    CHECK_NORM_BAD("/books/../../secret", "路径穿越 深层 .. 拒绝");
    CHECK_NORM_BAD("/books/./test.txt", "点段 . 拒绝(不归一化)");
    CHECK_NORM_BAD("/books/..", "尾部 .. 拒绝");
    CHECK_NORM_BAD("/foo\\bar", "反斜杠拒绝");
    CHECK_NORM_BAD("/foo\\..\\bar", "反斜杠穿越拒绝");
    CHECK_NORM_BAD("/foo/bar\x01", "控制字符拒绝");
    CHECK_NORM_BAD("/foo\x7f", "DEL 控制字符拒绝");

    // URL 编码穿越: server.arg 已解码后到达本函数。编码形态按字面量处理（无穿越风险——
    // 字面 "%2e%2e" 只是普通文件名, SD.open 不会因此逃逸）；解码后的 "../" 由上一组用例拦截
    CHECK_NORM_OK("/%2e%2e%2fetc", "/%2e%2e%2fetc", "编码形态按字面量处理(客户端须先解码)");
    CHECK_NORM_BAD("/..%5c..", "编码形态: 尾部 .. 段拒绝(解码后形态同样被拦截)");

    // 超长
    {
        char longp[600];
        memset(longp, 'a', sizeof(longp) - 1);
        longp[0] = '/';
        longp[sizeof(longp) - 1] = '\0';
        char out[512];
        CHECK(!normalizeApiPath(longp, out, sizeof(out)), "超长路径拒绝");
    }

    printf("\n== sanitizeUploadName 上传文件名净化 ==\n");
    CHECK_NAME_OK("test.txt", "test.txt", "普通文件名");
    CHECK_NAME_OK("C:\\fakepath\\book.epub", "book.epub", "浏览器 fakepath 剥除");
    CHECK_NAME_OK("/books/novel.txt", "novel.txt", "路径取 basename");
    CHECK_NAME_OK("..\\evil.exe", "evil.exe", "反斜杠穿越取 basename");
    CHECK_NAME_OK("三体.txt", "三体.txt", "中文文件名");
    CHECK_NAME_OK("a b+c.txt", "a b+c.txt", "空格与+号保留");
    CHECK_NAME_OK("weird name .txt", "weird name .txt", "含点与空格");
    CHECK_NAME_BAD("", "空串拒绝");
    CHECK_NAME_BAD(".", "单点拒绝");
    CHECK_NAME_BAD("..", "双点拒绝");
    CHECK_NAME_OK("a/b.txt", "b.txt", "含 / 取 basename(净化而非拒绝, 结果无分隔符)");
    CHECK_NAME_OK("a\\b.txt", "b.txt", "含 \\ 取 basename(净化而非拒绝, 结果无分隔符)");
    CHECK_NAME_BAD("bad\x01name", "控制字符拒绝");
    {
        char longn[300];
        memset(longn, 'x', sizeof(longn) - 1);
        longn[sizeof(longn) - 1] = '\0';
        char out[256];
        CHECK(!sanitizeUploadName(longn, out, sizeof(out)), "超长文件名拒绝(>240)");
    }

    printf("\n== isProtectedPath 保护规则 ==\n");
    // 系统目录: 前缀边界
    CHECK(isProtectedPath("/.tiemereader"), "系统目录本身受保护");
    CHECK(isProtectedPath("/.tiemereader/weather.dat"), "系统目录内文件受保护");
    CHECK(isProtectedPath("/.tiemereader/a/b.txt"), "系统目录深层受保护");
    CHECK(!isProtectedPath("/.tiemereader_backup"), "相似前缀非保护(目录边界)");
    CHECK(!isProtectedPath("/.tiemereader_backup/a.txt"), "相似前缀文件非保护(目录边界)");
    // 索引/数据扩展名: 精确扩展名匹配
    CHECK(isProtectedPath("/books/novel.i1"), ".i1 受保护");
    CHECK(isProtectedPath("/books/novel.z1"), ".z1 受保护");
    CHECK(isProtectedPath("/books/novel.i1p"), ".i1p sidecar 受保护");
    CHECK(isProtectedPath("/books/novel.v1"), ".v1 受保护");
    CHECK(isProtectedPath("/books/novel.vz1"), ".vz1 受保护");
    CHECK(isProtectedPath("/books/novel.v1p"), ".v1p 受保护");
    CHECK(isProtectedPath("/books/novel.bm"), ".bm 标签受保护");
    CHECK(isProtectedPath("/books/novel.bmt"), ".bmt 标签临时受保护");
    CHECK(isProtectedPath("/books/novel.i2"), ".i2 受保护");
    CHECK(isProtectedPath("/books/novel.z2"), ".z2 受保护");
    CHECK(!isProtectedPath("/books/novel.i1.bak"), "子串不误判(.i1.bak 扩展名是 .bak)");
    CHECK(!isProtectedPath("/books/novel.txt"), ".txt 不受保护");
    CHECK(!isProtectedPath("/books/novel.bmp"), ".bmp 不受保护");
    CHECK(!isProtectedPath("/books/novel.i1b"), ".i1b 不误判(非精确扩展名)");
    CHECK(!isProtectedPath("/books/目录.i1x"), "目录名含 .i1x 不误判");
    CHECK(!isProtectedPath("/"), "根目录不受保护");

    printf("\n== isUploadingTemp 临时文件识别 ==\n");
    CHECK(isUploadingTemp("/books/test.txt.uploading"), ".uploading 识别");
    CHECK(!isUploadingTemp("/books/test.txt"), "普通文件非临时");
    CHECK(!isUploadingTemp("/books/test.uploadingx"), "后缀不完整不误判");

    printf("\n== sdMoveDestAllowed 移动防环 ==\n");
    CHECK(sdMoveDestAllowed("/books/a.txt", "/books"), "文件移到同目录父级");
    CHECK(sdMoveDestAllowed("/books/a.txt", "/docs"), "文件移到其他目录");
    CHECK(sdMoveDestAllowed("/books", "/"), "目录移到根");
    CHECK(!sdMoveDestAllowed("/books", "/books"), "移到自身拒绝");
    CHECK(!sdMoveDestAllowed("/books", "/books/fiction"), "移到子孙目录拒绝");
    CHECK(!sdMoveDestAllowed("/books", "/books/fiction/sub"), "移到深层子孙拒绝");
    CHECK(sdMoveDestAllowed("/books", "/bookshelf"), "相似前缀非子孙(允许)");
    CHECK(sdMoveDestAllowed("/books/a.txt", "/books/a.txt.backup"), "目标同名前缀非子孙(允许)");
    CHECK(!sdMoveDestAllowed("", "/x"), "空路径拒绝");
    CHECK(!sdMoveDestAllowed("books", "/x"), "无前导斜杠拒绝");

    printf("\n结果: %d pass, %d fail\n", g_pass, g_fail);
    if (g_fail == 0) { printf("ALL PASS\n"); return 0; }
    return 1;
}

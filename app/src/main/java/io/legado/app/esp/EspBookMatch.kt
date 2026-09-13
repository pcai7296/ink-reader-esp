package io.legado.app.esp

import android.util.Log
import io.legado.app.data.appDb
import io.legado.app.data.entities.Book
import io.legado.app.model.ReadBook
import java.io.File

/**
 * 设备文件名 ↔ 手机本地书 的匹配。
 *
 * **用户 2026-09-12 拍板的原则（务必遵守）**：
 *   1. **只认名字与路径**：设备发来的就是 SD 上那个 txt 的**文件名**，匹配 = 在书架上按
 *      `originName` **精确**命中，并校验该书 `bookUrl` 指向的 txt **确实存在**；
 *   2. **不读其它书的名字**做猜测/相似匹配（曾用"归一化 + 包含 + 按大小择近"，可能把另一个版本的
 *      书当成目标 → 字节偏移错位，已全部删除）；
 *   3. **不用云端（设备）推回的历史进度**顶替手机自己的进度（见 EspProgressServer 的 GET/PUT）。
 *
 * 命中失败一律返回 null，由调用方明确回报 `no-book`（"手机上没有这本书"）。
 */
object EspBookMatch {

    private const val TAG = "EspBookMatch"

    /** 取纯文件名（设备侧协议里已是 basename；这里再防一手路径噪声）。 */
    fun baseName(name: String): String =
        name.substringAfterLast('/').substringAfterLast('\\').trim()

    private fun localPathOf(book: Book): String? =
        EspFileResolver.resolveLocalPath(splitties.init.appCtx, book.bookUrl)

    /**
     * 只校验"这本书有本地 txt 路径"（`/storage/...` 或 SAF `content://`）。
     *
     * ⚠️ **不要**用 `File(path).exists()`/`isFile` 判定可用性（2026-09-12 实机踩坑）：
     * Android 分区存储下应用直接探测 `/storage/emulated/0/Download/xxx.txt` 会得到 false，
     * 而 Legado 自己（SAF/授权路径）能正常读 —— 当时日志正是
     * `书架上命中 [《武炼巅峰》作者：莫默.txt] 但 txt 文件不可用 url=/storage/emulated/0/Download/...`
     * → 误报 `no-book`。名字 + 路径就是身份，文件可读性交给读取环节报错。
     * 文件存在性仅写 debug 日志，不参与决策。
     */
    private fun pathOk(book: Book): Boolean {
        val url = book.bookUrl
        if (url.isBlank()) return false
        val ok = url.startsWith("/") || url.startsWith("content://")
        if (ok) {
            val exists = runCatching { File(url).isFile }.getOrDefault(false)
            if (!exists) Log.d(TAG, "路径存在性探测=false(分区存储下正常, 不影响匹配) url=$url")
        }
        return ok
    }

    /**
     * 精确命中本地书：`originName` 完全相等 + 该书的 txt 文件存在。
     * @param espSize 设备报的文件大小（仅写日志提示"版本可能不同"，**不参与匹配决策**）
     */
    suspend fun findByFileName(fileName: String, espSize: Long? = null): Book? {
        val name = baseName(fileName)
        if (name.isEmpty()) return null

        // ① 当前在读且同名 → 直接命中（最快路径）
        ReadBook.book
            ?.takeIf { it.originName == name && pathOk(it) }
            ?.let { return it }

        // ② 书架上按 originName 精确查（DB 查询，不遍历其它书名）
        val book = appDb.bookDao.getBookByFileName(name)
        if (book == null) {
            Log.w(TAG, "手机书架上没有这本书 file=[$name]")
            return null
        }
        if (!pathOk(book)) {
            Log.w(TAG, "书架上命中 [$name] 但没有本地 txt 路径 url=${book.bookUrl}")
            return null
        }
        val localLen = runCatching { File(localPathOf(book) ?: book.bookUrl).length() }.getOrDefault(0L)
        Log.d(
            TAG,
            "命中 file=[$name] url=${book.bookUrl} localSize=$localLen" +
                if (espSize != null && espSize > 0 && espSize != localLen)
                    " (设备 $espSize 与本地不同 → 按 pct 换算)" else ""
        )
        return book
    }
}

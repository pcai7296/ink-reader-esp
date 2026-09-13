package io.legado.app.esp

import android.util.Log
import io.legado.app.constant.BookType
import io.legado.app.data.appDb
import io.legado.app.data.entities.Book
import io.legado.app.model.ReadBook
import io.legado.app.ui.book.read.page.entities.TextChapter
import io.legado.app.ui.book.read.page.entities.TextPage
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import splitties.init.appCtx
import java.io.File
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

/**
 * ReadBook.saveRead() 的旁观者：
 * 本地书翻页时，防抖 ~500ms 后把当前进度写入 EspPositionStore（GET 服务用）。
 *
 * 也负责翻译字节偏移 ↔ (chapterIndex, charPos) —— 字节偏移是两端互通的标准单位。
 */
object EspSyncProgressBridge {

    private const val TAG = "EspSyncBridge"
    private const val DEBOUNCE_MS = 500L
    /** ESP 一页 ≈ 8 行 × ~20 汉字 × 3B ≈ 480B; 章尾 600B 内视为"标题页"场景 (页首在上一章尾)。 */
    private const val CHAPTER_TAIL_BYTES = 600L

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var debounceJob: Job? = null

    /** ReadBook.saveRead() 末尾调用（非侵入，只旁观）。 */
    fun onReadProgressSaved(book: Book, chapterIndex: Int, chapterPos: Int, ts: Long) {
        if (book.origin != BookType.localTag) return
        if (!EspSyncConfig.enabled) return
        debounceJob?.cancel()
        debounceJob = scope.launch {
            delay(DEBOUNCE_MS)
            if (!EspSyncConfig.enabled) return@launch
            runCatching { savePhonePosition(book, chapterIndex, chapterPos, ts) }
                .onFailure { Log.w(TAG, "保存 ESP 手机进度失败", it) }
        }
    }

    /**
     * 保存手机侧进度到快照（供 ESP GET 用）。
     * 计算字节偏移（实时 chapter list 或从 curTextChapter 内存文本）。
     */
    private suspend fun savePhonePosition(book: Book, chapterIndex: Int, chapterPos: Int, ts: Long) {
        val fileName = book.originName
        if (fileName.isBlank()) return

        // SAF content:// bookUrl → 真实路径 (SAF 导入的书 File(bookUrl) 打不开, 进度曾长期存不上)
        val filePath = EspFileResolver.resolveLocalPath(appCtx, book.bookUrl) ?: return

        // 文件大小
        val size = runCatching {
            File(filePath).length()
        }.getOrDefault(0L)
        if (size <= 0) return

        // 字节偏移
        val byteOffset = computeByteOffset(book, chapterIndex, chapterPos, filePath)
        val pct = if (size > 0) (byteOffset.toFloat() / size * 100f).coerceIn(0f, 100f) else 0f

        EspPositionStore.putPhonePosition(
            bookId = book.bookUrl,
            pos = EspPosition(fileName, byteOffset, size, pct, ts)
        )
    }

    /**
     * 计算当前阅读位置的字节偏移。
     * 优先从内存 curTextChapter 取（快），否则读文件片段。
     */
    private suspend fun computeByteOffset(book: Book, chapterIndex: Int, chapterPos: Int, filePath: String): Long {
        val chapters = appDb.bookChapterDao.getChapterList(book.bookUrl)
        if (chapters.isEmpty()) return 0L
        val idx = chapterIndex.coerceIn(0, chapters.size - 1)
        val chapter = chapters[idx]
        val chapterStart = chapter.start ?: 0L
        val charPos = chapterPos.coerceAtLeast(0)

        // 内存优先
        val curChapter = ReadBook.curTextChapter
        if (curChapter?.chapter?.index == idx) {
            val fullText = curChapter.pages.joinToString("") { page -> page.text }
            val safePos = charPos.coerceAtMost(fullText.length)
            val prefixBytes = fullText.substring(0, safePos).toByteArray(
                charset(book.charset)
            ).size.toLong()
            return chapterStart + prefixBytes
        }

        // 从文件读片段计算 (流式解码)
        return chapterStart + bytesBeforeCharInFile(
            filePath, charset(book.charset), chapterStart, charPos
        )
    }

    private fun charset(cs: String?): Charset {
        return try {
            cs?.takeIf { it.isNotBlank() }?.let { Charset.forName(it) }
                ?: StandardCharsets.UTF_8
        } catch (_: Exception) {
            StandardCharsets.UTF_8
        }
    }

    /**
     * **手机自身**的阅读进度（Legado DB 的 durChapterIndex/durChapterPos）→ LUMI1 字节偏移。
     *
     * 2026-09-12 用户实测：新导入的书在手机上明明有进度（DB `durChapterIndex=5235`），
     * 但设备 GET 却回 `no-progress` —— 因为原先只查 `EspPositionStore.phoneSnapshot`
     * （该快照只由"开启同步后在本机翻过页"的旁观者写入）。
     * 用户要求：**校验/取数只认手机自己的书（名字+路径+本机进度）**，不读设备推回的历史数据。
     * 所以这里用 DB 进度直接换算，任何一步失败返回 null（调用方回 no-progress）。
     */
    suspend fun dbPositionOf(book: Book): EspPosition? {
        val origin = book.originName ?: return null
        val idx = book.durChapterIndex
        if (idx < 0) return null
        val chapters = runCatching { appDb.bookChapterDao.getChapterList(book.bookUrl) }
            .getOrDefault(emptyList())
        if (chapters.isEmpty()) return null
        val ch = chapters.getOrNull(idx) ?: return null
        val start = ch.start ?: return null
        val total = chapters.lastOrNull()?.end ?: 0L
        if (total <= 0L) return null
        val path = EspFileResolver.resolveLocalPath(appCtx, book.bookUrl)
        // 分区存储下可能读不到文件 → bytesBeforeCharInFile 返回 0 → 退化为"章首偏移"(仍可用)
        val bytes = if (path != null) {
            bytesBeforeCharInFile(path, charset(book.charset), start, book.durChapterPos.coerceAtLeast(0))
        } else 0L
        val offset = (start + bytes).coerceIn(0L, total)
        val pct = (offset.toDouble() * 100.0 / total.toDouble()).toFloat()
        Log.d(TAG, "DB 进度 → offset=$offset size=$total pct=$pct ch=$idx pos=${book.durChapterPos} title=[${book.durChapterTitle}]")
        return EspPosition(
            fileName = origin,
            byteOffset = offset,
            size = total,
            pct = pct,
            ts = book.durChapterTime
        )
    }

    /**
     * 在文件 [filePath] 的 [startByte, +∞) 区间内，取前 [charCount] 个字符的字节长度。
     * CharsetDecoder 流式解码（保留跨块未完成字符），O(章节大小)，毫秒级。
     * 被 EspProgressServer 复用（internal）。
     */
    internal fun bytesBeforeCharInFile(
        filePath: String,
        cs: Charset,
        startByte: Long,
        charCount: Int
    ): Long {
        if (charCount <= 0) return 0L
        val file = File(filePath)
        if (!file.exists() || file.length() <= startByte) return 0L
        return try {
            file.inputStream().use { fis ->
                fis.skip(startByte)
                val decoder = cs.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPLACE)
                    .onUnmappableCharacter(CodingErrorAction.REPLACE)
                val byteBuf = ByteBuffer.allocate(8192)
                val charBuf = CharBuffer.allocate(8192)
                var charsSeen = 0
                var bytesConsumed = 0L
                while (charsSeen < charCount) {
                    byteBuf.compact()
                    val n = fis.read(byteBuf.array(), byteBuf.position(), byteBuf.remaining())
                    if (n < 0) {
                        // EOF: 剩余 held 字节不足一个完整字符, 就此结束
                        break
                    }
                    byteBuf.limit(byteBuf.position() + n)
                    byteBuf.position(0)
                    val before = charBuf.position()
                    decoder.decode(byteBuf, charBuf, false)
                    val decodedChars = charBuf.position() - before
                    val consumedBytes = byteBuf.position()
                    if (charsSeen + decodedChars >= charCount && decodedChars > 0) {
                        // 目标字符在本批内: 前 need 个字符重编码回字节, 得精确偏移
                        val need = charCount - charsSeen
                        val prefixLen = String(charBuf.array(), before, need).toByteArray(cs).size
                        return bytesConsumed + prefixLen
                    }
                    charsSeen += decodedChars
                    bytesConsumed += consumedBytes
                    charBuf.clear()
                }
                bytesConsumed
            }
        } catch (_: Exception) {
            0L
        }
    }

    /**
     * 字节偏移 → (chapterIndex, charPos)。
     * 二分章节列表 + 从 curTextChapter 或文件计算章内字符偏移。
     */
    suspend fun byteOffsetToChapterChar(
        book: Book,
        byteOffset: Long
    ): Pair<Int, Int> {
        val chapters = appDb.bookChapterDao.getChapterList(book.bookUrl)
        if (chapters.isEmpty()) return 0 to 0

        // 二分: 最后一个 start <= byteOffset 的章节 (章节按字节偏移单调递增)
        var chapterIdx = 0
        var lo = 0
        var hi = chapters.size - 1
        while (lo <= hi) {
            val mid = (lo + hi) / 2
            if ((chapters[mid].start ?: 0L) <= byteOffset) {
                chapterIdx = mid
                lo = mid + 1
            } else {
                hi = mid - 1
            }
        }

        // 章尾修正 (标题页场景): ESP 页首常落在上一章末尾 (该页内容含下一章标题),
        // 按字节定位会落到上一章 → 若 offset 距本章 end 不足一页字节量, 对齐到下一章开头
        // (= ESP 视觉上的"下一页"; 用户在章尾正常阅读时下一页也恰是下一章开头, 不误伤)
        val chapterEnd = chapters[chapterIdx].end ?: Long.MAX_VALUE
        if (chapterIdx < chapters.size - 1 && byteOffset >= chapterEnd - CHAPTER_TAIL_BYTES) {
            return (chapterIdx + 1) to 0
        }

        val chapter = chapters[chapterIdx]
        val chapterStart = chapter.start ?: 0L
        val cs = charset(book.charset)

        val charPos: Int
        val curChapter = ReadBook.curTextChapter
        if (curChapter?.chapter?.index == chapterIdx) {
            // 从内存 curTextChapter.text 计算
            val fullText = curChapter.pages.joinToString("") { page -> page.text }
            val localByteOffset = (byteOffset - chapterStart).toInt().coerceAtLeast(0)
            val prefixBytes = fullText.toByteArray(cs).copyOf(minOf(localByteOffset, fullText.toByteArray(cs).size))
            charPos = String(prefixBytes, cs).length
        } else {
            // 读文件片段 (流式解码); SAF content:// → 解析真实路径
            val byteLen = (byteOffset - chapterStart).toInt().coerceAtLeast(0)
            val filePath = EspFileResolver.resolveLocalPath(appCtx, book.bookUrl)
            charPos = if (filePath != null) {
                charsBeforeByteInFile(filePath, cs, chapterStart, byteLen)
            } else 0
        }
        return chapterIdx to charPos.coerceAtLeast(0)
    }

    /**
     * 在文件 [filePath] 的 [startByte, startByte+byteLen) 内，统计完整解码的字符数。
     * CharsetDecoder 流式解码；byteLen 若落在多字节字符中间则向下取整（少计半个字符，安全）。
     */
    internal fun charsBeforeByteInFile(
        filePath: String,
        cs: Charset,
        startByte: Long,
        byteLen: Int
    ): Int {
        if (byteLen <= 0) return 0
        return try {
            File(filePath).inputStream().use { fis ->
                fis.skip(startByte)
                val decoder = cs.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPLACE)
                    .onUnmappableCharacter(CodingErrorAction.REPLACE)
                val byteBuf = ByteBuffer.allocate(8192)
                val charBuf = CharBuffer.allocate(8192)
                var charCount = 0
                var target = byteLen
                while (target > 0) {
                    byteBuf.compact()
                    val n = fis.read(byteBuf.array(), byteBuf.position(), minOf(byteBuf.remaining(), target))
                    if (n <= 0) break
                    target -= n
                    byteBuf.limit(byteBuf.position() + n)
                    byteBuf.position(0)
                    val before = charBuf.position()
                    decoder.decode(byteBuf, charBuf, false)
                    charCount += charBuf.position() - before
                    charBuf.clear()
                }
                charCount
            }
        } catch (_: Exception) {
            0
        }
    }
}

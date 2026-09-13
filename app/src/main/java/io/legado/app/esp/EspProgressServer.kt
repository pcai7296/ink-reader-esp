package io.legado.app.esp

import fi.iki.elonen.NanoHTTPD
import io.legado.app.constant.BookType
import io.legado.app.data.appDb
import io.legado.app.data.entities.Book
import io.legado.app.model.ReadBook
import kotlinx.coroutines.runBlocking
import splitties.init.appCtx
import java.io.File
import java.nio.charset.Charset
import java.nio.charset.StandardCharsets

/**
 * ESP 进度服务器：NanoHTTPD 监听 8384。
 * 传入 bindIp（局域网 IP）时仅监听该接口 → 移动数据流量无法到达（方案一：只接受局域网）。
 * 不传或为空时监听 0.0.0.0（全接口）。
 */
class EspProgressServer private constructor(
    private val serverPort: Int,
    hostname: String
) : NanoHTTPD(hostname, serverPort) {

    override fun serve(session: IHTTPSession): Response {
        // ★ 2026-09-12 健壮性: 只服务**局域网来源**（绑 0.0.0.0 后，手机切到移动数据时会同时暴露在蜂窝侧）
        val remote: String? = runCatching { session.remoteIpAddress }.getOrNull()
        // fail-closed: 来源 IP 取不到时不服务（原来 remote==null 会放行）
        if (remote == null || !isPrivateIp(remote)) {
            android.util.Log.w(TAG, "拒绝非局域网来源: $remote")
            return toResponse(403 to "lan only")
        }
        val method = session.method.name
        val uri = session.uri
        val result = runCatching {
            runBlocking {
                handleProgressRequest(
                    method = method,
                    path = uri,
                    params = LumiProgressCodec.rawParams(session.queryParameterString),
                    body = readBody(session),
                    remoteIp = remote            // ★ 2026-09-13: 请求来源就是墨水屏，用于记录"设备联系过我们"
                )
            }
        }.getOrElse { error ->
            android.util.Log.e(TAG, "serve $method $uri failed: ${error.message}", error)
            500 to "internal error"
        }
        // ★ 只有成功（200）才算一次有效联系；404/403 等不记，避免把误报写进状态
        if (result.first == 200) {
            runCatching {
                val pct = Regex("pct=([0-9.]+)").find(result.second)?.groupValues?.get(1)?.toFloatOrNull()
                EspSyncConfig.onDeviceContact(remote, if (method == "PUT") "推送" else "拉取", pct)
            }
        }
        android.util.Log.d(TAG, "REQ $method $uri -> ${result.first} ${result.second.take(48)}")
        return toResponse(result)
    }

    /** 按 Content-Length 读 body；3s 上限：body 不完整时防连接线程无限挂起。 */
    private fun readBody(session: IHTTPSession): String {
        val contentLength = session.headers["content-length"]?.toLongOrNull() ?: 0L
        if (contentLength <= 0L) return ""
        val toRead = minOf(contentLength, 513L).toInt()
        val bytes = ByteArray(toRead)
        var total = 0
        val input = session.inputStream
        val deadline = System.currentTimeMillis() + 3000L
        while (total < bytes.size) {
            if (System.currentTimeMillis() >= deadline) break
            val n = input.read(bytes, total, bytes.size - total)
            if (n < 0) break
            total += n
        }
        return String(bytes, 0, total, StandardCharsets.UTF_8)
    }

    /** 私网地址判定（RFC1918 + 回环），用于"只服务局域网来源"。 */
    private fun isPrivateIp(ip: String): Boolean =
        ip.startsWith("10.") || ip.startsWith("192.168.") || ip.startsWith("127.") ||
            Regex("^172\\.(1[6-9]|2[0-9]|3[01])\\.").containsMatchIn(ip)
    private fun toResponse(result: Pair<Int, String>): Response {
        val status = when (result.first) {
            200 -> NanoHTTPD.Response.Status.OK
            400 -> NanoHTTPD.Response.Status.BAD_REQUEST
            404 -> NanoHTTPD.Response.Status.NOT_FOUND
            else -> NanoHTTPD.Response.Status.INTERNAL_ERROR
        }
        val body = result.second
        val resp = newFixedLengthResponse(status, "text/plain; charset=utf-8", body)
        // 防 keep-alive: NanoHTTPD 一连接一线程, 连接不关闭会挂起线程,
        // 实测运行一段时间后新请求排队 30-40s (重启服务才恢复)。强制关闭连接。
        resp.addHeader("Connection", "close")
        return resp
    }

    companion object {
        private const val TAG = "EspProgressServer"
        const val PROGRESS_PATH = "/progress"
        const val DEVICE_KEY_PREFIX = "file:"

        /** 创建服务器实例；bindIp 为空时监听 0.0.0.0（全接口）。 */
        fun create(serverPort: Int = 8384, bindIp: String? = null): EspProgressServer {
            return EspProgressServer(serverPort, bindIp ?: "0.0.0.0")
        }
    }
}

const val DEF_PORT = 8384

/**
 * 路由纯函数。
 * @param body PUT body（LUMI1 文本）
 */
suspend fun handleProgressRequest(
    method: String,
    path: String,
    params: Map<String, List<String>>,
    body: String,
    /** 请求来源 IP（= 墨水屏 IP）；仅用于记录"设备联系过我们"，不影响协议行为。 */
    remoteIp: String? = null,
): Pair<Int, String> {
    if (path != EspProgressServer.PROGRESS_PATH) {
        return 404 to "not found"
    }
    if (method == "GET") {
        val fileNameRaw = params["file"]?.firstOrNull() ?: return 400 to "missing file param"
        val fileName = LumiProgressCodec.decodePercent(fileNameRaw)
        // ★ 2026-09-12（用户拍板）：书**不在手机书架上**时必须明确回报，而不是拿旧快照瞎返回或静默失败。
        //   先按"归一化文件名"在书架里核实；不中 → 404 + 可识别 body `no-book`（ESP 显示"手机上没有这本书"）。
        val book = EspBookMatch.findByFileName(fileName) ?: run {
            android.util.Log.w("EspProgressServer", "GET /progress 手机书架上没有这本书 file=[$fileName]")
            return 404 to "no-book"
        }
        // 优先 ESP 位置快照；没有则用**手机自身 DB 进度**换算（2026-09-12：新导入的书在 DB 里有进度，
        // 原先只查快照 → 误回 no-progress；两者都是"手机自己的进度"，不涉及设备推回的数据）
        val progress = EspPositionStore.getPhonePosition(book.bookUrl)
            ?: EspSyncProgressBridge.dbPositionOf(book)
        if (progress == null) {
            // ★ 2026-09-12（用户实测踩坑）：手机删旧书→导入同名新书后，deviceSnapshot 里仍残留
            //   之前 ESP PUT 过的旧进度；原先 findProgressByFileName 兜底把它捞出来返回给 ESP，
            //   ESP 再推回来 → 新书直接被覆写成旧位置。
            //   修法：手机侧还没为这本书存过自己的进度时，不返回任何过期数据，
            //   明确告知"手机无此书进度"。
            android.util.Log.w("EspProgressServer", "GET /progress 手机侧无该书进度 file=[$fileName] bookUrl=${book.bookUrl}")
            return 404 to "no-progress"
        }
        android.util.Log.d("EspProgressServer", "GET /progress 取手机进度 bookUrl=${book.bookUrl} off=${progress.byteOffset}")
        // v3: 附手机文件指纹（实时快照；失败→无指纹→ESP 侧 UNKNOWN）—— 指纹永远描述**本地这本**文件
        val fp = EspFileResolver.resolveLocalPath(appCtx, book.bookUrl)?.let { FileFingerprintTool.of(it) }
        val payload = LumiProgressCodec.encode(
            ts = progress.ts,
            size = progress.size,
            offset = progress.byteOffset,
            pct = progress.pct,
            fs = fp?.size,
            h0 = fp?.h0,
            h1 = fp?.h1,
            h2 = fp?.h2
        )
        return 200 to payload
    }
    if (method == "PUT") {
        val parsed = LumiProgressCodec.parse(body) ?: return 400 to "invalid LUMI1"
        val fileName = extractFileFromBody(body) ?: parsed.file
            ?: return 400 to "missing file in body"
        // ★ 2026-09-12（用户拍板）：书不在手机书架上 → **直接回报 no-book**（ESP 显示"手机上没有这本书"），
        //   不要先收下进度再让用户点弹窗时才失败（原先就是这个观感：ESP 说"同步成功"、手机点跳转没反应）。
        val book = EspBookMatch.findByFileName(fileName, parsed.size) ?: run {
            android.util.Log.w("EspProgressServer", "PUT /progress 手机书架上没有这本书 file=[$fileName]")
            return 404 to "no-book"
        }
        val key = book.bookUrl
        // v3: 指纹组随推送存入（原子——hasFingerprint 才传，否则全 null）
        EspPositionStore.putDevicePosition(
            key, parsed.toPosition(fileName), pendingConfirm = true,
            espFs = if (parsed.hasFingerprint) parsed.fs else null,
            espH0 = if (parsed.hasFingerprint) parsed.h0 else null,
            espH1 = if (parsed.hasFingerprint) parsed.h1 else null,
            espH2 = if (parsed.hasFingerprint) parsed.h2 else null
        )
        return 200 to "ok"
    }
    return 400 to "unsupported method"
}

private fun extractFileFromBody(body: String): String? {
    for (line in body.split('\n')) {
        val trimmed = line.trim()
        if (trimmed.startsWith("file=")) {
            return trimmed.substring(5)
        }
    }
    return null
}

private fun LumiProgress.toPosition(fileName: String) = EspPosition(
    fileName = fileName,
    byteOffset = offset,
    size = size,
    pct = pct,
    ts = ts
)

/** 按文件名找进度（含 bookUrl）：当前在读 → 快照/DB。返回 (位置, bookUrl)。 */
private suspend fun findProgressByFileName(fileName: String): Pair<EspPosition, String>? {
    // 1. 当前阅读器正在读的书
    val live = ReadBook.book
    if (live != null && live.origin == BookType.localTag && live.originName == fileName) {
        val livePos = computeLiveProgress(live)
        if (livePos != null) return livePos to live.bookUrl
    }
    // 2. 从快照/DB 查（key=bookUrl 或 file: 伪 key）
    return EspPositionStore.findByFileName(fileName)?.let { it.second to it.first }
}

/** 计算当前在读书的字节进度（实时，从内存章节文本计算）。 */
private fun computeLiveProgress(book: Book): EspPosition? {
    val fileName = book.originName
    if (fileName.isBlank()) return null
    // 文件大小 (SAF content:// bookUrl → 解析真实路径)
    val filePath = EspFileResolver.resolveLocalPath(appCtx, book.bookUrl) ?: return null
    val size = try { File(filePath).length() } catch (_: Exception) { 0L }
    if (size <= 0) return null
    // 章节列表
    val chapters = appDb.bookChapterDao.getChapterList(book.bookUrl)
    if (chapters.isEmpty()) return null
    val idx = book.durChapterIndex.coerceIn(0, chapters.size - 1)
    val chapter = chapters[idx]
    val chapterStart = chapter.start ?: 0L
    val charset = if (book.charset.isNullOrBlank()) StandardCharsets.UTF_8
                  else try { Charset.forName(book.charset!!) } catch (_: Exception) { StandardCharsets.UTF_8 }
    val charPos = book.durChapterPos.coerceAtLeast(0)
    // 从内存 curTextChapter 文本直接算，避免读文件
    val textChapter = ReadBook.curTextChapter
    val byteOffset: Long
    if (textChapter != null && textChapter.chapter.index == idx) {
        // 从内存章节文本取前 charPos 字符的字节长度
        val text = textChapter.pages.joinToString("") { page -> page.text }
        val charCount = charPos.coerceAtMost(text.length)
        val prefixBytes = text.substring(0, charCount).toByteArray(charset).size.toLong()
        byteOffset = chapterStart + prefixBytes
    } else {
        // 读文件片段 (流式解码, 处理跨块多字节字符)
        byteOffset = chapterStart + EspSyncProgressBridge.bytesBeforeCharInFile(filePath, charset, chapterStart, charPos)
    }
    val pct = if (size > 0) (byteOffset.toFloat() / size * 100).coerceIn(0f, 100f) else 0f
    return EspPosition(fileName, byteOffset, size, pct, System.currentTimeMillis())
}

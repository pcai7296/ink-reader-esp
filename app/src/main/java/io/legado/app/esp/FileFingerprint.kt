package io.legado.app.esp

import java.io.RandomAccessFile
import java.security.MessageDigest

/**
 * v3 文件指纹快照（docs/progress-lumi1.md §3.4）。
 * 区域公式与固件 lumiFingerprintRegions / 单测 LumiProgressCodecTest 完全一致：
 *   h0 = [0, min(size,1024));  h1 = [max(0,size/2-512), min(size,size/2+512));  h2 = [max(0,size-1024), size)
 * 单次打开文件生成不可变快照；任一步失败返回 null（整组省略，绝无半组）。
 */
data class FileFingerprint(
    val size: Long,
    val h0: String,
    val h1: String,
    val h2: String,
)

object FileFingerprintTool {

    /** 生成快照；open/seek/read/hash 任一步失败 → null。 */
    fun of(path: String): FileFingerprint? {
        return try {
            RandomAccessFile(path, "r").use { raf ->
                val size = raf.length()
                fun region(start: Long, end: Long): String {
                    val len = (end - start).toInt()
                    val buf = ByteArray(len)
                    raf.seek(start)
                    raf.readFully(buf)
                    val md = MessageDigest.getInstance("SHA-1")
                    return md.digest(buf).joinToString("") { "%02x".format(it.toInt() and 0xFF) }
                }
                val h0e = minOf(size, 1024L)
                val cen = size / 2
                val h1s = maxOf(0L, cen - 512)
                val h1e = minOf(size, cen + 512)
                val h2s = maxOf(0L, size - 1024)
                FileFingerprint(size, region(0L, h0e), region(h1s, h1e), region(h2s, size))
            }
        } catch (_: Exception) {
            null
        }
    }
}

package io.legado.app.esp

import java.util.Locale

/**
 * LUMI1 阅读进度格式（与固件 progress_lumi.cpp 逐规则 1:1）。
 * 规范: J:\code\esp8266\file_manager\docs\progress-lumi1.md
 */
data class LumiProgress(
    val ts: Long,
    val size: Long,
    val offset: Long,
    val pct: Float,
    val file: String? = null,
    // v3 文件指纹组（原子）：四字段全部非空才 hasFingerprint=true
    val fs: Long? = null,
    val h0: String? = null,
    val h1: String? = null,
    val h2: String? = null,
) {
    val hasFingerprint: Boolean get() = fs != null && h0 != null && h1 != null && h2 != null
}

object LumiProgressCodec {

    const val MAX_LINE = 128
    const val MAX_FILE_NAME = 64
    const val VERSION_LINE = "LUMI1"
    const val MAX_PACKET_BYTES = 256   // 设备 gBuf[256] 接收上限: 编码后实际字节 (含行尾, 不含 \0)

    private const val HEX = "0123456789ABCDEF"

    /** 生成 LUMI1 文本（无指纹组；含结尾 '\n'）。 */
    fun encode(ts: Long, size: Long, offset: Long, pct: Float): String =
        encode(ts, size, offset, pct, null, null, null, null)

    /**
     * 生成 LUMI1 文本（可带 v3 指纹组）。
     * 容量预算：指纹组是原子组——fs/h0/h1/h2 全给才附加；编码后超 256B 硬上限 → 整组省略（绝不截断成半组）。
     */
    fun encode(
        ts: Long, size: Long, offset: Long, pct: Float,
        fs: Long?, h0: String?, h1: String?, h2: String?
    ): String {
        val sb = StringBuilder(192)
        sb.append("LUMI1\nts=$ts\nsize=$size\noffset=$offset\npct=${String.format(Locale.US, "%.2f", pct)}\n")
        val group = if (fs != null && h0 != null && h1 != null && h2 != null)
            "fs=$fs\nh0=$h0\nh1=$h1\nh2=$h2\n" else null
        if (group != null && sb.length + group.length <= MAX_PACKET_BYTES) {
            sb.append(group)
        }
        return sb.toString()
    }

    /** 解析 LUMI1。无效返回 null。 */
    fun parse(text: String): LumiProgress? {
        if (text.isEmpty()) return null

        var gotTs = false
        var gotSize = false
        var gotOffset = false
        var gotPct = false
        var ts = 0L
        var size = 0L
        var offset = 0L
        var pct = 0.0f
        var file: String? = null
        // v3 指纹组（原子）：任一字段缺失/重复/非法 → 整组视为不存在
        var fpBad = false
        var fpFs: Long? = null
        var fpH0: String? = null
        var fpH1: String? = null
        var fpH2: String? = null

        var lineNo = 0
        var pos = 0
        val len = text.length
        while (pos < len) {
            val nl = text.indexOf('\n', pos)
            val end = if (nl >= 0) nl else len
            val line = text.substring(pos, end)

            if (line.indexOf('\r') >= 0) return null
            if (line.toByteArray(Charsets.UTF_8).size > MAX_LINE) return null

            if (lineNo == 0) {
                if (line != VERSION_LINE) return null
            } else {
                val eq = line.indexOf('=')
                if (eq <= 0 || eq == line.length - 1) return null
                val key = line.substring(0, eq)
                val value = line.substring(eq + 1)

                when (key) {
                    "ts" -> {
                        if (gotTs || value.length > 19 || !allDigits(value)) return null
                        ts = java.lang.Long.parseUnsignedLong(value)
                        gotTs = true
                    }
                    "size" -> {
                        if (gotSize || value.length > 10 || !allDigits(value)) return null
                        size = value.toLong()
                        gotSize = true
                    }
                    "offset" -> {
                        if (gotOffset || value.length > 10 || !allDigits(value)) return null
                        offset = value.toLong()
                        gotOffset = true
                    }
                    "pct" -> {
                        if (gotPct) return null
                        val parsed = parsePct(value) ?: return null
                        pct = parsed
                        gotPct = true
                    }
                    "file" -> {
                        if (file == null && value.toByteArray(Charsets.UTF_8).size <= MAX_FILE_NAME) {
                            file = value
                        }
                    }
                    "fs" -> {
                        // v3: uint64 十进制 (≤20 位); 重复/非法 → 整组无效 (不判整包无效)
                        if (fpFs != null || value.length > 20 || !allDigits(value)) fpBad = true
                        else fpFs = java.lang.Long.parseUnsignedLong(value)
                    }
                    "h0" -> {
                        if (fpH0 != null || !isHex40(value)) fpBad = true else fpH0 = value
                    }
                    "h1" -> {
                        if (fpH1 != null || !isHex40(value)) fpBad = true else fpH1 = value
                    }
                    "h2" -> {
                        if (fpH2 != null || !isHex40(value)) fpBad = true else fpH2 = value
                    }
                }
            }

            pos = end
            if (nl >= 0) pos = nl + 1 else break
            lineNo++
        }

        if (!gotTs || !gotSize || !gotOffset || !gotPct) return null
        if (offset > size) return null
        if (pct < 0.0f || pct > 100.0f) return null
        // v3 原子判定: 四字段全部存在且合法 → 完整指纹; 否则整组置 null
        val hasFp = !fpBad && fpFs != null && fpH0 != null && fpH1 != null && fpH2 != null
        return LumiProgress(
            ts, size, offset, pct, file,
            if (hasFp) fpFs else null,
            if (hasFp) fpH0 else null,
            if (hasFp) fpH1 else null,
            if (hasFp) fpH2 else null
        )
    }

    /** v3: 恰好 40 个小写 hex。 */
    private fun isHex40(s: String): Boolean {
        if (s.length != 40) return false
        for (c in s) {
            if (c !in '0'..'9' && c !in 'a'..'f') return false
        }
        return true
    }

    /** RFC3986 文件名 percent-encoding（匹配固件 lumiUrlEncodeFilename）。 */
    fun urlEncodeFilename(s: String): String {
        val sb = StringBuilder(s.length)
        for (b in s.toByteArray(Charsets.UTF_8)) {
            val c = b.toInt() and 0xFF
            if (c in 'A'.code..'Z'.code ||
                c in 'a'.code..'z'.code ||
                c in '0'.code..'9'.code ||
                c == '-'.code || c == '.'.code || c == '_'.code || c == '~'.code
            ) {
                sb.append(c.toChar())
            } else {
                sb.append('%')
                sb.append(HEX[(c shr 4).toInt()])
                sb.append(HEX[(c and 15).toInt()])
            }
        }
        return sb.toString()
    }

    /** RFC3986 percent 解码（lumiUrlEncodeFilename 的逆）。 */
    fun decodePercent(input: String): String {
        if (input.indexOf('%') < 0) return input
        val out = java.io.ByteArrayOutputStream(input.length)
        var i = 0
        while (i < input.length) {
            val c = input[i]
            if (c == '%' && i + 2 < input.length) {
                val hi = Character.digit(input[i + 1], 16)
                val lo = Character.digit(input[i + 2], 16)
                if (hi >= 0 && lo >= 0) {
                    out.write((hi shl 4) or lo)
                    i += 3
                    continue
                }
            }
            val bs = c.toString().toByteArray(Charsets.UTF_8)
            out.write(bs, 0, bs.size)
            i++
        }
        return out.toString(Charsets.UTF_8.name())
    }

    /** 原始 query 字符串拆成未解码参数表（key -> [values]）。 */
    fun rawParams(query: String?): Map<String, List<String>> {
        if (query.isNullOrEmpty()) return emptyMap()
        val result = LinkedHashMap<String, MutableList<String>>()
        for (pair in query.split('&')) {
            if (pair.isEmpty()) continue
            val eq = pair.indexOf('=')
            val key = if (eq >= 0) pair.substring(0, eq) else pair
            val value = if (eq >= 0) pair.substring(eq + 1) else ""
            result.getOrPut(key) { mutableListOf() }.add(value)
        }
        return result
    }

    private fun allDigits(s: String): Boolean {
        if (s.isEmpty()) return false
        for (c in s) if (c !in '0'..'9') return false
        return true
    }

    private fun parsePct(value: String): Float? {
        if (value.isEmpty()) return null
        if (value.length >= 40) return null
        var dots = 0
        for (c in value) {
            when (c) {
                '.' -> { if (++dots > 1) return null }
                in '0'..'9' -> { }
                else -> return null
            }
        }
        return if (value == ".") 0.0f else value.toFloat()
    }
}

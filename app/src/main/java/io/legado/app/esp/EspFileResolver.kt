package io.legado.app.esp

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.MediaStore
import java.io.File

/**
 * SAF content:// (DocumentsContract) → 真实文件路径；file:// 直接截取。
 * 背景: App 通过 SAF 导入本地 txt 后 bookUrl 是 content:// 树/document 形式,
 * 此前代码一律 File(bookUrl.removePrefix("file://")) → 打不开 → 进度/指纹/实时位置全失效。
 * 解析优先级: DocumentsContract docId ("primary:相对路径" 最常见) → MediaStore _data 兜底。
 * 失败返回 null (调用方降级, 不得抛异常)。
 */
object EspFileResolver {

    fun resolveLocalPath(context: Context, bookUrl: String): String? {
        if (bookUrl.isEmpty()) return null
        if (bookUrl.startsWith("file://")) return bookUrl.removePrefix("file://")
        if (!bookUrl.startsWith("content://")) return null

        // 1) DocumentsContract: "<卷标>:<相对路径>", 内置存储卷标 = primary
        try {
            val docId = DocumentsContract.getDocumentId(Uri.parse(bookUrl))
            val sep = docId.indexOf(':')
            if (sep > 0 && sep < docId.length - 1) {
                val authority = docId.substring(0, sep)
                val rel = docId.substring(sep + 1)
                if (authority.equals("primary", true)) {
                    val p = "/storage/emulated/0/$rel"
                    if (File(p).exists()) return p
                } else {
                    // 外置卡卷标 (可能含空格/中文, 先按 /storage/<卷标>/ 试)
                    val p = "/storage/$authority/$rel"
                    if (File(p).exists()) return p
                }
            }
        } catch (_: Exception) {
        }

        // 2) MediaStore 兜底: 部分 content:// (非 DocumentsContract) 可查 _data
        try {
            context.contentResolver.query(
                Uri.parse(bookUrl), arrayOf(MediaStore.MediaColumns.DATA), null, null, null
            )?.use { c ->
                if (c.moveToFirst()) {
                    val p = c.getString(0)
                    if (!p.isNullOrBlank() && File(p).exists()) return p
                }
            }
        } catch (_: Exception) {
        }
        return null
    }
}

package io.legado.app.esp

import android.app.AlertDialog
import android.content.Context
import io.legado.app.R
import io.legado.app.constant.BookType
import io.legado.app.data.appDb
import io.legado.app.data.entities.Book
import io.legado.app.help.LifecycleHelp
import io.legado.app.model.ReadBook
import io.legado.app.ui.book.read.ReadBookActivity
import io.legado.app.utils.startActivityForBook
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import splitties.init.appCtx
import java.io.File
import java.nio.charset.Charset
import java.nio.charset.StandardCharsets

/**
 * ESP 推送进度 → 弹窗/通知 + 确认跳转。
 * 挂载点：EspSyncService 启动后，EspSyncDialog.start() 开始监听 deviceProgressEvents。
 * 前台 Activity → AlertDialog；后台 → 发通知（由调用方处理）。
 */
object EspSyncDialog {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var activeDialog: AlertDialog? = null
    @Volatile
    private var started = false

    /** 开始监听设备进度事件，驱动弹窗（幂等：重复调用不重复 collect）。 */
    fun start() {
        if (started) return
        started = true
        scope.launch {
            EspPositionStore.deviceProgressEvents.collect { (key, devProgress) ->
                // 防崩溃: 弹窗/通知任何一步异常都不允许拖垮 App
                try {
                    if (devProgress.pendingConfirm) showDialog(key, devProgress)
                } catch (e: Exception) {
                    android.util.Log.w("EspSyncDialog", "处理设备进度事件失败", e)
                }
            }
        }
    }

    /**
     * 补弹未处理的设备推送确认框（2026-09-13 后台闭环）。
     * 背景: 推送到达时 App 在后台 → 只发通知，而通知的 extra 原先无人消费、
     * SharedFlow 无 replay → 确认框永远不出现，进度既不生效也不标记已处理。
     * 现由 ReadBookActivity.onResume 调用（点通知打开阅读器自然命中），历史遗留 pending 也获得出口。
     * 服务被用户关闭时不弹（避免已停用同步还弹历史推送）。
     */
    fun showPendingIfAny() {
        if (!EspSyncConfig.enabled) return
        val pending = EspPositionStore.firstPendingConfirm() ?: return
        scope.launch {
            try {
                showDialog(pending.first, pending.second)
            } catch (e: Exception) {
                android.util.Log.w("EspSyncDialog", "补弹设备推送确认框失败", e)
            }
        }
    }

    /**
     * 前台 Activity 中弹出确认对话框。
     */
    private suspend fun showDialog(key: String, devProgress: DeviceProgress) {
        // 取消已显示的旧对话框
        activeDialog?.dismiss()
        activeDialog = null

        val readActivityVisible = LifecycleHelp.isExistActivity(ReadBookActivity::class.java)
        val activity = LifecycleHelp.getTopActivity()
        if (!readActivityVisible || activity == null) {
            // 无前台阅读器 Activity → 交给 EspSyncNotificationHelper 发通知
            EspSyncNotificationHelper.show(key, devProgress)
            return
        }

        val pos = devProgress.pos
        val pctStr = "%.1f".format(pos.pct)
        val offsetStr = "%,d".format(pos.byteOffset)
        // v3 指纹三态: 0=UNKNOWN 1=MATCH 2=MISMATCH; mask bit0=size bit1=head bit2=middle bit3=tail
        val (fpState, fpMask) = withContext(Dispatchers.IO) { compareFingerprint(devProgress) }
        val warnText = if (fpState == 2) buildWarnText(fpMask) else null
        val title = buildString {
            if (warnText != null) append("⚠️ ").append(warnText).append('\n')
            append(
                if (pos.ts == 0L) activity.getString(R.string.esp_sync_confirm_title_no_ts, pctStr)
                else activity.getString(R.string.esp_sync_confirm_title, pctStr)
            )
        }

        activeDialog = AlertDialog.Builder(activity)
            .setTitle(title)
            .setMessage(
                if (warnText != null) {
                    activity.getString(R.string.esp_sync_confirm_msg, pos.fileName, offsetStr) +
                        "\n（继续可能导致阅读位置偏移）"
                } else {
                    activity.getString(R.string.esp_sync_confirm_msg, pos.fileName, offsetStr)
                }
            )
            .setPositiveButton(R.string.confirm) { _, _ ->
                scope.launch(Dispatchers.IO) {
                    applyAndJump(key, devProgress)
                }
            }
            .setNegativeButton(R.string.cancel) { _, _ ->
                scope.launch(Dispatchers.IO) {
                    EspPositionStore.markDevicePositionHandled(key)
                }
            }
            .setOnDismissListener {
                activeDialog = null
            }
            .create()
        activeDialog?.show()
    }

    /** v3 三态比较：ESP 推送指纹 vs 本地书文件指纹（仅双方完整指纹才比较；任一方缺失→UNKNOWN）。 */
    private suspend fun compareFingerprint(devProgress: DeviceProgress): Pair<Int, Int> {
        if (!devProgress.espHasFingerprint) return 0 to 0   // UNKNOWN
        val book = findLocalBookByOriginName(devProgress.pos.fileName, devProgress.pos.size) ?: return 0 to 0
        val path = EspFileResolver.resolveLocalPath(appCtx, book.bookUrl) ?: return 0 to 0
        val local = FileFingerprintTool.of(path) ?: return 0 to 0
        var mask = 0
        if (local.size != devProgress.espFs) mask = mask or 1
        if (local.h0 != devProgress.espH0) mask = mask or 2
        if (local.h1 != devProgress.espH1) mask = mask or 4
        if (local.h2 != devProgress.espH2) mask = mask or 8
        return if (mask == 0) 1 to 0 else 2 to mask
    }

    private fun buildWarnText(mask: Int): String {
        val parts = mutableListOf<String>()
        if (mask and 1 != 0) parts.add("大小")
        if (mask and 2 != 0) parts.add("头部")
        if (mask and 4 != 0) parts.add("中部")
        if (mask and 8 != 0) parts.add("尾部")
        return "文件不一致：${parts.joinToString("/")}"
    }

    /**
     * 确认后：字节偏移 → (chapterIndex, charPos) → 写 DB → 跳转。
     * 入口包 runCatching：任何一步失败都不崩溃 App，只记录状态。
     */
    private suspend fun applyAndJump(key: String, devProgress: DeviceProgress) {
        runCatching {
            doApplyAndJump(key, devProgress)
        }.onFailure {
            android.util.Log.w("EspSyncDialog", "应用设备进度失败", it)
            EspSyncConfig.status = "应用进度失败"
        }
    }

    private suspend fun doApplyAndJump(key: String, devProgress: DeviceProgress) {
        val pos = devProgress.pos
        val fileName = pos.fileName
        // 找本地书（2026-09-12 用户拍板：**只按文件名精确锁定 + 校验 txt 路径存在**，
        // 不读其它书名做猜测匹配，也不用设备推回的历史进度）
        val book = EspBookMatch.findByFileName(fileName, pos.size) ?: run {
            EspSyncConfig.status = "手机上没有这本书：$fileName"
            android.util.Log.w("EspSyncDialog", "手机书架上没有这本书 file=[$fileName]")
            return
        }
        // 跨版本规则 (LUMI1 §4, 镜像固件 APPLY):
        //   本地文件大小 == 推送 size → 直接用字节偏移;
        //   != → 按 pct 换算 (offset 基于 ESP 文件, 直接套本地文件会系统性错位, 如差一章)
        val localSize = runCatching {
            val p = EspFileResolver.resolveLocalPath(appCtx, book.bookUrl)
            if (p != null) File(p).length() else 0L
        }.getOrDefault(0L)
        val targetOffset = if (localSize > 0L && localSize != pos.size &&
            pos.pct.isFinite() && pos.pct in 0f..100f
        ) {
            (pos.pct / 100f * localSize).toLong().coerceIn(0L, localSize)
        } else {
            pos.byteOffset
        }
        // 字节偏移 → (chapterIndex, charPos)
        val (chapterIndex, charPos) = EspSyncProgressBridge.byteOffsetToChapterChar(book, targetOffset)
        // 章节标题同步为新章 (index 与 title 必须一致, 否则恢复定位可能错位)
        val chapters = runCatching {
            appDb.bookChapterDao.getChapterList(book.bookUrl)
        }.getOrDefault(emptyList())
        val newTitle = chapters.getOrNull(chapterIndex)?.title?.takeIf { it.isNotBlank() }
            ?: book.durChapterTitle ?: ""
        // 写 DB（durChapterIndex/durChapterPos/durChapterTime）
        withContext(Dispatchers.IO) {
            // 原版延续版的 BookDao 没有 updateReadProgress(那是自研分支加的)，改用实体赋值 + update
            book.lastCheckCount = book.lastCheckCount
            book.durChapterTitle = newTitle
            book.durChapterIndex = chapterIndex
            book.durChapterPos = charPos
            book.durChapterTime = System.currentTimeMillis()
            appDb.bookDao.update(book)
        }
        // 即时跳转
        EspSyncConfig.status = "已同步 ${"%.1f".format(pos.pct)}%"
        EspSyncConfig.lastSyncTs = System.currentTimeMillis()
        withContext(Dispatchers.Main) {
            val readActivity = LifecycleHelp.isExistActivity(ReadBookActivity::class.java)
            if (readActivity && ReadBook.book?.bookUrl == book.bookUrl) {
                // 同书在读 → openChapter 即时跳转
                ReadBook.openChapter(chapterIndex, charPos.coerceAtLeast(0))
            } else {
                // 关闭现有阅读器，重新打开
                LifecycleHelp.finishActivity(ReadBookActivity::class.java)
                appCtx.startActivityForBook(book) {
                    putExtra("index", chapterIndex)
                    putExtra("chapterPos", charPos.coerceAtLeast(0))
                }
            }
            EspPositionStore.markDevicePositionHandled(key)
        }
    }

    /** 按 originName 找本地书（优先当前在读，其次 DB 查询）。
     *  2026-09-12: 委托给 `EspBookMatch`（精确 → 规范化 → 按 size 择近），见其注释。 */
    private suspend fun findLocalBookByOriginName(fileName: String, espSize: Long? = null): Book? =
        EspBookMatch.findByFileName(fileName, espSize)
}

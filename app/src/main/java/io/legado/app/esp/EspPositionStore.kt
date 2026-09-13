package io.legado.app.esp

import android.util.Log
import io.legado.app.data.appDb
import io.legado.app.utils.defaultSharedPreferences
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import splitties.init.appCtx

private const val PREFS_NAME = "esp_positions"
private const val KEY_POSITIONS = "esp_positions"

internal const val DEVICE_KEY_PREFIX = "file:"

/**
 * 手机/设备两侧阅读进度的单点存储。
 * 持久化：SharedPreferences 单 key JSON，所有写操作经 edit {} 事务原子更新。
 * 读：两个 @Volatile 内存快照，服务线程（NanoHTTPD）无锁同步读。
 */
object EspPositionStore {

    private const val TAG = "EspPositionStore"

    @Volatile
    private var phoneSnapshot: Map<String, EspPosition> = emptyMap()

    @Volatile
    private var deviceSnapshot: Map<String, DeviceProgress> = emptyMap()

    /** 每次收到设备推送进度时发射 (key, progress)，供 UI 弹窗。 */
    private val _deviceProgressEvents = MutableSharedFlow<Pair<String, DeviceProgress>>(
        extraBufferCapacity = 4,
        onBufferOverflow = kotlinx.coroutines.channels.BufferOverflow.DROP_OLDEST
    )
    val deviceProgressEvents: SharedFlow<Pair<String, DeviceProgress>> = _deviceProgressEvents

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    init {
        scope.launch(Dispatchers.IO) {
            runCatching {
                decodePositions(readPositions()).let {
                    phoneSnapshot = it.phone
                    deviceSnapshot = it.device
                }
            }.onFailure {
                Log.w(TAG, "加载 ESP 位置快照失败", it)
            }
        }
    }

    private fun readPositions(): String? =
        appCtx.defaultSharedPreferences.getString(KEY_POSITIONS, null)

    private fun writePositions(json: String) {
        appCtx.defaultSharedPreferences.edit().putString(KEY_POSITIONS, json).apply()
    }

    /** 手机侧当前进度（GET /progress 服务它）。 */
    suspend fun putPhonePosition(bookId: String, pos: EspPosition) {
        withContext(Dispatchers.IO) {
            val snapshot = decodePositions(readPositions())
            val updated = snapshot.copy(phone = snapshot.phone + (bookId to pos))
            writePositions(encodePositions(updated))
            phoneSnapshot = updated.phone
            Log.d(TAG, "保存手机位置 book=${bookId.take(12)} file=${pos.fileName} offset=${pos.byteOffset} pct=${"%.1f".format(pos.pct)}")
        }
    }

    /** 服务线程同步读手机侧进度（@Volatile 快照，无锁）。 */
    fun getPhonePosition(bookId: String): EspPosition? = phoneSnapshot[bookId]

    /** 按文件名查 bookId + 位置；先查手机侧，再查设备侧。 */
    fun findByFileName(fileName: String): Pair<String, EspPosition>? {
        phoneSnapshot.entries.firstOrNull { it.value.fileName == fileName }
            ?.let { return it.key to it.value }
        deviceSnapshot.entries.firstOrNull { it.value.pos.fileName == fileName }
            ?.let { return it.key to it.value.pos }
        return null
    }

    /** 设备推送来的进度（PUT /progress 写入，默认 pendingConfirm=true）。 */
    suspend fun putDevicePosition(
        bookId: String,
        pos: EspPosition,
        pendingConfirm: Boolean,
        espFs: Long? = null,
        espH0: String? = null,
        espH1: String? = null,
        espH2: String? = null
    ) {
        withContext(Dispatchers.IO) {
            val snapshot = decodePositions(readPositions())
            val updated = snapshot.copy(
                device = snapshot.device + (bookId to DeviceProgress(
                    pos, pendingConfirm, espFs, espH0, espH1, espH2
                ))
            )
            writePositions(encodePositions(updated))
            deviceSnapshot = updated.device
            _deviceProgressEvents.tryEmit(bookId to DeviceProgress(
                pos, pendingConfirm, espFs, espH0, espH1, espH2
            ))
            Log.d(TAG, "收到设备进度 key=${bookId.take(20)} file=${pos.fileName} offset=${pos.byteOffset} pct=${"%.1f".format(pos.pct)} pending=$pendingConfirm fp=${if (espFs != null) "yes" else "no"}")
        }
    }

    /** 读设备推送进度。 */
    fun getDevicePosition(bookId: String): DeviceProgress? = deviceSnapshot[bookId]

    /** 首条待确认的设备推送（后台收推送→回到前台/点通知进阅读器时，onResume 据此补弹确认框）。 */
    fun firstPendingConfirm(): Pair<String, DeviceProgress>? =
        deviceSnapshot.entries.firstOrNull { it.value.pendingConfirm }?.let { it.key to it.value }

    /** 按 bookId 或 fileName 查设备推送进度。 */
    fun findDevicePosition(bookId: String, fileName: String): Pair<String, DeviceProgress>? {
        deviceSnapshot[bookId]?.let { return bookId to it }
        return deviceSnapshot.entries.firstOrNull { it.value.pos.fileName == fileName }
            ?.let { it.key to it.value }
    }

    /** 用户点"忽略"或跳转后清除待确认标记（位置保留）。 */
    suspend fun markDevicePositionHandled(bookId: String) {
        withContext(Dispatchers.IO) {
            val snapshot = decodePositions(readPositions())
            val current = snapshot.device[bookId] ?: return@withContext
            val updated = snapshot.copy(
                device = snapshot.device + (bookId to current.copy(pendingConfirm = false)))
            writePositions(encodePositions(updated))
            deviceSnapshot = updated.device
        }
    }

    /** 移除设备推送进度（整条删除）。 */
    suspend fun removeDevicePosition(bookId: String) {
        withContext(Dispatchers.IO) {
            val snapshot = decodePositions(readPositions())
            val updated = snapshot.copy(device = snapshot.device - bookId)
            writePositions(encodePositions(updated))
            deviceSnapshot = updated.device
        }
    }

    /**
     * ★ 2026-09-12（用户实测踩坑）：手机删旧书→导入同名新书后，deviceSnapshot 残留旧 PUT 的进度，
     * 被 GET 捞回返回给 ESP → ESP 再推回来覆写新书。
     * 修法：服务启动时扫描 deviceSnapshot，凡 bookUrl 在书架上已不存在的条目一律清除。
     */
    suspend fun clearStaleDeviceEntries() = withContext(Dispatchers.IO) {
        val snapshot = decodePositions(readPositions())
        val stale = snapshot.device.keys.filter { bookId ->
            bookId.startsWith("/") && appDb.bookDao.getBook(bookId) == null
        }
        if (stale.isNotEmpty()) {
            val cleaned = snapshot.device.filterKeys { it !in stale }
            val updated = snapshot.copy(device = cleaned)
            writePositions(encodePositions(updated))
            deviceSnapshot = cleaned
            Log.i(TAG, "清除 ${stale.size} 条设备进度残留 (书已从书架删除): $stale")
        }
    }

    // ---- JSON 编解码（纯函数） ----

    private fun encodePositions(snapshot: PositionsSnapshot): String {
        val root = JSONObject()
        val phone = JSONObject()
        snapshot.phone.forEach { (k, v) -> phone.put(k, posToJson(v)) }
        root.put("phone", phone)
        val device = JSONObject()
        snapshot.device.forEach { (k, v) ->
            val entry = JSONObject()
            entry.put("pos", posToJson(v.pos))
            entry.put("pendingConfirm", v.pendingConfirm)
            // v3 指纹组（可选，旧数据无）
            v.espFs?.let { entry.put("espFs", it) }
            v.espH0?.let { entry.put("espH0", it) }
            v.espH1?.let { entry.put("espH1", it) }
            v.espH2?.let { entry.put("espH2", it) }
            device.put(k, entry)
        }
        root.put("device", device)
        return root.toString()
    }

    private fun decodePositions(json: String?): PositionsSnapshot {
        if (json.isNullOrBlank()) return PositionsSnapshot()
        return try {
            val root = JSONObject(json)
            val phoneMap = LinkedHashMap<String, EspPosition>()
            root.optJSONObject("phone")?.let { phones ->
                phones.keys().forEach { key ->
                    phones.optJSONObject(key)?.let { obj ->
                        runCatching { phoneMap[key] = jsonToPos(obj) }
                    }
                }
            }
            val deviceMap = LinkedHashMap<String, DeviceProgress>()
            root.optJSONObject("device")?.let { devices ->
                devices.keys().forEach { key ->
                    runCatching {
                        val entry = devices.optJSONObject(key) ?: return@runCatching
                        val pos = jsonToPos(entry.optJSONObject("pos") ?: return@runCatching)
                        deviceMap[key] = DeviceProgress(
                            pos,
                            entry.optBoolean("pendingConfirm", true),
                            entry.optLong("espFs", -1L).takeIf { it >= 0L },
                            entry.optString("espH0", "").takeIf { it.isNotBlank() },
                            entry.optString("espH1", "").takeIf { it.isNotBlank() },
                            entry.optString("espH2", "").takeIf { it.isNotBlank() }
                        )
                    }
                }
            }
            PositionsSnapshot(phoneMap, deviceMap)
        } catch (_: Exception) {
            PositionsSnapshot()
        }
    }

    private fun posToJson(pos: EspPosition): JSONObject = JSONObject()
        .put("fileName", pos.fileName)
        .put("byteOffset", pos.byteOffset)
        .put("size", pos.size)
        .put("pct", pos.pct.toDouble())
        .put("ts", pos.ts)

    private fun jsonToPos(json: JSONObject): EspPosition = EspPosition(
        fileName = json.optString("fileName"),
        byteOffset = json.optLong("byteOffset", 0L),
        size = json.optLong("size", 0L),
        pct = json.optDouble("pct", 0.0).toFloat(),
        ts = json.optLong("ts", 0L)
    )
}

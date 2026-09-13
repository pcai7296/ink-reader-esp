package io.legado.app.esp

/** 一份阅读进度（与固件 LUMI1 的 ts/size/offset/pct 语义一致）。 */
data class EspPosition(
    val fileName: String,
    val byteOffset: Long,
    val size: Long,
    val pct: Float,
    val ts: Long
)

/** 设备推送来的进度：pendingConfirm=true 表示尚未被用户处理（忽略/跳转）。 */
data class DeviceProgress(
    val pos: EspPosition,
    val pendingConfirm: Boolean,
    // v3 指纹组（ESP 发送方文件；原子——四字段全非 null 才 espHasFingerprint）
    val espFs: Long? = null,
    val espH0: String? = null,
    val espH1: String? = null,
    val espH2: String? = null,
) {
    val espHasFingerprint: Boolean
        get() = espFs != null && espH0 != null && espH1 != null && espH2 != null
}

/** 快照 → SharedPreferences JSON 文本。 */
internal data class PositionsSnapshot(
    val phone: Map<String, EspPosition> = emptyMap(),
    val device: Map<String, DeviceProgress> = emptyMap()
)

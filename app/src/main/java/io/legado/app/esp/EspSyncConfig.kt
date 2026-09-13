package io.legado.app.esp

import android.content.Context
import android.net.wifi.WifiManager
import io.legado.app.constant.PreferKey
import io.legado.app.utils.defaultSharedPreferences
import io.legado.app.utils.getPrefBoolean
import io.legado.app.utils.putPrefBoolean
import splitties.init.appCtx

/** ESP 同步服务开关与状态。 */
object EspSyncConfig {
    /** 服务开关（默认开启）。 */
    var enabled: Boolean
        get() = appCtx.getPrefBoolean(PreferKey.espSyncEnable, true)
        set(value) {
            appCtx.putPrefBoolean(PreferKey.espSyncEnable, value)
        }

    /**
     * 热点（SoftAp）是否开启。
     * isWifiApState 是 hidden API（Android 11 无公开等价），反射读；失败=未知，按"没开热点"处理。
     * ★ 开热点时 STA Wi-Fi 通常被系统断开，而 wifiIpv4() 的私网兜底会把热点地址误判为可用 → 必须显式排除。
     */
    fun isHotspotActive(): Boolean = runCatching {
        val wm = appCtx.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            ?: return false
        val method = WifiManager::class.java.getMethod("isWifiApEnabled")
        (method.invoke(wm) as? Boolean) ?: false
    }.getOrDefault(false)

    /**
     * 共享开启守卫（设备页 Switch 与阅读菜单共用）：
     * 允许开启 = 已连接普通 Wi-Fi（拿到私网 IPv4）且未开热点。
     * @return null = 允许；否则返回给用户看的原因文案
     */
    fun denyReason(): String? {
        if (isHotspotActive()) return "已开启热点，无法作为同步服务端（热点下设备连不上本机）"
        if (!EspNetwork.wifiReady()) return "未连接局域网 Wi-Fi，无法开启同步服务"
        return null
    }

    /** 开关变化的广播 key（设备页/阅读菜单共用，保证两处 UI 即时一致）。 */
    const val EVENT_ENABLED_CHANGED = "espSyncEnabledChanged"

    /**
     * **唯一的开关写入入口**（设备页 Switch 与阅读菜单都必须走这里）：
     * 写 pref → 启停服务 → 广播事件。
     * @return false = 被守卫拒绝（未连 Wi-Fi / 开了热点），状态未改动
     */
    fun requestEnabled(context: Context, enable: Boolean): Boolean {
        if (enable) {
            if (denyReason() != null) return false
        }
        enabled = enable
        if (enable) EspSyncService.start(context) else EspSyncService.stop(context)
        runCatching { io.legado.app.utils.postEvent(EVENT_ENABLED_CHANGED, enable) }
        return true
    }

    /** 最近同步时间戳（0=从未）。 */
    var lastSyncTs: Long
        get() = appCtx.defaultSharedPreferences.getLong("esp_last_sync_ts", 0L)
        set(value) {
            appCtx.defaultSharedPreferences.edit().putLong("esp_last_sync_ts", value).apply()
        }

    /** 最近一次"墨水屏主动来连我们"的设备 IP（"" = 从未联系；这是最可靠的连通证据）。 */
    var deviceIp: String
        get() = appCtx.defaultSharedPreferences.getString("esp_device_last_ip", "") ?: ""
        set(value) {
            appCtx.defaultSharedPreferences.edit().putString("esp_device_last_ip", value.trim()).apply()
        }

    /** 最近一次设备联系时间戳（0=从未）。 */
    var lastContactTs: Long
        get() = appCtx.defaultSharedPreferences.getLong("esp_last_contact_ts", 0L)
        set(value) {
            appCtx.defaultSharedPreferences.edit().putLong("esp_last_contact_ts", value).apply()
        }

    /** 最近操作状态文案。 */
    var status: String
        get() = appCtx.defaultSharedPreferences.getString("esp_sync_status", "") ?: ""
        set(value) {
            appCtx.defaultSharedPreferences.edit().putString("esp_sync_status", value).apply()
        }

    /** 电池白名单按钮是否已移到设备页底部（一次性引导，持久化，重启后仍位于底部）。 */
    var batteryBtnMoved: Boolean
        get() = appCtx.getPrefBoolean("esp_battery_btn_moved", false)
        set(value) {
            appCtx.putPrefBoolean("esp_battery_btn_moved", value)
        }

    // ---- 方案 D-1: 墨水屏绑定（UI 只暴露既有能力，不改协议） ----

    /** 墨水屏（设备）IP —— 手机端唯一需要填的地址；与设备静态 IP 默认值一致。 */
    var devIp: String
        get() = appCtx.defaultSharedPreferences.getString("esp_dev_ip", "192.168.0.100") ?: "192.168.0.100"
        set(value) {
            appCtx.defaultSharedPreferences.edit().putString("esp_dev_ip", value.trim()).apply()
        }

    /** 已绑定的墨水屏 IP（"" = 未绑定）。 */
    var boundEspIp: String
        get() = appCtx.defaultSharedPreferences.getString("esp_bound_ip", "") ?: ""
        set(value) {
            appCtx.defaultSharedPreferences.edit().putString("esp_bound_ip", value).apply()
        }

    /** 是否允许自动绑定/自动重绑（「解除绑定」把它关掉，避免 2 分钟后又自动绑回来）。 */
    var autoBind: Boolean
        get() = appCtx.getPrefBoolean("esp_auto_bind", true)
        set(value) {
            appCtx.putPrefBoolean("esp_auto_bind", value)
        }

    /** 绑定过程状态文案（如"正在重新发现…"/"绑定失败"），供 UI 显示。 */
    var bindState: String
        get() = appCtx.defaultSharedPreferences.getString("esp_bind_state", "") ?: ""
        set(value) {
            appCtx.defaultSharedPreferences.edit().putString("esp_bind_state", value).apply()
        }

    /**
     * 记录"墨水屏主动连到了我们"这一事实（GET 拉取 / PUT 推送时由服务端调用）。
     *
     * 2026-09-13 用户反馈：手表上"完全没有关于同步的信息，还显示未连接" ——
     * 原因是既有的 `status`/`lastSyncTs` 只在"设备推送弹窗被用户确认"时才写，
     * 而**现在的正常流程是设备主动来拉进度**，所以设备明明已经同步成功，手机/手表端却毫无记录。
     * 服务端在处理请求时天然知道来源 IP（就是墨水屏），这里把它记下来，UI 就能显示真实连通状态。
     *
     * @param remoteIp 请求来源 IP（= 墨水屏 IP）
     * @param action   "拉取"（设备取走我们的进度）或 "推送"（设备把进度推给我们）
     * @param pct     该请求涉及到的百分比（未知传 null）
     */
    fun onDeviceContact(remoteIp: String?, action: String, pct: Float?) {
        val now = System.currentTimeMillis()
        if (!remoteIp.isNullOrBlank()) {
            deviceIp = remoteIp
            // 源地址就是墨水屏真实 IP：输入框里的地址跟着校正，避免用户填错后一直连不上
            if (Regex("^\\d{1,3}(\\.\\d{1,3}){3}$").matches(remoteIp) && devIp != remoteIp) {
                devIp = remoteIp
            }
        }
        lastContactTs = now
        lastSyncTs = now
        val pctText = if (pct != null && pct.isFinite()) "（%.2f%%）".format(pct) else ""
        status = "墨水屏${action}进度$pctText"
    }
}

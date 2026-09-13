package io.legado.app.esp

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.util.Log
import io.legado.app.utils.NetworkUtils
import splitties.init.appCtx
import java.net.Inet4Address
import java.net.InetAddress

/**
 * Android 侧网络判定（方案 D-1 的地址来源，2026-09-12 用户要求）。
 *
 * **为什么不能直接 `getLocalIPAddress().first()`**：手机可能同时存在
 * Wi-Fi / 移动数据 / 热点 / VPN / 虚拟网卡，第一个 IPv4 很可能是 VPN 或热点地址 ✗
 * → 一旦把这种地址报给设备（LUMIBIND），设备绑定后**永远连不上** ✗。
 *
 * 约定（与固件协议一致，**只用 IPv4**，不引入 IPv6）：
 *   phone_ip   = **当前 Wi-Fi 网络**上、ESP 所在链路可路由的 IPv4
 *   phone_port = 8384
 */
object EspNetwork {

    private const val TAG = "EspNetwork"

    fun isPrivateV4(ip: String): Boolean =
        ip.startsWith("10.") || ip.startsWith("192.168.") || ip.startsWith("127.") ||
            Regex("^172\\.(1[6-9]|2[0-9]|3[01])\\.").containsMatchIn(ip)

    private fun v4Of(lp: android.net.LinkProperties?): String? {
        val addr: InetAddress = lp?.linkAddresses
            ?.firstOrNull {
                it.address is Inet4Address && !it.address.isLoopbackAddress && !it.address.isLinkLocalAddress
            }?.address ?: return null
        return addr.hostAddress
    }

    /**
     * 当前 **Wi-Fi 网络**的 IPv4（排除 VPN；热点/移动数据天然不带 WIFI transport）。
     * 找不到时回退"私网 IPv4"，再不行返回 null。
     */
    fun wifiIpv4(ctx: Context = appCtx): String? {
        runCatching {
            val cm = ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
                ?: return@runCatching
            val candidates = LinkedHashSet<Network>()
            cm.activeNetwork?.let { candidates.add(it) }
            runCatching { cm.allNetworks?.forEach { candidates.add(it) } }
            for (n in candidates) {
                val caps = cm.getNetworkCapabilities(n) ?: continue
                if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) continue
                if (caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) continue
                val ip = v4Of(cm.getLinkProperties(n))
                if (ip != null) {
                    Log.d(TAG, "Wi-Fi IPv4 = $ip")
                    return ip
                }
            }
        }.onFailure { Log.w(TAG, "ConnectivityManager 取 Wi-Fi IPv4 失败: ${it.message}") }
        // 兜底：老办法 + 私网过滤（VPN 地址多为 10.x 也可能命中，故仅作兜底）
        val fallback = NetworkUtils.getLocalIPAddress()
            .mapNotNull { it.hostAddress }
            .firstOrNull { isPrivateV4(it) }
        Log.d(TAG, "Wi-Fi IPv4(兜底) = ${fallback ?: "-"}")
        return fallback
    }

    /** 当前是否处于"Wi-Fi 已连接且拿到可用 IPv4"的状态（绑定/重绑的触发条件）。 */
    fun wifiReady(ctx: Context = appCtx): Boolean = wifiIpv4(ctx) != null

    /** 注册 Wi-Fi 网络变化回调（服务用；返回可注销的回调对象）。 */
    fun registerWifiCallback(
        onUp: (String?) -> Unit,
        onDown: () -> Unit
    ): ConnectivityManager.NetworkCallback? {
        return runCatching {
            val cm = appCtx.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
                ?: return null
            val req = NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .build()
            val cb = object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: Network) {
                    Log.i(TAG, "Wi-Fi 网络可用 → 触发重绑")
                    onUp(wifiIpv4())
                }
                override fun onLost(network: Network) {
                    Log.w(TAG, "Wi-Fi 网络断开")
                    onDown()
                }
            }
            cm.registerNetworkCallback(req, cb)
            cb
        }.getOrElse {
            Log.w(TAG, "注册网络回调失败: ${it.message}")
            null
        }
    }

    fun unregister(cb: ConnectivityManager.NetworkCallback?) {
        if (cb == null) return
        runCatching {
            (appCtx.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager)
                ?.unregisterNetworkCallback(cb)
        }
    }
}

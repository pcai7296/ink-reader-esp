package io.legado.app.esp

import android.util.Log
import io.legado.app.utils.NetworkUtils
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.SocketTimeoutException

/**
 * 方案 D-1 客户端：**手机主动找墨水屏**并把本机 IP 报给它。
 *
 * 背景（2026-09-12 实测）：某些路由器会**丢弃"无线→无线"的 UDP 广播**，
 * 导致设备发的 LUMIDISC 到不了手机（手机日志里连一条都没有），而单播/TCP 完全正常。
 * 于是反过来：手机**逐 IP 单播**发 LUMIWHO 找设备（设备常驻监听回 LUMIHERE），
 * 找到后发 LUMIBIND 让它记住本机 IP —— 设备那边以**数据包来源 IP 为准**，不采信 payload 里的地址。
 */
object EspBindClient {

    private const val TAG = "EspBindClient"
    const val DISCOVERY_PORT = 8390
    private const val DISCOVER_QUERY = "LUMIWHO"
    private const val DISCOVER_REPLY = "LUMIHERE "
    private const val BIND_REPLY = "LUMIOK "

    /**
     * 手机当前**局域网** IP —— 必须是"当前 Wi-Fi 网络的实际 IPv4"。
     * ⚠️ 不能用 `getLocalIPAddress().first()`：VPN/热点/移动数据并存时会拿到错误地址 ✗
     * （会把 VPN 地址报给设备 → 设备绑定后永远连不上）。
     */
    private fun lanIp(): String? = EspNetwork.wifiIpv4()

    /**
     * 逐 IP 单播扫描找墨水屏；返回设备 IP（找不到返回 null）。
     * 254 个小包 ≈0.3s 发完，随后等待窗口（默认 2.5s）收 LUMIHERE。
     */
    suspend fun discover(timeoutMs: Int = 2500): String? = withContext(Dispatchers.IO) {
        val self = lanIp() ?: run { Log.w(TAG, "无局域网 IP，跳过发现"); return@withContext null }
        val prefix = self.substringBeforeLast('.')
        var socket: DatagramSocket? = null
        try {
            socket = DatagramSocket()
            socket.broadcast = true
            socket.soTimeout = 200
            val selfTail = self.substringAfterLast('.').toIntOrNull() ?: -1
            val payload = DISCOVER_QUERY.toByteArray(Charsets.UTF_8)
            val buf = ByteArray(128)
            val deadline = System.currentTimeMillis() + timeoutMs
            var sweep = 0
            // ★ 反复扫描：设备可能只在"同步中/配网中"才开着 WiFi（省电设计），单次扫描很容易错过窗口
            while (System.currentTimeMillis() < deadline) {
                sweep++
                for (h in 1..254) {
                    if (h == selfTail) continue
                    try {
                        socket.send(DatagramPacket(payload, payload.size, InetAddress.getByName("$prefix.$h"), DISCOVERY_PORT))
                    } catch (_: Exception) { /* 单地址失败忽略 */ }
                }
                Log.d(TAG, "第 $sweep 轮 LUMIWHO 扫描完成，等待应答…")
                val roundEnd = minOf(deadline, System.currentTimeMillis() + 2500)
                while (System.currentTimeMillis() < roundEnd) {
                    try {
                        val pkt = DatagramPacket(buf, buf.size)
                        socket.receive(pkt)
                        val text = String(pkt.data, 0, pkt.length)
                        if (text.startsWith(DISCOVER_REPLY)) {
                            val ip = text.removePrefix(DISCOVER_REPLY).trim()
                            Log.i(TAG, "发现墨水屏 $ip（第 $sweep 轮扫描）")
                            return@withContext ip
                        }
                    } catch (_: SocketTimeoutException) {
                        // 继续等
                    }
                }
            }
            Log.d(TAG, "未发现墨水屏（$timeoutMs ms / $sweep 轮扫描内无应答）")
            null
        } catch (e: Exception) {
            Log.w(TAG, "发现失败: ${e.message}")
            null
        } finally {
            runCatching { socket?.close() }
        }
    }

    /**
     * 把自己的 IP 报给设备（设备以数据包来源 IP 为准）。
     * @param espIp 设备 IP（来自 discover）
     * @param port  本机进度服务器端口（默认 8384）
     */
    suspend fun bind(espIp: String, port: Int = 8384): Boolean = withContext(Dispatchers.IO) {
        var socket: DatagramSocket? = null
        try {
            socket = DatagramSocket()
            socket.soTimeout = 2000
            val msg = "LUMIBIND port=$port".toByteArray(Charsets.UTF_8)
            val dest = InetAddress.getByName(espIp)
            repeat(3) { i ->
                socket.send(DatagramPacket(msg, msg.size, dest, DISCOVERY_PORT))
                val buf = ByteArray(128)
                try {
                    val pkt = DatagramPacket(buf, buf.size)
                    socket.receive(pkt)
                    val text = String(pkt.data, 0, pkt.length)
                    if (text.startsWith(BIND_REPLY)) {
                        Log.i(TAG, "绑定成功: $text")
                        return@withContext true
                    }
                } catch (_: SocketTimeoutException) {
                    Log.w(TAG, "LUMIBIND 第 ${i + 1} 次无应答")
                }
            }
            false
        } catch (e: Exception) {
            Log.w(TAG, "绑定失败: ${e.message}")
            false
        } finally {
            runCatching { socket?.close() }
        }
    }
}

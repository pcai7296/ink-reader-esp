package io.legado.app.esp

import android.annotation.SuppressLint
import android.app.NotificationChannel
import android.app.NotificationManager
import android.util.Log
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Build
import androidx.core.app.NotificationCompat
import io.legado.app.R
import io.legado.app.base.BaseService
import io.legado.app.constant.AppConst
import io.legado.app.constant.IntentAction
import io.legado.app.constant.NotificationId
import io.legado.app.esp.EspProgressServer
import io.legado.app.utils.NetworkUtils
import io.legado.app.utils.servicePendingIntent
import io.legado.app.utils.startService
import io.legado.app.utils.stopService
import io.legado.app.utils.toastOnUi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import splitties.init.appCtx
import splitties.systemservices.wifiManager
import java.net.DatagramPacket
import java.net.DatagramSocket

class EspSyncService : BaseService() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var httpServer: EspProgressServer? = null
    private var udpJob: Job? = null
    private var udpSocket: DatagramSocket? = null   // ★ 类级字段，onDestroy 可直接关闭；防 EADDRINUSE
    private var bindJob: Job? = null                // 方案 D-1: 主动找墨水屏并上报本机 IP
    private var watchdogJob: Job? = null            // 服务自检自愈
    private var netCallback: android.net.ConnectivityManager.NetworkCallback? = null   // Wi-Fi 变化回调
    private val bindMutex = kotlinx.coroutines.sync.Mutex()   // 绑定单飞锁(防并发扫描)
    private var wifiLock: android.net.wifi.WifiManager.WifiLock? = null
    private var multicastLock: android.net.wifi.WifiManager.MulticastLock? = null

    @SuppressLint("WakelockTimeout")
    override fun onCreate() {
        super.onCreate()
        if (refuseWhenDisabled()) return
        ensureChannel()     // 必须先建渠道, 否则 startForeground 抛 CannotPostForegroundServiceNotificationException
        acquireWifiLock()
        acquireMulticastLock()   // WiFi 栈默认过滤广播/组播包, 不持锁 UDP LUMIDISC 发现可能收不到
        startHttpServer()
        startUdpResponder()
        // ★ 2026-09-12：启动时清理已从书架删除的书在 deviceSnapshot 中的残留，
        //   防止旧 PUT 过的进度被 GET 捞回后覆写新书。
        scope.launch { EspPositionStore.clearStaleDeviceEntries() }
        startBindLoop()         // 方案 D-1: 启动即主动找墨水屏并绑定（此后每 2 分钟重试）
        startWatchdog()         // 健壮性: 每 30s 自检 HTTP/UDP/socket/唤醒锁, 失效即自愈
        isRunning = true        // UI 显示"服务运行中"的真实依据（不是仅凭开关为真）
        // 健壮性: 手机换 Wi-Fi / 重连 / 漫游后立即重绑（不等下一次 2 分钟轮询）
        netCallback = EspNetwork.registerWifiCallback(
            onUp = { ip: String? -> Log.i(TAG, "Wi-Fi 就绪 ip=${ip ?: "-"}"); scope.launch { tryBind(8_000) } },
            onDown = { Log.w(TAG, "Wi-Fi 断开, 等待恢复") }
        )
        EspSyncDialog.start()   // 开始监听设备推送事件（幂等）
    }

    /**
     * 方案 D-1（用户 2026-09-12 批准）：手机**主动**找墨水屏并上报本机 IP。
     * 为什么需要：部分路由器丢弃"无线→无线"UDP 广播，设备发不出、手机收不到；
     * 改为手机逐 IP 单播 LUMIWHO 找设备（设备常驻监听回 LUMIHERE）→ 发 LUMIBIND 绑定。
     * 服务启动即试一次，之后每 2 分钟一次；失败静默重试（不打扰用户）。
     */
    private fun startBindLoop() {
        bindJob = scope.launch {
            var first = true
            while (isActive) {
                try {
                    if (!EspSyncConfig.autoBind) {           // 用户点过"解除绑定" → 不再自动重绑
                        kotlinx.coroutines.delay(30_000)
                        continue
                    }
                    // 首次给长窗口（设备可能只在"同步中/配网中"才开 WiFi，需多轮扫描才有机会命中）
                    tryBind(if (first) 20_000 else 4_000)
                    first = false
                } catch (e: Exception) {
                    Log.w(TAG, "自动绑定异常: ${e.message}")
                }
                kotlinx.coroutines.delay(120_000)
            }
        }
    }

    /**
     * 一次绑定尝试（自动循环 / Wi-Fi 变化回调 / 手动按钮共用）。
     * 触发条件严格限定为"**当前 Wi-Fi 网络已拿到可用 IPv4**"——
     * 避免在只有移动数据/IPv6/VPN 或网络切换中途去做无意义的扫描（用户 2026-09-12 的地址语义要求）。
     */
    suspend fun tryBind(timeoutMs: Int) {
        // ★ 单飞：自动循环、Wi-Fi 变化回调、手动按钮可能同时触发（实测会并发扫描 ✗ 浪费电+日志刷屏）
        if (!bindMutex.tryLock()) {
            Log.d(TAG, "已有绑定尝试在进行，跳过本次")
            return
        }
        try {
            if (!EspNetwork.wifiReady()) {
                Log.d(TAG, "Wi-Fi 未就绪（无可用 IPv4），跳过本次绑定")
                return
            }
            // ★ 2026-09-12 用户拍板简化：**直连配置的墨水屏 IP**（默认 192.168.0.100，与设备静态 IP 一致），
            //   不再依赖全网扫描；扫描仅在"直连失败"时作为兜底。
            // ★ 2026-09-13 去冗余：直连 bind 成功即绑定完成（设备端收到 LUMIBIND 已保存），
            //   不再二次 bind —— 自动循环每 2 分钟跑一次，双发会让设备重复落盘（设备端已加零变化守卫，双保险）。
            val configured = EspSyncConfig.devIp
            if (configured.isNotBlank() && EspBindClient.bind(configured)) {
                Log.i(TAG, "绑定墨水屏 $configured -> 成功")
                boundEspIp = configured
                EspSyncConfig.boundEspIp = configured
                EspSyncConfig.bindState = ""
                return
            }
            val esp = EspBindClient.discover(timeoutMs) ?: return
            val ok = EspBindClient.bind(esp)
            Log.i(TAG, "绑定墨水屏 $esp -> ${if (ok) "成功" else "失败"}")
            if (ok) {
                boundEspIp = esp
                EspSyncConfig.boundEspIp = esp
                EspSyncConfig.bindState = ""
            }
        } finally {
            bindMutex.unlock()
        }
    }

    @Suppress("DEPRECATION")
    private fun acquireMulticastLock() {
        try {
            wifiManager?.createMulticastLock("legado:EspSyncService")
                ?.apply { setReferenceCounted(false); acquire(); multicastLock = this }
        } catch (_: Exception) {
            // ignore
        }
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    "ESP 进度同步",
                    NotificationManager.IMPORTANCE_LOW
                ).apply {
                    description = "墨水屏进度同步服务常驻"
                    setSound(null, null)
                }
            )
        }
    }

    /** specialUse 类型无时长上限, 正常不会触发; 保留兜底(若未来改回限时类型). */
    override fun onTimeout(startId: Int, fgsType: Int) {
        android.util.Log.w(TAG, "dataSync FGS onTimeout, stopping")
        EspSyncConfig.status = "系统限时停止，重新打开 App 恢复"
        stopSelf(startId)
    }

    /**
     * 开关为关时拒绝启动，返回 true 表示已拒绝（调用方直接 return）。
     *
     * ★ 2026-09-13 用户实测踩坑：START_STICKY 下系统回收进程会用 **null intent** 重建服务，
     *   而 onCreate 原来无条件起 HTTP/UDP + 挂通知 → 用户关了开关，通知/服务又回来了。
     *   这里统一闸门；并先满足 startForegroundService 的 5 秒契约再停（onDestroy 会摘掉通知）。
     */
    private fun refuseWhenDisabled(): Boolean {
        if (EspSyncConfig.enabled) return false
        Log.i(TAG, "开关为关，拒绝启动并自行停止")
        runCatching {
            ensureChannel()
            startForegroundNotification()   // 满足前台服务契约；马上会被 onDestroy 摘掉
        }
        stopSelf()
        return true
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == IntentAction.stop) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (refuseWhenDisabled()) return START_NOT_STICKY
        super.onStartCommand(intent, flags, startId)
        // ★ 2026-09-12 健壮性: 改为 START_STICKY —— 系统内存压力/厂商省电杀掉服务后**自动重建**
        //   （原 START_NOT_STICKY 需要用户手动再开 App；实测用户就是碰上"服务悄悄没了"）
        return START_STICKY
    }

    /**
     * ★ 2026-09-12 健壮性加固（用户要求"考虑服务开启期间所有可能情况"）。
     * 每 30s 自检并自愈：HTTP 服务是否还活着、UDP 应答 socket 是否还在、WifiLock/组播锁是否还持有。
     * 覆盖场景：服务被系统杀掉后重建、HTTP 端口被占用后重试成功、WiFi 重连导致 socket 失效、
     * 锁被意外释放、长时间运行后的偶发失效。
     */
    private fun startWatchdog() {
        watchdogJob = scope.launch {
            while (isActive) {
                kotlinx.coroutines.delay(30_000)
                try {
                    // ① HTTP 服务
                    val srv = httpServer
                    if (srv == null || !srv.isAlive) {
                        Log.w(TAG, "看门狗: HTTP 服务不在, 重启")
                        startHttpServer()
                    }
                    // ② UDP 应答 socket（自愈循环会重绑，这里只做兜底检查）
                    if (udpSocket == null || udpSocket?.isClosed == true) {
                        Log.w(TAG, "看门狗: UDP socket 不在, 触发重绑")
                        udpSocket?.close()
                        udpSocket = null
                        udpJob?.cancel()
                        startUdpResponder()
                    }
                    // ③ 唤醒锁
                    if (wifiLock?.isHeld != true) { Log.w(TAG, "看门狗: 重新获取 WifiLock"); acquireWifiLock() }
                    if (multicastLock?.isHeld != true) { Log.w(TAG, "看门狗: 重新获取组播锁"); acquireMulticastLock() }
                } catch (e: Exception) {
                    Log.w(TAG, "看门狗异常: ${e.message}")
                }
            }
        }
    }
    override fun startForegroundNotification() {
        val addresses = buildAddressList()
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .setSmallIcon(R.drawable.ic_web_service_noti)
            .setOngoing(true)
            .setContentTitle(getString(R.string.esp_sync_service))
            .setContentText(addresses.firstOrNull() ?: getString(R.string.esp_sync_starting))
            .setContentIntent(servicePendingIntent<EspSyncService>("open"))
        builder.addAction(
            R.drawable.ic_stop_black_24dp,
            getString(R.string.cancel),
            servicePendingIntent<EspSyncService>(IntentAction.stop)
        )
        startForeground(NOTIFICATION_ID, builder.build())
    }

    private fun buildAddressList(): List<String> {
        return NetworkUtils.getLocalIPAddress().map { addr ->
            "${addr.hostAddress}:$DEF_PORT"
        }
    }

    private fun startHttpServer() {
        // ★ 健壮性: 先停掉可能存在的旧实例（重复 start/系统重建时防端口自占）
        runCatching { httpServer?.stop() }
        httpServer = null
        // ⚠️ 2026-09-12 实机踩坑：原先绑 `NetworkUtils.getLanServerIp(this)`（**当时的**局域网 IP），
        //   手机 WiFi 地址一变（实测 .10 → .18），8384 仍绑在旧地址上 → `/proc/net/tcp` 里查不到 20C0，
        //   设备连新旧地址都超时；而 UDP 8390 应答正常（发现 OK、HTTP 不通）→ 现象极具误导性。
        //   改为绑 **0.0.0.0**（任意网卡），地址变化不再需要重绑。
        // ★ 健壮性: 端口被上一实例/其它进程短暂占用时**异步重试 3 次**（原实现一次性失败就永久没有 HTTP 服务）
        scope.launch {
            for (attempt in 1..3) {
                try {
                    val srv = EspProgressServer.create(DEF_PORT)
                    srv.start()
                    httpServer = srv
                    Log.i(TAG, "HTTP server started on $DEF_PORT bind=0.0.0.0 attempt=$attempt")
                    return@launch
                } catch (e: Exception) {
                    Log.e(TAG, "HTTP server start failed attempt=$attempt: ${e.message}")
                    kotlinx.coroutines.delay(3000)
                }
            }
            Log.e(TAG, "HTTP server 三次启动均失败（$DEF_PORT 被占用？）")
            toastOnUi(getString(R.string.esp_sync_port_failed, DEF_PORT))
        }
    }

    private fun startUdpResponder() {
        udpJob = scope.launch {
            // ★ 2026-09-12 自愈式应答器：实测出现"服务活着、8384 正常，但 8390 没人听"的状态
            //   （PC 单播探测收到 ICMP 端口不可达；`/proc/net/udp` 查不到 20C6）——原实现里
            //   内层 `catch (_: Exception)` 连 "Socket closed" 一起吞掉，socket 一旦被关就**空转且永不重绑**。
            //   改为：外层 while 重试 + 2s 收包超时（定期醒来自检）→ socket 已关/异常就重绑，绝不永久退出。
            while (isActive) {
                try {
                    udpSocket?.close(); udpSocket = null
                    val s = DatagramSocket(null).apply {
                        reuseAddress = true
                        bind(java.net.InetSocketAddress(UDP_PORT))
                    }
                    udpSocket = s
                    s.soTimeout = 2000            // 定期醒来检查 isActive / socket 状态
                    Log.i(TAG, "UDP responder started on $UDP_PORT")
                    val buf = ByteArray(256)
                    while (isActive) {
                        if (s.isClosed) throw java.net.SocketException("socket closed, 重绑")
                        try {
                            val pkt = DatagramPacket(buf, buf.size)
                            s.receive(pkt)
                            val text = String(pkt.data, 0, pkt.length)
                            if (text == DISCOVERY_QUERY) {
                                // 回包必须是【手机自身】IP（pkt.address 是发送方地址；曾错用 → 设备收到自己的 IP）
                                // 多网卡时优先取与发送方同 /24 网段的本机地址。
                                // ★ 只报"当前 Wi-Fi 网络的实际 IPv4"（VPN/热点/移动数据地址会让设备绑定后永远连不上）
                                val selfIp = EspNetwork.wifiIpv4()
                                if (selfIp != null) {
                                    // 不带尾部换行: 设备端 IPAddress::fromString 遇 '\n' 整串判失败
                                    val reply = DISCOVERY_REPLY + selfIp
                                    val replyBytes = reply.toByteArray(Charsets.UTF_8)
                                    s.send(DatagramPacket(replyBytes, replyBytes.size, pkt.address, pkt.port))
                                    Log.d(TAG, "UDP discovery reply -> ${pkt.address} $selfIp")
                                }
                            }
                        } catch (_: java.net.SocketTimeoutException) {
                            // 正常：用来定期检查 isActive
                        } catch (e: Exception) {
                            // 收包异常不再退出循环（原实现吞掉一切导致 socket 死了也永远不重绑）
                            Log.w(TAG, "UDP recv 异常, 200ms 后继续", e)
                            kotlinx.coroutines.delay(200)
                        }
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "UDP responder 重绑(${e.message}), 1s 后重试", e)
                    kotlinx.coroutines.delay(1000)
                } finally {
                    runCatching { udpSocket?.close() }
                    udpSocket = null
                }
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun acquireWifiLock() {
        try {
            if (wifiLock?.isHeld == true) return
            runCatching { wifiLock?.release() }
            wifiLock = wifiManager?.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "legado:EspSyncService")
                ?.apply { setReferenceCounted(false); acquire() }
        } catch (_: Exception) {
            // ignore
        }
    }

    override fun onDestroy() {
        // ★ 先取消服务级协程: startHttpServer 的重试循环若在下面 "httpServer = null" 之后才被调度,
        // 会把已停掉的服务器重新武装并回写 httpServer —— onDestroy 之后"复活"
        scope.cancel()
        httpServer?.stop()
        httpServer = null
        udpSocket?.close()   // ★ 先关 socket 再取消协程（防进程被杀后残留）
        udpSocket = null
        udpJob?.cancel()
        udpJob = null
        bindJob?.cancel()
        bindJob = null
        watchdogJob?.cancel()
        watchdogJob = null
        EspNetwork.unregister(netCallback)
        netCallback = null
        runCatching { multicastLock?.release() }
        multicastLock = null
        runCatching { wifiLock?.release() }
        wifiLock = null
        // ★ 摘掉前台通知（缺这步服务死了通知还挂着 —— 用户实测踩坑）
        runCatching { stopForeground(true) }
        runCatching {
            getSystemService(NotificationManager::class.java)?.cancel(NOTIFICATION_ID)
        }
        isRunning = false
        super.onDestroy()
    }

    companion object {
        private const val TAG = "EspSyncService"
        private const val NOTIFICATION_ID = 112  // 不同于 WebService 105
        private const val CHANNEL_ID = "channel_esp_sync"
        private const val UDP_PORT = 8390
        private const val DISCOVERY_QUERY = "LUMIDISC"
        private const val DISCOVERY_REPLY = "LUMIACK "

        /** 服务是否真的在跑（onCreate 置真 / onDestroy 置假）；UI 据此显示"服务运行中" */
        @Volatile
        var isRunning: Boolean = false
            private set

        /** 方案 D-1: 最近一次成功绑定的墨水屏 IP（UI/状态显示用；空=未绑定） */
        @Volatile
        var boundEspIp: String = ""

        fun start(context: Context = appCtx) {
            context.startService<EspSyncService>()
        }

        fun stop(context: Context = appCtx) {
            context.stopService(Intent(context, EspSyncService::class.java))
        }
    }
}

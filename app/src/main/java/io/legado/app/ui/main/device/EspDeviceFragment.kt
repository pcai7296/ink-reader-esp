package io.legado.app.ui.main.device

import android.app.AlertDialog
import android.content.Context
import android.os.PowerManager
import android.content.Intent
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.provider.Settings
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import io.legado.app.utils.ColorUtils
import androidx.lifecycle.lifecycleScope
import io.legado.app.help.WatchUi
import io.legado.app.R
import io.legado.app.base.BaseFragment
import io.legado.app.databinding.FragmentEspDeviceBinding
import io.legado.app.esp.DEF_PORT
import io.legado.app.esp.EspDeviceInfo
import io.legado.app.esp.EspBindClient
import io.legado.app.esp.EspDeviceManager
import io.legado.app.esp.EspNetwork
import io.legado.app.esp.EspSyncConfig
import io.legado.app.esp.EspSyncService
import io.legado.app.lib.theme.UiCorner
import io.legado.app.lib.theme.dialogSurfaceBackground
import io.legado.app.lib.theme.getPrimaryTextColor
import io.legado.app.lib.theme.getSecondaryTextColor
import io.legado.app.lib.theme.backgroundColor
import io.legado.app.lib.theme.primaryColor
import io.legado.app.lib.theme.primaryColorDark
import io.legado.app.ui.main.MainFragmentInterface
import io.legado.app.utils.NetworkUtils
import io.legado.app.utils.observeEvent
import io.legado.app.utils.toastOnUi
import io.legado.app.utils.viewbindingdelegate.viewBinding
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 设备页：同步区（ESP 进度同步开关/状态/同步服务）+「前往水墨屏管理」入口。
 * 所有网络请求均经由 EspDeviceManager，本 Fragment 不直接持有任何 HTTP 实现。
 */
class EspDeviceFragment() : BaseFragment(R.layout.fragment_esp_device),
    MainFragmentInterface {

    constructor(position: Int) : this() {
        arguments = Bundle().apply {
            putInt("position", position)
        }
    }

    override val position: Int? get() = arguments?.getInt("position")

    private val binding by viewBinding(FragmentEspDeviceBinding::bind)
    private var syncRefreshJob: kotlinx.coroutines.Job? = null   // 页面打开期间的定时刷新


    override fun onFragmentCreated(view: View, savedInstanceState: Bundle?) {
        // ★ 开关变化广播：阅读菜单改开关时，本页 Switch 立即跟上（不必等 onResume/定时器）
        observeEvent<Boolean>(EspSyncConfig.EVENT_ENABLED_CHANGED) { refreshSyncStatus() }
        // 手表模式：标题避开顶部圆角（左上角“设备”二字碰到圆角）
        WatchUi.applyContentSafeArea(
            binding.scrollRoot,
            leftPx = 0, topPx = WatchUi.CORNER_PX, rightPx = 0, bottomPx = 0
        )
        binding.tvTitle.text = getString(R.string.esp_device_title)
        binding.btnOpenManage.setOnClickListener {
            runCatching {
                startActivity(
                    Intent(
                        Intent.ACTION_VIEW,
                        Uri.parse(EspDeviceManager.getBaseUrl())
                    )
                )
            }.onFailure { toastOnUi(it.message ?: "打开失败") }
        }
        initSyncSection()
        // 已移动过则本次启动直接置于底部（持久化）
        if (EspSyncConfig.batteryBtnMoved) {
            moveBatterySectionToBottom()
        }
        applyTheme()
        refreshSyncStatus()
    }

    override fun onResume() {
        super.onResume()
        refreshSyncStatus()
        // ★ 2026-09-13：页面打开期间**定时刷新**——设备随时可能来拉/推进度，
        //   而服务端只在处理请求时写状态；不做定时刷新的话，用户盯着页面也看不到变化（手表上就是这个现象）。
        syncRefreshJob?.cancel()
        syncRefreshJob = viewLifecycleOwner.lifecycleScope.launch {
            while (isActive) {
                kotlinx.coroutines.delay(3000)
                if (isAdded) refreshSyncStatus()
            }
        }
    }

    override fun onPause() {
        syncRefreshJob?.cancel()
        syncRefreshJob = null
        super.onPause()
    }

    // ==================== 同步区 ====================

    /** Switch 的点击处理（refreshSyncStatus 同步状态时需先摘除 listener，再重新挂回） */
    private fun initSwitchListener() {
        binding.switchSyncEnable.setOnCheckedChangeListener { _, isChecked ->
            // 唯一写入入口：守卫（未连 Wi-Fi / 开热点）+ 写 pref + 启停服务 + 广播
            if (!EspSyncConfig.requestEnabled(requireContext(), isChecked)) {
                val deny = EspSyncConfig.denyReason() ?: getString(R.string.esp_sync_no_lan_msg)
                AlertDialog.Builder(requireContext())
                    .setTitle(R.string.esp_sync_no_lan_title)
                    .setMessage(deny)
                    .setPositiveButton(R.string.ok) { _, _ -> }
                    .show()
                binding.switchSyncEnable.isChecked = false
                return@setOnCheckedChangeListener
            }
            refreshSyncStatus()
        }
    }

    private fun initSyncSection() {
        initSwitchListener()
        // ★ 2026-09-12 用户拍板简化：三个按钮（同步服务/重新绑定/解除绑定）→ **一个 IP 输入框 + 一个确认按钮**
        //   语义：输入框 = 墨水屏（设备）的地址，默认 192.168.0.100（与设备端静态 IP 默认值一致）；
        //   点确认 = 保存地址 + 把自己的 Wi-Fi IPv4 主动报给设备（设备也可自己发现手机，双向兜底）。
        binding.editDevIp.setText(EspSyncConfig.devIp)
        binding.btnDevIpConfirm.setOnClickListener {
            val ip = binding.editDevIp.text.toString().trim()
            if (!Regex("^\\d{1,3}(\\.\\d{1,3}){3}$").matches(ip) ||
                ip.split(".").any { (it.toIntOrNull() ?: 999) > 255 }
            ) {
                toastOnUi(R.string.esp_dev_ip_bad)
                return@setOnClickListener
            }
            EspSyncConfig.devIp = ip
            // 注意: 不动设备管理区地址——那里是"当前模式下的管理地址"(AP=192.168.4.1 / 连 WiFi=192.168.0.100)
            EspSyncConfig.autoBind = true
            if (!EspSyncConfig.enabled) toastOnUi(R.string.esp_sync_need_enable)
            refreshSyncStatus()
            viewLifecycleOwner.lifecycleScope.launch {
                val ok = EspBindClient.bind(ip)
                toastOnUi(if (ok) getString(R.string.esp_dev_ip_saved, ip) else getString(R.string.esp_bind_status_failed))
                if (ok) {
                    EspSyncConfig.boundEspIp = ip
                    EspSyncConfig.bindState = ""
                } else {
                    EspSyncConfig.bindState = getString(R.string.esp_bind_status_failed)
                }
                refreshSyncStatus()
            }
        }

        // 电池优化白名单按钮（布局内 btn_battery_whitelist，下方 OEM 提示 tv_battery_oem_hint）
        // 点击：打开系统电池优化设置页，同时把按钮+说明移到设备页最底部并持久化
        binding.btnBatteryWhitelist.setOnClickListener {
            // ★ 2026-09-12 健壮性：优先**直接申请本应用豁免**（系统弹窗点一下即可），
            //   比跳到"电池优化列表页"更不容易找不到；失败再退回列表页。
            val ctx = requireContext()
            val pm = ctx.getSystemService(Context.POWER_SERVICE) as? PowerManager
            val already = pm?.isIgnoringBatteryOptimizations(ctx.packageName) == true
            if (!already) {
                val ok = runCatching {
                    startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
                        data = Uri.parse("package:${ctx.packageName}")
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    })
                }.isSuccess
                if (!ok) {
                    startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS).apply {
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    })
                }
            } else {
                startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS).apply {
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                })
            }
            moveBatterySectionToBottom()
        }
    }

    /**
     * 启动同步服务前的局域网门禁：WiFi 或热点都没有时弹窗提示并拒绝启动。
     * 返回是否有局域网可用。
     */
    private fun ensureLanAvailable(): Boolean {
        if (EspNetwork.wifiIpv4() != null) return true
        AlertDialog.Builder(requireContext())
            .setTitle(R.string.esp_sync_no_lan_title)
            .setMessage(R.string.esp_sync_no_lan_msg)
            .setPositiveButton(R.string.ok) { _, _ -> }
            .show()
        return false
    }

    /**
     * 电池授权按钮区移到设备页最底部（System 卡下方）并持久化；
     * 幂等（EspBatterySectionMover 保证），可安全重复调用。
     */
    private fun moveBatterySectionToBottom() {
        val topMarginPx = (16 * resources.displayMetrics.density).toInt()
        EspBatterySectionMover.move(
            binding.cardSync,
            binding.dividerBattery,
            binding.btnBatteryWhitelist,
            binding.tvBatteryOemHint,
            binding.llBatteryBottom,
            topMarginPx
        )
        EspSyncConfig.batteryBtnMoved = true
    }

    /**
     * 按 App 主题给页面着色：卡片底色/圆角/透明度跟随主题（dialogSurfaceBackground），
     * 文字按 XML 中的 tag 角色（esp_primary / esp_secondary / esp_edit / esp_divider）统一配色。
     */
    private fun applyTheme() {
        // ★ 2026-09-13 修「白底白字看不见」：基座的 isDarkTheme 是近似判断
        //   （val Context.isDarkTheme get() = ColorUtils.isColorLight(ThemeStore.primaryColor(this))），
        //   在浅色主题下会误判 → primaryTextColor 取到浅色文字，而我们卡片底色又是浅色 → 白底白字。
        //   改成**按我们实际涂抹的底色亮度**决定文字深浅，并显式给页面铺同一底色，任何主题都有对比度。
        val context = requireContext()
        val surface = context.backgroundColor
        val surfaceIsLight = ColorUtils.isColorLight(surface)
        val primary = context.getPrimaryTextColor(surfaceIsLight)
        val secondaryRaw = context.getSecondaryTextColor(surfaceIsLight)
        // 基座在深色主题下 secondary 与 primary 同为纯白，这里压一档透明度做出层次
        val secondary = if (secondaryRaw == primary) ColorUtils.adjustAlpha(primary, 0.62f) else secondaryRaw
        val dividerColor = ColorUtils.adjustAlpha(primary, 0x14 / 255f)
        // 每张卡片独立创建背景 Drawable（同一 Drawable 实例不能复用给多个 View，
        // 否则所有卡片共享最后一个 View 的 bounds，内容会画出背景板）
        listOf(
            binding.cardSync
        ).forEach { it.background = requireContext().dialogSurfaceBackground }
        // 根 LinearLayout（ScrollView 唯一子 View）
        val content = (binding.root as? ViewGroup)?.getChildAt(0) as? ViewGroup ?: return
        // 页面底色与文字色同源，避免"窗口底色深、卡片浅"造成另一种看不见
        binding.root.setBackgroundColor(surface)
        content.setBackgroundColor(surface)
        themeCard(content, primary, secondary, dividerColor)

        val radius = UiCorner.actionRadius(requireContext())
        val isNight = !surfaceIsLight
        fun roundedSolid(color: Int): GradientDrawable = GradientDrawable().apply {
            cornerRadius = radius
            setColor(color)
        }
        // 普通灰色按钮：统一圆角（浅色/深色各一套）
        val greyNormal = if (isNight) 0xFF3C3C3C.toInt() else 0xFFE3E3E3.toInt()
        val greyPressed = if (isNight) 0xFF4D4D4D.toInt() else 0xFFC8C8C8.toInt()
        listOf(
            binding.btnDevIpConfirm,
            binding.btnOpenManage
        ).forEach { btn ->
            btn.background = StateListDrawable().apply {
                addState(intArrayOf(android.R.attr.state_pressed), roundedSolid(greyPressed))
                addState(IntArray(0), roundedSolid(greyNormal))
            }
        }
        // 电池授权按钮：主题主色圆角按钮（圆角跟随 App 圆角缩放设置）
        binding.btnBatteryWhitelist.background = StateListDrawable().apply {
            addState(
                intArrayOf(android.R.attr.state_pressed),
                roundedSolid(requireContext().primaryColorDark)
            )
            addState(IntArray(0), roundedSolid(requireContext().primaryColor))
        }
        // 按钮底色是主题主色，文字色按主色亮度取（原来硬编码白色，浅色主色下也会糊）
        binding.btnBatteryWhitelist.setTextColor(
            context.getPrimaryTextColor(ColorUtils.isColorLight(context.primaryColor))
        )
    }

    private fun themeCard(group: ViewGroup, primary: Int, secondary: Int, dividerColor: Int) {
        for (i in 0 until group.childCount) {
            val v = group.getChildAt(i)
            when (v.tag) {
                "esp_divider" -> v.setBackgroundColor(dividerColor)
                "esp_primary" -> (v as? TextView)?.setTextColor(primary)
                "esp_secondary" -> (v as? TextView)?.setTextColor(secondary)
                "esp_edit" -> (v as? EditText)?.let {
                    it.setTextColor(primary)
                    it.setHintTextColor(secondary)
                }
            }
            if (v is ViewGroup) themeCard(v, primary, secondary, dividerColor)
        }
    }

    private fun refreshSyncStatus() {
        // ★ 2026-09-13 用户反馈"手表上完全没有关于同步的信息"：
        //   状态文案只在"设备推送弹窗被确认"时才写，而正常流程是**设备主动来拉进度**。
        //   现在服务端每次成功响应都会记 deviceIp/lastContactTs（见 EspSyncConfig.onDeviceContact），
        //   这里据此显示真实连通状态与最近联系时间。
        // ★ 2026-09-13 共享状态：阅读菜单也能开关同步 → Switch 每次刷新时对齐真源
        //   （避免"阅读界面开了、设备页还显示关"）。注意：这里不触发 listener，不会反向启停服务。
        binding.switchSyncEnable.setOnCheckedChangeListener(null)
        binding.switchSyncEnable.isChecked = EspSyncConfig.enabled
        initSwitchListener()
        val status = EspSyncConfig.status
        val addresses = NetworkUtils.getLocalIPAddress().map { "${it.hostAddress}:$DEF_PORT" }
        binding.tvSyncStatus.text = buildString {
            append(getString(R.string.esp_sync_status))
            append("：")
            append(
                status.ifBlank {
                    if (EspSyncService.isRunning) getString(R.string.esp_sync_service_running)
                    else getString(R.string.esp_sync_never)
                }
            )
            append('\n')
            append(getString(R.string.esp_sync_local_address))
            append("：")
            append(if (addresses.isEmpty()) "-" else addresses.joinToString(", "))
        }
        val lastTs = EspSyncConfig.lastSyncTs
        val lastText = if (lastTs <= 0L) {
            getString(R.string.esp_sync_never)
        } else {
            SimpleDateFormat("MM-dd HH:mm:ss", Locale.getDefault()).format(Date(lastTs))
        }
        val deviceIp = EspSyncConfig.deviceIp
        binding.tvSyncLast.text = if (deviceIp.isNotBlank() && EspSyncConfig.lastContactTs > 0L) {
            getString(R.string.esp_sync_last_contact, deviceIp, lastText)
        } else {
            getString(R.string.esp_sync_last_sync, lastText)
        }

        // 绑定/连通状态：设备真的连过我们（有 deviceIp）就算"已连接"，
        // 这比"主动绑定是否成功"更能反映实际（设备阅读时 WiFi 关闭，主动绑定本来就会失败）。
        val bound = EspSyncConfig.boundEspIp
        binding.tvBindStatus.text = when {
            deviceIp.isNotBlank() -> getString(R.string.esp_bind_status_connected, deviceIp)
            bound.isNotBlank() -> getString(R.string.esp_bind_status_bound, bound)
            EspSyncConfig.bindState.isNotBlank() -> EspSyncConfig.bindState
            else -> getString(R.string.esp_bind_status_none)
        }
        // （管理区的"未连接"提示已随水墨屏设置区移除；同步状态见 tvBindStatus）
    }

    /**
     * 主动发现并绑定墨水屏（方案 D-1）。
     * manual=true（点"重新绑定"）：给长窗口 + 过程/失败提示；否则为静默尝试。
     */
    private fun doBind(manual: Boolean) {
        if (!EspSyncConfig.enabled) {
            if (manual) toastOnUi(R.string.esp_sync_need_enable)
            return
        }
        if (!ensureLanAvailable()) return
        if (manual) {
            EspSyncConfig.bindState = getString(R.string.esp_bind_status_searching)
            refreshSyncStatus()
        }
        viewLifecycleOwner.lifecycleScope.launch {
            val esp = EspBindClient.discover(if (manual) 20_000 else 4_000)
            val ok = esp != null && EspBindClient.bind(esp)
            if (ok) {
                EspSyncConfig.boundEspIp = esp!!
                EspSyncConfig.autoBind = true
                EspSyncConfig.bindState = ""
            } else if (manual) {
                EspSyncConfig.bindState = getString(R.string.esp_bind_status_failed)
            }
            refreshSyncStatus()
        }
    }

    // ==================== 管理区 ====================




    private fun buildDeviceStatusText(info: EspDeviceInfo): String = buildString {
        appendLine(getString(R.string.esp_device_status_addr, EspDeviceManager.getBaseUrl()))
        appendLine(
            getString(R.string.esp_device_status_state, info.state?.takeIf { it.isNotEmpty() } ?: "-")
        )
        appendLine(
            if (info.staConnected) {
                getString(
                    R.string.esp_device_status_wifi_connected,
                    info.staIp?.takeIf { it.isNotEmpty() } ?: "-"
                )
            } else {
                getString(R.string.esp_device_status_wifi_disconnected)
            }
        )
        append(
            getString(
                R.string.esp_device_status_firmware,
                info.version?.takeIf { it.isNotEmpty() } ?: "-"
            )
        )
        append("  ")
        append(getString(R.string.esp_device_status_space, ((info.freeSketchSpace ?: 0L) / 1024L).toInt()))
    }








    /** OTA：IO 读文件字节 → 后台上传（读取与上传均在 EspDeviceManager 内切 IO）。 */

    private fun queryOtaName(uri: Uri): String {
        var name: String? = uri.lastPathSegment
        runCatching {
            requireContext().contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0) cursor.getString(idx)?.let { name = it }
                }
            }
        }
        return name?.takeIf { it.isNotEmpty() } ?: "firmware.bin"
    }


}

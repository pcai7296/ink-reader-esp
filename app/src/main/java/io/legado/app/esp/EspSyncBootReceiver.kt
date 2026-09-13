package io.legado.app.esp

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * 开机 / 升级后自动恢复 ESP 同步服务（RelayBootReceiver 同款模式）。
 * 仅在同步开关为开启状态时启动，避免用户手动关闭后也被强制拉起。
 */
class EspSyncBootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action !in setOf(
                Intent.ACTION_BOOT_COMPLETED,
                Intent.ACTION_MY_PACKAGE_REPLACED
            )
        ) return
        if (EspSyncConfig.enabled) {
            EspSyncService.start(context)
        }
    }
}

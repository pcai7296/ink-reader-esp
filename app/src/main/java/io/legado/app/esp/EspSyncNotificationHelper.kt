package io.legado.app.esp

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat
import io.legado.app.R
import io.legado.app.constant.NotificationId
import io.legado.app.ui.book.read.ReadBookActivity
import io.legado.app.utils.servicePendingIntent

/**
 * ESP 推送进度时，如果 App 在后台（无前台阅读器 Activity），
 * 发送通知，用户点击后打开阅读器并弹框确认。
 */
object EspSyncNotificationHelper {

    private const val CHANNEL_ID = "channel_esp_sync_notify"
    private const val NOTIFICATION_ID = NotificationId.EspSyncService

    fun show(key: String, devProgress: DeviceProgress) {
        val ctx = splitties.init.appCtx
        val pos = devProgress.pos
        ensureChannel(ctx)

        val intent = Intent(ctx, ReadBookActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK
            putExtra(EXTRA_KEY, key)
        }
        val pending = PendingIntent.getActivity(
            ctx, 0, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val notification = NotificationCompat.Builder(ctx, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_web_service_noti)
            .setContentTitle(ctx.getString(R.string.esp_sync_notify_title))
            .setContentText(ctx.getString(
                R.string.esp_sync_notify_text,
                pos.fileName,
                "%.1f".format(pos.pct)
            ))
            .setContentIntent(pending)
            .setAutoCancel(true)
            .build()

        val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(NOTIFICATION_ID, notification)
    }

    fun dismiss() {
        val ctx = splitties.init.appCtx
        val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.cancel(NOTIFICATION_ID)
    }

    private fun ensureChannel(ctx: Context) {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
            val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            nm.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    ctx.getString(R.string.esp_sync_notify_channel),
                    NotificationManager.IMPORTANCE_DEFAULT
                )
            )
        }
    }

    const val EXTRA_KEY = "esp_sync_key"
}

package io.legado.app.lib.theme

import android.content.Context
import android.graphics.drawable.GradientDrawable

/**
 * 移植垫片（2026-09-13）：io.legado.app.esp 设备页原本用自研分支的 UiCorner /
 * dialogSurfaceBackground（自研分支有 uiCornerScale + dimens），原版延续版没有这套。
 * 这里给出最小等价实现，让移植过来的设备页代码不用改。
 */
object UiCorner {

    fun actionRadius(context: Context): Float =
        ACTION_RADIUS_DP * context.resources.displayMetrics.density

    fun panelRadius(context: Context): Float =
        PANEL_RADIUS_DP * context.resources.displayMetrics.density

    private const val ACTION_RADIUS_DP = 8f
    private const val PANEL_RADIUS_DP = 12f
}

val Context.dialogSurfaceBackground: GradientDrawable
    get() = GradientDrawable().apply {
        setColor(backgroundColor)
        cornerRadius = UiCorner.panelRadius(this@dialogSurfaceBackground)
    }
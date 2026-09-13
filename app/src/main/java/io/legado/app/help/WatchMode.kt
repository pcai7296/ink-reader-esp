package io.legado.app.help

import android.os.Build
import android.util.Log
import io.legado.app.constant.PreferKey
import io.legado.app.utils.getPrefBoolean
import io.legado.app.utils.putPrefBoolean
import splitties.init.appCtx
import kotlin.math.hypot

/**
 * 手表模式：安装后**首次启动自动识别一次并固定**，之后只读用户选择，绝不重新识别。
 *
 * 两个 pref 的语义（必须分清，否则用户手动关掉后会被硬件检测重新打开）：
 * - [PreferKey.watchMode]      = 当前模式（用户可改）
 * - [PreferKey.watchModeFixed] = 首次自动识别是否已完成（只写一次，之后不再参与判定）
 *
 * 判定强弱分级（本机 Watch W527 命中"强判定"）：
 * - 强：ro.build.characteristics 含 watch
 * - 弱：物理对角线 < 3.0" / smallestWidthDp ≤ 340 / 型号含 watch|wear
 * 任一命中即为手表；命中原因写日志，便于日后误判时直接从 logcat 定位是哪条规则。
 */
object WatchMode {

    private const val TAG = "WatchMode"

    /** 当前是否启用（读用户选择） */
    fun isEnabled(): Boolean = appCtx.getPrefBoolean(PreferKey.watchMode, false)

    /** 首次自动识别是否已完成 */
    fun isFixed(): Boolean = appCtx.getPrefBoolean(PreferKey.watchModeFixed, false)

    /**
     * 首次启动自动识别并固定。**只在未固定时执行一次**：
     * 绝不出现 `if (!fixed || watchMode == false) re-detect` 这种形态（会覆盖用户的选择）。
     * 调用点：App.onCreate 最早期（早于任何 AppConfig 读取与 UI 创建）。
     */
    fun ensureFixedOnFirstLaunch() {
        if (isFixed()) return
        val detected = detect()
        appCtx.putPrefBoolean(PreferKey.watchMode, detected)
        appCtx.putPrefBoolean(PreferKey.watchModeFixed, true)
        Log.i(TAG, "首次识别完成并固定: watchMode=$detected")
    }

    /** `ro.build.characteristics`（Build.CHARACTERISTICS 是 @hide，只能反射读 SystemProperties） */
    private fun characteristics(): String = try {
        val cls = Class.forName("android.os.SystemProperties")
        val get = cls.getMethod("get", String::class.java)
        (get.invoke(null, "ro.build.characteristics") as? String).orEmpty()
    } catch (t: Throwable) {
        Log.w(TAG, "读取 ro.build.characteristics 失败: ${t.message}")
        ""
    }

    /** 检测（只读硬件/系统属性，可反复调用；判定结果由调用方决定是否落盘） */
    fun detect(): Boolean {
        val characteristics = characteristics()
        val dm = appCtx.resources.displayMetrics
        val w = dm.widthPixels
        val h = dm.heightPixels
        val xdpi = if (dm.xdpi > 0f) dm.xdpi else dm.densityDpi.toFloat()
        val ydpi = if (dm.ydpi > 0f) dm.ydpi else dm.densityDpi.toFloat()
        val diagonalInch = hypot(w / xdpi, h / ydpi)
        val smallestWidthDp = minOf(w, h) * 160 / dm.densityDpi
        val model = "${Build.MODEL} ${Build.PRODUCT}".lowercase()

        val strong = characteristics.contains("watch", ignoreCase = true)
        val byDiagonal = diagonalInch < 3.0f
        val byWidth = smallestWidthDp <= 340
        val byModel = model.contains("watch") || model.contains("wear")
        val detected = strong || byDiagonal || byWidth || byModel
        val reason = when {
            strong -> "characteristics"
            byDiagonal -> "diagonal<3.0in"
            byWidth -> "smallestWidthDp<=340"
            byModel -> "model"
            else -> "none"
        }
        Log.i(
            TAG,
            "characteristics=$characteristics diagonal=%.2fin smallestWidthDp=%d model=%s ".format(
                diagonalInch, smallestWidthDp, Build.MODEL
            ) + "densityDpi=${dm.densityDpi} size=${w}x$h detected=$detected reason=$reason"
        )
        return detected
    }
}

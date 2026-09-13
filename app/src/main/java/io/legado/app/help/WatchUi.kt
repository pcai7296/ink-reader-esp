package io.legado.app.help

import android.graphics.Rect
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import androidx.core.view.WindowInsetsCompat
import io.legado.app.utils.dpToPx
import io.legado.app.utils.SystemUtils
import java.util.WeakHashMap

/**
 * 手表模式的尺寸中枢：所有"小屏 + 物理圆角"相关的像素值只在这里定义。
 *
 * **两条铁律**（来自实施方案 P0-2）：
 * 1. 每个 View 的"基准 padding"只记录**一次**（[basePadding]），之后永远 `base + extra`，
 *    绝不基于"当前 padding"再累加 —— 否则 WindowInsets 重放 / Activity 重建 / IME 弹出收起会累计变胖。
 * 2. 顶部目标是**总共** CORNER_PX（不是 状态栏+CORNER_PX）；本类的调用者只负责把 extra 传对。
 *
 * 表面规则（用户定稿 2026-09-13）：
 * - 主界面 / 设置页 / 设备页：**只避让顶部/底部的圆角**（上 44 下 44），左右（中部）一律不动
 * - 阅读界面：左右避让手势 30px、页脚下限 44px（防碰圆角），其余由 ReadBookConfig 决定
 * - 底栏：蓝色背景铺满屏宽（可越出屏幕），5 个按钮收窄居中、尺寸不变
 */
object WatchUi {

    private const val TAG = "WatchUi"

    /** 你实测的物理圆角（px，屏宽 412 下） */
    const val CORNER_PX = 44

    /** 左右返回手势热区（px，实测 30px = 24dp） */
    const val GESTURE_EDGE_PX = 30

    /** 手表模式底栏按钮组目标宽度：5 项 × 48dp（图标 24dp + 两侧 12dp 触控余量） */
    val bottomBarWidthPx: Int get() = 5 * 48.dpToPx()

    private val basePadding = WeakHashMap<View, Rect>()

    /** 每个 View 只记一次基准 padding（不受后续 setPadding 影响） */
    private fun baseOf(v: View): Rect = basePadding.getOrPut(v) {
        Rect(v.paddingLeft, v.paddingTop, v.paddingRight, v.paddingBottom)
    }

    /**
     * 按手表模式给内容容器加安全区（背景仍可铺满）。
     * 非手表模式调用它是安全的：会原样恢复基准 padding。
     *
     * 入参是**要额外增加的像素**（不是最终值）；最终 = base + extra，永不累加。
     */
    fun applyContentSafeArea(
        v: View,
        leftPx: Int = CORNER_PX,
        topPx: Int = CORNER_PX,
        rightPx: Int = CORNER_PX,
        bottomPx: Int = CORNER_PX
    ) {
        val base = baseOf(v)
        if (!WatchMode.isEnabled()) {
            v.setPadding(base.left, base.top, base.right, base.bottom)
            return
        }
        v.setPadding(
            base.left + leftPx,
            base.top + topPx,
            base.right + rightPx,
            base.bottom + bottomPx
        )
        Log.i(
            TAG,
            "surface=${v.id} extra=[$leftPx,$topPx,$rightPx,$bottomPx] " +
                "base=[${base.left},${base.top},${base.right},${base.bottom}] " +
                "result=[${v.paddingLeft},${v.paddingTop},${v.paddingRight},${v.paddingBottom}]"
        )
    }

    /**
     * 主界面 / 设置页 / 设备页：**只避让顶部/底部的圆角**，左右（中间区域）一律不动。
     *
     * 顶部按"总共 44px"折算：系统若已用状态栏 inset 消耗了一部分，只补差值（避免 30+44=74 的叠加）。
     *
     * @param bottomTargetPx 底部总 inset（默认 0：底栏背景自身铺满、覆盖底部圆角，内容不再额外内缩）
     */
    fun applyMainSafeArea(v: View, statusBarInsetPx: Int = 0, bottomTargetPx: Int = 0) {
        val extraTop = (CORNER_PX - statusBarInsetPx).coerceAtLeast(0)
        applyContentSafeArea(
            v,
            leftPx = 0,
            topPx = extraTop,
            rightPx = 0,
            bottomPx = bottomTargetPx
        )
    }

    /** 阅读页专用：只避让左右手势热区（顶/底保持 ReadBookConfig 的原逻辑） */
    fun applyReaderGestureInset(v: View) {
        applyContentSafeArea(v, leftPx = GESTURE_EDGE_PX, topPx = 0, rightPx = GESTURE_EDGE_PX, bottomPx = 0)
    }

    /**
     * 阅读页页脚（左下书名 / 右下章节进度）：手表模式下保证不碰物理圆角 ——
     * 左/右/下各留出 [footerFloorPx] 的下限（用户设置的更大值不受影响）。
     * 由 PageView 在设置 llFooter padding 时调用。
     */
    val footerFloorPx: Int
        get() = if (WatchMode.isEnabled()) CORNER_PX else 0

    /**
     * 底栏：**蓝色背景铺满屏宽（用户要求"长度可以超出屏幕"）**，
     * 而 5 个按钮仍收窄在中间 [bottomBarWidthPx] 里、位置不变（用 bar 自身的左右 padding 实现）。
     * 底部 padding 由系统 insets（导航栏）另管，这里不碰。
     */
    fun applyBottomBar(bar: View) {
        val lp = bar.layoutParams
        lp.width = ViewGroup.LayoutParams.MATCH_PARENT
        if (lp is LinearLayout.LayoutParams) {
            lp.gravity = Gravity.NO_GRAVITY
        }
        bar.layoutParams = lp
        if (WatchMode.isEnabled()) {
            val side = ((SystemUtils.screenWidthPx - bottomBarWidthPx) / 2).coerceAtLeast(0)
            bar.setPadding(side, bar.paddingTop, side, bar.paddingBottom)
        } else {
            bar.setPadding(0, bar.paddingTop, 0, bar.paddingBottom)
        }
    }
}

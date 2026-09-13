package io.legado.app.ui.main.device

import android.view.View
import android.view.ViewGroup

/**
 * 电池授权按钮区（按钮 + 说明 + 上方分隔线）从同步卡移动到设备页最底部容器。
 * 幂等：重复调用不崩溃、不重复挂载（视图已移除/已挂载时直接跳过）。
 */
object EspBatterySectionMover {

    fun move(
        card: ViewGroup,
        divider: View,
        button: View,
        hint: View,
        bottom: ViewGroup,
        topMarginPx: Int
    ) {
        // 分隔线随按钮区一起移除，避免同步卡底部残留悬空线
        if (divider.parent is ViewGroup) {
            (divider.parent as ViewGroup).removeView(divider)
        }
        if (button.parent === card) {
            card.removeView(button)
        }
        if (hint.parent === card) {
            card.removeView(hint)
        }
        if (button.parent !== bottom) {
            (button.layoutParams as? ViewGroup.MarginLayoutParams)?.topMargin = topMarginPx
            bottom.addView(button)
        }
        if (hint.parent !== bottom) {
            bottom.addView(hint)
        }
    }
}

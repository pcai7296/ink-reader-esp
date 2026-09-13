package io.legado.app.utils

import androidx.core.view.WindowInsetsCompat

val WindowInsetsCompat.navigationBarHeight
    get() = (getInsets(WindowInsetsCompat.Type.systemBars()).bottom - imeHeight).coerceAtLeast(0)

val WindowInsetsCompat.imeHeight
    get() = getInsets(WindowInsetsCompat.Type.ime()).bottom

/** 状态栏高度（手表模式下与圆角安全区取 max 用） */
val WindowInsetsCompat.statusBarHeight
    get() = getInsets(WindowInsetsCompat.Type.statusBars()).top

package io.legado.app.esp

import android.net.Uri
import io.legado.app.utils.defaultSharedPreferences
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import splitties.init.appCtx
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.Base64

/** ESP 设备基础地址的 SharedPreferences key（集中管理）。 */
object EspDevicePrefs {
    const val BASE_URL = "esp_device_base_url"
}

/**
 * 设备基础地址归一化（纯函数，JVM 单测可直接调用）。
 * 规则（与 WebDavProxy DeviceManager.setBaseUrl 一致）：
 * 1. trim 首尾空白；
 * 2. 无 http:// 或 https:// 前缀时补 "http://"；
 * 3. 去掉末尾所有 "/"。
 * 空白输入原样返回（不补前缀、不截断）。
 */
fun normalizeBaseUrl(url: String): String {
    var u = url.trim()
    if (u.isEmpty()) return u
    if (!u.startsWith("http://") && !u.startsWith("https://")) u = "http://$u"
    while (u.endsWith("/")) u = u.dropLast(1)
    return u
}

/** /info 端点返回的设备信息（字段对齐 ESP 固件 info JSON，见 Todo 2 ENDPOINT TABLE）。 */
data class EspDeviceInfo(
    val version: String?,
    val state: String?,
    val staConnected: Boolean,
    val staIp: String?,
    val clockFormat: Int?,
    val tzOffsetMin: Int?,
    val hitokotoEnabled: Boolean?,
    val portrait: Int?,
    val freeSketchSpace: Long?
)

/** /target 端点返回的连接对象（staIp/apIp）。 */
data class EspDeviceTarget(
    val staIp: String?,
    val apIp: String?
)

/**
 * ESP12F 设备管理网络层：调用墨水屏管理 web (ESP8266WebServer) 的端点。
 * 移植自 WebDavProxy DeviceManager.java（网络层 L88-200、表单构造 L216-220、迷你 JSON 解析 L32-86）。
 *
 * 端点表（已验证基线）：
 * - GET  /info    -> 设备状态/配置 JSON
 * - POST /wifi    -> WiFi {ssid,password} 与天气 {city,wkey,night} 共用（固件无 /weather 端点）
 * - POST /settings-> {clockFormat,tz,hitokoto,portrait}
 * - GET/POST /target -> {staIp,apIp}
 * - POST /clear   -> {confirm:"1"}
 * - POST /update  -> multipart OTA 固件，Authorization Basic admin:333333，120s 读超时
 */
object EspDeviceManager {

    /**
     * 阅读旋转方向：固件存储编码（SettingsConfig.portrait / /settings 的 portrait 参数）↔ UI 显示度数。
     * 对齐固件权威映射（file_manager.ino storedToRot/rotToStored + 显示角约定）：
     * 内部角 0/90/180/270，显示角 = (内部角 − 90 + 360) % 360（默认横屏内部 90° = 0°，顺时针递增）；
     * 存储编码 0横/1竖/2横翻/3竖翻。
     *   stored 0(横屏) → 0°；stored 1(竖屏) → 270°；stored 2(横翻) → 180°；stored 3(竖翻) → 90°
     * 非法存储值兜底 0°（同固件 storedToRot 非法残留 → 旧默认横屏）。
     */
    fun storedToDisplayAngle(stored: Int): Int = when (stored) {
        1 -> 270
        2 -> 180
        3 -> 90
        else -> 0
    }

    /** [storedToDisplayAngle] 的逆映射：显示度数 → 固件存储编码（0°→0、90°→3、180°→2、270°→1）；非法角度兜底 0。 */
    fun displayAngleToStored(angle: Int): Int = when (angle) {
        90 -> 3
        180 -> 2
        270 -> 1
        else -> 0
    }

    private const val DEFAULT_BASE_URL = "http://192.168.4.1"   // 热点(AP)模式地址（用户 2026-09-12 明确：热点保持 4.1；连 WiFi 后设备为静态 192.168.0.100）
    private const val TIMEOUT_MS = 8_000
    private const val OTA_READ_TIMEOUT_MS = 120_000

    /** OTA 认证封装在类内，不向调用方暴露。 */
    private const val OTA_USERNAME = "admin"
    private const val OTA_PASSWORD = "333333"

    /** 设备基础地址（默认热点地址），始终从 SharedPreferences 读。 */
    fun getBaseUrl(): String =
        appCtx.defaultSharedPreferences.getString(EspDevicePrefs.BASE_URL, DEFAULT_BASE_URL)
            ?: DEFAULT_BASE_URL

    /** 保存设备基础地址并持久化（入库前归一化）。 */
    fun saveBaseUrl(url: String) {
        appCtx.defaultSharedPreferences.edit()
            .putString(EspDevicePrefs.BASE_URL, normalizeBaseUrl(url))
            .apply()
    }

    /** GET {base}/info —— 设备状态与配置。 */
    suspend fun getInfo(): Result<String> = getEndpoint("/info")

    /** GET {base}/target —— 设备推送目标（staIp/apIp）。 */
    suspend fun getTarget(): Result<String> = getEndpoint("/target")

    /**
     * POST {base}{path} —— application/x-www-form-urlencoded（UTF-8 URLEncoder）。
     * 供 /wifi、/settings、/target、/clear 共用；8s 连接+读超时。
     */
    suspend fun postForm(path: String, params: Map<String, String>): Result<String> =
        withContext(Dispatchers.IO) {
            runCatching {
                val body = params.entries.joinToString("&") { (k, v) ->
                    URLEncoder.encode(k, "UTF-8") + "=" + URLEncoder.encode(v, "UTF-8")
                }.toByteArray(Charsets.UTF_8)
                val conn = (URL(getBaseUrl() + path).openConnection() as HttpURLConnection)
                try {
                    conn.connectTimeout = TIMEOUT_MS
                    conn.readTimeout = TIMEOUT_MS
                    conn.requestMethod = "POST"
                    conn.doOutput = true
                    conn.setFixedLengthStreamingMode(body.size)
                    conn.setRequestProperty(
                        "Content-Type", "application/x-www-form-urlencoded; charset=UTF-8")
                    conn.outputStream.use { it.write(body) }
                    respond(conn)
                } finally {
                    conn.disconnect()
                }
            }
        }

    /**
     * POST {base}/update —— multipart/form-data OTA 固件上传。
     * 字段 name="file"，filename=fileName，Content-Type application/octet-stream，
     * Authorization Basic admin:333333，120s 读超时（上传+烧录较慢）。
     */
    suspend fun uploadFirmware(bin: ByteArray, fileName: String): Result<String> =
        withContext(Dispatchers.IO) {
            runCatching {
                val boundary = "----InkProxyBoundary" + System.currentTimeMillis()
                val body = buildUpdateRequest(boundary, fileName, bin)
                val conn = (URL(getBaseUrl() + "/update").openConnection() as HttpURLConnection)
                try {
                    conn.connectTimeout = TIMEOUT_MS
                    conn.readTimeout = OTA_READ_TIMEOUT_MS
                    conn.requestMethod = "POST"
                    conn.doOutput = true
                    conn.setRequestProperty("Authorization", buildAuthorizationHeader())
                    conn.setRequestProperty("Content-Type", "multipart/form-data; boundary=$boundary")
                    conn.outputStream.use { it.write(body) }
                    respond(conn)
                } finally {
                    conn.disconnect()
                }
            }
        }

    // ---- 纯函数（JVM 单测可直接调用，不依赖 Android 上下文） ----

    /** OTA Basic 认证头（封装 admin:333333）。 */
    fun buildAuthorizationHeader(): String = basicAuthHeader(OTA_USERNAME, OTA_PASSWORD)

    /** "Basic " + Base64(user:pass)。java.util.Base64 已由 desugar_jdk_libs 支持（minSdk 21 可用）。 */
    fun basicAuthHeader(user: String, pass: String): String =
        "Basic " + Base64.getEncoder().encodeToString("$user:$pass".toByteArray(Charsets.UTF_8))

    /** multipart/form-data 请求体：头行 + bin + 结束边界（布局与 DeviceManager.uploadFirmware 一致）。 */
    fun buildUpdateRequest(boundary: String, fileName: String, bin: ByteArray): ByteArray {
        val head = "--$boundary\r\n" +
            "Content-Disposition: form-data; name=\"file\"; filename=\"$fileName\"\r\n" +
            "Content-Type: application/octet-stream\r\n\r\n"
        val tail = "\r\n--$boundary--\r\n"
        val headBytes = head.toByteArray(Charsets.UTF_8)
        val tailBytes = tail.toByteArray(Charsets.UTF_8)
        val out = ByteArray(headBytes.size + bin.size + tailBytes.size)
        headBytes.copyInto(out, 0)
        bin.copyInto(out, headBytes.size)
        tailBytes.copyInto(out, headBytes.size + bin.size)
        return out
    }

    /**
     * 解析 /info JSON 为 [EspDeviceInfo]；畸形/空/非对象输入返回 null，绝不抛异常。
     *
     * 实现说明（与任务的偏差，已在报告中说明）：org.json 是 android.jar 中的类，在本仓库的
     * JVM 单测里被 mockable jar 桩化（调用抛 "not mocked"），而本任务禁止新增依赖与改动
     * app/build.gradle（无法引入 org.json:json 测试件），因此这里移植 DeviceManager.java
     * 自带的迷你 JSON 解析器（纯 Kotlin，字段级容错），行为与解析语义保持一致且可单测。
     */
    fun parseInfo(json: String): EspDeviceInfo? {
        if (json.isBlank()) return null
        val raw = json.trim()
        if (!raw.startsWith("{") || !raw.endsWith("}")) return null
        return try {
            EspDeviceInfo(
                version = jsonFieldString(raw, "version"),
                state = jsonFieldString(raw, "state"),
                staConnected = jsonFieldBoolean(raw, "staConnected", default = false),
                staIp = jsonFieldString(raw, "staIp"),
                clockFormat = jsonFieldInt(raw, "clockFormat"),
                tzOffsetMin = jsonFieldInt(raw, "tzOffsetMin"),
                hitokotoEnabled = jsonFieldBooleanOrNull(raw, "hitokotoEnabled"),
                portrait = jsonFieldInt(raw, "portrait"),
                freeSketchSpace = jsonFieldLong(raw, "freeSketchSpace")
            )
        } catch (_: Exception) {
            null
        }
    }

    /**
     * 解析 /target JSON 为 [EspDeviceTarget]；畸形/空/非对象输入返回 null，绝不抛异常。
     * 语义与 [parseInfo] 一致（复用同一迷你 JSON 解析器）。
     */
    fun parseTarget(json: String): EspDeviceTarget? {
        if (json.isBlank()) return null
        val raw = json.trim()
        if (!raw.startsWith("{") || !raw.endsWith("}")) return null
        return try {
            EspDeviceTarget(
                staIp = jsonFieldString(raw, "staIp"),
                apIp = jsonFieldString(raw, "apIp")
            )
        } catch (_: Exception) {
            null
        }
    }

    // ---- 内部实现 ----

    /**
     * 读取 SAF content Uri 的全部字节（IO 线程，读取失败返回 failure）。
     * OTA 上传前读取固件文件用；调用方无需接触底层流 API。
     */
    suspend fun readUriBytes(uri: Uri): Result<ByteArray?> = withContext(Dispatchers.IO) {
        runCatching {
            appCtx.contentResolver.openInputStream(uri)?.use { it.readBytes() }
        }
    }

    private suspend fun getEndpoint(path: String): Result<String> = withContext(Dispatchers.IO) {
        runCatching {
            val conn = (URL(getBaseUrl() + path).openConnection() as HttpURLConnection)
            try {
                conn.connectTimeout = TIMEOUT_MS
                conn.readTimeout = TIMEOUT_MS
                conn.requestMethod = "GET"
                respond(conn)
            } finally {
                conn.disconnect()
            }
        }
    }

    /** 读响应体；2xx 成功，其余抛 IOException("HTTP <code> <body>") 供 runCatching 转 Result.failure。 */
    private fun respond(conn: HttpURLConnection): String {
        val code = conn.responseCode
        val body = readAll(if (code in 200..299) conn.inputStream else conn.errorStream).trim()
        if (code in 200..299) return body
        throw IOException("HTTP $code $body")
    }

    private fun readAll(stream: InputStream?): String {
        if (stream == null) return ""
        return stream.bufferedReader(Charsets.UTF_8).use { it.readText() }
    }

    /**
     * 极简扁平 JSON 对象取值：定位 "key" 后读取冒号后的值 token。
     * 字符串值返回去转义内容；数字/布尔/null 返回原样 token；字段缺失返回 null。
     * 语义与 DeviceManager.Json.get() 一致（不支持嵌套对象/数组）。
     */
    private fun jsonField(raw: String, key: String): String? {
        val idx = raw.indexOf("\"$key\"")
        if (idx < 0) return null
        var i = idx + key.length + 2
        while (i < raw.length && raw[i] != ':') i++
        if (i >= raw.length) return null
        i++
        while (i < raw.length &&
            (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r' || raw[i] == '\n')
        ) i++
        if (i >= raw.length) return null
        return when (raw[i]) {
            '"' -> {
                val sb = StringBuilder()
                var j = i + 1
                while (j < raw.length) {
                    val c = raw[j]
                    if (c == '\\' && j + 1 < raw.length) {
                        when (val n = raw[j + 1]) {
                            'n' -> sb.append('\n')
                            'r' -> sb.append('\r')
                            't' -> sb.append('\t')
                            '"' -> sb.append('"')
                            '\\' -> sb.append('\\')
                            else -> sb.append(n)
                        }
                        j += 2
                    } else if (c == '"') {
                        return sb.toString()
                    } else {
                        sb.append(c)
                        j++
                    }
                }
                null
            }
            else -> {
                val start = i
                while (i < raw.length && raw[i] != ',' && raw[i] != '}' &&
                    raw[i] != ' ' && raw[i] != '\t' && raw[i] != '\r' && raw[i] != '\n'
                ) i++
                raw.substring(start, i)
            }
        }
    }

    private fun jsonFieldString(raw: String, key: String): String? {
        val v = jsonField(raw, key) ?: return null
        if (v == "null") return null
        return v.takeIf { it.isNotEmpty() }
    }

    private fun jsonFieldBoolean(raw: String, key: String, default: Boolean): Boolean {
        val v = jsonField(raw, key) ?: return default
        return when (v.lowercase()) {
            "true", "1" -> true
            "false", "0" -> false
            else -> default
        }
    }

    private fun jsonFieldBooleanOrNull(raw: String, key: String): Boolean? {
        val v = jsonField(raw, key) ?: return null
        if (v == "null") return null
        return when (v.lowercase()) {
            "true", "1" -> true
            "false", "0" -> false
            else -> null
        }
    }

    private fun jsonFieldInt(raw: String, key: String): Int? {
        val v = jsonField(raw, key) ?: return null
        if (v == "null") return null
        return v.toDoubleOrNull()?.toInt()
    }

    private fun jsonFieldLong(raw: String, key: String): Long? {
        val v = jsonField(raw, key) ?: return null
        if (v == "null") return null
        return v.toLongOrNull() ?: v.toDoubleOrNull()?.toLong()
    }
}

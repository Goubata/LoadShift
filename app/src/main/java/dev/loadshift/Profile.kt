package dev.loadshift

import android.content.Context
import org.json.JSONObject

/**
 * Per-app tuning profile.
 *
 * Dynamic knobs (applied only while the app is in front, restored afterwards):
 *  - gpuFloor  : 0 off, 1 mid, 2 high, 3 max  -> Adreno kgsl min_pwrlevel floor
 *  - cpuBoost  : 0 off, 1 light, 2 strong, 3 max -> top-app uclamp.min
 *  - pinThreads: pin render / game main threads to the big cores
 *
 * In-game hooks (LoadShift Zygisk module, next app start) — CPU work moved to / overlapped with GPU:
 *  - hookVkIdle   : vkQueueWaitIdle/vkDeviceWaitIdle return at once; wait postponed (lazy)
 *  - vkIdleLevel  : 1 safe, 2 aggressive (descriptor updates don't wait)
 *  - vkIdleDepth  : max unfinished markers before WaitIdle really waits (1..8)
 *  - hookVkCache  : persistent Vulkan pipeline cache
 *  - hookOccl     : unfinished occlusion queries answer "visible" instead of stalling
 *  - hookGlBuf    : glBufferSubData / write-maps become GPU copies from a staging ring
 *  - hookPbo      : texture uploads staged in a persistent ring, GPU converts/tiles/copies
 *  - hookFinish   : glFinish → fence+flush (finishDepth frames ahead)
 *  - hookReadback : repeated glReadPixels answered from the previous async read (1 frame late)
 *  - flushUs      : drop glFlush calls closer than this (single-context apps), 0 = off
 *  - buffer/ring/copy tuning: bufMin, texMinKB, bufRingMB, texRingMB, copyThreads, copyThresholdKB
 *
 * Static knobs (system settings, next app start):
 *  - glDriver  : 0 default, 1 ANGLE (GL→Vulkan), 2 native
 *  - gameMode  : 0 untouched, 1 standard, 2 performance, 3 battery
 *  - downscale : 1.0 = off, e.g. 0.7 = render at 70 % resolution (Game Mode intervention)
 *  - fps       : 0 = off, otherwise Game Mode frame-rate override (Hz, read from the display)
 */
data class Profile(
    val pkg: String,
    val enabled: Boolean = true,
    val gpuFloor: Int = 2,
    val cpuBoost: Int = 1,
    val pinThreads: Boolean = true,
    val glDriver: Int = 0,
    val gameMode: Int = 0,
    val downscale: Float = 1.0f,
    val fps: Float = 0f,
    // in-game hooks
    val preset: Int = 1,
    val hookFinish: Boolean = true,
    val finishDepth: Int = 2,
    val hookPbo: Boolean = true,
    val hookVkCache: Boolean = true,
    val hookVkIdle: Boolean = true,
    val vkIdleLevel: Int = 1,
    val vkIdleDepth: Int = 3,
    val hookGlBuf: Boolean = true,
    val hookOccl: Boolean = true,
    val hookReadback: Boolean = false,
    val flushUs: Int = 0,
    val bufMin: Int = 1024,
    val texMinKB: Int = 64,
    val bufRingMB: Int = 32,
    val texRingMB: Int = 48,
    val copyThreads: Int = 4,
    val copyThresholdKB: Int = 2048,
    // coexistence with other graphics hook modules (lybfghook, Portable GameFX, …)
    val compatInstall: Int = 1,      // 0 early, 1 auto, 2 late
    val compatTimeoutMs: Int = 3000,
    val compatExempt: Boolean = true,
) {
    val anyHook
        get() = hookFinish || hookPbo || hookVkCache || hookVkIdle || hookGlBuf || hookOccl || hookReadback || flushUs > 0

    /** Applies one of the presets (0 safe, 1 standard, 2 aggressive, 3 max); keeps app/system knobs. */
    fun withPreset(i: Int): Profile = when (i) {
        0 -> copy(preset = 0, hookFinish = true, finishDepth = 1, hookPbo = true, hookVkCache = true, hookVkIdle = true,
            vkIdleLevel = 1, vkIdleDepth = 2, hookGlBuf = true, hookOccl = false, hookReadback = false, flushUs = 0,
            bufMin = 4096, texMinKB = 256, bufRingMB = 32, texRingMB = 32, copyThreads = 4, copyThresholdKB = 2048)
        2 -> copy(preset = 2, hookFinish = true, finishDepth = 3, hookPbo = true, hookVkCache = true, hookVkIdle = true,
            vkIdleLevel = 2, vkIdleDepth = 4, hookGlBuf = true, hookOccl = true, hookReadback = true, flushUs = 1000,
            bufMin = 512, texMinKB = 32, bufRingMB = 64, texRingMB = 64, copyThreads = 4, copyThresholdKB = 1024)
        3 -> copy(preset = 3, hookFinish = true, finishDepth = 3, hookPbo = true, hookVkCache = true, hookVkIdle = true,
            vkIdleLevel = 2, vkIdleDepth = 6, hookGlBuf = true, hookOccl = true, hookReadback = true, flushUs = 2000,
            bufMin = 256, texMinKB = 16, bufRingMB = 128, texRingMB = 96, copyThreads = 4, copyThresholdKB = 1024)
        else -> copy(preset = 1, hookFinish = true, finishDepth = 2, hookPbo = true, hookVkCache = true, hookVkIdle = true,
            vkIdleLevel = 1, vkIdleDepth = 3, hookGlBuf = true, hookOccl = true, hookReadback = false, flushUs = 0,
            bufMin = 1024, texMinKB = 64, bufRingMB = 32, texRingMB = 48, copyThreads = 4, copyThresholdKB = 2048)
    }

    fun toJson(): JSONObject = JSONObject()
        .put("pkg", pkg).put("enabled", enabled).put("gpuFloor", gpuFloor)
        .put("cpuBoost", cpuBoost).put("pinThreads", pinThreads).put("glDriver", glDriver)
        .put("gameMode", gameMode).put("downscale", downscale.toDouble()).put("fps", fps.toDouble())
        .put("preset", preset)
        .put("hookFinish", hookFinish).put("finishDepth", finishDepth)
        .put("hookPbo", hookPbo).put("hookVkCache", hookVkCache)
        .put("hookVkIdle", hookVkIdle).put("vkIdleLevel", vkIdleLevel).put("vkIdleDepth", vkIdleDepth)
        .put("hookGlBuf", hookGlBuf).put("hookOccl", hookOccl).put("hookReadback", hookReadback)
        .put("flushUs", flushUs).put("bufMin", bufMin).put("texMinKB", texMinKB)
        .put("bufRingMB", bufRingMB).put("texRingMB", texRingMB)
        .put("copyThreads", copyThreads).put("copyThresholdKB", copyThresholdKB)
        .put("compatInstall", compatInstall).put("compatTimeoutMs", compatTimeoutMs).put("compatExempt", compatExempt)

    companion object {
        fun fromJson(o: JSONObject) = Profile(
            pkg = o.getString("pkg"),
            enabled = o.optBoolean("enabled", true),
            gpuFloor = o.optInt("gpuFloor", 2),
            cpuBoost = o.optInt("cpuBoost", 1),
            pinThreads = o.optBoolean("pinThreads", true),
            glDriver = o.optInt("glDriver", 0),
            gameMode = o.optInt("gameMode", 0),
            downscale = o.optDouble("downscale", 1.0).toFloat(),
            fps = o.optDouble("fps", 0.0).toFloat(),
            preset = o.optInt("preset", 1),
            hookFinish = o.optBoolean("hookFinish", true),
            finishDepth = o.optInt("finishDepth", 2),
            hookPbo = o.optBoolean("hookPbo", true),
            hookVkCache = o.optBoolean("hookVkCache", true),
            hookVkIdle = o.optBoolean("hookVkIdle", true),
            vkIdleLevel = o.optInt("vkIdleLevel", 1),
            vkIdleDepth = o.optInt("vkIdleDepth", 3),
            hookGlBuf = o.optBoolean("hookGlBuf", true),
            hookOccl = o.optBoolean("hookOccl", true),
            hookReadback = o.optBoolean("hookReadback", false),
            flushUs = o.optInt("flushUs", 0),
            bufMin = o.optInt("bufMin", 1024),
            texMinKB = o.optInt("texMinKB", 64),
            bufRingMB = o.optInt("bufRingMB", 32),
            texRingMB = o.optInt("texRingMB", 48),
            copyThreads = o.optInt("copyThreads", 4),
            copyThresholdKB = o.optInt("copyThresholdKB", 2048),
            compatInstall = o.optInt("compatInstall", 1),
            compatTimeoutMs = o.optInt("compatTimeoutMs", 3000),
            compatExempt = o.optBoolean("compatExempt", true),
        )
    }
}

object ProfileStore {
    private const val PREFS = "profiles"

    private fun prefs(c: Context) = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun all(c: Context): Map<String, Profile> =
        prefs(c).all.mapNotNull { (k, v) ->
            runCatching { k to Profile.fromJson(JSONObject(v as String)) }.getOrNull()
        }.toMap()

    fun get(c: Context, pkg: String): Profile? = all(c)[pkg]

    fun put(c: Context, p: Profile) {
        prefs(c).edit().putString(p.pkg, p.toJson().toString()).apply()
    }

    fun remove(c: Context, pkg: String) {
        prefs(c).edit().remove(pkg).apply()
    }

    fun serviceEnabled(c: Context) = c.getSharedPreferences("main", 0).getBoolean("service", false)
    fun setServiceEnabled(c: Context, on: Boolean) =
        c.getSharedPreferences("main", 0).edit().putBoolean("service", on).apply()
}

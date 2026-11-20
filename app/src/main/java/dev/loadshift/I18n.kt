package dev.loadshift

import android.content.Context
import android.content.res.Configuration
import android.hardware.display.DisplayManager
import android.os.Build
import android.view.Display

/**
 * Language policy: Japanese UI only when the system language is Japanese; any other language
 * falls back to English. Re-evaluated on every configuration change (no restart needed).
 */
object I18n {
    fun isJa(c: Configuration): Boolean = !c.locales.isEmpty && c.locales[0].language == "ja"

    /** Current UI language; refreshed in Activity.onCreate (a locale change recreates the activity). */
    @Volatile var ja: Boolean = false
        private set

    fun update(c: Context) { ja = isJa(c.resources.configuration) }
}

/** Usable anywhere in the UI, including plain (non-composable) lambdas. */
fun tr(ja: String, en: String): String = if (I18n.ja) ja else en

fun Context.tr(ja: String, en: String): String = if (I18n.isJa(resources.configuration)) ja else en

/**
 * Refresh rates of the device the app runs on, read from the display at runtime:
 * every supported display mode, each mode's alternative (seamless) rates, and the integer
 * divisors of those that the compositor can render at (e.g. 120 → 60, 40, 30).
 */
object RefreshRates {
    data class Rate(val hz: Float, val native: Boolean)

    fun read(c: Context, minHz: Float = 24f): List<Rate> {
        val dm = c.getSystemService(DisplayManager::class.java)
        val d: Display = dm.getDisplay(Display.DEFAULT_DISPLAY) ?: return emptyList()
        val native = d.supportedModes.flatMap { m ->
            buildList {
                add(m.refreshRate)
                if (Build.VERSION.SDK_INT >= 31) m.alternativeRefreshRates.forEach { add(it) }
            }
        }.map { round1(it) }.toSet()
        val divided = native.flatMap { r -> (2..8).map { round1(r / it) } }.filter { it >= minHz }.toSet()
        return (native.map { Rate(it, true) } + (divided - native).map { Rate(it, false) })
            .sortedByDescending { it.hz }
    }

    private fun round1(x: Float) = Math.round(x * 10f) / 10f

    fun label(hz: Float): String = if (hz == Math.round(hz).toFloat()) "${Math.round(hz)}" else "%.1f".format(java.util.Locale.US, hz)
}

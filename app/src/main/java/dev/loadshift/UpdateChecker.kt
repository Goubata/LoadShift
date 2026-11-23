package dev.loadshift

import android.content.Context
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

/**
 * Checks GitHub for a newer LoadShift release when the app opens.
 * Only the public "latest release" endpoint is queried; nothing about the device is sent.
 */
object UpdateChecker {
    private const val API = "https://api.github.com/repos/Goubata/LoadShift/releases/latest"

    data class Release(val version: String, val url: String, val notes: String)

    fun enabled(c: Context) = c.getSharedPreferences("main", 0).getBoolean("updateCheck", true)
    fun setEnabled(c: Context, on: Boolean) = c.getSharedPreferences("main", 0).edit().putBoolean("updateCheck", on).apply()

    /** Returns the latest release if it is newer than the installed version, else null. Never throws. */
    fun check(c: Context): Release? = runCatching {
        val conn = (URL(API).openConnection() as HttpURLConnection).apply {
            connectTimeout = 5000
            readTimeout = 5000
            setRequestProperty("Accept", "application/vnd.github+json")
            setRequestProperty("User-Agent", "LoadShift")
        }
        try {
            if (conn.responseCode != 200) return null
            val o = JSONObject(conn.inputStream.bufferedReader().readText())
            val tag = o.optString("tag_name").removePrefix("v")
            val installed = c.packageManager.getPackageInfo(c.packageName, 0).versionName ?: return null
            if (tag.isEmpty() || compare(tag, installed) <= 0) return null
            Release(tag, o.optString("html_url"), o.optString("body").lineSequence().take(12).joinToString("\n"))
        } finally {
            conn.disconnect()
        }
    }.getOrNull()

    /** Numeric dotted-version compare: "0.10.0" > "0.9.3". */
    fun compare(a: String, b: String): Int {
        val x = a.split('.', '-').map { it.toIntOrNull() ?: 0 }
        val y = b.split('.', '-').map { it.toIntOrNull() ?: 0 }
        for (i in 0 until maxOf(x.size, y.size)) {
            val d = (x.getOrElse(i) { 0 }).compareTo(y.getOrElse(i) { 0 })
            if (d != 0) return d
        }
        return 0
    }
}

package dev.loadshift

import java.util.concurrent.TimeUnit

/** Minimal root shell helper (KernelSU / Magisk `su`). */
object Root {
    data class Result(val ok: Boolean, val out: String)

    fun run(cmd: String, timeoutSec: Long = 15): Result = try {
        val p = ProcessBuilder("su", "-c", cmd).redirectErrorStream(true).start()
        val text = p.inputStream.bufferedReader().readText()
        if (!p.waitFor(timeoutSec, TimeUnit.SECONDS)) {
            p.destroy(); Result(false, "timeout")
        } else Result(p.exitValue() == 0, text.trim())
    } catch (e: Exception) {
        Result(false, e.message ?: "su failed")
    }

    fun out(cmd: String): String? = run(cmd).let { if (it.ok) it.out else null }

    fun available(): Boolean = out("id")?.contains("uid=0") == true

    /** Shell-quote a value for single-quoted use. */
    fun q(s: String) = "'" + s.replace("'", "'\\''") + "'"
}

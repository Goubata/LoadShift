package dev.loadshift

import android.content.Context

/**
 * All system-side knobs. Everything dynamic is saved before it is touched and restored when the
 * target app leaves the foreground (also at service start, in case the process was killed while
 * boosted).
 */
object Tuner {
    private const val KGSL = "/sys/class/kgsl/kgsl-3d0"
    private const val TOPAPP = "/dev/cpuctl/top-app"

    data class Device(
        val root: Boolean,
        val gpuModel: String = "",
        val numPwrLevels: Int = 0,
        val maxPwrLevel: Int = 0,
        val uclamp: Boolean = false,
        val clusters: List<Pair<String, Long>> = emptyList(), // related_cpus -> max kHz
        val bigMask: String = "",
        val bigCpus: String = "",
        val allMask: String = "ff",
        val angle: Boolean = false,
        val hwuiRenderer: String = "",
    ) {
        val kgsl get() = numPwrLevels > 0
    }

    @Volatile var device: Device = Device(root = false)
        private set

    fun probe(): Device {
        val script = """
            echo "model=$(cat $KGSL/gpu_model 2>/dev/null)"
            echo "num=$(cat $KGSL/num_pwrlevels 2>/dev/null)"
            echo "max=$(cat $KGSL/max_pwrlevel 2>/dev/null)"
            [ -f $TOPAPP/cpu.uclamp.min ] && echo "uclamp=1"
            for d in /sys/devices/system/cpu/cpufreq/policy*; do
              echo "pol=$(cat ${'$'}d/related_cpus):$(cat ${'$'}d/cpuinfo_max_freq)"
            done
            if [ -e /system/lib64/libEGL_angle.so ] || [ -e /vendor/lib64/egl/libEGL_angle.so ] || pm path com.android.angle >/dev/null 2>&1 || [ -n "$(getprop ro.gfx.angle.supported)" ]; then echo "angle=1"; fi
            echo "hwui=$(getprop debug.hwui.renderer)"
            id
        """.trimIndent()
        val r = Root.run(script)
        if (!r.out.contains("uid=0")) return Device(root = false).also { device = it }
        val kv = r.out.lines()
        fun v(k: String) = kv.firstOrNull { it.startsWith("$k=") }?.substringAfter("=")?.trim() ?: ""
        val pols = kv.filter { it.startsWith("pol=") }.mapNotNull {
            val s = it.removePrefix("pol=")
            val cpus = s.substringBeforeLast(":").trim()
            val f = s.substringAfterLast(":").trim().toLongOrNull() ?: return@mapNotNull null
            cpus to f
        }.sortedBy { it.second }
        // Big cores: everything except the slowest cluster (or just the fastest one when there are two).
        val big = when {
            pols.size >= 3 -> pols.drop(1)
            pols.isNotEmpty() -> listOf(pols.last())
            else -> emptyList()
        }
        val cpus = big.flatMap { p -> p.first.split(" ").mapNotNull { it.toIntOrNull() } }.sorted()
        val mask = cpus.fold(0L) { m, c -> m or (1L shl c) }
        return Device(
            root = true,
            gpuModel = v("model"),
            numPwrLevels = v("num").toIntOrNull() ?: 0,
            maxPwrLevel = v("max").toIntOrNull() ?: 0,
            uclamp = v("uclamp") == "1",
            clusters = pols,
            bigMask = if (mask != 0L) java.lang.Long.toHexString(mask) else "",
            bigCpus = cpus.joinToString(","),
            allMask = pols.flatMap { p -> p.first.split(" ").mapNotNull { it.toIntOrNull() } }
                .fold(0L) { m, c -> m or (1L shl c) }.let { if (it == 0L) "ff" else java.lang.Long.toHexString(it) },
            angle = v("angle") == "1",
            hwuiRenderer = v("hwui"),
        ).also { device = it }
    }

    // ---- dynamic knobs ------------------------------------------------------------------------

    private fun saved(c: Context) = c.getSharedPreferences("saved", Context.MODE_PRIVATE)

    fun gpuLevelFor(floor: Int, d: Device = device): Int? {
        if (!d.kgsl || floor <= 0) return null
        val slowest = d.numPwrLevels - 1
        return when (floor) {
            1 -> d.maxPwrLevel + (slowest - d.maxPwrLevel) / 2
            2 -> d.maxPwrLevel + (slowest - d.maxPwrLevel) / 4
            else -> d.maxPwrLevel
        }
    }

    fun uclampFor(level: Int) = when (level) { 1 -> 70; 2 -> 85; 3 -> 100; else -> 0 }

    /** Apply the dynamic part of [p]. Returns a log line. */
    fun enter(c: Context, p: Profile): String {
        val d = device
        val sp = saved(c)
        val sb = StringBuilder()
        // Save originals once (only if nothing is saved yet = we are not already boosted).
        if (!sp.getBoolean("active", false)) {
            val cur = Root.out("cat $KGSL/min_pwrlevel 2>/dev/null; echo; cat $TOPAPP/cpu.uclamp.min 2>/dev/null")
                ?.lines()?.map { it.trim() } ?: emptyList()
            sp.edit()
                .putString("min_pwrlevel", cur.getOrNull(0) ?: "")
                .putString("uclamp_min", cur.getOrNull(1) ?: "")
                .putBoolean("active", true)
                .commit()
        }
        val cmds = mutableListOf<String>()
        gpuLevelFor(p.gpuFloor, d)?.let {
            cmds += "echo $it > $KGSL/min_pwrlevel"
            sb.append("GPU floor L$it ")
        }
        // Only ever raise top-app uclamp.min (this ROM already ships 60).
        val baseClamp = sp.getString("uclamp_min", "")?.substringBefore('.')?.toIntOrNull() ?: 0
        if (d.uclamp && p.cpuBoost > 0 && uclampFor(p.cpuBoost) > baseClamp) {
            cmds += "echo ${uclampFor(p.cpuBoost)} > $TOPAPP/cpu.uclamp.min"
            sb.append("uclamp ${uclampFor(p.cpuBoost)}% ")
        }
        if (cmds.isNotEmpty()) Root.run(cmds.joinToString("; "))
        if (p.pinThreads) sb.append("pinned ${pin(p.pkg)} thr")
        return sb.toString().trim()
    }

    private const val HOT_THREADS =
        "RenderThread*|UnityMain|UnityGfx*|UnityChoreograp*|GameThread|RHIThread|GLThread*|hwuiTask*|RenderThread|MainThread-UE4"

    /** Pin render/game threads (and the main thread) of [pkg] to the big cores. Returns count. */
    fun pin(pkg: String, mask: String = device.bigMask): Int {
        if (mask.isEmpty()) return 0
        val script = """
            n=0
            for p in $(pidof ${Root.q(pkg)}); do
              for t in /proc/${'$'}p/task/*; do
                tid=${'$'}{t##*/}
                c=$(cat ${'$'}t/comm 2>/dev/null)
                if [ "${'$'}tid" = "${'$'}p" ]; then hit=1; else case "${'$'}c" in $HOT_THREADS) hit=1;; *) hit=0;; esac; fi
                if [ ${'$'}hit = 1 ]; then taskset -p $mask ${'$'}tid >/dev/null 2>&1 && n=$((n+1)); fi
              done
            done
            echo ${'$'}n
        """.trimIndent()
        return Root.out(script)?.lines()?.lastOrNull()?.trim()?.toIntOrNull() ?: 0
    }

    private fun unpin(pkg: String) {
        Root.run(
            "for p in $(pidof ${Root.q(pkg)}); do for t in /proc/\$p/task/*; do " +
                "taskset -p ${device.allMask} \${t##*/} >/dev/null 2>&1; done; done"
        )
    }

    /** Restore everything we changed. Safe to call any time. */
    fun leave(c: Context, pkg: String?) {
        val sp = saved(c)
        if (sp.getBoolean("active", false)) {
            val cmds = mutableListOf<String>()
            sp.getString("min_pwrlevel", "")?.takeIf { it.isNotEmpty() }?.let { cmds += "echo $it > $KGSL/min_pwrlevel" }
            sp.getString("uclamp_min", "")?.takeIf { it.isNotEmpty() }?.let {
                cmds += "echo ${it.substringBefore('.').ifEmpty { "0" }} > $TOPAPP/cpu.uclamp.min"
            }
            if (cmds.isNotEmpty()) Root.run(cmds.joinToString("; "))
            sp.edit().putBoolean("active", false).commit()
        }
        if (pkg != null) unpin(pkg)
    }

    // ---- static knobs -------------------------------------------------------------------------

    /** Write ANGLE selection + Game Mode interventions for [p]. Returns command output. */
    fun applyStatic(p: Profile): String {
        val log = StringBuilder()
        // ANGLE per-app selection (Android global settings, read at app process start).
        val pk = Root.out("settings get global angle_gl_driver_selection_pkgs") ?: ""
        val vl = Root.out("settings get global angle_gl_driver_selection_values") ?: ""
        fun list(s: String) = if (s.isBlank() || s == "null") mutableListOf() else s.split(",").map { it.trim() }.toMutableList()
        val pkgs = list(pk); val vals = list(vl)
        while (vals.size < pkgs.size) vals += "default"
        val i = pkgs.indexOf(p.pkg)
        if (i >= 0) { pkgs.removeAt(i); vals.removeAt(i) }
        if (p.enabled && p.glDriver != 0) {
            pkgs += p.pkg; vals += if (p.glDriver == 1) "angle" else "native"
        }
        if (pkgs.isEmpty()) {
            Root.run("settings delete global angle_gl_driver_selection_pkgs; settings delete global angle_gl_driver_selection_values")
        } else {
            Root.run(
                "settings put global angle_gl_driver_selection_pkgs ${Root.q(pkgs.joinToString(","))}; " +
                    "settings put global angle_gl_driver_selection_values ${Root.q(vals.joinToString(","))}"
            )
        }
        log.append("GL: ").append(listOf("default", "ANGLE", "native")[p.glDriver]).append('\n')

        // Game Mode interventions.
        val q = Root.q(p.pkg)
        val wantGm = p.enabled && (p.gameMode != 0 || p.downscale < 0.99f || p.fps > 0)
        if (!wantGm) {
            log.append(Root.run("cmd game reset $q").out.ifBlank { "game: reset" })
        } else if (p.downscale < 0.99f || p.fps > 0) {
            // Interventions live in the CUSTOM mode; `cmd game set` selects it. A later
            // `cmd game mode` would switch away and drop them, so it is not issued here.
            val args = buildString {
                append("--downscale ").append(if (p.downscale < 0.99f) "%.2f".format(java.util.Locale.US, p.downscale) else "disable")
                append(" --fps ").append(if (p.fps > 0) Math.round(p.fps).toString() else "0")
            }
            val r1 = Root.run("cmd game set $args $q")
            log.append("game set $args: ").append(r1.out.ifBlank { if (r1.ok) "ok" else "failed" })
        } else {
            val name = when (p.gameMode) { 1 -> "standard"; 3 -> "battery"; else -> "performance" }
            val r2 = Root.run("cmd game mode $name $q")
            log.append("game mode $name: ").append(r2.out.ifBlank { if (r2.ok) "ok" else "failed" })
        }
        return log.toString().trim()
    }

    // ---- in-game hook (Zygisk module) ------------------------------------------------------

    const val PROFILE_DIR = "/data/adb/loadshift/profiles"

    fun moduleState(): String = Root.out(
        "if [ -f /data/adb/modules/loadshift/remove ]; then echo removing; " +
            "elif [ -f /data/adb/modules/loadshift/disable ]; then echo disabled; " +
            "elif [ -d /data/adb/modules/loadshift ]; then echo installed; " +
            "elif [ -d /data/adb/modules_update/loadshift ]; then echo reboot; else echo missing; fi"
    ) ?: "unknown"

    /** Writes (or removes) the per-app config read by the Zygisk companion. */
    fun writeHookConfig(p: Profile): Boolean {
        val f = "$PROFILE_DIR/${p.pkg}.conf"
        if (!p.enabled || !p.anyHook) return Root.run("rm -f ${Root.q(f)}").ok
        val text = buildString {
            append("enabled=1\n")
            append("gl_async_finish=${if (p.hookFinish) 1 else 0}\n")
            append("gl_async_finish_depth=${p.finishDepth}\n")
            append("gl_pbo_upload=${if (p.hookPbo) 2 else 0}\n")
            append("gl_pbo_min_kb=${p.texMinKB}\n")
            append("gl_buffer_offload=${if (p.hookGlBuf) 1 else 0}\n")
            append("gl_buffer_min=${p.bufMin}\n")
            append("gl_readback_async=${if (p.hookReadback) 1 else 0}\n")
            append("gl_flush_coalesce_us=${p.flushUs}\n")
            append("gl_buf_ring_mb=${p.bufRingMB}\n")
            append("gl_tex_ring_mb=${p.texRingMB}\n")
            append("copy_threads=${p.copyThreads}\n")
            append("copy_threshold_kb=${p.copyThresholdKB}\n")
            append("occlusion_nowait=${if (p.hookOccl) 1 else 0}\n")
            append("vk_pipeline_cache=${if (p.hookVkCache) 1 else 0}\n")
            append("vk_lazy_idle=${if (p.hookVkIdle) 1 else 0}\n")
            append("vk_idle_level=${p.vkIdleLevel}\n")
            append("vk_idle_depth=${p.vkIdleDepth}\n")
            append("compat_install=${p.compatInstall}\n")
            append("compat_timeout_ms=${p.compatTimeoutMs}\n")
            append("compat_exempt=${if (p.compatExempt) 1 else 0}\n")
        }
        return Root.run("mkdir -p $PROFILE_DIR && chmod 700 /data/adb/loadshift $PROFILE_DIR && printf %s ${Root.q(text)} > ${Root.q(f)} && chmod 600 ${Root.q(f)}").ok
    }

    /** Other in-process graphics hook modules that are installed and enabled. */
    fun foreignModules(): List<String> = Root.out(
        "for m in lybfghook portable_gamefx; do d=/data/adb/modules/\$m; " +
            "[ -d \$d ] && [ ! -f \$d/disable ] && [ ! -f \$d/remove ] && echo \$m; done"
    )?.lines()?.map { it.trim() }?.filter { it.isNotEmpty() } ?: emptyList()

    fun removeHookConfig(pkg: String) = Root.run("rm -f ${Root.q("$PROFILE_DIR/$pkg.conf")}")

    /** Stats written by the in-game hook, or null if it never ran in that app. */
    fun hookStats(pkg: String): Map<String, String>? {
        val o = Root.out("cat ${Root.q("/data/data/$pkg/cache/loadshift/stats.txt")} 2>/dev/null") ?: return null
        if (o.isBlank()) return null
        return o.lines().mapNotNull { l -> l.indexOf('=').takeIf { it > 0 }?.let { l.substring(0, it) to l.substring(it + 1) } }.toMap()
    }

    fun clearVkCache(pkg: String) = Root.run("rm -f ${Root.q("/data/data/$pkg/cache/loadshift/vk_pipeline.bin")}")

    fun forceStop(pkg: String) = Root.run("am force-stop ${Root.q(pkg)}")

    /** Live GPU stats: busy %, current MHz. */
    fun gpuStats(): Pair<Int, Int>? {
        val o = Root.out("cat $KGSL/gpu_busy_percentage 2>/dev/null; echo; cat $KGSL/gpuclk 2>/dev/null") ?: return null
        val l = o.lines().map { it.trim() }.filter { it.isNotEmpty() }
        val busy = l.getOrNull(0)?.filter { it.isDigit() }?.toIntOrNull() ?: return null
        val clk = (l.getOrNull(1)?.toLongOrNull() ?: 0L) / 1_000_000
        return busy to clk.toInt()
    }
}

package dev.loadshift

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.app.usage.UsageEvents
import android.app.usage.UsageStatsManager
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Watches which app is in front (UsageStats, granted to ourselves via root appops) and applies
 * that app's profile while it stays in front.
 */
class ShiftService : Service() {

    data class Status(val running: Boolean = false, val active: String? = null, val detail: String = "")

    companion object {
        private const val CH = "loadshift"
        private const val NID = 1
        private val _status = MutableStateFlow(Status())
        val status: StateFlow<Status> = _status

        fun start(c: Context) = c.startForegroundService(Intent(c, ShiftService::class.java))
        fun stop(c: Context) = c.stopService(Intent(c, ShiftService::class.java))
        /** Ask a running service to re-read profiles (after the user edits one). */
        fun reload(c: Context) {
            if (_status.value.running) c.startService(Intent(c, ShiftService::class.java).setAction("reload"))
        }
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var active: String? = null
    @Volatile private var reloadRequested = false
    private var lastPkg: String? = null
    private var lastQuery = 0L

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CH, "LoadShift", NotificationManager.IMPORTANCE_LOW))
        startForeground(NID, notif(tr("待機中", "Idle")), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        _status.value = Status(running = true)
        scope.launch { loop() }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == "reload") reloadRequested = true
        return START_STICKY
    }

    private suspend fun loop() {
        Root.run("appops set $packageName GET_USAGE_STATS allow")
        Tuner.probe()
        Tuner.leave(this, null) // restore anything left from a killed previous run
        var tick = 0
        while (scope.isActive) {
            val fg = foreground()
            val profile = fg?.let { ProfileStore.get(this, it) }?.takeIf { it.enabled }
            val want = profile?.pkg
            if (want != active || reloadRequested) {
                reloadRequested = false
                active?.let { Tuner.leave(this, it) }
                active = want
                if (profile != null) {
                    val d = Tuner.enter(this, profile)
                    publish(profile.pkg, d)
                } else publish(null, "")
                tick = 0
            } else if (active != null && ++tick % 5 == 0) {
                // New threads appear while games load; keep them pinned.
                ProfileStore.get(this, active!!)?.takeIf { it.pinThreads }?.let { Tuner.pin(it.pkg) }
            }
            delay(1000)
        }
    }

    private fun foreground(): String? {
        val usm = getSystemService(UsageStatsManager::class.java)
        val now = System.currentTimeMillis()
        val from = if (lastQuery == 0L) now - 10 * 60_000 else lastQuery - 2000
        val ev = usm.queryEvents(from, now)
        val e = UsageEvents.Event()
        while (ev.hasNextEvent()) {
            ev.getNextEvent(e)
            if (e.eventType == UsageEvents.Event.ACTIVITY_RESUMED) lastPkg = e.packageName
        }
        lastQuery = now
        return lastPkg
    }

    private fun publish(pkg: String?, detail: String) {
        _status.value = Status(running = true, active = pkg, detail = detail)
        val label = pkg?.let {
            runCatching { packageManager.getApplicationLabel(packageManager.getApplicationInfo(it, 0)).toString() }
                .getOrDefault(it)
        }
        getSystemService(NotificationManager::class.java)
            .notify(NID, notif(if (label != null) tr("適用中: ", "Active: ") + "$label  $detail" else tr("待機中", "Idle")))
    }

    private fun notif(text: String): Notification {
        val pi = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CH)
            .setSmallIcon(android.R.drawable.ic_menu_manage)
            .setContentTitle("LoadShift")
            .setContentText(text)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
    }

    override fun onDestroy() {
        scope.cancel()
        val a = active
        // Restore synchronously-ish on a plain thread (scope is cancelled).
        Thread { Tuner.leave(applicationContext, a) }.start()
        _status.value = Status(running = false)
        super.onDestroy()
    }
}

package dev.loadshift

import android.Manifest
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.graphics.Bitmap
import android.graphics.Canvas
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class AppEntry(val pkg: String, val label: String, val icon: ImageBitmap?, val isGame: Boolean)

private val Muted = Color(0xFF9AA0A6)
private val CardBg = Color(0xFF1A1C21)
private val Warn = Color(0xFFF2B36B)

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        I18n.update(this)
        enableEdgeToEdge()
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED)
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme(primary = Color(0xFF7FD4A8), secondary = Color(0xFF8AB4F8))) {
                Surface(Modifier.fillMaxSize(), color = Color(0xFF0E0F12)) { App() }
            }
        }
    }
}

private fun loadApps(ctx: android.content.Context): List<AppEntry> {
    val pm = ctx.packageManager
    val launch = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
    return pm.queryIntentActivities(launch, 0).map { it.activityInfo.applicationInfo }
        .distinctBy { it.packageName }
        .filter { it.packageName != ctx.packageName }
        .map { ai ->
            val d = runCatching { pm.getApplicationIcon(ai) }.getOrNull()
            val bmp = d?.let {
                val b = Bitmap.createBitmap(96, 96, Bitmap.Config.ARGB_8888)
                it.setBounds(0, 0, 96, 96); it.draw(Canvas(b)); b.asImageBitmap()
            }
            AppEntry(ai.packageName, pm.getApplicationLabel(ai).toString(), bmp, ai.category == ApplicationInfo.CATEGORY_GAME)
        }
        .sortedWith(compareBy({ !it.isGame }, { it.label.lowercase() }))
}

@Composable
private fun App() {
    val ctx = LocalContext.current
    var device by remember { mutableStateOf<Tuner.Device?>(null) }
    var module by remember { mutableStateOf("…") }
    var foreign by remember { mutableStateOf<List<String>>(emptyList()) }
    var update by remember { mutableStateOf<UpdateChecker.Release?>(null) }
    var updateCheck by remember { mutableStateOf(UpdateChecker.enabled(ctx)) }
    var apps by remember { mutableStateOf<List<AppEntry>>(emptyList()) }
    var profiles by remember { mutableStateOf(ProfileStore.all(ctx)) }
    var editing by remember { mutableStateOf<AppEntry?>(null) }
    var query by remember { mutableStateOf("") }
    var gpu by remember { mutableStateOf<Pair<Int, Int>?>(null) }
    val status by ShiftService.status.collectAsState()
    var serviceOn by remember { mutableStateOf(ProfileStore.serviceEnabled(ctx)) }

    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            device = Tuner.probe()
            module = Tuner.moduleState()
            foreign = Tuner.foreignModules()
            apps = loadApps(ctx)
        }
        if (serviceOn && !status.running) ShiftService.start(ctx)
    }
    LaunchedEffect(updateCheck) {
        if (updateCheck) update = withContext(Dispatchers.IO) { UpdateChecker.check(ctx) }
    }
    LaunchedEffect(Unit) {
        while (true) { gpu = withContext(Dispatchers.IO) { Tuner.gpuStats() }; delay(1000) }
    }

    editing?.let { app ->
        BackHandler { editing = null }
        Editor(app, profiles[app.pkg], device, onClose = { editing = null; profiles = ProfileStore.all(ctx) })
        return
    }

    Column(Modifier.fillMaxSize().statusBarsPadding().padding(horizontal = 16.dp)) {
        Spacer(Modifier.height(12.dp))
        Text("LoadShift", fontSize = 26.sp, fontWeight = FontWeight.Bold, color = Color.White)
        Text(tr("選んだアプリの CPU 負荷を GPU に委託して FPS を稼ぐ", "Offload CPU work to the GPU in selected apps for higher FPS"),
            fontSize = 13.sp, color = Muted)
        Spacer(Modifier.height(12.dp))
        update?.let { u ->
            Card(Modifier.fillMaxWidth().padding(bottom = 10.dp), colors = CardDefaults.cardColors(containerColor = Color(0xFF1E3A2C))) {
                Column(Modifier.padding(14.dp)) {
                    Text(tr("新しいバージョン v${u.version} が公開されています", "New version v${u.version} is available"),
                        color = Color.White, fontWeight = FontWeight.SemiBold)
                    if (u.notes.isNotBlank()) Text(u.notes.take(400), fontSize = 12.sp, color = Color(0xFFBDC1C6), maxLines = 6)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button({ runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, android.net.Uri.parse(u.url))) } }) {
                            Text(tr("GitHub で開く", "Open on GitHub"))
                        }
                        TextButton({ update = null }) { Text(tr("閉じる", "Dismiss")) }
                    }
                }
            }
        }

        Card(colors = CardDefaults.cardColors(containerColor = CardBg)) {
            Column(Modifier.padding(14.dp)) {
                val d = device
                StatusRow("root", if (d == null) tr("確認中…", "Checking…") else if (d.root) "OK"
                    else tr("なし（KernelSU で許可してください）", "Not granted (allow it in KernelSU)"), d?.root == true)
                StatusRow(tr("フックモジュール", "Hook module"), when (module) {
                    "installed" -> tr("有効", "Active")
                    "reboot" -> tr("再起動待ち", "Reboot pending")
                    "disabled" -> tr("無効化中", "Disabled")
                    "missing" -> tr("未導入（/sdcard/Download/LoadShift/ の ZIP を導入）", "Not installed (flash the ZIP in /sdcard/Download/LoadShift/)")
                    else -> module
                }, module == "installed")
                if (foreign.isNotEmpty())
                    StatusRow(tr("他のフック", "Other hooks"), foreign.joinToString(", ") + tr("（共存モード）", " (coexistence mode)"), true)
                if (d?.root == true) {
                    StatusRow("GPU", "${d.gpuModel}  ${gpu?.let { "${it.first}% / ${it.second} MHz" } ?: ""}", true)
                    StatusRow(tr("高速コア", "Big cores"), "cpu${d.bigCpus}", true)
                }
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(tr("システム制御サービス", "System control service"), color = Color.White, fontWeight = FontWeight.Medium)
                        Text(
                            if (!status.running) tr("停止中", "Stopped")
                            else status.active?.let { a -> tr("適用中: ", "Active: ") + "${apps.firstOrNull { it.pkg == a }?.label ?: a}  ${status.detail}" }
                                ?: tr("待機中", "Idle"),
                            fontSize = 12.sp, color = Muted
                        )
                    }
                    Switch(serviceOn, onCheckedChange = {
                        serviceOn = it
                        ProfileStore.setServiceEnabled(ctx, it)
                        if (it) ShiftService.start(ctx) else ShiftService.stop(ctx)
                    })
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(tr("起動時に更新を確認", "Check for updates on launch"), Modifier.weight(1f), color = Color.White, fontSize = 14.sp)
                    Switch(updateCheck, { updateCheck = it; UpdateChecker.setEnabled(ctx, it); if (!it) update = null })
                }
                HelpBlock(HelpText.main())
            }
        }
        Spacer(Modifier.height(12.dp))
        OutlinedTextField(query, { query = it }, Modifier.fillMaxWidth(), singleLine = true,
            placeholder = { Text(tr("アプリを検索", "Search apps")) })
        Spacer(Modifier.height(8.dp))
        val list = apps.filter { query.isBlank() || it.label.contains(query, true) || it.pkg.contains(query, true) }
            .sortedBy { if (profiles[it.pkg]?.enabled == true) 0 else 1 }
        if (apps.isEmpty()) Box(Modifier.fillMaxWidth().padding(24.dp), Alignment.Center) { CircularProgressIndicator() }
        LazyColumn(Modifier.weight(1f), contentPadding = PaddingValues(bottom = 24.dp)) {
            items(list, key = { it.pkg }) { app ->
                val p = profiles[app.pkg]
                Row(
                    Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).clickable { editing = app }.padding(vertical = 8.dp, horizontal = 4.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    app.icon?.let { Image(it, null, Modifier.size(40.dp)) } ?: Spacer(Modifier.size(40.dp))
                    Spacer(Modifier.width(12.dp))
                    Column(Modifier.weight(1f)) {
                        Text(app.label, color = Color.White, maxLines = 1)
                        Text(app.pkg, fontSize = 11.sp, color = Color(0xFF7D838A), maxLines = 1)
                    }
                    if (app.isGame) Tag("GAME", Color(0xFF8AB4F8))
                    if (p?.enabled == true) { Spacer(Modifier.width(6.dp)); Tag("ON", Color(0xFF7FD4A8)) }
                }
            }
        }
    }
}

@Composable
private fun StatusRow(k: String, v: String, ok: Boolean) {
    Row(Modifier.padding(vertical = 2.dp)) {
        Text(k, Modifier.width(120.dp), fontSize = 13.sp, color = Muted)
        Text(v, fontSize = 13.sp, color = if (ok) Color.White else Color(0xFFF28B82))
    }
}

@Composable
private fun Tag(t: String, c: Color) {
    Text(t, Modifier.background(c.copy(alpha = 0.18f), RoundedCornerShape(6.dp)).padding(horizontal = 6.dp, vertical = 2.dp),
        fontSize = 10.sp, color = c, fontWeight = FontWeight.Bold)
}

@Composable
private fun Section(title: String, note: String? = null, help: List<HelpText.Item>? = null, content: @Composable ColumnScope.() -> Unit) {
    Card(Modifier.fillMaxWidth().padding(vertical = 6.dp), colors = CardDefaults.cardColors(containerColor = CardBg)) {
        Column(Modifier.padding(14.dp)) {
            Text(title, color = Color.White, fontWeight = FontWeight.SemiBold)
            note?.let { Text(it, fontSize = 12.sp, color = Muted) }
            Spacer(Modifier.height(6.dp))
            content()
            help?.let { HelpBlock(it) }
        }
    }
}

/** Collapsible detailed explanation of every item in a section. */
@Composable
private fun HelpBlock(items: List<HelpText.Item>) {
    var open by remember { mutableStateOf(false) }
    TextButton({ open = !open }, contentPadding = PaddingValues(0.dp)) {
        Text(if (open) tr("説明を閉じる", "Hide explanations") else tr("ⓘ 各項目の説明を表示", "ⓘ Show explanations"), fontSize = 13.sp)
    }
    if (open) {
        Column(Modifier.fillMaxWidth().background(Color(0xFF23262D), RoundedCornerShape(8.dp)).padding(10.dp)) {
            items.forEachIndexed { idx, it ->
                if (idx > 0) Spacer(Modifier.height(8.dp))
                Text(it.title, color = Color(0xFF7FD4A8), fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                Text(it.body, color = Color(0xFFD5D8DC), fontSize = 12.sp, lineHeight = 17.sp)
            }
        }
    }
}

@Composable
private fun Toggle(title: String, desc: String, value: Boolean, warn: Boolean = false, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(title, color = Color.White, fontSize = 14.sp)
                if (warn) { Spacer(Modifier.width(6.dp)); Tag(tr("攻め", "AGGRESSIVE"), Warn) }
            }
            Text(desc, color = Muted, fontSize = 12.sp)
        }
        Switch(value, onChange)
    }
}

@Composable
private fun Choice(title: String, options: List<String>, selected: Int, onSelect: (Int) -> Unit) {
    Text(title, color = Color.White, fontSize = 14.sp, modifier = Modifier.padding(top = 6.dp))
    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        options.forEachIndexed { i, o ->
            SegmentedButton(selected == i, { onSelect(i) }, SegmentedButtonDefaults.itemShape(i, options.size)) { Text(o, fontSize = 12.sp, maxLines = 1) }
        }
    }
}

/** Chip list for value sets that may be long (refresh rates, sizes). */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun <T> Chips(title: String, values: List<T>, selected: T, label: (T) -> String, note: String? = null, onSelect: (T) -> Unit) {
    Text(title, color = Color.White, fontSize = 14.sp, modifier = Modifier.padding(top = 6.dp))
    note?.let { Text(it, color = Muted, fontSize = 12.sp) }
    FlowRow(Modifier.fillMaxWidth().padding(vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        values.forEach { v -> FilterChip(selected = v == selected, onClick = { onSelect(v) }, label = { Text(label(v), fontSize = 12.sp) }) }
    }
}

@Composable
private fun Editor(app: AppEntry, existing: Profile?, device: Tuner.Device?, onClose: () -> Unit) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var p by remember { mutableStateOf(existing ?: Profile(app.pkg)) }
    var stats by remember { mutableStateOf<Map<String, String>?>(null) }
    var log by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var advanced by remember { mutableStateOf(false) }
    // Read from THIS device's display every time the screen opens.
    val rates = remember { RefreshRates.read(ctx) }
    LaunchedEffect(app.pkg) {
        while (true) { stats = withContext(Dispatchers.IO) { Tuner.hookStats(app.pkg) }; delay(2000) }
    }
    // Any manual change of a hook knob turns the preset into "custom" (-1).
    fun hook(f: Profile.() -> Profile) { p = p.f().copy(preset = -1) }

    fun save(restart: Boolean) {
        busy = true
        scope.launch {
            val out = withContext(Dispatchers.IO) {
                ProfileStore.put(ctx, p)
                val hookOk = Tuner.writeHookConfig(p)
                val st = Tuner.applyStatic(p)
                if (restart) Tuner.forceStop(p.pkg)
                ShiftService.reload(ctx)
                (if (hookOk) ctx.tr("フック設定を書き込みました", "Hook settings written") else ctx.tr("フック設定の書き込みに失敗", "Failed to write hook settings")) +
                    "\n" + st + "\n" +
                    if (restart) ctx.tr("アプリを終了しました。次の起動から反映されます。", "App stopped. Changes apply on its next launch.")
                    else ctx.tr("フックと起動時設定はアプリの次回起動から反映されます。", "Hooks and launch settings apply on the app's next launch.")
            }
            log = out; busy = false
        }
    }

    Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().verticalScroll(rememberScrollState()).padding(16.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            app.icon?.let { Image(it, null, Modifier.size(48.dp)) }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(app.label, fontSize = 20.sp, fontWeight = FontWeight.Bold, color = Color.White)
                Text(app.pkg, fontSize = 12.sp, color = Color(0xFF7D838A))
            }
            Switch(p.enabled, { p = p.copy(enabled = it) })
        }
        Spacer(Modifier.height(8.dp))

        Section(tr("委託の強さ（プリセット）", "Offload strength (preset)"),
            tr("ゲーム内フックの組み合わせを一括で切り替えます。個別に変えると「カスタム」になります",
               "Switches all in-game hooks at once. Changing any single item makes it \"Custom\""), help = HelpText.preset()) {
            val names = listOf(tr("安全", "Safe"), tr("標準", "Standard"), tr("攻め", "Aggressive"), tr("最大", "Max"))
            Choice(if (p.preset < 0) tr("現在: カスタム", "Current: Custom") else tr("現在: ", "Current: ") + names[p.preset],
                names, p.preset) { p = p.withPreset(it) }
            Text(when (p.preset) {
                0 -> tr("確実に安全な委託のみ。オクルージョン即答・読み戻し非同期・Flush 間引きはオフ", "Only provably safe offloads. Occlusion, readback and flush tricks off")
                2 -> tr("1 フレーム遅れの読み戻し・Flush 間引き・記述子更新で待たない Vulkan を追加", "Adds 1-frame-late readback, flush coalescing, Vulkan without descriptor-update waits")
                3 -> tr("攻め＋大きなステージング領域と深い先行。メモリを多めに使います", "Aggressive + large staging rings and deep run-ahead. Uses more memory")
                1 -> tr("実測で安全と確認できた委託をすべて有効", "Every offload verified safe in testing")
                else -> tr("個別設定", "Individually tuned")
            }, fontSize = 12.sp, color = Muted)
        }

        Section(tr("Vulkan — CPU を GPU 待ちから解放", "Vulkan — free the CPU from GPU waits"),
            tr("ドライバの関数テーブルを差し替えます（次回起動から）", "Swaps driver function-table entries (next launch)"), help = HelpText.vulkan()) {
            Toggle(tr("遅延 WaitIdle", "Lazy WaitIdle"),
                tr("vkQueueWaitIdle / vkDeviceWaitIdle で止まらず、本当に必要な瞬間まで待ちを後回し。破棄は GPU 完了後に専用スレッドが代行（実測 104→163fps）",
                   "vkQueueWaitIdle / vkDeviceWaitIdle return at once; the wait happens only where truly needed. Destroys run on a reaper thread after the GPU is done (measured 104→163 fps)"),
                p.hookVkIdle) { v -> hook { copy(hookVkIdle = v) } }
            if (p.hookVkIdle) Choice(tr("待ち合わせの厳しさ", "Synchronization"),
                listOf(tr("安全", "Safe"), tr("攻め（記述子更新で待たない）", "Aggressive (no wait on descriptor updates)")), p.vkIdleLevel - 1) { v -> hook { copy(vkIdleLevel = v + 1) } }
            Toggle(tr("パイプライン永続キャッシュ", "Persistent pipeline cache"),
                tr("シェーダーコンパイル結果を保存・再利用し、CPU のカクつきを削減（実測 20.6→0.06ms/本）",
                   "Stores and reuses compiled pipelines, removing CPU compile hitches (measured 20.6→0.06 ms each)"),
                p.hookVkCache) { v -> hook { copy(hookVkCache = v) } }
            Chips(tr("パイプライン並列コンパイル", "Parallel pipeline compilation"), listOf(0, 2, 4, 6), p.pipeSplit,
                { if (it == 0) tr("オフ", "Off") else tr("${it} スレッド", "$it threads") },
                tr("まとめて作られるシェーダーを複数コアで同時にコンパイル（実測 474→156ms、約 3 倍速）",
                   "Compiles batched pipelines on several cores at once (measured 474→156 ms, ~3x)")) { v -> hook { copy(pipeSplit = v) } }
            Chips(tr("メモリ再利用", "Memory recycling"), listOf(0, 64, 128, 256), p.memPoolMB,
                { if (it == 0) tr("オフ", "Off") else "$it MB" },
                tr("解放したメモリを保持して再利用し、確保とページフォルトの停止をなくす（8MB 確保 274→1µs、初回書き込み 1.6→0.2ms）［攻め］",
                   "Keeps freed memory for reuse, removing allocation and page-fault stalls (8 MB alloc 274→1 µs, first write 1.6→0.2 ms) [aggressive]")) { v -> hook { copy(memPoolMB = v) } }
            Toggle(tr("オクルージョンを待たない", "Don't wait for occlusion"),
                tr("結果が未完了なら「見えている」と即答（表示は欠けない）。GL でも有効（実測 141→248fps）",
                   "Unfinished queries answer \"visible\" at once (nothing disappears). Also for GL (measured 141→248 fps)"),
                p.hookOccl) { v -> hook { copy(hookOccl = v) } }
        }

        Section(tr("OpenGL ES — CPU の仕事を GPU へ", "OpenGL ES — move CPU work to the GPU"),
            tr("ドライバの関数テーブルを差し替えます（次回起動から）", "Swaps driver function-table entries (next launch)"), help = HelpText.gles()) {
            Toggle(tr("バッファ更新を GPU コピー化", "Buffer updates as GPU copies"),
                tr("glBufferSubData / 書き込み Map をステージング＋GPU コピーに置換。使用中バッファでも CPU は止まらない（実測 138→211〜245fps）",
                   "glBufferSubData / write maps become staging + GPU copy; the CPU never stalls on busy buffers (measured 138→211–245 fps)"),
                p.hookGlBuf) { v -> hook { copy(hookGlBuf = v) } }
            Toggle(tr("テクスチャ転送を GPU 変換", "GPU texture conversion"),
                tr("常時マップのリングへ並列コピーし、形式変換・タイル化・転送は GPU が実行（RGBA8 3.2→1.0ms、RGBA4 3.3→0.3ms、ETC2 0.7→0.2ms）",
                   "Parallel copy into a persistent ring; the GPU converts, tiles and copies (RGBA8 3.2→1.0 ms, RGBA4 3.3→0.3 ms, ETC2 0.7→0.2 ms)"),
                p.hookPbo) { v -> hook { copy(hookPbo = v) } }
            Toggle(tr("非同期 glFinish", "Async glFinish"),
                tr("glFinish で GPU の完了を待たずフェンスで先行（実測 66→164fps）。常時読み取りマップを使うアプリでは自動で無効",
                   "glFinish no longer drains the GPU; fences run ahead (measured 66→164 fps). Auto-disabled for apps with persistent read maps"),
                p.hookFinish) { v -> hook { copy(hookFinish = v) } }
            if (p.hookFinish) Choice(tr("先行フレーム数（大きいほど速いが遅延増）", "Frames ahead (more = faster, more latency)"),
                listOf("1", "2", "3"), p.finishDepth - 1) { v -> hook { copy(finishDepth = v + 1) } }
            Toggle(tr("読み戻しを非同期化", "Async readback"),
                tr("毎フレーム同じ glReadPixels を GPU に非同期で読ませ、1 フレーム前の結果を即返す。初回・単発は正確（実測 147→244fps）",
                   "Repeated identical glReadPixels are read asynchronously by the GPU and answered with the previous frame's result. First/one-off reads stay exact (measured 147→244 fps)"),
                p.hookReadback, warn = true) { v -> hook { copy(hookReadback = v) } }
            Chips(tr("glFlush 間引き", "glFlush coalescing"), listOf(0, 500, 1000, 2000, 4000), p.flushUs,
                { if (it == 0) tr("オフ", "Off") else "${it / 1000.0} ms" },
                tr("間隔より頻繁な Flush（1 回 約 20µs のカーネル送信）を省く。単一コンテキストかつフェンスなしの時だけ",
                   "Drops flushes (each ~20 µs kernel submit) closer than the interval. Single-context apps without pending fences only")) { v -> hook { copy(flushUs = v) } }
        }

        Section(tr("詳細設定", "Advanced"), tr("委託の閾値・ステージング容量・並列度", "Thresholds, staging capacity, parallelism"), help = HelpText.advanced()) {
            TextButton({ advanced = !advanced }) { Text(if (advanced) tr("閉じる", "Hide") else tr("表示する", "Show")) }
            if (advanced) {
                Chips(tr("Vulkan 先行マーカー数", "Vulkan markers in flight"), (1..8).toList(), p.vkIdleDepth, { "$it" },
                    tr("この数を超えると WaitIdle が本当に待つ", "WaitIdle really waits beyond this many")) { v -> hook { copy(vkIdleDepth = v) } }
                Chips(tr("バッファ GPU 化の最小サイズ", "Min size for GPU buffer copies"), listOf(256, 512, 1024, 4096, 16384), p.bufMin,
                    { if (it >= 1024) "${it / 1024} KB" else "$it B" }) { v -> hook { copy(bufMin = v) } }
                Chips(tr("テクスチャ GPU 化の最小サイズ", "Min size for GPU texture uploads"), listOf(16, 32, 64, 256, 1024), p.texMinKB,
                    { "$it KB" }) { v -> hook { copy(texMinKB = v) } }
                Chips(tr("バッファ用ステージング", "Buffer staging ring"), listOf(16, 32, 64, 128), p.bufRingMB, { "$it MB" },
                    tr("コンテキストごとに確保", "Allocated per GL context")) { v -> hook { copy(bufRingMB = v) } }
                Chips(tr("テクスチャ用ステージング", "Texture staging ring"), listOf(32, 48, 96, 192), p.texRingMB, { "$it MB" },
                    tr("1 回の転送はこの半分まで", "A single upload may use up to half")) { v -> hook { copy(texRingMB = v) } }
                Chips(tr("並列コピーのスレッド数", "Parallel copy threads"), (1..8).toList(), p.copyThreads, { "$it" },
                    tr("実測では 4 が最速（6 以上は遅くなる）", "4 measured fastest (6+ gets slower)")) { v -> hook { copy(copyThreads = v) } }
                Chips(tr("並列化する最小コピー量", "Min copy size to parallelize"), listOf(512, 1024, 2048, 4096), p.copyThresholdKB,
                    { if (it >= 1024) "${it / 1024} MB" else "$it KB" }) { v -> hook { copy(copyThresholdKB = v) } }
            }
        }

        Section(tr("他のフックとの共存", "Coexistence with other hooks"),
            tr("lybfghook や Portable GameFX など、同じゲームに入る他のモジュールと両立させる設定（次回起動から）",
               "Settings for running alongside other modules in the same game, such as lybfghook or Portable GameFX (next launch)"),
            help = HelpText.coexist()) {
            Choice(tr("導入タイミング", "Install timing"), listOf(tr("自動", "Auto"), tr("早い", "Early"), tr("遅い", "Late")),
                listOf(1, 0, 2).indexOf(p.compatInstall).coerceAtLeast(0)) { v -> hook { copy(compatInstall = listOf(1, 0, 2)[v]) } }
            Chips(tr("他のフックを待つ最大時間", "Max wait for other hooks"), listOf(1000, 3000, 5000, 10000), p.compatTimeoutMs,
                { "${it / 1000} s" }) { v -> hook { copy(compatTimeoutMs = v) } }
            Toggle(tr("他モジュール自身の呼び出しは素通し", "Pass other modules' own calls through"),
                tr("他のフックが自分で行う glFinish・読み戻し・WaitIdle などには LoadShift の処理を適用しません",
                   "LoadShift leaves glFinish, readbacks, WaitIdle etc. issued by other hook modules untouched"),
                p.compatExempt) { v -> hook { copy(compatExempt = v) } }
        }

        Section(tr("システム制御 — 前面にいる間だけ", "System control — only while in front"),
            tr("サービスが ON のとき、このアプリが前面の間だけ適用し、離れたら元に戻します", "When the service is on, applied only while this app is in front and restored afterwards"), help = HelpText.system()) {
            Choice(tr("GPU クロック下限", "GPU clock floor"), listOf(tr("オフ", "Off"), tr("中", "Mid"), tr("高", "High"), tr("最大", "Max")), p.gpuFloor) { p = p.copy(gpuFloor = it) }
            Choice(tr("CPU ブースト (uclamp)", "CPU boost (uclamp)"), listOf(tr("オフ", "Off"), tr("弱", "Low"), tr("強", "High"), tr("最大", "Max")), p.cpuBoost) { p = p.copy(cpuBoost = it) }
            Toggle(tr("描画スレッドを高速コアに固定", "Pin render threads to big cores"),
                tr("RenderThread / UnityGfx / GameThread などを cpu${device?.bigCpus ?: "?"} に固定", "Pins RenderThread / UnityGfx / GameThread etc. to cpu${device?.bigCpus ?: "?"}"),
                p.pinThreads) { p = p.copy(pinThreads = it) }
        }

        Section(tr("起動時設定", "Launch settings"),
            if (app.isGame) null else tr("Game Mode の項目はゲームカテゴリのアプリにのみ有効です", "Game Mode items only work for apps in the game category"), help = HelpText.launch()) {
            Choice(tr("GL ドライバ", "GL driver"), listOf(tr("標準", "Default"), "ANGLE (GL→Vulkan)", tr("ネイティブ", "Native")), p.glDriver) { p = p.copy(glDriver = it) }
            if (app.isGame) {
                Choice("Game Mode", listOf(tr("変更なし", "Unchanged"), tr("標準", "Standard"), tr("性能", "Performance"), tr("省電力", "Battery")), p.gameMode) { p = p.copy(gameMode = it) }
                Text(tr("解像度スケール ", "Resolution scale ") + if (p.downscale >= 0.99f) tr("オフ", "Off") else "%.0f%%".format(p.downscale * 100),
                    color = Color.White, fontSize = 14.sp)
                Slider(p.downscale, { p = p.copy(downscale = (Math.round(it * 20) / 20f).coerceIn(0.3f, 1f)) }, valueRange = 0.3f..1f)
                val fpsValues = listOf(0f) + rates.map { it.hz }
                Chips(tr("FPS 指定", "FPS limit"), fpsValues, if (p.fps in fpsValues) p.fps else 0f,
                    { hz -> if (hz == 0f) tr("オフ", "Off") else RefreshRates.label(hz) + if (rates.firstOrNull { it.hz == hz }?.native == true) " Hz" else "" },
                    tr("この端末のディスプレイから読み取った値（Hz 付き＝表示モード、数字のみ＝分周した描画レート）",
                       "Read from this device's display (with Hz = display mode, number only = divided render rate)")) { p = p.copy(fps = it) }
                if ((p.fps > 0 || p.downscale < 0.99f) && p.gameMode != 0)
                    Text(tr("FPS/解像度を指定すると Game Mode はカスタムになります", "Setting FPS/resolution switches Game Mode to custom"), fontSize = 12.sp, color = Muted)
            }
        }

        Section(tr("フック統計", "Hook stats"), tr("ゲーム内フックがこのアプリで最後に動いたときの記録", "From the last time the in-game hook ran in this app"), help = HelpText.stats()) {
            val s = stats
            if (s == null) Text(tr("まだ記録がありません（モジュール導入後、アプリを起動すると表示されます）", "No data yet (appears after installing the module and launching the app)"),
                fontSize = 12.sp, color = Muted)
            else {
                StatusRow(tr("フック", "Hooks"), s["hooks"] ?: "-", true)
                StatusRow(tr("共存", "Coexistence"), "${s["compat"] ?: "-"} / " + tr("他フック ", "other hook ") +
                    (if (s["foreign_module"] == "1") tr("あり", "yes") else tr("なし", "no")), true)
                StatusRow(tr("検出・譲渡", "Found / yielded"), tr("他フック検出 ", "foreign slots ") + "${s["foreign_slots"] ?: 0} / " +
                    tr("譲った ", "yielded ") + "${s["yielded_slots"] ?: 0} / " + tr("素通し ", "passed through ") + "${s["foreign_calls"] ?: 0}", true)
                StatusRow(tr("ドライバ", "Driver"), s["driver_copies"] ?: "-", s["driver_copies"].let { it == null || it == "vk1/gl1" || it.contains("vk0") || it.contains("gl0") })
                StatusRow(tr("遅延 WaitIdle", "Lazy WaitIdle"), "${s["vk_idle_skipped"] ?: 0} (" + tr("後払い待ち ", "deferred wait ") + "${s["vk_lazy_wait_ms"] ?: 0} ms / " + tr("破棄代行 ", "deferred destroys ") + "${s["vk_deferred_destroys"] ?: 0})", true)
                StatusRow(tr("オクルージョン", "Occlusion"), "GL ${s["occl_gl_answered"] ?: 0} / VK ${s["occl_vk_answered"] ?: 0}", true)
                StatusRow(tr("バッファ GPU 化", "GPU buffer copies"), "${s["gl_buf_sub"] ?: 0}+${s["gl_buf_map"] ?: 0} / ${s["gl_buf_sub_mb"] ?: 0}+${s["gl_buf_map_mb"] ?: 0} MB", true)
                StatusRow(tr("テクスチャ GPU 化", "GPU texture uploads"), "${s["gl_pbo_uploads"]} / ${s["gl_pbo_mb"]} MB (" + tr("圧縮 ", "compressed ") + "${s["gl_tex_compressed"] ?: 0})", true)
                StatusRow(tr("読み戻し", "Readback"), tr("非同期 ", "async ") + "${s["gl_readback_async"] ?: 0} / " + tr("正確 ", "exact ") + "${s["gl_readback_exact"] ?: 0}", true)
                StatusRow("glFlush", tr("省略 ", "dropped ") + "${s["gl_flush_dropped"] ?: 0} / " + tr("実行 ", "issued ") + "${s["gl_flush_real"] ?: 0}", true)
                StatusRow(tr("非同期 Finish", "Async Finish"), "${s["gl_finish_async"]} (" + tr("待ち合計 ", "total wait ") + "${s["gl_finish_wait_ms"]} ms)", true)
                StatusRow("Vulkan", "${s["vk_pipelines"]} " + tr("本 平均 ", "pipelines, avg ") + "${s["vk_pipeline_avg_ms"]} ms", true)
                StatusRow(tr("並列コンパイル", "Parallel compile"), "${s["vk_split_calls"] ?: 0} " + tr("回 / ", "calls / ") + "${s["vk_split_pipelines"] ?: 0} " + tr("本", "pipelines"), true)
                StatusRow(tr("メモリ再利用", "Memory recycling"), tr("再利用 ", "reused ") + "${s["vk_mem_pool_hits"] ?: 0} / " + tr("新規 ", "new ") +
                    "${s["vk_mem_pool_misses"] ?: 0} / " + tr("節約 ", "saved ") + "${s["vk_mem_saved_mb"] ?: 0} MB / " + tr("保持中 ", "held ") + "${s["vk_mem_pool_now_mb"] ?: 0} MB", true)
                StatusRow(tr("キャッシュ", "Cache"), tr("読込 ", "loaded ") + "${s["vk_cache_loaded_kb"] ?: 0} KB / " + tr("保存 ", "saved ") + "${s["vk_cache_saved_kb"] ?: 0} KB", true)
                StatusRow(tr("ゲーム側キャッシュ", "Game's own cache"), "${s["vk_app_caches"] ?: 0} / " + tr("注入 ", "injected ") + "${s["vk_cache_injected"] ?: 0} / " + tr("統合 ", "merged ") + "${s["vk_cache_merges"] ?: 0}", true)
                TextButton({ scope.launch(Dispatchers.IO) { Tuner.clearVkCache(app.pkg) } }) { Text(tr("Vulkan キャッシュを削除", "Clear Vulkan cache")) }
            }
        }

        if (log.isNotBlank()) Text(log, fontSize = 12.sp, color = Color(0xFFBDC1C6), modifier = Modifier.padding(vertical = 8.dp))
        Row(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button({ save(false) }, Modifier.weight(1f), enabled = !busy) { Text(tr("保存", "Save")) }
            Button({ save(true) }, Modifier.weight(1f), enabled = !busy) { Text(tr("保存して再起動", "Save & restart app")) }
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton({
                scope.launch {
                    withContext(Dispatchers.IO) {
                        ProfileStore.remove(ctx, app.pkg)
                        Tuner.removeHookConfig(app.pkg)
                        Tuner.applyStatic(p.copy(enabled = false))
                        ShiftService.reload(ctx)
                    }
                    onClose()
                }
            }, Modifier.weight(1f)) { Text(tr("設定を削除", "Remove settings")) }
            OutlinedButton(onClose, Modifier.weight(1f)) { Text(tr("戻る", "Back")) }
        }
    }
}

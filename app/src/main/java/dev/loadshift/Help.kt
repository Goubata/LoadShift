package dev.loadshift

/**
 * Detailed explanation of every setting (what it does, effect, risk, when to use it).
 * Japanese when the system language is Japanese, English otherwise (see [tr]).
 */
object HelpText {
    data class Item(val title: String, val body: String)

    private fun i(jt: String, et: String, jb: String, eb: String) = Item(tr(jt, et), tr(jb, eb))

    fun main() = listOf(
        i("root", "root",
            "LoadShift は KernelSU/Magisk の root で動きます。設定ファイルの書き込み、GPU/CPU の調整、ゲームの再起動に必要です。「なし」の場合は root 管理アプリで LoadShift を許可してください。",
            "LoadShift needs root (KernelSU/Magisk) to write hook settings, tune GPU/CPU and restart games. If it shows \"Not granted\", allow LoadShift in your root manager."),
        i("フックモジュール", "Hook module",
            "ゲームのプロセス内で CPU の仕事を GPU に回す本体（Zygisk モジュール）です。/sdcard/Download/LoadShift/ の ZIP を root 管理アプリで導入し、再起動すると「有効」になります。未導入でもシステム制御は使えますが、ゲーム内フックは動きません。",
            "The Zygisk module that runs inside the game and moves CPU work to the GPU. Flash the ZIP from /sdcard/Download/LoadShift/ in your root manager and reboot; it then shows \"Active\". Without it only the system controls work, not the in-game hooks."),
        i("GPU / 高速コア", "GPU / Big cores",
            "この端末の GPU 名・現在の使用率とクロック、そして描画スレッドを固定する先の高速 CPU コアです。端末から自動で読み取ります。",
            "This device's GPU, its current load and clock, and the fast CPU cores used for thread pinning. Read automatically from the device."),
        i("起動時に更新を確認", "Check for updates on launch",
            "アプリを開いたとき GitHub（Goubata/LoadShift）の最新リリースを確認し、新しい版があれば上に表示します。送信するのは公開 API への問い合わせだけで、端末の情報は送りません。",
            "When the app opens it asks GitHub (Goubata/LoadShift) for the latest release and shows a notice if a newer one exists. Only the public API is queried; no device information is sent."),
        i("システム制御サービス", "System control service",
            "ON にすると常駐し、設定したアプリが前面にいる間だけ GPU クロック下限・CPU ブースト・スレッド固定を適用し、離れたら元に戻します。ゲーム内フック（モジュール側）はこのサービスが OFF でも動きます。",
            "When on, it stays in the background and applies the GPU clock floor, CPU boost and thread pinning only while a configured app is in front, restoring everything afterwards. The in-game hooks work even when this service is off."),
    )

    fun preset() = listOf(
        i("安全", "Safe",
            "常に結果が変わらないことが確認できた委託だけを使います。オクルージョン即答・読み戻し非同期・Flush 間引きはオフ、先行も浅めです。不具合が出たらまずこれを試してください。",
            "Uses only offloads that never change results. Occlusion answering, async readback and flush coalescing are off and run-ahead is shallow. Try this first if a game misbehaves."),
        i("標準", "Standard",
            "テストで安全と確認できた委託をすべて有効にします（既定）。ほとんどのゲームで効果と安定性のバランスが最も良い設定です。",
            "Enables every offload verified safe in testing (default). The best balance of gain and stability for most games."),
        i("攻め", "Aggressive",
            "標準に加え、1 フレーム遅れの読み戻し・glFlush の間引き・記述子更新で待たない Vulkan を使います。さらに FPS が伸びることがありますが、まれに一瞬の表示の乱れが出る可能性があります。",
            "Standard plus 1-frame-late readback, glFlush coalescing and Vulkan that doesn't wait on descriptor updates. Can add more FPS, with a small chance of brief visual glitches."),
        i("最大", "Max",
            "攻めに加えて、ステージング領域を大きく、先行を深くします。メモリを多く使い（ゲームあたり最大 200MB 程度）、入力遅延がわずかに増えることがあります。",
            "Aggressive plus larger staging rings and deeper run-ahead. Uses more memory (up to ~200 MB per game) and may add slight input latency."),
        i("カスタム", "Custom",
            "プリセット選択後に項目を個別に変えると「カスタム」になります。プリセットを押し直すといつでも戻せます。",
            "Changing any single item after picking a preset makes it \"Custom\". Tap a preset again to return to it."),
    )

    fun vulkan() = listOf(
        i("遅延 WaitIdle", "Lazy WaitIdle",
            "ゲームが vkQueueWaitIdle / vkDeviceWaitIdle で「GPU が全部終わるまで CPU を止める」処理を、目印のフェンスを置いてすぐ戻るように変えます。待ちは、そのコマンドバッファの再利用やメモリの読み取りなど本当に必要な瞬間まで後回しにし、オブジェクト破棄は GPU 完了後に専用スレッドが代行します。CPU のゲーム処理と GPU の描画が重なり FPS が上がります（テスト 104→163fps）。",
            "Games that call vkQueueWaitIdle / vkDeviceWaitIdle stop the CPU until the GPU is completely done. Here the call drops a marker fence and returns at once; the wait is postponed until it is truly needed (reusing that command buffer, reading memory, …) and object destruction is done by a helper thread after the GPU finishes. CPU game logic and GPU rendering overlap, raising FPS (tested 104→163 fps)."),
        i("待ち合わせの厳しさ", "Synchronization",
            "安全: 記述子セットの更新時にも GPU の完了を待ちます。攻め: 記述子更新では待ちません。速くなりますが、更新中の記述子を GPU がまだ使っていると一瞬の表示の乱れが出る可能性があります。",
            "Safe: also waits for the GPU before descriptor-set updates. Aggressive: doesn't wait there. Faster, but if the GPU still uses a descriptor being updated a brief glitch can appear."),
        i("パイプライン永続キャッシュ", "Persistent pipeline cache",
            "シェーダー（パイプライン）のコンパイル結果をアプリのキャッシュフォルダに保存し、次回起動時に再利用します。ゲーム自前のキャッシュ（Unreal/Unity の PSO キャッシュ）の中身も取り込んで保存し、次の起動でそのキャッシュに注入します。初回のカクつき・読み込み中の CPU スパイクが減ります（テスト 17.8→0.06ms/本）。",
            "Saves compiled shaders (pipelines) in the app's cache folder and reuses them next launch. The game's own cache (Unreal/Unity PSO caches) is merged in and saved too, then injected back into that cache on the next launch. Reduces first-run stutter and CPU spikes while loading (tested 17.8→0.06 ms per pipeline)."),
        i("パイプライン並列コンパイル", "Parallel pipeline compilation",
            "ゲームが 1 回の呼び出しで複数のシェーダー（パイプライン）を作ると、ドライバは 1 本ずつ順番にコンパイルします。LoadShift はそれを分割して複数の CPU コアで同時にコンパイルさせ、読み込み時間とカクつきを減らします（48 本 474→156ms）。仕様上分割できない場合（派生パイプライン、外部同期キャッシュなど）は分割しません。",
            "When a game creates several pipelines in one call, the driver compiles them one after another. LoadShift splits the batch so several CPU cores compile at once, cutting load times and hitches (48 pipelines 474→156 ms). Batches that must not be split (derivative pipelines, externally synchronized caches, …) are left alone."),
        i("メモリ再利用", "Memory recycling",
            "GPU メモリの確保はカーネル呼び出しと新しいページのフォルトで CPU を止めます（8MB で約 1.9ms）。解放されたメモリを指定量まで保持し、同じ種類・近いサイズの確保に再利用します（PC の Vulkan メモリアロケータと同じ考え方）。再利用したメモリには以前の内容が残ります（仕様上は問題ありませんが、ゼロを前提にした作りのゲームでは表示が乱れる可能性があるため「攻め」扱い）。数字は保持する上限です。",
            "Allocating GPU memory stops the CPU for a kernel call and fresh page faults (~1.9 ms for 8 MB). Freed memory is kept up to the chosen amount and reused for allocations of the same type and similar size, like a PC Vulkan memory allocator. Reused memory still holds old contents (allowed by the spec, but games that wrongly expect zeroed memory might glitch, so it is marked aggressive). The number is the maximum kept."),
        i("オクルージョンを待たない", "Don't wait for occlusion",
            "ゲームが「この物体は見えているか」を GPU に問い合わせて結果を待つ処理で、結果がまだ出ていなければ「見えている」と即答します。物体が消えることはなく、最悪でも余分に 1 フレーム描くだけです。GL と Vulkan の両方に効きます（テスト 141→248fps）。glFinish 後の問い合わせには正確な結果を返します。",
            "When a game asks the GPU \"is this object visible?\" and waits, unfinished queries are answered \"visible\" immediately. Nothing disappears; at worst one extra object is drawn for a frame. Works for GL and Vulkan (tested 141→248 fps). Queries made after glFinish still get the exact result."),
    )

    fun gles() = listOf(
        i("バッファ更新を GPU コピー化", "Buffer updates as GPU copies",
            "glBufferSubData や書き込み用の glMapBufferRange で、GPU が使用中のバッファを書き換えると、ドライバは CPU を止めるかバッファ全体を CPU でコピーします。代わりに常時マップしたステージング領域へ書き、移動は GPU のコピー（glCopyBufferSubData）で行います。CPU はほぼ止まりません（テスト 138→211〜245fps）。",
            "Writing a buffer the GPU still reads (glBufferSubData, write-only glMapBufferRange) makes the driver stall the CPU or copy the whole buffer on the CPU. Instead data goes into a persistently mapped staging ring and the GPU moves it with glCopyBufferSubData. The CPU almost never stalls (tested 138→211–245 fps)."),
        i("テクスチャ転送を GPU 変換", "GPU texture conversion",
            "テクスチャ転送（glTexImage/glTexSubImage、3D・配列・圧縮形式を含む）で、ドライバが CPU で行う形式変換・タイル化を GPU に任せます。データは事前に確保したリングへ複数コアで並列コピーされます。読み込み時のカクつきが減ります（RGBA8 3.2→1.0ms、RGBA4 3.3→0.3ms、ETC2 0.7→0.2ms）。",
            "For texture uploads (glTexImage/glTexSubImage incl. 3D, arrays and compressed formats) the format conversion and tiling the driver does on the CPU is handed to the GPU. Data is copied in parallel into a pre-allocated ring. Less stutter while loading (RGBA8 3.2→1.0 ms, RGBA4 3.3→0.3 ms, ETC2 0.7→0.2 ms)."),
        i("非同期 glFinish", "Async glFinish",
            "glFinish は「GPU が全部終わるまで CPU を止める」命令です。これをフェンス＋Flush に置き換え、数フレーム前のフェンスだけ待ちます。CPU と GPU が並行して動きます（テスト 66→164fps）。GPU の結果をメモリから直接読むゲームでは自動的に無効になります。",
            "glFinish stops the CPU until the GPU is completely idle. It becomes fence + flush, waiting only on a fence from a few frames back, so CPU and GPU run in parallel (tested 66→164 fps). Automatically disabled for games that read GPU results straight from memory."),
        i("先行フレーム数", "Frames ahead",
            "非同期 glFinish で CPU が GPU より何フレーム先行してよいか。大きいほど並行度が上がり FPS が伸びやすい反面、入力から表示までの遅延がわずかに増えます。",
            "How many frames the CPU may run ahead of the GPU with async glFinish. Higher = more overlap and FPS, but slightly more input-to-display latency."),
        i("読み戻しを非同期化", "Async readback",
            "毎フレーム同じ範囲を glReadPixels で読み戻すゲーム（自動露出・タッチ判定など）で、GPU に非同期で読ませ、1 フレーム前の結果を即座に返します。CPU の待ちがなくなります（テスト 147→244fps）。初回や単発の読み戻し（スクリーンショット等）は常に正確です。結果が 1 フレーム遅れるため「攻め」扱いです。",
            "For games that read the same region with glReadPixels every frame (auto-exposure, touch picking, …) the GPU reads asynchronously and the previous frame's result is returned instantly, removing the CPU wait (tested 147→244 fps). First and one-off reads (screenshots) stay exact. Marked aggressive because results are one frame late."),
        i("glFlush 間引き", "glFlush coalescing",
            "glFlush は呼ぶたびにカーネルへの送信（約 20µs の CPU 時間）が発生します。指定間隔より頻繁な Flush を省きます。GL コンテキストが 1 つでフェンス待ちがない時だけ省くので、複数コンテキスト間の同期は壊しません。",
            "Each glFlush is a kernel submission (~20 µs of CPU). Flushes closer together than the interval are dropped — only when the game has a single GL context and no pending fence, so cross-context synchronization is never broken."),
    )

    fun advanced() = listOf(
        i("Vulkan 先行マーカー数", "Vulkan markers in flight",
            "遅延 WaitIdle で未完了のまま許す目印の数です。これを超えると WaitIdle が本当に待ちます。大きいほど並行度が上がりますが、GPU 処理が溜まりやすくなります。",
            "How many unfinished WaitIdle markers are allowed; beyond this WaitIdle really waits. Higher = more overlap but more queued GPU work."),
        i("バッファ GPU 化の最小サイズ", "Min size for GPU buffer copies",
            "これより小さいバッファ更新はドライバに任せます（小さな更新は命令列に直接埋め込まれ元々速いため）。小さくすると多くの更新を GPU コピー化します。",
            "Smaller buffer updates are left to the driver (tiny updates are embedded in the command stream and already fast). Lower = more updates become GPU copies."),
        i("テクスチャ GPU 化の最小サイズ", "Min size for GPU texture uploads",
            "これより小さいテクスチャ転送はドライバに任せます。小さくすると UI 用の小さなテクスチャも GPU 変換します。",
            "Smaller texture uploads are left to the driver. Lower = even small UI textures use the GPU path."),
        i("バッファ用ステージング", "Buffer staging ring",
            "バッファ更新を一時的に置く領域の大きさ（GL コンテキストごと）。大量に更新するゲームでは大きいほど待ちが起きにくくなりますが、その分メモリを使います。",
            "Size of the area holding pending buffer updates (per GL context). Bigger means fewer waits for update-heavy games, at the cost of memory."),
        i("テクスチャ用ステージング", "Texture staging ring",
            "テクスチャ転送用の領域の大きさ。1 回の転送はこの半分までが GPU 経由になり、超えるとドライバに任せます。高解像度テクスチャを使うゲームでは大きくしてください。",
            "Size of the texture upload area. A single upload up to half of it goes through the GPU path; larger ones fall back to the driver. Increase for games with very large textures."),
        i("並列コピーのスレッド数", "Parallel copy threads",
            "大きなデータをステージング領域へ書き込む時に使う CPU スレッド数。テストでは 4 が最速で、6 以上は逆に遅くなりました。",
            "CPU threads used to write large data into the staging rings. 4 was fastest in testing; 6 or more got slower."),
        i("並列化する最小コピー量", "Min copy size to parallelize",
            "これより小さいコピーは 1 スレッドで行います。小さくすると中くらいのテクスチャも並列コピーになります。",
            "Copies smaller than this use a single thread. Lower = medium textures are copied in parallel too."),
    )

    fun coexist() = listOf(
        i("何が問題だったか", "What the problem was",
            "lybfghook などのフレーム生成モジュールも、LoadShift と同じくゲーム内でグラフィックス関数を横取りします。どちらが先に読み込まれるかは root 環境で変わり、後から入った側が先に入った側の差し替えを見つけられず、片方（または両方）が効かなくなることがありました。",
            "Frame-generation modules such as lybfghook intercept graphics functions inside the game just like LoadShift. Which one loads first depends on the root setup, and the later one could miss the earlier one's changes, so one (or both) stopped working."),
        i("LoadShift の対処", "How LoadShift handles it",
            "差し替える場所をドライバファイルと libGLESv2 の命令列から割り出すので、相手が先に書き換えていても見つけられます。相手のフックが既にある関数は「LoadShift → 相手 → ドライバ」と連結し、呼び出しを完結させてしまう機能（遅延 WaitIdle・非同期 glFinish など）はその関数だけ相手に譲ります。相手のモジュールのコードからの呼び出しは素通しします。相手のファイルには一切触れません。",
            "Slot locations are computed from the driver file and libGLESv2's code, so they are found even if the other module changed them first. Where the other module already hooks a function, calls are chained LoadShift → other module → driver, and features that could complete a call on their own (lazy WaitIdle, async glFinish, …) are left to the other module for that function only. Calls made by the other module's own code pass straight through. The other module's files are never touched."),
        i("導入タイミング", "Install timing",
            "自動: 他のフックモジュールが有効なら、それが差し替えを終えるのを待ってから入ります（無ければすぐ入る）。早い: アプリ起動直後に入ります。遅い: 常に他のフックを待ちます。どちらか片方が効かない時は「早い」と「遅い」を切り替えて試してください。",
            "Auto: if another hook module is active, LoadShift waits until it has finished patching (otherwise it installs at once). Early: installs right at app start. Late: always waits for other hooks. If one of the two still doesn't work, try switching between Early and Late."),
        i("他のフックを待つ最大時間", "Max wait for other hooks",
            "相手の差し替えが見つからない時に待つ上限です。短いほどゲームの起動処理を取りこぼしにくく、長いほど起動の遅い相手にも合わせられます。",
            "How long to wait when the other module's changes are not seen. Shorter catches more of the game's startup; longer accommodates modules that install late."),
        i("他モジュール自身の呼び出しは素通し", "Pass other modules' own calls through",
            "フレーム生成モジュールが自分で行う読み戻しや待ち合わせに、1 フレーム遅れの読み戻しや非同期化を適用しないようにします。通常は ON のままにしてください。",
            "Keeps 1-frame-late readback or async waits away from the frame-generation module's own work. Leave it on unless told otherwise."),
        i("統計の見方", "Reading the stats",
            "共存: early（すぐ導入）/ late:after-other-hook（相手を待ってから導入）/ late:other-hook-already-there（相手が先にいた）/ late:timeout。検出＝相手が既に差し替えていた箇所、譲った＝相手に任せた関数、素通し＝相手のコードからの呼び出し回数。ドライバが vk2 や gl2 の場合、ドライバが複数読み込まれており、報告時に役立ちます。",
            "Coexistence: early / late:after-other-hook / late:other-hook-already-there / late:timeout. Found = slots the other module had already changed, yielded = functions left to it, passed through = calls from its code. Driver vk2 or gl2 means the driver is loaded more than once — useful in bug reports."),
    )

    fun system() = listOf(
        i("GPU クロック下限", "GPU clock floor",
            "アプリが前面の間、GPU の最低クロックを引き上げます。GPU の立ち上がりが速くなり、CPU が GPU を待つ時間が減ります。高くするほど電池消費と発熱が増えます。",
            "Raises the GPU's minimum clock while the app is in front, so the GPU ramps up faster and the CPU waits less. Higher = more battery use and heat."),
        i("CPU ブースト (uclamp)", "CPU boost (uclamp)",
            "前面アプリのスレッドを、スケジューラに「最低これだけの性能が必要」と伝えて高クロック・高速コアに乗りやすくします。端末の既定値より下げることはありません。",
            "Tells the scheduler the foreground app needs at least this much performance, so its threads get higher clocks and fast cores. Never lowers the device's default."),
        i("描画スレッドを高速コアに固定", "Pin render threads to big cores",
            "RenderThread・UnityGfx・GameThread などの描画/メインスレッドを高速コアに固定し、遅いコアへ移されて詰まるのを防ぎます。数秒ごとに新しいスレッドも固定し直します。",
            "Pins render/main threads (RenderThread, UnityGfx, GameThread, …) to the fast cores so they never get stuck on slow cores. New threads are re-pinned every few seconds."),
    )

    fun launch() = listOf(
        i("GL ドライバ", "GL driver",
            "OpenGL ES の実装を選びます。ANGLE は GL を Vulkan に変換して動かします（ゲームによって速くも遅くもなります）。ネイティブは端末の GL ドライバを強制します。次回起動から有効です。",
            "Chooses the OpenGL ES implementation. ANGLE runs GL on top of Vulkan (faster or slower depending on the game). Native forces the device's own GL driver. Applies on next launch."),
        i("Game Mode", "Game Mode",
            "Android 標準のゲームモード（標準・性能・省電力）を切り替えます。ゲームカテゴリのアプリのみ対応です。",
            "Switches Android's built-in game mode (standard, performance, battery). Only for apps in the game category."),
        i("解像度スケール", "Resolution scale",
            "ゲームの描画解像度を下げて GPU 負荷を減らします（例: 70%）。画質は少し下がります。Game Mode のカスタム設定として適用されます。",
            "Renders the game at a lower resolution (e.g. 70%) to cut GPU load. Slightly lower image quality. Applied as a Game Mode custom setting."),
        i("FPS 指定", "FPS limit",
            "この端末のディスプレイから読み取ったリフレッシュレートと、それを整数で割った描画レートから選べます。上限を決めて発熱や電池消費を抑えたり、ディスプレイと揃えて表示を安定させたりできます。",
            "Pick from the refresh rates read from this device's display and their integer divisions. Cap FPS to reduce heat and battery use, or match the display for steadier frame pacing."),
    )

    fun stats() = listOf(
        i("フック統計の見方", "Reading the stats",
            "ゲーム内フックが実際に働いた回数です。数字が増えていれば、その機能がそのゲームで効いています。0 のままの項目は、そのゲームがその処理を使っていない（例: Vulkan のゲームでは GL 側は 0）ことを意味します。",
            "How often each in-game hook actually worked. Growing numbers mean the feature is active in that game. Items staying at 0 mean the game doesn't use that path (e.g. GL items stay 0 in a Vulkan game)."),
        i("フック", "Hooks",
            "どの関数を差し替えたかの一覧です。gles: は OpenGL ES、vk: は Vulkan、+merge はゲーム自前キャッシュの取り込みが有効なことを示します。",
            "Which functions were swapped. gles: = OpenGL ES, vk: = Vulkan, +merge = merging of the game's own pipeline cache is active."),
        i("Vulkan キャッシュを削除", "Clear Vulkan cache",
            "保存したパイプラインキャッシュを消します。ゲームやドライバの更新後に動作がおかしい時に使ってください。次回起動から再び蓄積されます。",
            "Deletes the saved pipeline cache. Use it if something looks wrong after a game or driver update; it rebuilds from the next launch."),
    )
}

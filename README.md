# LoadShift

**日本語** | [English](#english)

選んだ Android アプリ（主にゲーム）の中で、CPU が GPU を待って止まっている時間や、ドライバが CPU で行っている処理を GPU 側へ移し、FPS とカクつきを改善する root 向けツールです。

- **Zygisk モジュール**（`native/`）: ゲームのプロセス内で動き、Adreno ドライバの関数テーブルのポインタを差し替えます。コードを書き換えるインラインフックは使わないため、他のフック系モジュールと同じゲームで共存できます。
- **管理アプリ**（`app/`）: アプリの選択、プリセットと詳細設定、前面アプリだけに効くシステム制御、フック統計の表示。

> 対象: arm64 / Snapdragon（Adreno GPU）/ Android 13 以降 / KernelSU または Magisk + Zygisk。

## 機能

### Vulkan
| 機能 | 内容 | 検証環境での実測 |
|---|---|---|
| 遅延 WaitIdle | `vkQueueWaitIdle` / `vkDeviceWaitIdle` を即座に返し、待ちは本当に必要な時点（コマンドバッファ再利用、メモリ読み取り等）まで後回し。破棄は GPU 完了後に専用スレッドが代行 | 104 → 163 fps |
| パイプライン永続キャッシュ | コンパイル結果を保存して再利用。ゲーム自前のキャッシュ（Unreal / Unity の PSO キャッシュ）も統合・保存し、次回起動時に注入 | 17.8 → 0.06 ms/本 |
| オクルージョンを待たない | 未完了のオクルージョンクエリを「見えている」と即答（物体は消えない） | — |

### OpenGL ES
| 機能 | 内容 | 検証環境での実測 |
|---|---|---|
| バッファ更新の GPU コピー化 | `glBufferSubData` / 書き込み Map を常時マップのステージング＋`glCopyBufferSubData` に置換 | 138 → 211〜245 fps |
| テクスチャ転送の GPU 変換 | 形式変換・タイル化・転送を GPU で実行（3D・配列・圧縮形式含む）、並列コピー | RGBA8 3.2 → 1.0 ms、RGBA4 3.3 → 0.3 ms |
| 非同期 glFinish | フェンス＋Flush に置換し、数フレーム前のフェンスのみ待つ | 66 → 164 fps |
| オクルージョンを待たない | GL 版 | 141 → 248 fps |
| 読み戻しの非同期化（攻め） | 毎フレーム同じ `glReadPixels` を非同期化し、1 フレーム前の結果を返す | 147 → 244 fps |
| glFlush 間引き（攻め） | 単一コンテキストかつフェンスなしの時だけ、頻繁な Flush を省く | 16 µs → 0 /回 |

数値は `native/lab.cpp` の合成ベンチマークによるもので、実際のゲームでの FPS 向上量はゲームにより異なります。

### システム制御（前面にいる間だけ適用・離れたら復元）
GPU クロック下限、CPU ブースト（uclamp、端末既定値より下げない）、描画スレッドの高速コア固定。起動時設定として ANGLE 切替、Game Mode（解像度スケール・端末から読み取ったリフレッシュレートによる FPS 指定）。

### プリセット
安全 / 標準（既定） / 攻め / 最大。各項目の詳しい説明はアプリ内の「ⓘ 各項目の説明を表示」から見られます。UI は端末の言語が日本語なら日本語、それ以外は英語です。

## 既知のリスク
- 遅延 WaitIdle・非同期 glFinish: GPU の結果を API を介さず常時マップメモリから直接読むゲームでは古いデータを読む可能性があります（glFinish は常時読み取りマップを検出すると自動で無効化）。
- 「攻め」以上: 一瞬の表示の乱れが出る可能性があります。不具合時は「安全」を試してください。

## ビルド
必要: Android SDK（platform 37、NDK r29）、JDK 17、clang/lld、python3。

```sh
export ANDROID_HOME=/path/to/sdk JAVA_HOME=/path/to/jdk17
./build-all.sh            # build/LoadShift.apk と build/LoadShift-module.zip
```
環境固有の値は `build.env`（Git 管理外）に書くと自動で読み込まれます。

## 導入
1. `LoadShift-module.zip` を KernelSU / Magisk で導入して再起動
2. `LoadShift.apk` をインストールし、root を許可
3. アプリでゲームを選び「保存して再起動」

設定は `/data/adb/loadshift/profiles/<package>.conf`、統計は `/data/data/<package>/cache/loadshift/stats.txt` に書かれます。

## 検証
`native/lab.cpp` はデバイス上で直接実行するテストです（`lab gles|glbuf|probe|probe2|occlgl|vk|vkidle|occlvk <libloadshift.so> <conf> <dir>`）。各機能の ON/OFF 比較と、結果の正確性（読み戻し比較）を確認します。

---

## English

A root tool that moves work off the CPU in selected Android apps (mainly games): time the CPU spends stalled waiting for the GPU, and work the driver does on the CPU, is shifted to the GPU to raise FPS and reduce stutter.

- **Zygisk module** (`native/`): runs inside the game and swaps pointers in the Adreno driver's function tables. No inline code patching, so it coexists with other hooking modules in the same game.
- **Manager app** (`app/`): app selection, presets and advanced settings, foreground-only system controls, hook statistics.

> Targets: arm64 / Snapdragon (Adreno GPU) / Android 13+ / KernelSU or Magisk with Zygisk.

### Features

**Vulkan**
| Feature | What it does | Measured (lab) |
|---|---|---|
| Lazy WaitIdle | `vkQueueWaitIdle` / `vkDeviceWaitIdle` return at once; the wait is postponed to where it is really needed (command-buffer reuse, memory reads, …). Destruction runs on a helper thread after the GPU is done | 104 → 163 fps |
| Persistent pipeline cache | Saves and reuses compiled pipelines; the game's own cache (Unreal / Unity PSO caches) is merged, saved and injected back on the next launch | 17.8 → 0.06 ms per pipeline |
| Don't wait for occlusion | Unfinished occlusion queries answer "visible" immediately (nothing disappears) | — |

**OpenGL ES**
| Feature | What it does | Measured (lab) |
|---|---|---|
| Buffer updates as GPU copies | `glBufferSubData` / write maps → persistent staging + `glCopyBufferSubData` | 138 → 211–245 fps |
| GPU texture conversion | Conversion, tiling and copy done by the GPU (incl. 3D, arrays, compressed), parallel staging copy | RGBA8 3.2 → 1.0 ms, RGBA4 3.3 → 0.3 ms |
| Async glFinish | Fence + flush, waiting only on a fence a few frames back | 66 → 164 fps |
| Don't wait for occlusion | GL variant | 141 → 248 fps |
| Async readback (aggressive) | Repeated identical `glReadPixels` answered from the previous frame's async read | 147 → 244 fps |
| glFlush coalescing (aggressive) | Drops frequent flushes only for single-context apps without pending fences | 16 µs → 0 per call |

Numbers come from the synthetic benchmarks in `native/lab.cpp`; real-game gains vary per game.

**System controls** (applied only while the app is in front, restored afterwards): GPU clock floor, CPU boost (uclamp, never below the device default), pinning render threads to big cores. Launch settings: ANGLE switch, Game Mode (resolution scale, FPS limit chosen from the refresh rates read from the device's display).

**Presets**: Safe / Standard (default) / Aggressive / Max. Every item is explained in the app via "ⓘ Show explanations". The UI is Japanese when the system language is Japanese and English otherwise.

### Known risks
- Lazy WaitIdle / async glFinish: a game reading GPU results straight from persistently mapped memory without any API call could see stale data (async glFinish disables itself when it detects persistent read maps).
- Aggressive and above may show brief visual glitches. Try "Safe" if a game misbehaves.

### Build
Requires Android SDK (platform 37, NDK r29), JDK 17, clang/lld, python3.

```sh
export ANDROID_HOME=/path/to/sdk JAVA_HOME=/path/to/jdk17
./build-all.sh            # → build/LoadShift.apk and build/LoadShift-module.zip
```
Machine-specific values can be put in `build.env` (git-ignored); it is sourced automatically.

### Install
1. Flash `LoadShift-module.zip` in KernelSU / Magisk and reboot
2. Install `LoadShift.apk` and grant root
3. Pick a game in the app and tap "Save & restart app"

Settings are written to `/data/adb/loadshift/profiles/<package>.conf`, statistics to `/data/data/<package>/cache/loadshift/stats.txt`.

### Testing
`native/lab.cpp` runs directly on the device (`lab gles|glbuf|probe|probe2|occlgl|vk|vkidle|occlvk <libloadshift.so> <conf> <dir>`) and compares each feature on/off and checks correctness by reading results back.

Third-party notices: see [THIRD_PARTY.md](THIRD_PARTY.md).

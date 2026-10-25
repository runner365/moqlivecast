# Android moq-lite push demo — how to use

`tools/android/` is a complete Android push client. It captures the camera and
microphone in Kotlin, encodes H.264 + AAC-LC with `MediaCodec`, hands the frames
to the `moqlite/` **C API** through JNI, and publishes them over **raw QUIC**
(ALPN `moq-lite-06`) to `moqlivecast`. The result can be played in
`tools/moq_js_client` (MoQ pull, `/moq-pull`) or by any HTTP-FLV player.

```
Camera2 ──> CameraGlRotator (GL) ──> MediaCodec H.264 ──┐
                                                         ├──> moq_pusher queue
AudioRecord ──> MediaCodec AAC-LC ─────────────────────┘        (uv loop thread)
                                                                      │
                                                    moqlite/ C API (publish /
                                                    publish_track / track_frame)
                                                                      │
                                                          QUIC, ALPN moq-lite-06
                                                                      │
                                                                 moqlivecast
                                                                      │
                                                       Browser /moq-pull, HTTP-FLV
```

The native half is **the same code on the host**: `tools/android/app/src/main/cpp/CMakeLists.txt`
compiles `moqlite/`, `quic/`, `tls/`, `udp/` and `logger/` directly into
`libmoqlitepush.so`, exactly like `moqlite_client_core` on desktop. There is no
WebTransport in this path — the browser demo and the phone speak different
bindings of the same server.

---

## 1. Requirements

| Tool | Version used here | Notes |
|---|---|---|
| Android Studio / SDK | compileSdk 37, targetSdk 37, minSdk 24 | Android 14+ foreground-service types are handled |
| NDK | `30.0.16248370` (pinned in `app/build.gradle.kts`) | install that exact version |
| CMake | `4.1.2` (the copy in the SDK, pinned) | do not let AGP download 3.22.1 |
| JDK | Gradle daemon toolchain 25 (`gradle/gradle-daemon-jvm.properties`) | resolved by the foojay resolver |
| AGP / Kotlin | 9.4.0 / 2.2.10 | `gradle/libs.versions.toml` |
| Device | **arm64-v8a** | other ABIs need their own OpenSSL build |

`local.properties` must point at your SDK (`sdk.dir=...`); it is git-ignored on
purpose and must never be committed.

---

## 2. Build

### 2.1 Build OpenSSL for Android (once per clean tree)

OpenSSL is cross-compiled with the **NDK clang** by a standalone script, then
imported by the app's CMake. Doing this inside Gradle would stack two
toolchains and is why the script exists.

```bash
bash tools/android/build_openssl_android.sh          # arm64-v8a (default)
# bash tools/android/build_openssl_android.sh x86_64 # if you also want an emulator
```

It needs `perl` and an NDK (found via `ANDROID_NDK_ROOT` / `$ANDROID_HOME/ndk`).
Output:

```
tools/android/3rdparty/openssl/<abi>/include
tools/android/3rdparty/openssl/<abi>/lib/{libcrypto.a,libssl.a}
```

If the app build says `OpenSSL for arm64-v8a not found`, run this script first.

### 2.2 Build the APK

```bash
cd tools/android
./gradlew :app:assembleDebug
```

The APK is `app/build/outputs/apk/debug/app-debug.apk`.

The first build also configures and compiles `3rdparty/libuv` and the C stack;
subsequent builds are incremental.

### 2.3 Install

```bash
adb install -r tools/android/app/build/outputs/apk/debug/app-debug.apk
```

### 2.4 Cleaning up disk space

A full build tree is ~1.6 GB (OpenSSL alone ~1.5 GB). To reclaim it:

```bash
bash tools/android/clean_all.sh                 # build outputs + prebuilt OpenSSL
bash tools/android/clean_all.sh --keep-openssl  # keep 3rdparty/openssl/
bash tools/android/clean_all.sh -n              # dry run
```

After a full clean, re-run `build_openssl_android.sh` before the next build.
Source + prebuilt libs are only ~29 MB.

---

## 3. Run

### 3.1 Start the server

```bash
cmake --build build --target moqlivecast
export MOQ_SSL_CERT_FILE=/path/to/fullchain.pem
export MOQ_SSL_KEY_FILE=/path/to/privkey.pem
./build/bin/moqlivecast moqlivecast/etc/config.yml
```

The server listens on UDP 4433 for both WebTransport (`h3`) and moq-lite
(`moq-lite-06`). Check that the phone and the server are on the same LAN and
that UDP 4433 is not blocked.

### 3.2 Permissions

Tap **Grant camera + microphone**. The notification permission is optional
(without it the foreground-service notification is hidden, capture still works).

### 3.3 Enter the target and press Start

| Field | Default | Meaning |
|---|---|---|
| `server url` | `moql://192.168.1.143:4433/anon` | replace with your server's **LAN IP** |
| `broadcast` | `android.hang` | on-wire broadcast name |

Use the **LAN IP**, not the development host name: `www.webrtcserver.com.cn`
maps to `127.0.0.1` through the *host machine's* `/etc/hosts`, which the phone
cannot resolve. An IP works because this client does not verify the server
certificate (it is a test/demo client). DNS resolution is **IPv4-only** — the
QUIC send path in this repo is IPv4.

The server stores the stream under `<entry path>/<broadcast>`, i.e.
`anon/android.hang`.

### 3.4 What the UI shows

| Row | Meaning |
|---|---|
| `state: RUNNING / idle` | the foreground service is collecting |
| `connected=…` | QUIC session established |
| `published=…` | the broadcast has at least one declared track (it has been announced) |
| `video=N audio=M dropped=D` | frames handed to the C API; `dropped` counts frames rejected because a track was not ready and frames evicted from the 256-item queue |
| `last: …` | last Kotlin-side status line |
| `native log (last 14)` | tail of the native/QUIC library log |

Buttons:

- **Start / Stop** — starts/stops capture and the push. Stop runs on a worker
  thread, so the UI does not freeze.
- **rotate +90°** — cycles the sensor correction 0→90→180→270 and **restarts**
  capture (a 90/270 correction swaps the encoder raster, so the encoder must be
  rebuilt). Use this to calibrate a device whose video comes out sideways.
- **mirror: on/off** — mirrors the front camera and restarts capture.
- **preview aspect** — changes only the preview crop; takes effect immediately
  and does not touch the stream.

A watchdog checks the native session twice a second. If it was connected and
then stays down for ~2 s, the service restarts capture and the push
automatically (up to 10 consecutive times; the counter resets after 30 s of
stable operation).

### 3.5 Watch the stream

**Browser (MoQ pull).** Run the web client:

```bash
cd tools/moq_js_client
npm install
npm run dev
```

Open `http://127.0.0.1:5174/moq-pull` and set the address to:

```
https://<server-host>:4433/moq?app=anon&stream=android.hang
```

`<server-host>` must be the name in the server certificate (the host entry used
by the desktop, e.g. `www.webrtcserver.com.cn`, with the proxy excluded for
that host). Pick **WebCodecs** (default) or **flv.js (MSE)**. WebCodecs needs
Chrome; flv.js falls back to MSE.

**HTTP-FLV (desktop):**

```bash
ffplay "https://<server-host>:4433/anon/android.hang.flv"
```

(or open the URL in the browser demo's `/http-flv` view).

---

## 4. Source map

| File | Role |
|---|---|
| `MainActivity.kt` | Compose UI, runtime permissions, preview surface, stats polling |
| `PushService.kt` | foreground service; start/stop/restart, watchdog and auto-reconnect |
| `CameraEncoder.kt` | Camera2 capture, H.264 `MediaCodec`, size/bitrate selection, csd-0/1 → avcC |
| `CameraGlRotator.kt` | EGL/OES pass-through: rotation + mirror, and the **frame timestamp** into the encoder surface |
| `MicEncoder.kt` | `AudioRecord` (48 kHz stereo PCM16) → AAC-LC, PTS generation, capture diagnostics |
| `MoqNative.kt` | JNI bridge (names must match `cpp/moq_jni.c` 1:1) |
| `NativeLog.kt` | lock-free tail buffer for native log lines shown in the UI |
| `cpp/moq_jni.c` | JNI exports, log forwarding, avcC assembly from SPS/PPS |
| `cpp/moq_pusher.c` | cross-thread queue → uv loop; owns the moq-lite session, publish and track handles |
| `cpp/CMakeLists.txt` | builds libuv + the C stack + `libmoqlitepush.so` |

The C API itself is documented in
[`moqlite/how_to_use_moq_lite_c_api.md`](../../moqlite/how_to_use_moq_lite_c_api.md).

---

## 5. Invariants the code depends on

These were each found the hard way; changing one usually fails **silently**
rather than with an error.

### 5.1 Audio must be 48 kHz / 2 channels

`AudioRecord` and the AAC encoder are both configured for `48000 Hz`, stereo,
PCM16. The web player's WebCodecs sink is fixed at 48 kHz / 2 ch
(`AudioContext({sampleRate: 48000})`, the worklet ring is sized for 2 ch), so a
mono or 44.1 kHz stream plays at the wrong speed and starves the ring. The
**AudioSpecificConfig (csd-0) is authoritative** — `MicEncoder` now reports the
ASC's rate/channels in the catalog instead of the requested constants, and logs
a WARN if they differ.

### 5.2 Audio and video must use the same clock

- Video PTS comes from Camera2 `SENSOR_TIMESTAMP`, set on the encoder surface
  via `eglPresentationTimeANDROID` by `CameraGlRotator`. Its timebase is
  **`SystemClock.elapsedRealtimeNanos()` (CLOCK_BOOTTIME, includes deep sleep)**.
- Audio PTS must therefore also use `SystemClock.elapsedRealtimeNanos()`
  (`MicEncoder.nowUs()`), **not** `System.nanoTime()` (CLOCK_MONOTONIC).

On a phone that has slept since boot, the two clocks differ by minutes or
hours. An earlier server revision compared audio and video against one shared
`last_in_dts_`, so every track switch looked like a clock reset and was
re-stamped `+40 ms`; the audio timeline then ran ~2.7× real time, the WebCodecs
player padded silence for every fake hole and the ring evicted real audio — it
sounded exactly like packet loss while `afps` still read 46.9 (MSE hides it by
re-timing the stream). The server now detects backward jumps **per track** and
only warns when the two raw timelines differ by >1 s, but a large skew still
breaks A/V sync — keep both clocks on `elapsedRealtimeNanos()`.

### 5.3 Video groups must start with a keyframe

`moq_lite_track_frame(..., key=1)` opens a new group and **must** be used on
IDR frames only. Non-keyframes that arrive before the first keyframe of a group
are dropped. Audio needs no key flag; the library groups it in 1 s windows.

### 5.4 Tracks are declared after connect

`csd-0`/`csd-1` only arrive after the encoders start, which is after the QUIC
session is up. The initial ANNOUNCE request therefore returns an empty set. The
C library compensates by sending **`ANNOUNCE_START (late)`** and a new catalog
group when the first track is published — do not remove that path, and do not
"fix" the empty ANNOUNCE_OK by delaying the connection.

### 5.5 Payloads are bare codec frames

`track_frame` receives **AVCC length-prefixed NALUs** and **raw AAC** (no ADTS).
No FLV tag header and no container timestamp prefix — the library adds those.
Timestamps are absolute microseconds.

### 5.6 Threading

- All moq-lite calls happen on the caller's `uv_loop_t` thread. Camera/mic
  threads only **enqueue** (mutex-protected queue); frames are copied, then
  consumed on the loop thread.
- `stop()` joins the loop thread and releases encoders; it runs on a dedicated
  worker executor, never on the main thread.
- JNI log callbacks arrive on native threads; the UI reads them from a
  concurrent ring buffer every 500 ms.

---

## 6. Logs and diagnostics

### 6.1 Where to look

- **In-app**: last Kotlin status line and the last 14 native lines.
- **logcat** (Kotlin side):

  ```bash
  adb logcat -s MoqPushSvc MoqCamera MoqGl MoqMic
  ```

- **Full native log** (the QUIC/moq-lite library writes its own file):

  ```bash
  adb exec-out run-as com.example.moqlitepush \
      cat files/moq_lite.log > moq_lite.log
  ```

### 6.2 Healthy startup, in order

```
nativeStart rc=0 url=moql://… broadcast=android.hang
connected, alpn="moq-lite-06"
video encoder started: 480x640 @…kbps, 30fps
video config: avcC … B (profile=42) -> native rc=0
audio capture: requested 48000Hz 2ch, AudioRecord 48000Hz 2ch encoding=2 …
audio config: ASC 2 B [aot=2 rate=48000 ch=2 hex=1190] -> native rc=0
ANNOUNCE_START (late) broadcast="android.hang"        ← library log
```

On the server you should then see `delivered=… frames` for `1.aac` / `0.h264`
and a non-zero GOP at subscribe time.

Every 5 s the mic logs a capture self-check:

```
audio tx: fed=48000 smp/s out=46.9 pkt/s 128 kbps pts_delta=[21333,21333]us drift=-3ms read_fail=0
```

- `fed≈48000`, `out≈46.9`, `pts_delta` within a few µs, `drift` within ~±20 ms
  → capture is complete and continuous.
- `drift` persistently negative → `AudioRecord` overran and dropped samples.
- `pts_delta` varying by >2 ms → timestamp discontinuities; the player will pad
  silence and misjudge frames.

---

## 7. Troubleshooting

| Symptom | Check | Likely cause |
|---|---|---|
| `connected=false` forever | `nativeStart rc`, URL, same LAN, UDP 4433 | wrong IP, server not running, firewall |
| `connected=true published=false` | `video encoder started`, `video config … rc=`, `audio capture`, `audio config … rc=` | an encoder failed or its csd never arrived |
| Browser connects but shows **"connected but no media"**, 0 fps, `pushed=0` | `published` in the app; server `subscribe gop packet count=0` | the publisher never announced a track (watch for `ANNOUNCE_START (late)` in the library log) |
| Audio choppy / "like packet loss" while `afps≈46.9` | `audio config` must be 48000/2ch; `audio tx` `pts_delta` and `drift`; server `audio/video timestamps differ by … ms` warning | clock timebase mismatch (5.2) or capture stalls |
| Video sideways / mirrored | use **rotate +90°** / **mirror** buttons then compare | sensor orientation not compensated in GL |
| Video squeezed or black bars in the browser | stream is 4:3 (640×480), the web container is 16:9 | change `CameraEncoder.WIDTH/HEIGHT` or the web aspect |
| Push stops by itself | `session down → auto-restart #n/10`, `stopping…` | network loss, server restart; after 10 retries the service gives up |
| Server logs `idle timeout … last_rx=20000ms ago` | was the app killed / permission revoked / phone asleep? | the phone stopped sending for 20 s (QUIC idle timeout) |

---

## 8. Related documents

- [`moqlite/how_to_use_moq_lite_c_api.md`](../../moqlite/how_to_use_moq_lite_c_api.md) — the C API used under `MoqNative`
- [`README.md`](../../README.md) — server, browser demo, HTTP-FLV

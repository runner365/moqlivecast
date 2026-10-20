package com.example.moqlitepush

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.SurfaceView
import android.view.SurfaceHolder
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.example.moqlitepush.ui.theme.MoqlitepushTheme
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            MoqlitepushTheme {
                Scaffold(modifier = Modifier.fillMaxSize()) { inner ->
                    PushScreen(Modifier.padding(inner))
                }
            }
        }
    }
}

private val NEEDED = arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)

/** 预览框可选的宽高比（宽/高）与名字。越小越高。 */
private val PREVIEW_ASPECTS = listOf(
    1.3333f to "4:3",
    1.0f    to "1:1",
    0.75f   to "3:4",
    0.6667f to "2:3",
    0.5625f to "9:16",
    /* 整屏档：主流手机的屏幕比例（1080x2340 / 1080x2400）。
     * ⚠️ 框会高到把下面的按钮顶出屏幕 —— 那一列本身可滚动，往下划即可。 */
    0.4615f to "9:19.5",
    0.45f   to "9:20",
)

private fun hasPerms(ctx: Context): Boolean =
    NEEDED.all { ctx.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

@Composable
fun PushScreen(modifier: Modifier = Modifier) {
    val ctx = LocalContext.current

    var url by remember { mutableStateOf(PushService.DEFAULT_URL) }
    var bc  by remember { mutableStateOf(PushService.DEFAULT_BROADCAST) }
    var permsOk by remember { mutableStateOf(hasPerms(ctx)) }
    var stats by remember { mutableStateOf<MoqNative.Stats?>(null) }
    var nativeErr by remember { mutableStateOf<String?>(null) }
    var version by remember { mutableStateOf("?") }
    var nativeLogs by remember { mutableStateOf<List<String>>(emptyList()) }
    /* 预览框的候选比例（宽/高），越往后越高。点按钮循环 —— 只是个**试**的旋钮。
     * ⚠️ 预览裁剪纯粹发生在 GL 那层（cover），改它**不用重起采集**：视图一改，
     * SurfaceView 就 surfaceChanged → 我们重建预览的 EGL window surface →
     * GL 按新尺寸重新 cover。所以这个按钮是即时生效的。 */
    var aspectIdx by remember {
        mutableStateOf(PREVIEW_ASPECTS.indexOfFirst { it.first == CameraEncoder.PREVIEW_ASPECT }
            .coerceAtLeast(0))
    }
    var previewAspect by remember { mutableStateOf(PREVIEW_ASPECTS[aspectIdx].first) }
    /* ⚠️ 单独存一份**快照**再显示：CameraGlRotator 里那个是普通 @Volatile 字段，
     * 不是 Compose 状态 —— 直接从组合里读它，Compose 不会因为它变化而重组，
     * 于是按钮上的角度可能一直停在上一次的值（实测就出现过日志已经是 180°、
     * 按钮还写着 0°）。由下面的轮询把它抄成 state。 */
    var rotationShown by remember { mutableStateOf(0) }

    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permsOk = hasPerms(ctx) }

    // 读一次版本号 —— 它同时是「.so 到底加载成功没有」的探针：加载失败会在
    // 这一句抛 UnsatisfiedLinkError，比后面某处莫名崩溃清楚得多。
    LaunchedEffect(Unit) {
        version = try {
            MoqNative.version()
        } catch (t: Throwable) {
            nativeErr = "libmoqlitepush.so load failed: ${t.message}"
            "n/a"
        }
    }

    // 原生日志转发到 [NativeLog]。回调在 **native 线程**上，所以只往那个
    // 线程安全的缓冲里塞；UI 由下面那个 500ms 的轮询取快照。
    LaunchedEffect(Unit) {
        MoqNative.onLog = { line -> NativeLog.add(line) }
        MoqNative.nativeSetLogForwarding(true)
    }

    // 轮询统计（读一次 native 的 long[]，很便宜）+ 取一次日志快照。
    LaunchedEffect(Unit) {
        while (true) {
            if (nativeErr == null) {
                try { stats = MoqNative.stats() }
                catch (t: Throwable) { nativeErr = t.message }
            }
            nativeLogs = NativeLog.tail()
            rotationShown = CameraGlRotator.lastRotationDeg
            /* ⚠️ 权限每轮重查一次。permsOk 原来是 remember 的一次性快照，只由
             * 那个申请回调更新 —— 于是**重装/在系统设置里改权限后，界面一直
             * 停在上次的值**：开始/rotate/mirror 全部灰掉，而且没有任何提示
             * （实测：重装后两个权限都是 granted=false，Start 点不了）。
             * checkSelfPermission ×2 每 500ms 的开销可以忽略。 */
            permsOk = hasPerms(ctx)
            delay(500)
        }
    }

    Column(
        modifier = modifier.fillMaxSize().padding(16.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text("moq-lite push (Android)", style = MaterialTheme.typography.titleMedium)
        Text("moq-lite $version", style = MaterialTheme.typography.bodySmall)

        // 摄像头预览。SurfaceView 比 Compose 的 Canvas 合适：相机直接往这块
        // Surface 上画，不经过 Compose 的重绘。
        // ⚠️ 它的 Surface 会在 Activity 销毁时失效 —— 那时采集也会跟着停
        // （见 CameraEncoder.start 的说明）。推流要在后台长期跑的话，
        // 就别把它挂在 session 上。
        /* 预览现在由 GL 层绘制（见 CameraGlRotator），它按画面真实比例做信箱式
         * 留边，所以**不会**被拉伸 —— 这里只负责把视图摆成同一个比例，
         * 免得 GL 画好的画面又被 SurfaceView 按视图形状拉一遍。 */
        AndroidView(
            modifier = Modifier.fillMaxWidth().aspectRatio(previewAspect),
            factory = { ctx2 ->
                SurfaceView(ctx2).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(h: SurfaceHolder) {
                            /* 预览不再是相机的输出（相机只往 GL 那层写），
                             * 所以这块 Surface 的尺寸随便多大都行 —— 没有
                             * 「必须是相机支持的尺寸」那个约束了。 */
                            PushService.setPreviewSurface(h.surface)
                        }
                        override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {
                            PushService.setPreviewSurface(h.surface)
                        }
                        override fun surfaceDestroyed(h: SurfaceHolder) {
                            PushService.setPreviewSurface(null)
                        }
                    })
                }
            })

        nativeErr?.let {
            Text("ERROR: $it",
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall)
        }

        OutlinedTextField(
            value = url, onValueChange = { url = it },
            label = { Text("server url — use the LAN IP, not the domain") },
            singleLine = true, modifier = Modifier.fillMaxWidth())

        OutlinedTextField(
            value = bc, onValueChange = { bc = it },
            label = { Text("broadcast") },
            singleLine = true, modifier = Modifier.fillMaxWidth())

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                enabled = permsOk && !PushService.running && nativeErr == null,
                onClick = { PushService.start(ctx, url.trim(), bc.trim()) },
            ) { Text("Start") }

            Button(
                enabled = PushService.running,
                onClick = { PushService.stop(ctx) },
            ) { Text("Stop") }
        }

        /* 方向标定：点一下把画面**多转 90°**（0→90→180→270 循环）并重起采集。
         * 四个角度各试一遍就知道哪个对 —— 比为了一个常量反复重编快得多。
         * ⚠️ 必须重起而不是「改了就生效」：旋转 90/270 时编码栅格的宽高是**换过来**
         * 的（640x480 → 480x640），那要重建编码器。 */
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                enabled = permsOk && nativeErr == null,
                onClick = {
                    CameraGlRotator.cycleRotationAdjust()
                    if (PushService.running) PushService.restart(ctx)
                },
            ) { Text("rotate +90°  →  ${rotationShown}°") }

            Button(
                enabled = permsOk && nativeErr == null,
                onClick = {
                    CameraGlRotator.toggleMirrorFront()
                    if (PushService.running) PushService.restart(ctx)
                },
            ) { Text("mirror: ${if (CameraGlRotator.mirrorFront) "on" else "off"}") }
        }

        /* 预览框形状。**即时生效**，不用重起采集（裁剪是 GL 那层的事）。 */
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = {
                    aspectIdx = (aspectIdx + 1) % PREVIEW_ASPECTS.size
                    previewAspect = PREVIEW_ASPECTS[aspectIdx].first
                },
            ) { Text("preview aspect: ${PREVIEW_ASPECTS[aspectIdx].second}") }
        }

        if (!permsOk) {
            Button(onClick = { launcher.launch(NEEDED) }) {
                Text("Grant camera + microphone")
            }
        }

        Spacer(Modifier.height(4.dp))
        Text("state: ${if (PushService.running) "RUNNING" else "idle"}",
            style = MaterialTheme.typography.bodyMedium)

        stats?.let { s ->
            Text(
                "connected=${s.connected}  published=${s.published}\n" +
                "video=${s.videoFrames}  audio=${s.audioFrames}  dropped=${s.dropped}",
                style = MaterialTheme.typography.bodyMedium)
        }

        Spacer(Modifier.height(4.dp))
        Text("last: ${PushService.lastLog.ifEmpty { "—" }}",
            style = MaterialTheme.typography.bodySmall)

        // 原生（C/C++）日志 —— 库自己的 + moq_pusher 的，经 JNI 转过来。
        // 全量在 files/moq_lite.log。
        Spacer(Modifier.height(8.dp))
        Text("native log (last ${nativeLogs.size}):",
            style = MaterialTheme.typography.bodySmall)
        nativeLogs.forEach { line ->
            Text(line,
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.fillMaxWidth())
        }

        Spacer(Modifier.height(8.dp))
        Text(
            "connected 一直是 false → 确认 url 用的是服务端的**局域网 IP**，" +
            "手机和服务端在同一网段。\n" +
            "connected=true 但 published=false → 两路编码器的 csd 还没到齐，" +
            "到齐之后才会建 catalog。\n" +
            "库里自己的日志在 files/moq_lite.log（adb pull 出来看）。",
            style = MaterialTheme.typography.bodySmall)
    }
}

<template>
  <div class="page">
    <header class="top">
      <h1>MoQ Pull</h1>
      <p class="sub">SUBSCRIBE → LOC → FLV → flv.js playback</p>
    </header>

    <section class="bar">
      <label class="lbl" for="addr">Pull URL</label>
      <input
        id="addr"
        v-model="address"
        class="input"
        :disabled="busy"
        placeholder="https://host:4433/moq?app=live&amp;stream=123456"
      />
      <select v-model="mode" class="sel" :disabled="busy" title="播放路径">
        <option value="flvjs">flv.js (MSE)</option>
        <option value="webcodecs" :disabled="!wcSupported">WebCodecs</option>
      </select>
      <button class="btn" :class="{ stop: connected }" :disabled="busy" @click="onClick">
        {{ btnText }}
      </button>
    </section>

    <p v-if="error" class="status err">{{ error }}</p>

    <div class="preview-wrap">
      <div class="preview">
        <video ref="videoEl" v-show="mode === 'flvjs'" playsinline autoplay></video>
        <canvas ref="canvasEl" v-show="mode === 'webcodecs'"></canvas>
        <div v-if="!connected && !busy" class="placeholder">16:9 preview</div>
        <PhaseOverlay :phase="phase" />
        <StatsOverlay v-if="connected" :stats="stats" />
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onUnmounted, ref } from 'vue'
import PhaseOverlay from '../components/PhaseOverlay.vue'
import StatsOverlay from '../components/StatsOverlay.vue'
import { defaultMoqPlaybackMode, defaultMoqPullUrl } from '../config'
import { log_error, log_info } from '../logger'
import { EMPTY_STATS, type MediaStatsSnapshot } from '../media/mediaStats'
import { MoqPuller } from '../media/moqPuller'
import { isWebCodecsPlaybackSupported, type PlaybackMode } from '../media/playbackMode'
import type { SessionPhase } from '../media/sessionPhase'
import { connectButtonLabel, isPullConnected } from '../media/sessionUi'

const address = ref(defaultMoqPullUrl())
const videoEl = ref<HTMLVideoElement | null>(null)
const canvasEl = ref<HTMLCanvasElement | null>(null)
/* WebCodecs 是默认路径；浏览器不支持时回退到 flv.js，避免选了个用不了的选项。 */
const wcSupported = isWebCodecsPlaybackSupported()
const mode = ref<PlaybackMode>(
  defaultMoqPlaybackMode() === 'webcodecs' && !wcSupported ? 'flvjs' : defaultMoqPlaybackMode(),
)
const connecting = ref(false)
const stopping = ref(false)
const error = ref('')
const phase = ref<SessionPhase>('init')
const stats = ref<MediaStatsSnapshot>({ ...EMPTY_STATS })

const puller = new MoqPuller()

const connected = computed(() => isPullConnected(phase.value))
const busy = computed(() => connecting.value || stopping.value)
const btnText = computed(() => connectButtonLabel(phase.value, connecting.value, stopping.value))

async function onClick() {
  if (connected.value) {
    await disconnect()
    return
  }
  await connect()
}

async function connect() {
  error.value = ''
  const url = address.value.trim()
  if (!url) {
    error.value = 'Enter a WebTransport pull URL'
    return
  }
  if (mode.value === 'webcodecs' && !canvasEl.value) return
  if (mode.value === 'flvjs' && !videoEl.value) return
  connecting.value = true
  phase.value = 'connecting'
  stats.value = { ...EMPTY_STATS }
  log_info('ui-moq-pull', `click connect ${url} mode=${mode.value}`)
  try {
    await puller.start(
      url,
      { video: videoEl.value, canvas: canvasEl.value },
      {
        mode: mode.value,
        onStats(s) {
          stats.value = s
        },
        onPhase(p) {
          phase.value = p
          if (p === 'disconnected') stats.value = { ...EMPTY_STATS }
        },
      },
    )
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e)
    log_error('ui-moq-pull', `connect failed: ${msg}`)
    error.value = 'Connect failed: ' + msg
    await puller.stop()
    phase.value = 'disconnected'
  } finally {
    connecting.value = false
  }
}

async function disconnect() {
  stopping.value = true
  log_info('ui-moq-pull', 'click disconnect')
  try {
    await puller.stop()
  } finally {
    stopping.value = false
    stats.value = { ...EMPTY_STATS }
    phase.value = 'disconnected'
  }
}

onUnmounted(() => {
  void puller.stop()
})
</script>

<style scoped>
.page {
  max-width: 960px;
  margin: 0 auto;
  padding: 0 20px 48px;
}

.top h1 {
  margin: 0;
  font-size: 28px;
  font-weight: 650;
}

.sub {
  margin: 6px 0 0;
  color: #8b949e;
  font-size: 14px;
}

.bar {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-top: 28px;
}

.lbl {
  flex: none;
  font-size: 14px;
  color: #c9d1d9;
}

.input {
  flex: 1;
  min-width: 0;
  height: 40px;
  padding: 0 12px;
  border: 1px solid #30363d;
  border-radius: 8px;
  background: #161b22;
  color: #e6edf3;
  font-size: 14px;
}

.input:focus {
  outline: none;
  border-color: #388bfd;
}

.sel {
  flex: none;
  height: 40px;
  padding: 0 10px;
  border: 1px solid #30363d;
  border-radius: 8px;
  background: #161b22;
  color: #e6edf3;
  font-size: 13px;
}

.btn {
  flex: none;
  height: 40px;
  min-width: 128px;
  padding: 0 18px;
  border: 0;
  border-radius: 8px;
  background: #1f6feb;
  color: #fff;
  font-size: 15px;
  cursor: pointer;
}

.btn:disabled {
  opacity: 0.55;
  cursor: default;
}

.btn.stop {
  background: #da3633;
}

.status {
  margin: 12px 0 0;
  font-size: 13px;
  color: #8b949e;
}

.status.err {
  color: #f85149;
}

.preview-wrap {
  margin-top: 20px;
}

.preview {
  position: relative;
  width: 100%;
  aspect-ratio: 16 / 9;
  border-radius: 12px;
  overflow: hidden;
  background: #010409;
  border: 1px solid #30363d;
}

.preview video,
.preview canvas {
  display: block;
  width: 100%;
  height: 100%;
  object-fit: contain;
  background: #000;
}

.placeholder {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #6e7681;
  font-size: 18px;
  pointer-events: none;
}
</style>

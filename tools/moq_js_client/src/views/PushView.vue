<template>
  <div class="page">
    <header class="top">
      <h1>WT FLV Push</h1>
      <p class="sub">Camera + mic → H.264 / AAC → FLV → WebTransport</p>
    </header>

    <section class="bar">
      <label class="lbl" for="addr">Push URL</label>
      <input
        id="addr"
        v-model="address"
        class="input"
        :disabled="busy"
        placeholder="https://host:4433/flv?method=push&amp;app=live&amp;stream=<stream>"
      />
      <button class="btn" :class="{ stop: connected }" :disabled="busy" @click="onClick">
        {{ btnText }}
      </button>
    </section>

    <p v-if="error" class="status err">{{ error }}</p>
    <p v-if="hint && !error" class="hint">{{ hint }}</p>

    <div class="preview-wrap">
      <div class="preview">
        <video ref="videoEl" playsinline autoplay muted></video>
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
import { defaultPushUrl } from '../config'
import { log_error, log_info } from '../logger'
import { EMPTY_STATS, type MediaStatsSnapshot } from '../media/mediaStats'
import { PushPipeline } from '../media/pipeline'
import type { SessionPhase } from '../media/sessionPhase'
import { connectButtonLabel } from '../media/sessionUi'

const address = ref(defaultPushUrl('flv'))
const videoEl = ref<HTMLVideoElement | null>(null)
const connected = ref(false)
const connecting = ref(false)
const stopping = ref(false)
const error = ref('')
const hint = ref('')
const phase = ref<SessionPhase>('init')
const stats = ref<MediaStatsSnapshot>({ ...EMPTY_STATS })

const pipeline = new PushPipeline()

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
  hint.value = ''
  const url = address.value.trim()
  if (!url) {
    error.value = 'Enter a WebTransport push URL'
    return
  }
  if (!videoEl.value) return
  connecting.value = true
  phase.value = 'connecting'
  stats.value = { ...EMPTY_STATS }
  log_info('ui-push', `click connect ${url}`)
  try {
    await pipeline.start(url, videoEl.value, {
      onStatus(s) {
        const a = s.audioOk ? `AAC ${s.audioCodec} @48k/2ch` : 'no audio'
        hint.value = `${s.videoCodec} ${s.width}x${s.height} + ${a}`
        log_info('ui-push', hint.value)
      },
      onError(msg) {
        hint.value = msg
      },
      onStats(s) {
        stats.value = s
      },
      onPhase(p) {
        phase.value = p
      },
    })
    connected.value = true
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e)
    log_error('ui-push', `connect failed: ${msg}`)
    error.value = 'Connect failed: ' + msg
    await pipeline.stop()
    connected.value = false
    phase.value = 'disconnected'
  } finally {
    connecting.value = false
  }
}

async function disconnect() {
  stopping.value = true
  log_info('ui-push', 'click disconnect')
  try {
    await pipeline.stop()
  } finally {
    connected.value = false
    stopping.value = false
    stats.value = { ...EMPTY_STATS }
    hint.value = ''
    phase.value = 'disconnected'
  }
}

onUnmounted(() => {
  void pipeline.stop()
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

.btn {
  flex: none;
  height: 40px;
  min-width: 128px;
  padding: 0 18px;
  border: 0;
  border-radius: 8px;
  background: #238636;
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

.hint {
  margin: 6px 0 0;
  font-size: 13px;
  color: #8b949e;
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

.preview video {
  display: block;
  width: 100%;
  height: 100%;
  object-fit: cover;
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

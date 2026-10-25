<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from 'vue'
import { MicCapture } from './audio/capture'
import { PcmPlayer } from './audio/player'
import { RealtimeClient, type ServerEvent, type TransportKind } from './realtime/client'

type Turn = { role: 'user' | 'assistant'; text: string }

const transportKind = ref<TransportKind>('ws')
const wsUrl = ref('ws://127.0.0.1:8000/ws')
const wtUrl = ref('https://127.0.0.1:5433/wt2ws')

const currentUrl = computed({
  get: () => (transportKind.value === 'wt' ? wtUrl.value : wsUrl.value),
  set: (value: string) => {
    if (transportKind.value === 'wt') wtUrl.value = value
    else wsUrl.value = value
  },
})
const connected = ref(false)
const recording = ref(false)
const starting = ref(false)
const status = ref('idle')
const errorText = ref('')
const turns = ref<Turn[]>([])
const eventLog = ref<string[]>([])
const chatEl = ref<HTMLElement | null>(null)

// Optional deep link for testing:
//   http://127.0.0.1:5173/?transport=wt&endpoint=https://127.0.0.1:5433/wt2ws
const _params = new URLSearchParams(location.search)
if (_params.get('transport') === 'wt') transportKind.value = 'wt'
if (_params.get('endpoint')) currentUrl.value = _params.get('endpoint') as string

function connectedStatus(): string {
  return `connected (${transportKind.value === 'wt' ? 'WT' : 'WS'})`
}

const client = new RealtimeClient()
const player = new PcmPlayer()
let capture: MicCapture | null = null
let pressed = false
let currentAssistant = -1
let transcriptBuffer = ''
let sawTextDelta = false

// GA-shaped session.update.  The server also accepts the older flat form
// (modalities / voice / input_audio_format), so both stay interoperable.
const sessionConfig = {
  type: 'realtime',
  output_modalities: ['audio'],
  audio: {
    input: {
      format: { type: 'audio/pcm', rate: 16000 },
      turn_detection: null,
    },
    output: {
      format: { type: 'audio/pcm', rate: 16000 },
      voice: 'longxiaochun_v2',
    },
  },
}

function scrollChat(): void {
  void nextTick(() => {
    if (chatEl.value) chatEl.value.scrollTop = chatEl.value.scrollHeight
  })
}

function logEvent(event: ServerEvent): void {
  eventLog.value.push(event.type)
  if (eventLog.value.length > 80) eventLog.value.splice(0, eventLog.value.length - 80)
}

function handleEvent(event: ServerEvent): void {
  if (event.type === 'pong') return
  logEvent(event)
  switch (event.type) {
    case 'session.created':
    case 'session.updated': {
      // The server advertises the real TTS rate; follow it (16000 by default).
      const rate = event.session?.audio?.output?.format?.rate
      if (typeof rate === 'number') player.setSampleRate(rate)
      status.value = recording.value ? 'recording…' : connectedStatus()
      break
    }

    case 'conversation.item.input_audio_transcription.completed':
      turns.value.push({ role: 'user', text: String(event.transcript ?? '') })
      scrollChat()
      status.value = 'thinking…'
      break

    case 'response.created':
      turns.value.push({ role: 'assistant', text: '' })
      currentAssistant = turns.value.length - 1
      transcriptBuffer = ''
      sawTextDelta = false
      status.value = 'responding…'
      scrollChat()
      break

    case 'response.output_text.delta':
    case 'response.text.delta':
      if (currentAssistant >= 0) {
        turns.value[currentAssistant].text += String(event.delta ?? '')
        sawTextDelta = true
        scrollChat()
      }
      break

    case 'response.output_audio_transcript.delta':
    case 'response.audio_transcript.delta':
      transcriptBuffer += String(event.delta ?? '')
      break

    case 'response.output_audio.delta':
    case 'response.audio.delta':
      if (typeof event.delta === 'string') player.playBase64(event.delta)
      break

    case 'response.done': {
      const failed = event.response?.status === 'failed'
      if (currentAssistant >= 0 && !sawTextDelta && transcriptBuffer) {
        turns.value[currentAssistant].text = transcriptBuffer
      }
      currentAssistant = -1
      transcriptBuffer = ''
      status.value = failed ? 'response failed' : connectedStatus()
      scrollChat()
      break
    }

    case 'error':
      errorText.value = String(event.error?.message ?? 'server error')
      status.value = 'error'
      break
  }
}

async function connect(): Promise<void> {
  if (connected.value) return
  errorText.value = ''
  status.value = 'connecting…'
  client.onEvent = handleEvent
  client.onClose = (reason: string) => {
    connected.value = false
    recording.value = false
    status.value = reason
  }
  try {
    await client.connect(transportKind.value, currentUrl.value, sessionConfig)
    connected.value = true
    status.value = connectedStatus()
  } catch (err: any) {
    status.value = 'connect failed'
    errorText.value = err?.message ? String(err.message) : String(err)
  }
}

function disconnect(): void {
  client.close()
  connected.value = false
  recording.value = false
  status.value = 'disconnected'
}

async function finishRecording(): Promise<void> {
  if (!recording.value) return
  recording.value = false
  status.value = 'thinking…'
  if (capture) {
    await capture.stop()
    capture = null
  }
  client.commit()
}

async function onDown(event: PointerEvent): Promise<void> {
  event.preventDefault()
  if (starting.value || recording.value) return
  pressed = true
  starting.value = true
  // Resume playback *before* any await: browsers only allow AudioContext
  // resume inside the user gesture (pointerdown).
  void player.resume().catch(() => {})
  try {
    if (!connected.value) await connect()
    if (!connected.value) return
    capture = new MicCapture()
    await capture.start((pcm) => client.sendAudio(pcm))
    if (!pressed) {
      // Released while the mic was still starting: send what we have.
      recording.value = true
      await finishRecording()
      return
    }
    recording.value = true
    status.value = 'recording…'
  } catch (err: any) {
    errorText.value = err?.message ? String(err.message) : String(err)
    status.value = 'microphone error'
    pressed = false
    if (capture) {
      await capture.stop()
      capture = null
    }
  } finally {
    starting.value = false
  }
}

async function onUp(): Promise<void> {
  pressed = false
  await finishRecording()
}

function clearAll(): void {
  turns.value = []
  eventLog.value = []
  errorText.value = ''
}

onMounted(() => {
  window.addEventListener('pointerup', onUp)
  window.addEventListener('pointercancel', onUp)
  void connect()
})

onBeforeUnmount(() => {
  window.removeEventListener('pointerup', onUp)
  window.removeEventListener('pointercancel', onUp)
  void capture?.stop()
  capture = null
  client.close()
  void player.close()
})
</script>

<template>
  <div class="page">
    <header>
      <div>
        <h1>StreamVox</h1>
        <p class="sub">WebSocket / WebTransport voice agent · OpenAI Realtime compatible</p>
      </div>
      <span class="status" :class="{ ok: connected, bad: !connected && status !== 'idle' }">
        {{ status }}
      </span>
    </header>

    <p v-if="errorText" class="error">{{ errorText }}</p>

    <main ref="chatEl" class="chat">
      <div v-if="turns.length === 0" class="empty">
        按住下面的按钮说话，松开后开始识别和回答。
      </div>
      <div v-for="(turn, i) in turns" :key="i" class="bubble" :class="turn.role">
        <div class="who">{{ turn.role === 'user' ? '你' : 'StreamVox' }}</div>
        <div class="text">{{ turn.text || '…' }}</div>
      </div>
    </main>

    <div class="row">
      <span class="label">连接</span>
      <label class="radio">
        <input v-model="transportKind" type="radio" value="ws" :disabled="connected" />
        WebSocket 直连
      </label>
      <label class="radio">
        <input v-model="transportKind" type="radio" value="wt" :disabled="connected" />
        WebTransport（proxy）
      </label>
    </div>

    <div class="row">
      <span class="label">{{ transportKind === 'wt' ? 'WT URL' : 'WS URL' }}</span>
      <input v-model="currentUrl" class="url" :disabled="connected" spellcheck="false" />
      <button class="ghost" @click="connected ? disconnect() : connect()">
        {{ connected ? '断开' : '连接' }}
      </button>
      <button class="ghost" @click="clearAll">清空</button>
    </div>

    <p v-if="transportKind === 'wt'" class="hint">
      WT 需要先启动 wt2ws_proxy（UDP 5433，backend ws://127.0.0.1:8000/ws），并让浏览器信任代理证书。
    </p>

    <button
      class="ptt"
      :class="{ active: recording }"
      @pointerdown="onDown"
      @contextmenu.prevent
    >
      {{ recording ? '松开发送' : '按住说话' }}
    </button>

    <details class="log">
      <summary>事件日志（{{ eventLog.length }}）</summary>
      <div v-for="(item, i) in eventLog" :key="i" class="log-line">{{ item }}</div>
    </details>
  </div>
</template>

<template>
  <div class="chat-app">
    <header class="chat-header">
      <h1>WebTransport Chat Meeting</h1>
    </header>

    <!-- 连接面板 -->
    <section class="connect-panel" v-if="!connected">
      <div class="field">
        <label>服务器地址</label>
        <input v-model="url" placeholder="https://www.webrtcserver.com.cn:4433" />
      </div>
      <div class="field">
        <label>Room ID</label>
        <input v-model="roomId" placeholder="123456" />
      </div>
      <div class="field">
        <label>用户名</label>
        <input v-model="username" placeholder="你的昵称" />
      </div>
      <button class="btn primary" @click="connect" :disabled="connecting">
        {{ connecting ? '连接中…' : '加入会议' }}
      </button>
      <div class="error" v-if="error">{{ error }}</div>
    </section>

    <!-- 聊天面板 -->
    <section class="chat-panel" v-else>
      <div class="room-info">
        <span>房间：{{ roomId }}</span>
        <span>用户：{{ username }}</span>
        <span class="hb-status" :class="{ ok: hbOk, bad: !hbOk }">
          心跳：{{ hbOk ? '正常' : '异常' }}
        </span>
        <button class="btn" @click="disconnect">退出</button>
      </div>

      <div class="chat-body">
        <aside class="member-panel">
          <div class="member-title">在线成员 ({{ onlineMembers.length }})</div>
          <ul class="member-list">
            <li
              v-for="name in onlineMembers"
              :key="name"
              class="member-item"
              :class="{ me: name === username }"
            >
              <span class="dot"></span>
              <span class="member-name">{{ name }}</span>
              <span v-if="name === username" class="me-tag">我</span>
            </li>
            <li v-if="onlineMembers.length === 0" class="member-empty">暂无成员</li>
          </ul>
        </aside>

        <div class="chat-main">
          <div class="messages" ref="messagesEl">
            <div
              v-for="(m, i) in messages"
              :key="i"
              class="message"
              :class="{ self: m.self, system: m.from === 'system' }"
            >
              <template v-if="m.from === 'system'">
                <div class="msg-body system-text">{{ m.msg }}</div>
              </template>
              <template v-else>
                <div class="msg-user">{{ m.from }}</div>
                <div class="msg-body">{{ m.msg }}</div>
              </template>
            </div>
          </div>

          <div class="composer">
            <input
              v-model="draft"
              @keyup.enter="send"
              placeholder="输入消息，回车发送"
            />
            <button class="btn primary" @click="send">发送</button>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup>
import { ref, nextTick } from 'vue'
import { log_info, log_warn, log_error, log_debug } from './logger.js'

/** 客户端心跳间隔（须明显小于服务端 20s 超时） */
const HB_INTERVAL_MS = 5000

function randomUsername() {
  const words = [
    'alpha', 'bravo', 'charlie', 'delta', 'echo', 'foxtrot',
    'golf', 'hotel', 'india', 'juliet', 'kilo', 'lima',
    'mike', 'november', 'oscar', 'papa', 'quebec', 'romeo',
    'sierra', 'tango', 'uniform', 'victor', 'whiskey', 'xray',
  ]
  const word = words[Math.floor(Math.random() * words.length)]
  const suffix = Math.random().toString(36).slice(2, 6)
  return `${word}-${suffix}`
}

const url = ref('https://www.webrtcserver.com.cn:4433')
const roomId = ref('123456')
const username = ref(randomUsername())
const connected = ref(false)
const connecting = ref(false)
const error = ref('')
const messages = ref([])
const draft = ref('')
const messagesEl = ref(null)
const hbOk = ref(true)
const onlineMembers = ref([])

let wt = null
let stream = null
let reader = null
let hbTimer = null
let closing = false
let recvBuf = ''
const textDecoder = new TextDecoder()

/* 从缓冲中拆出完整 JSON 对象。
 * WT 流没有消息边界：一次 read() 可能粘包（多条），也可能半包。 */
function extractJsonObjects(chunk) {
  recvBuf += chunk
  const objs = []
  let i = 0
  while (i < recvBuf.length) {
    while (i < recvBuf.length && /\s/.test(recvBuf[i])) i++
    if (i >= recvBuf.length) break
    if (recvBuf[i] !== '{' && recvBuf[i] !== '[') {
      /* 无法对齐，丢掉到下一个 { */
      const next = recvBuf.indexOf('{', i)
      if (next < 0) {
        recvBuf = ''
        break
      }
      i = next
    }
    const start = i
    let depth = 0
    let inStr = false
    let escape = false
    for (; i < recvBuf.length; i++) {
      const c = recvBuf[i]
      if (inStr) {
        if (escape) escape = false
        else if (c === '\\') escape = true
        else if (c === '"') inStr = false
        continue
      }
      if (c === '"') { inStr = true; continue }
      if (c === '{' || c === '[') depth++
      else if (c === '}' || c === ']') {
        depth--
        if (depth === 0) {
          const raw = recvBuf.slice(start, i + 1)
          try {
            objs.push(JSON.parse(raw))
          } catch {
            log_warn('recv malformed JSON object:', raw)
          }
          i++
          break
        }
      }
    }
    if (depth !== 0) {
      recvBuf = recvBuf.slice(start)
      return objs
    }
  }
  recvBuf = recvBuf.slice(i)
  return objs
}

function scrollToBottom() {
  nextTick(() => {
    if (messagesEl.value)
      messagesEl.value.scrollTop = messagesEl.value.scrollHeight
  })
}

function pushMessage(from, msg, self = false) {
  messages.value.push({ from, msg, self })
  scrollToBottom()
}

function setMembers(users) {
  const list = Array.isArray(users) ? users.filter(Boolean) : []
  // 去重并保持稳定顺序
  const seen = new Set()
  onlineMembers.value = list.filter((u) => {
    if (seen.has(u)) return false
    seen.add(u)
    return true
  })
}

function addMember(name) {
  if (!name) return
  if (!onlineMembers.value.includes(name)) {
    onlineMembers.value = [...onlineMembers.value, name]
  }
}

function removeMember(name) {
  if (!name) return
  onlineMembers.value = onlineMembers.value.filter((u) => u !== name)
}

function stopHeartbeat() {
  if (hbTimer != null) {
    clearInterval(hbTimer)
    hbTimer = null
  }
}

async function writePayload(obj) {
  if (!stream) throw new Error('stream not ready')
  const payload = JSON.stringify(obj)
  log_debug('send raw:', payload)
  const writer = stream.writable.getWriter()
  try {
    await writer.write(new TextEncoder().encode(payload))
  } finally {
    writer.releaseLock()
  }
}

async function forceLeave(reason) {
  if (closing) return
  closing = true
  log_warn('force leave:', reason)
  stopHeartbeat()
  hbOk.value = false

  try {
    if (reader) { reader.cancel().catch(() => {}); reader = null }
    if (stream) { stream = null }
    if (wt) { try { wt.close() } catch (_) {} wt = null }
  } catch (e) {
    log_warn('force leave cleanup error:', e.message)
  }

  connected.value = false
  messages.value = []
  onlineMembers.value = []
  recvBuf = ''
  error.value = reason || '连接已断开'
  closing = false
}

async function sendHeartbeat() {
  if (!connected.value || !stream) return
  try {
    await writePayload({ user: username.value, type: 'heartbeat' })
    hbOk.value = true
    log_debug('heartbeat ok')
  } catch (e) {
    log_error('heartbeat failed:', e.message)
    hbOk.value = false
    await forceLeave('连接已断开（心跳发送失败，WebTransport session 异常）')
  }
}

function startHeartbeat() {
  stopHeartbeat()
  hbOk.value = true
  hbTimer = setInterval(() => { sendHeartbeat() }, HB_INTERVAL_MS)
}

function handleIncoming(obj) {
  const from = obj.from || 'unknown'
  const msg = obj.msg || ''
  const type = obj.type || ''

  // 成员列表事件
  if (from === 'system' && type === 'members') {
    setMembers(obj.users)
    log_info('members snapshot:', onlineMembers.value.join(', '))
    return
  }
  if (from === 'system' && type === 'join') {
    const u = obj.user || ''
    addMember(u)
    pushMessage('system', msg || `${u} joined`, false)
    log_info('member join:', u)
    return
  }
  if (from === 'system' && type === 'leave') {
    const u = obj.user || ''
    removeMember(u)
    pushMessage('system', msg || `${u} left`, false)
    log_info('member leave:', u)
    return
  }

  // 普通 / 旧版 system 文本
  if (from === username.value) return
  pushMessage(from, msg, false)
}

async function connect() {
  error.value = ''
  connecting.value = true
  closing = false
  onlineMembers.value = []
  recvBuf = ''
  const fullUrl = `${url.value.replace(/\/$/, '')}/chat?roomid=${encodeURIComponent(roomId.value)}`
  log_info('connecting to', fullUrl, 'user=', username.value)
  try {
    wt = new WebTransport(fullUrl)
    await wt.ready
    log_info('webtransport ready')

    wt.closed.then(() => {
      log_warn('webtransport closed by peer/network')
      forceLeave('WebTransport session 已关闭')
    }).catch((e) => {
      log_warn('webtransport closed with error:', e && e.message)
      forceLeave('WebTransport session 已关闭：' + (e && e.message ? e.message : 'unknown'))
    })

    stream = await wt.createBidirectionalStream()
    reader = stream.readable.getReader()
    log_info('bidirectional stream opened')

    connected.value = true
    log_info('joined room', roomId.value)
    readLoop()

    const joinMsg = `${username.value} is joining the room.`
    await writePayload({ user: username.value, msg: joinMsg })
    pushMessage(username.value, joinMsg, true)
    // 快照到达前先显示自己
    setMembers([username.value])
    log_info('sent join message')

    startHeartbeat()
  } catch (e) {
    log_error('connect failed:', e.message)
    error.value = '连接失败：' + e.message
    stopHeartbeat()
    wt = null
    stream = null
    reader = null
    connected.value = false
    onlineMembers.value = []
  } finally {
    connecting.value = false
  }
}

async function readLoop() {
  try {
    while (true) {
      const { value, done } = await reader.read()
      if (done) {
        log_info('stream closed by peer')
        await forceLeave('消息流已关闭，连接断开')
        break
      }
      const text = textDecoder.decode(value, { stream: true })
      log_debug('recv raw:', text)
      const objs = extractJsonObjects(text)
      if (objs.length === 0 && text.trim()) {
        log_debug('recv incomplete JSON, buffered')
      }
      for (const obj of objs) handleIncoming(obj)
    }
  } catch (e) {
    if (!closing) {
      log_warn('read loop ended:', e.message)
      await forceLeave('读消息失败，连接断开：' + e.message)
    }
  }
}

async function send() {
  const text = draft.value.trim()
  if (!text || !stream) return
  log_info('send message:', text)
  try {
    await writePayload({ user: username.value, msg: text })
    pushMessage(username.value, text, true)
    draft.value = ''
  } catch (e) {
    log_error('send failed:', e.message)
    await forceLeave('发送失败，连接断开：' + e.message)
  }
}

async function disconnect() {
  log_info('leaving room', roomId.value)
  stopHeartbeat()
  closing = true
  try {
    if (reader) { reader.cancel().catch(() => {}); reader = null }
    if (stream) { stream = null }
    if (wt) { wt.close(); wt = null }
  } catch (e) {
    log_warn('disconnect error:', e.message)
  }
  connected.value = false
  messages.value = []
  onlineMembers.value = []
  recvBuf = ''
  hbOk.value = true
  error.value = ''
  closing = false
}
</script>

<style scoped>
.chat-app {
  max-width: 900px;
  margin: 0 auto;
  padding: 20px;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
}
.chat-header h1 { margin: 0 0 16px; font-size: 20px; }
.connect-panel, .chat-panel {
  border: 1px solid #e0e0e0;
  border-radius: 8px;
  padding: 16px;
  background: #fff;
}
.field { margin-bottom: 12px; }
.field label { display: block; margin-bottom: 4px; font-size: 13px; color: #666; }
.field input {
  width: 100%; padding: 8px; box-sizing: border-box;
  border: 1px solid #ccc; border-radius: 4px; font-size: 14px;
}
.btn {
  padding: 8px 16px; border: 1px solid #ccc; border-radius: 4px;
  background: #f5f5f5; cursor: pointer; font-size: 14px;
}
.btn.primary { background: #2563eb; color: #fff; border-color: #2563eb; }
.btn:disabled { opacity: 0.6; cursor: not-allowed; }
.error { margin-top: 12px; color: #dc2626; font-size: 13px; }
.room-info {
  display: flex; gap: 16px; align-items: center; flex-wrap: wrap;
  padding-bottom: 12px; border-bottom: 1px solid #eee;
  font-size: 13px; color: #555;
}
.room-info .btn { margin-left: auto; }
.hb-status.ok { color: #15803d; }
.hb-status.bad { color: #dc2626; font-weight: 600; }

.chat-body {
  display: flex;
  gap: 12px;
  margin-top: 12px;
  min-height: 440px;
}
.member-panel {
  width: 180px;
  flex-shrink: 0;
  border: 1px solid #e5e7eb;
  border-radius: 8px;
  background: #f8fafc;
  padding: 10px;
  box-sizing: border-box;
}
.member-title {
  font-size: 13px;
  font-weight: 600;
  color: #334155;
  margin-bottom: 8px;
}
.member-list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.member-item {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 13px;
  color: #0f172a;
  padding: 4px 6px;
  border-radius: 4px;
}
.member-item.me {
  background: #dbeafe;
}
.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #22c55e;
  flex-shrink: 0;
}
.member-name {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 12px;
}
.me-tag {
  margin-left: auto;
  font-size: 11px;
  color: #1d4ed8;
}
.member-empty {
  font-size: 12px;
  color: #94a3b8;
  padding: 4px 6px;
}

.chat-main {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
}
.messages {
  height: 400px; overflow-y: auto; padding: 12px 0;
  display: flex; flex-direction: column; gap: 10px;
}

.message {
  max-width: 78%;
  padding: 8px 12px;
  border-radius: 8px;
  background: #f1f5f9;
  align-self: flex-start;
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.message.self {
  background: #dbeafe;
  align-self: flex-end;
  text-align: right;
}
.message.system {
  align-self: center;
  max-width: 90%;
  background: #fef3c7;
  text-align: center;
}

.msg-user {
  font-size: 12px;
  font-weight: 600;
  letter-spacing: 0.02em;
  color: #64748b;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
.message.self .msg-user {
  color: #1d4ed8;
}
.msg-body {
  font-size: 15px;
  line-height: 1.45;
  color: #0f172a;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  word-break: break-word;
}
.system-text {
  font-size: 12px;
  color: #92400e;
}

.composer { display: flex; gap: 8px; margin-top: 12px; }
.composer input {
  flex: 1; padding: 8px; border: 1px solid #ccc;
  border-radius: 4px; font-size: 14px;
}

@media (max-width: 640px) {
  .chat-body { flex-direction: column; }
  .member-panel { width: 100%; }
  .member-list { flex-direction: row; flex-wrap: wrap; }
}
</style>

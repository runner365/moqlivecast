// 轻量日志模块 —— 对齐 C 端 logger（logger/logger.c）
// 级别：ERROR=0 < WARN=1 < INFO=2 < DEBUG=3，默认 INFO
// 格式：[文件:行][时间戳][级别] 消息

export const LogLevel = {
  ERROR: 0,
  WARN: 1,
  INFO: 2,
  DEBUG: 3,
}

const LEVEL_NAMES = ['ERROR', 'WARN ', 'INFO ', 'DEBUG']

let g_level = LogLevel.INFO

export function log_set_level(level) {
  g_level = level
}

export function log_get_level() {
  return g_level
}

function pad(n, w = 2) {
  return String(n).padStart(w, '0')
}

function timestamp() {
  const d = new Date()
  return (
    `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
    `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`
  )
}

// 从 Error stack 提取调用文件名 + 行号（近似 C 端的 __FILE__:__LINE__）
function caller() {
  try {
    const stack = new Error().stack || ''
    const lines = stack.split('\n')
    // lines[0]="Error", lines[1]=log_write, lines[2]=调用者(LOG_XXX)
    const frame = lines[3] || lines[2] || ''
    const m = frame.match(/\((.+?):(\d+):\d+\)/) || frame.match(/@(.+?):(\d+):\d+/)
    if (m) {
      const path = m[1].split(/[/\\]/).pop()
      return `${path}:${m[2]}`
    }
  } catch {}
  return '?:?'
}

function write(level, args) {
  if (level > g_level) return
  const msg = args.map((a) => (typeof a === 'object' ? JSON.stringify(a) : String(a))).join(' ')
  const line = `[${caller()}][${timestamp()}][${LEVEL_NAMES[level]}] ${msg}`
  const fn = level <= LogLevel.WARN ? console.warn : console.log
  fn(line)
}

export function log_error(...args) { write(LogLevel.ERROR, args) }
export function log_warn(...args) { write(LogLevel.WARN, args) }
export function log_info(...args) { write(LogLevel.INFO, args) }
export function log_debug(...args) { write(LogLevel.DEBUG, args) }

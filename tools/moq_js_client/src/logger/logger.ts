export type LogLevel = 'debug' | 'info' | 'warn' | 'error'

const LEVEL_NUM: Record<LogLevel, number> = {
  debug: 10,
  info: 20,
  warn: 30,
  error: 40,
}

function parseLevel(raw: string | null): LogLevel | null {
  if (raw === 'debug' || raw === 'info' || raw === 'warn' || raw === 'error') return raw
  return null
}

function defaultLevel(): LogLevel {
  try {
    const q = parseLevel(new URLSearchParams(location.search).get('log'))
    if (q) return q
    const stored = parseLevel(localStorage.getItem('moq_log_level'))
    if (stored) return stored
  } catch {
    /* ignore */
  }
  return 'info'
}

let minLevel: LogLevel = defaultLevel()

export function set_log_level(level: LogLevel) {
  minLevel = level
}

export function get_log_level(): LogLevel {
  return minLevel
}

function enabled(level: LogLevel): boolean {
  return LEVEL_NUM[level] >= LEVEL_NUM[minLevel]
}

function stamp(): string {
  const d = new Date()
  const hh = String(d.getHours()).padStart(2, '0')
  const mm = String(d.getMinutes()).padStart(2, '0')
  const ss = String(d.getSeconds()).padStart(2, '0')
  const ms = String(d.getMilliseconds()).padStart(3, '0')
  return `${hh}:${mm}:${ss}.${ms}`
}

function line(level: string, tag: string, msg: string): string {
  return `${stamp()} [${level}] [${tag}] ${msg}`
}

export function log_debug(tag: string, msg: string, ...extra: unknown[]) {
  if (!enabled('debug')) return
  extra.length ? console.debug(line('DEBUG', tag, msg), ...extra) : console.debug(line('DEBUG', tag, msg))
}

export function log_info(tag: string, msg: string, ...extra: unknown[]) {
  if (!enabled('info')) return
  extra.length ? console.info(line('INFO', tag, msg), ...extra) : console.info(line('INFO', tag, msg))
}

export function log_warn(tag: string, msg: string, ...extra: unknown[]) {
  if (!enabled('warn')) return
  extra.length ? console.warn(line('WARN', tag, msg), ...extra) : console.warn(line('WARN', tag, msg))
}

export function log_error(tag: string, msg: string, ...extra: unknown[]) {
  if (!enabled('error')) return
  extra.length ? console.error(line('ERROR', tag, msg), ...extra) : console.error(line('ERROR', tag, msg))
}

export function hex_preview(data: Uint8Array, max = 48): string {
  const n = Math.min(data.byteLength, max)
  const parts: string[] = []
  for (let i = 0; i < n; i++) parts.push(data[i].toString(16).padStart(2, '0'))
  return parts.join(' ') + (data.byteLength > max ? ' ...' : '')
}

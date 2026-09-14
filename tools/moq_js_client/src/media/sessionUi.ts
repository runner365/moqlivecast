import type { SessionPhase } from './sessionPhase'

export function connectButtonLabel(
  phase: SessionPhase,
  connecting: boolean,
  stopping: boolean,
): string {
  if (connecting) return 'Connecting…'
  if (stopping) return 'Disconnecting…'
  if (phase === 'init' || phase === 'disconnected') return 'Connect'
  return 'Disconnect'
}

export function isSessionLive(phase: SessionPhase): boolean {
  return (
    phase === 'connecting' ||
    phase === 'connected and waiting for stream' ||
    phase === 'receiving stream' ||
    phase === 'pushing stream'
  )
}

export function isPullConnected(phase: SessionPhase): boolean {
  return (
    phase === 'connected and waiting for stream' ||
    phase === 'receiving stream'
  )
}

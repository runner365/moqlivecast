export const SESSION_PHASES = [
  'init',
  'connecting',
  'connected and waiting for stream',
  'receiving stream',
  'pushing stream',
  'disconnected',
] as const

export type SessionPhase = (typeof SESSION_PHASES)[number]

export type SessionPhaseHooks = {
  onPhase?: (phase: SessionPhase) => void
}

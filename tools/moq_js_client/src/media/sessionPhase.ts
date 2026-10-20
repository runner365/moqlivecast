export const SESSION_PHASES = [
  'init',
  'connecting',
  'connected and waiting for stream',
  /* SUBSCRIBE 成功但 N 秒内一个媒体对象都没到：不是网络问题，是服务端没有
   * 可发的东西（发布端没公告广播/没有轨，或推流端根本没在推）。 */
  'connected but no media',
  'receiving stream',
  'pushing stream',
  'disconnected',
] as const

export type SessionPhase = (typeof SESSION_PHASES)[number]

export type SessionPhaseHooks = {
  onPhase?: (phase: SessionPhase) => void
}

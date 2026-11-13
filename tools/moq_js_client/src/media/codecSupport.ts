export type VideoCodecChoice = 'h264' | 'h265'

export const H265_CODEC = 'hvc1.1.6.L93.B0'

/** Probe whether this Chrome can actually encode HEVC.
 *
 * isConfigSupported() is necessary but not sufficient on every platform; the
 * UI treats a true result as "offer the option" and the encoder still reports
 * its own configure error if the platform cannot start it. */
export async function probeH265Encode(
  width = 640,
  height = 360,
): Promise<boolean> {
  if (typeof VideoEncoder === 'undefined') return false
  const cfg = {
    codec: H265_CODEC,
    width,
    height,
    bitrate: 800_000,
    framerate: 15,
    hardwareAcceleration: 'prefer-hardware',
    latencyMode: 'realtime',
    hevc: { format: 'hevc' },
  } as VideoEncoderConfig & { hevc?: { format: string } }
  try {
    const r = await VideoEncoder.isConfigSupported(cfg)
    return !!r.supported
  } catch {
    return false
  }
}

export async function probeH265Decode(): Promise<boolean> {
  if (typeof VideoDecoder === 'undefined') return false
  try {
    const r = await VideoDecoder.isConfigSupported({ codec: H265_CODEC })
    return !!r.supported
  } catch {
    return false
  }
}
